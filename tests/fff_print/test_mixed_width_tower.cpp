// Mixed nozzle diameters (per-nozzle P2): both prime towers lay each tool's ramming and purge at that
// tool's own width (nozzle x 1.25) and keep one structure width for the footprint, the wall, the brim and
// the sparse fill. WipeTower2 follows FOrca 0b1f80cd22 (AGPL-3.0); WipeTower (Bambu) is the same design.

#include <catch2/catch.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/PrintConfig.hpp"
#include "test_data.hpp"

#include <cmath>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;
using Catch::Matchers::WithinAbs;

namespace {

void add_cube_per_tool(Model &model)
{
    for (int tool = 1; tool <= 2; ++tool) {
        ModelObject *object = model.add_object();
        object->name        = "cube-" + std::to_string(tool) + ".stl";
        object->add_volume(mesh(TestMesh::cube_20x20x20));
        object->add_instance()->set_offset(Vec3d(60. + 40. * tool, 60., 0.));
        object->ensure_on_bed();
        object->volumes.front()->config.set("extruder", tool);
    }
}

// A two-tool toolchanger (not a single-nozzle MMU) with the prime tower on.
DynamicPrintConfig two_tool_tower_config(const char *nozzles, const char *tower_type)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("filament_diameter")->values = { 1.75, 1.75 };
    config.option<ConfigOptionStrings>("filament_colour")->values  = { "#FF0000", "#0000FF" };
    config.option<ConfigOptionFloats>("wipe_tower_x")->values      = { 15. };
    config.option<ConfigOptionFloats>("wipe_tower_y")->values      = { 15. };
    config.set_deserialize_strict({
        { "nozzle_diameter", nozzles },
        { "single_extruder_multi_material", "0" },
        { "enable_prime_tower", "1" },
        { "wipe_tower_type", tower_type },
        { "prime_tower_width", "35" },
        { "wipe_tower_extra_flow", "100%" },
        { "enable_arc_fitting", "0" },
        { "skirt_loops", "0" },
        { "brim_type", "no_brim" },
        { "machine_start_gcode", "" },
        // A plain T<n> line marks each toolchange in both towers' G-code.
        { "change_filament_gcode", "T{next_extruder}" },
        { "layer_height", "0.2" },
        { "initial_layer_print_height", "0.2" },
    });
    return config;
}

std::string slice(const DynamicPrintConfig &config, bool bbl)
{
    Print print;
    Model model;
    add_cube_per_tool(model);
    print.apply(model, config);
    if (bbl)
        print.is_BBL_printer() = true;
    print.validate();
    print.set_status_silent();
    return Test::gcode(print);
}

// For each tool, the ;WIDTH values announced for its purge lines: the first width tag after
// "; CP TOOLCHANGE WIPE" in a toolchange block, against the last T<n> before it.
std::map<int, std::set<long>> wipe_widths_per_tool(const std::string &gcode)
{
    std::map<int, std::set<long>> out;
    std::istringstream            in(gcode);
    std::string                   line;
    int                           tool    = 0;
    bool                          in_wipe = false;
    static const std::regex       tool_re(R"(^T([0-9]+)\b)");
    static const std::regex       width_re(R"(^;\s*(?:LINE_)?WIDTH:\s*([0-9.]+))");
    while (std::getline(in, line)) {
        std::smatch m;
        if (std::regex_search(line, m, tool_re)) {
            tool = std::stoi(m[1].str());
            continue;
        }
        if (line.find("; CP TOOLCHANGE WIPE") != std::string::npos) {
            in_wipe = true;
            continue;
        }
        if (line.find("; CP TOOLCHANGE END") != std::string::npos) {
            in_wipe = false;
            continue;
        }
        if (in_wipe && std::regex_search(line, m, width_re)) {
            // In micrometres, so a set can hold them.
            out[tool].insert(std::lround(std::stod(m[1].str()) * 1000.));
            in_wipe = false;
        }
    }
    return out;
}

