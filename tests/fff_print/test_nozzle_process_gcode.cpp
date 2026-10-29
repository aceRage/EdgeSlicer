// Per-nozzle process P1: a per-nozzle process changes the feed rates of the features its extruder prints,
// and nothing else; the same values on every extruder slice exactly as without one.

#include <catch2/catch.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/NozzleProcess.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "test_data.hpp"

#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

void add_cube_per_tool(Model &model)
{
    for (int tool = 1; tool <= 2; ++tool) {
        ModelObject *object = model.add_object();
        object->name        = "cube-" + std::to_string(tool) + ".stl";
        object->add_volume(mesh(TestMesh::cube_20x20x20));
        object->add_instance()->set_offset(Vec3d(40. + 40. * tool, 40., 0.));
        object->ensure_on_bed();
        object->volumes.front()->config.set("extruder", tool);
    }
}

// A two-tool toolchanger (no prime tower, no layer-time slow-down) whose role speeds are all different,
// so a feed rate names the role that printed it.
DynamicPrintConfig two_tool_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_filaments(2);
    config.set_deserialize_strict({
        { "nozzle_diameter", "0.4,0.4" },
        { "single_extruder_multi_material", "0" },
        { "enable_prime_tower", "0" },
        { "enable_arc_fitting", "0" },
        { "slow_down_for_layer_cooling", "0,0" },
        { "outer_wall_speed", "41" },
        { "inner_wall_speed", "57" },
        { "sparse_infill_speed", "73" },
        { "internal_solid_infill_speed", "89" },
        { "top_surface_speed", "97" },
    });
    return config;
}

// Per tool, the F values of the extruding G1 moves.
std::map<int, std::set<int>> extrusion_feeds_per_tool(const std::string &gcode)
{
    std::map<int, std::set<int>> out;
    std::istringstream           in(gcode);
    std::string                  line;
    int                          tool = 0;
    static const std::regex      tool_re(R"(^T([0-9]+)\b)");
    static const std::regex      f_re(R"(\bF([0-9]+))");
    static const std::regex      e_re(R"(\bE[0-9.]+)");
    int                          feed = 0;
    while (std::getline(in, line)) {
        std::smatch m;
        if (std::regex_search(line, m, tool_re)) {
            tool = std::stoi(m[1].str());
            continue;
        }
        if (line.compare(0, 2, "G1") != 0)
            continue;
        if (std::regex_search(line, m, f_re))
            feed = std::stoi(m[1].str());
        if (std::regex_search(line, e_re))
            out[tool].insert(feed);
    }
    return out;
}

std::string slice(const DynamicPrintConfig &config)
{
    Print print;
    Model model;
    add_cube_per_tool(model);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    return Test::gcode(print);
}

std::string without_config_block(const std::string &gcode)
{
    const size_t pos = gcode.find("; CONFIG_BLOCK_START");
    return pos == std::string::npos ? gcode : gcode.substr(0, pos);
}

} // namespace

TEST_CASE("A per-nozzle process changes only its own extruder's feed rates", "[NozzleProcess][GCode]")
{
    const DynamicPrintConfig plate = two_tool_config();
    const std::string        base  = slice(plate);

    DynamicPrintConfig second;
    second.set_key_value("outer_wall_speed", new ConfigOptionFloats({ 23 }));
    second.set_key_value("sparse_infill_speed", new ConfigOptionFloats({ 111 }));
    DynamicPrintConfig with_nozzle_process = plate;
    with_nozzle_process.option<ConfigOptionStrings>("nozzle_process_presets", true)->values = { "", "second" };
    REQUIRE(fill_nozzle_process_slots(with_nozzle_process, { nullptr, &second }));
    const std::string gcode = slice(with_nozzle_process);

    const auto base_feeds = extrusion_feeds_per_tool(base);
    const auto feeds      = extrusion_feeds_per_tool(gcode);
    REQUIRE(feeds.count(0) == 1);
    REQUIRE(feeds.count(1) == 1);
    // Tool 0 keeps the plate's outer wall and sparse infill (41 / 73 mm/s); tool 1 takes the second
    // process's (23 / 111 mm/s) and no longer prints at the plate's.
    CHECK(base_feeds.at(1).count(41 * 60) == 1);
    CHECK(feeds.at(0).count(41 * 60) == 1);
    CHECK(feeds.at(0).count(73 * 60) == 1);
    CHECK(feeds.at(0).count(23 * 60) == 0);
    CHECK(feeds.at(1).count(23 * 60) == 1);
    CHECK(feeds.at(1).count(111 * 60) == 1);
    CHECK(feeds.at(1).count(41 * 60) == 0);
    CHECK(feeds.at(1).count(73 * 60) == 0);
    // Tool 0's moves are exactly what it printed without a per-nozzle process.
    CHECK(feeds.at(0) == base_feeds.at(0));
    // The config block names the per-nozzle process; the plain job's does not.
    CHECK(gcode.find("; nozzle_process_presets = ") != std::string::npos);
    CHECK(base.find("; nozzle_process_presets = ") == std::string::npos);
    CHECK(base.find("; process_extruder_values = ") == std::string::npos);
}

TEST_CASE("A per-nozzle process equal to the plate's slices exactly as without it", "[NozzleProcess][GCode]")
{
    const DynamicPrintConfig plate = two_tool_config();
    const std::string        base  = slice(plate);

    DynamicPrintConfig same;
    same.set_key_value("outer_wall_speed", new ConfigOptionFloats({ 41 }));
    DynamicPrintConfig filled = plate;
    REQUIRE(fill_nozzle_process_slots(filled, { nullptr, &same }));
    REQUIRE_FALSE(process_extruder_slots_differ(filled));
    // The slots are identical, so every reader keeps the plate's column: the moves are byte-identical.
    CHECK(without_config_block(slice(filled)) == without_config_block(base));
}
