// Per-nozzle process P4 in the G-code: diameter slots of one process, per-nozzle filament values, rack nozzle
// sizes, the physical nozzle declaration, and tool numbers that follow the filament nozzle assignment.

#include <catch2/catch.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ToolSlots.hpp"
#include "test_data.hpp"

#include <algorithm>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

void add_cube_per_filament(Model &model)
{
    for (int filament = 1; filament <= 2; ++filament) {
        ModelObject *object = model.add_object();
        object->name        = "cube-" + std::to_string(filament) + ".stl";
        object->add_volume(mesh(TestMesh::cube_20x20x20));
        object->add_instance()->set_offset(Vec3d(40. + 40. * filament, 40., 0.));
        object->ensure_on_bed();
        object->volumes.front()->config.set("extruder", filament);
    }
}

// A two-tool toolchanger, no prime tower, no layer-time slow-down, every role speed different.
DynamicPrintConfig two_tools(const char *diameters)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_filaments(2);
    config.set_deserialize_strict({
        { "nozzle_diameter", diameters },
        { "single_extruder_multi_material", "0" },
        { "enable_prime_tower", "0" },
        { "enable_arc_fitting", "0" },
        { "slow_down_for_layer_cooling", "0,0" },
        { "filament_max_volumetric_speed", "200,200" },
        { "outer_wall_speed", "41" },
        { "inner_wall_speed", "57" },
        { "sparse_infill_speed", "73" },
        { "internal_solid_infill_speed", "89" },
        { "top_surface_speed", "97" },
        { "nozzle_temperature", "210,210" },
        { "nozzle_temperature_initial_layer", "210,210" },
    });
    return config;
}

std::string slice(const DynamicPrintConfig &config)
{
    Print print;
    Model model;
    add_cube_per_filament(model);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    return Test::gcode(print);
}

// Per tool (as the G-code names it), the F values of the extruding G1 moves and the ;WIDTH values.
struct PerTool
{
    std::map<int, std::set<int>>    feeds;
    std::map<int, std::set<double>> widths;
};

PerTool per_tool(const std::string &gcode)
{
    PerTool                 out;
    std::istringstream      in(gcode);
    std::string             line;
    int                     tool = 0, feed = 0;
    static const std::regex tool_re(R"(^T([0-9]+)\b)");
    static const std::regex f_re(R"(\bF([0-9]+))");
    static const std::regex e_re(R"(\bE[0-9.]+)");
    static const std::regex width_re(R"(^;WIDTH:([0-9.]+))");
    while (std::getline(in, line)) {
        std::smatch m;
        if (std::regex_search(line, m, tool_re)) {
            tool = std::stoi(m[1].str());
            continue;
        }
        if (std::regex_search(line, m, width_re)) {
            out.widths[tool].insert(std::stod(m[1].str()));
            continue;
        }
        if (line.compare(0, 2, "G1") != 0)
            continue;
        if (std::regex_search(line, m, f_re))
            feed = std::stoi(m[1].str());
        if (std::regex_search(line, e_re))
            out.feeds[tool].insert(feed);
    }
    return out;
}

bool has_line(const std::string &gcode, const std::string &pattern)
{
    return std::regex_search(gcode, std::regex(pattern, std::regex::multiline));
}

} // namespace

TEST_CASE("One process with diameter slots prints each tool at its nozzle size's speed", "[ToolSlots][GCode]")
{
    DynamicPrintConfig config = two_tools("0.4,0.6");
    set_process_tool_slots(config, { 0, 0 }, { "Direct Drive Standard", "Direct Drive Standard" }, { 0.4, 0.6 });
    config.option<ConfigOptionFloats>("outer_wall_speed")->values = { 41, 23 };
    const PerTool tools = per_tool(slice(config));
    REQUIRE(tools.feeds.count(0) == 1);
    REQUIRE(tools.feeds.count(1) == 1);
    CHECK(tools.feeds.at(0).count(41 * 60) == 1);
    CHECK(tools.feeds.at(0).count(23 * 60) == 0);
    CHECK(tools.feeds.at(1).count(23 * 60) == 1);
    CHECK(tools.feeds.at(1).count(41 * 60) == 0);
}

TEST_CASE("One filament preset on a 0.4 and a 0.6 tool prints each nozzle's values", "[ToolSlots][GCode][FilamentValues]")
{
    DynamicPrintConfig config = two_tools("0.4,0.6");
    const std::string  preset = "0.6: nozzle_temperature=230, nozzle_temperature_initial_layer=232, filament_max_volumetric_speed=30";
    config.option<ConfigOptionStrings>("filament_nozzle_variants", true)->values = { preset, preset };
    const std::string gcode = slice(config);
    // The config block shows what printed: the 0.6 tool's filament at 230 / 232 and 30 mm3/s, the 0.4's unchanged.
    CHECK(gcode.find("; nozzle_temperature = 210,230") != std::string::npos);
    CHECK(gcode.find("; nozzle_temperature_initial_layer = 210,232") != std::string::npos);
    CHECK(gcode.find("; filament_max_volumetric_speed = 200,30") != std::string::npos);
    CHECK(gcode.find("; filament_nozzle_variants = ") != std::string::npos);
    // The tools are heated to their own temperatures.
    CHECK(has_line(gcode, R"(^M10[49] S23[02] T1\b)"));
    CHECK_FALSE(has_line(gcode, R"(^M10[49] S23[02] T0\b)"));
    // A job without per-nozzle values does not mention the key.
    CHECK(slice(two_tools("0.4,0.6")).find("; filament_nozzle_variants") == std::string::npos);
}

