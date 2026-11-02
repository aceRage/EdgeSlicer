// Per-nozzle process P9 in the G-code: a filament on a rack slot with a process of its own prints that process's region
// settings and speeds; the other filaments, also on the same extruder, print what they printed before. The rack nozzle change
// itself (M632 / M633 between slots) is covered in tests/libslic3r/test_nozzle_rack_slots.cpp.

#include <catch2/catch.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/NozzleProcess.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ToolSlots.hpp"
#include "test_data.hpp"

#include <cctype>
#include <set>
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

// Two tools; the second has a rack of two 0.4 nozzles and filament 2 is pinned to its slot 2.
DynamicPrintConfig rack_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_filaments(2);
    config.set_deserialize_strict({
        { "nozzle_diameter", "0.4,0.4" },
        { "single_extruder_multi_material", "0" },
        { "enable_prime_tower", "0" },
        { "enable_arc_fitting", "0" },
        { "slow_down_for_layer_cooling", "0,0" },
        { "filament_max_volumetric_speed", "200,200" },
        { "wall_loops", "2" },
        { "outer_wall_speed", "41" },
    });
    config.option<ConfigOptionStrings>("extruder_rack_nozzle_diameter", true)->values = { "", "0.4,0.4" };
    config.option<ConfigOptionInts>("filament_rack_nozzle_slot", true)->values        = { 0, 2 };
    return config;
}

std::string slice(const DynamicPrintConfig &config, Print &print)
{
    Model model;
    add_cube_per_tool(model);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    return Test::gcode(print);
}

// The F values of one tool's extruding moves of one feature type.
std::set<int> feeds_in_tool(const std::string &gcode, int tool, const std::string &type)
{
    std::set<int> out;
    int           current = 0, feed = 0;
    std::string   current_type;
    size_t        start = 0;
    while (start < gcode.size()) {
        size_t end = gcode.find('\n', start);
        if (end == std::string::npos)
            end = gcode.size();
        std::string line = gcode.substr(start, end - start);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.size() >= 2 && line[0] == 'T' && std::isdigit(static_cast<unsigned char>(line[1])))
            current = std::atoi(line.c_str() + 1);
        else if (line.rfind(";TYPE:", 0) == 0)
            current_type = line.substr(6);
        else if (line.rfind("G1 ", 0) == 0) {
            if (const size_t f = line.find(" F"); f != std::string::npos)
                feed = std::atoi(line.c_str() + f + 2);
            if (current == tool && current_type == type && line.find(" E") != std::string::npos)
                out.insert(feed);
        }
        start = end + 1;
    }
    return out;
}

} // namespace

TEST_CASE("A rack slot's process prints the walls and speeds of the filament pinned to it", "[NozzleRackSlots][GCode]")
{
    const DynamicPrintConfig plate = rack_config();
    REQUIRE(filament_rack_slot(plate, 2) == 2);
    DynamicPrintConfig slot_process = plate;
    slot_process.set_key_value("wall_loops", new ConfigOptionInt(4));
    slot_process.option<ConfigOptionFloats>("outer_wall_speed")->values = { 23. };

    DynamicPrintConfig filled = plate;
    const std::vector<const DynamicPrintConfig *> filament_sources{ nullptr, &slot_process };
    REQUIRE(fill_nozzle_process_slots(filled, { nullptr, nullptr }, &filament_sources));

    Print             print;
    const std::string gcode = slice(filled, print);
    REQUIRE(print.objects().size() == 2);
    CHECK(print.objects()[0]->all_regions().front().get().config().wall_loops.value == 2);
    CHECK(print.objects()[1]->all_regions().front().get().config().wall_loops.value == 4);

    Print             plain_print;
    const std::string plain = slice(plate, plain_print);
    // The slot's filament (G-code T1): its outer wall at 23 mm/s; T0's as before.
    CHECK(feeds_in_tool(gcode, 1, "Outer wall").count(23 * 60) == 1);
    CHECK(feeds_in_tool(plain, 1, "Outer wall").count(23 * 60) == 0);
    CHECK(feeds_in_tool(gcode, 0, "Outer wall") == feeds_in_tool(plain, 0, "Outer wall"));
    // Internal keys never reach the config block; the pin does.
    CHECK(gcode.find("; nozzle_filament_process_slot") == std::string::npos);
    CHECK(gcode.find("; nozzle_slot_process_missing") == std::string::npos);
    CHECK(gcode.find("; filament_rack_nozzle_slot = 0,2") != std::string::npos);
}

TEST_CASE("Without slot pins or slot processes the config block stays as it was", "[NozzleRackSlots][GCode][Identity]")
{
    DynamicPrintConfig config = rack_config();
    config.option<ConfigOptionInts>("filament_rack_nozzle_slot")->values = { 0 };
    Print             print;
    const std::string gcode = slice(config, print);
    CHECK(gcode.find("; filament_rack_nozzle_slot") == std::string::npos);
    CHECK(gcode.find("; extruder_rack_nozzle_override") == std::string::npos);
    CHECK(gcode.find("; nozzle_slot_process_presets") == std::string::npos);
}