std::set<long> all_widths(const std::string &gcode)
{
    std::set<long>          out;
    std::istringstream      in(gcode);
    std::string             line;
    static const std::regex width_re(R"(^;\s*(?:LINE_)?WIDTH:\s*([0-9.]+))");
    bool                    in_tower = false;
    while (std::getline(in, line)) {
        if (line.find("; CP TOOLCHANGE START") != std::string::npos)
            in_tower = true;
        else if (line.find("; CP TOOLCHANGE END") != std::string::npos)
            in_tower = false;
        std::smatch m;
        if (in_tower && std::regex_search(line, m, width_re))
            out.insert(std::lround(std::stod(m[1].str()) * 1000.));
    }
    return out;
}

} // namespace

TEST_CASE("WipeTower2 purges each tool at its own nozzle's width", "[MixedNozzle][WipeTower]")
{
    const DynamicPrintConfig config = two_tool_tower_config("0.4,0.6", "type2");

    Print print;
    Model model;
    add_cube_per_tool(model);
    print.apply(model, config);
    // Mixed diameters no longer refuse the tower.
    const StringObjectException err = print.validate();
    INFO(err.string);
    CHECK(err.string.find("prime tower") == std::string::npos);

    const std::string gcode  = slice(config, false);
    const auto        widths = wipe_widths_per_tool(gcode);
    REQUIRE(widths.count(0) == 1);
    REQUIRE(widths.count(1) == 1);
    // 0.4 x 1.25 = 0.5 mm for T0, 0.6 x 1.25 = 0.75 mm for T1 (first layer included: the extra flow is 100 %).
    CHECK(widths.at(0) == std::set<long>{ 500 });
    CHECK(widths.at(1) == std::set<long>{ 750 });
}

TEST_CASE("WipeTower2 with uniform nozzles keeps one tower width", "[MixedNozzle][WipeTower]")
{
    const std::string gcode  = slice(two_tool_tower_config("0.4,0.4", "type2"), false);
    const auto        widths = wipe_widths_per_tool(gcode);
    REQUIRE(widths.count(0) == 1);
    REQUIRE(widths.count(1) == 1);
    CHECK(widths.at(0) == std::set<long>{ 500 });
    CHECK(widths.at(1) == std::set<long>{ 500 });
}

TEST_CASE("The Bambu tower purges each tool at its own nozzle's width", "[MixedNozzle][WipeTower]")
{
    const std::string gcode = slice(two_tool_tower_config("0.4,0.6", "type1"), true);
    REQUIRE(gcode.find("CP TOOLCHANGE") != std::string::npos);
    const auto widths = wipe_widths_per_tool(gcode);
    // Every purge announces its tool's width: 0.5 for the 0.4 nozzle, 0.75 for the 0.6 (x 1.15 on the
    // first layer).
    for (const auto &[tool, set] : widths)
        for (long w : set) {
            INFO("tool " << tool << " width " << w);
            // Bambu G-code writes "; LINE_WIDTH: 0.862500" on the first layer (0.75 x 1.15): within 1 micrometre.
            const long plain = tool == 0 ? 500 : 750;
            const long first = tool == 0 ? 575 : 863;
            CHECK((std::abs(w - plain) <= 1 || std::abs(w - first) <= 1));
        }
    // T0 purges at the structure width (0.5 mm) and, past the first layer, announces nothing new: only T1 is
    // sure to have announced widths.
    REQUIRE(widths.count(1) == 1);
    // The 0.6 tool's purge is announced at its own width, not the tower's 0.5 mm structure width.
    CHECK(*widths.at(1).rbegin() >= 750);
}

TEST_CASE("The Bambu tower with uniform nozzles announces no per-tool width", "[MixedNozzle][WipeTower]")
{
    // Uniform nozzles: no new ;WIDTH tags inside the tower blocks besides the first-layer ones Bambu
    // already wrote (1.15 x and back to 1 x the single width).
    const std::string gcode  = slice(two_tool_tower_config("0.4,0.4", "type1"), true);
    const auto        widths = all_widths(gcode);
    REQUIRE_FALSE(widths.empty()); // the first layer's 1.15 x tags at least
    for (long w : widths) {
        INFO("width " << w);
        CHECK((w == 0 || w == 500 || w == 575));
    }
}