TEST_CASE("The physical nozzles are declared in the G-code of a mixed job only", "[ToolSlots][GCode][Header]")
{
    CHECK(slice(two_tools("0.4,0.6")).find("; physical_nozzle_properties = 0.4-standard,0.6-standard") != std::string::npos);
    CHECK(slice(two_tools("0.4,0.4")).find("; physical_nozzle_properties") == std::string::npos);
    DynamicPrintConfig mapping = two_tools("0.4,0.4");
    mapping.option<ConfigOptionBool>("support_filament_nozzle_mapping", true)->value = true;
    const std::string gcode = slice(mapping);
    CHECK(gcode.find("; physical_nozzle_properties = 0.4-standard,0.4-standard") != std::string::npos);
    CHECK(gcode.find("; support_filament_nozzle_mapping = 1") != std::string::npos);
}

TEST_CASE("A filament pinned to a smaller rack nozzle slices at that nozzle's widths", "[ToolSlots][GCode][Rack]")
{
    DynamicPrintConfig config = two_tools("0.4,0.4");
    config.set_deserialize_strict({
        { "layer_height", "0.1" },
        { "initial_layer_print_height", "0.1" },
        { "line_width", "100%" },
        { "outer_wall_line_width", "100%" },
        { "inner_wall_line_width", "100%" },
        { "sparse_infill_line_width", "100%" },
        { "internal_solid_infill_line_width", "100%" },
        { "top_surface_line_width", "100%" },
        { "initial_layer_line_width", "0" },
    });
    const PerTool before = per_tool(slice(config));
    REQUIRE(before.widths.count(1) == 1);
    CHECK(*before.widths.at(1).rbegin() > 0.35);

    // The second extruder's rack holds a 0.4 and a 0.2; filament 2 asks for the 0.2.
    config.option<ConfigOptionStrings>("extruder_rack_nozzle_diameter", true)->values = { "", "0.4,0.2" };
    config.option<ConfigOptionFloats>("filament_rack_nozzle_diameter", true)->values  = { 0., 0.2 };
    const std::string gcode = slice(config);
    const PerTool     after = per_tool(gcode);
    REQUIRE(after.widths.count(0) == 1);
    REQUIRE(after.widths.count(1) == 1);
    CHECK(*after.widths.at(1).rbegin() < 0.25);
    CHECK(after.widths.at(0) == before.widths.at(0));
    CHECK(gcode.find("; physical_nozzle_properties = 0.4-standard,0.4-standard,0.2-standard") != std::string::npos);
}

TEST_CASE("Filaments assigned to other tools are named by those tools in the G-code", "[ToolSlots][GCode][OutputTools]")
{
    DynamicPrintConfig config = two_tools("0.4,0.4");
    config.set_deserialize_strict({ { "nozzle_temperature", "205,225" }, { "nozzle_temperature_initial_layer", "205,225" } });
    const std::string plain = slice(config);
    CHECK(has_line(plain, R"(^M10[49] S205 T0\b)"));
    CHECK(has_line(plain, R"(^M10[49] S225 T1\b)"));

    // Swap: filament 1 on T2, filament 2 on T1.
    config.option<ConfigOptionInts>("filament_nozzle_assignment", true)->values = { 2, 1 };
    const std::string gcode = slice(config);
    CHECK(has_line(gcode, R"(^M10[49] S205 T1\b)"));
    CHECK(has_line(gcode, R"(^M10[49] S225 T0\b)"));
    CHECK_FALSE(has_line(gcode, R"(^M10[49] S205 T0\b)"));
    // The config block is by tool too, and the assignment is spent.
    CHECK(gcode.find("; nozzle_temperature = 225,205") != std::string::npos);
    CHECK(gcode.find("; filament_nozzle_assignment = ") == std::string::npos);
    // Both tools still print, at the same speeds as before (the process is the same).
    const PerTool before = per_tool(plain);
    const PerTool after  = per_tool(gcode);
    REQUIRE(after.feeds.count(0) == 1);
    REQUIRE(after.feeds.count(1) == 1);
    CHECK(after.feeds.at(0) == before.feeds.at(1));
    CHECK(after.feeds.at(1) == before.feeds.at(0));
}
