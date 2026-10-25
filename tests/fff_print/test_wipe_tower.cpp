#include <catch2/catch.hpp>

#include <string>
#include <vector>

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/WipeTower.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

// Orca #15133: on Klipper the wipe tower's planner flush is M400 and its dwell is G4 P<ms>.
// Klipper's G4 reads only P (milliseconds), so "G4 S0" neither synchronized nor waited there.

// Taken from the config enum map rather than hand-listed, so a flavor added to GCodeFlavor later
// is covered here without editing this file.
static std::vector<GCodeFlavor> non_klipper_flavors()
{
    std::vector<GCodeFlavor> flavors;
    for (const auto &kv : ConfigOptionEnum<GCodeFlavor>::get_enum_values())
        if (GCodeFlavor(kv.second) != gcfKlipper)
            flavors.push_back(GCodeFlavor(kv.second));
    return flavors;
}

static std::string flavor_name(GCodeFlavor flavor)
{
    return ConfigOptionEnum<GCodeFlavor>::get_enum_names()[int(flavor)];
}

TEST_CASE("Klipper flushes the wipe tower planner queue with M400", "[WipeTower][KlipperFlush]")
{
    CHECK(std::string(flush_planner_queue_command(gcfKlipper)) == "M400\n");
}

TEST_CASE("Other flavors flush the wipe tower planner queue with a zero dwell", "[WipeTower][KlipperFlush]")
{
    const GCodeFlavor flavor = GENERATE(from_range(non_klipper_flavors()));
    INFO("gcode flavor: " << flavor_name(flavor));
    CHECK(std::string(flush_planner_queue_command(flavor)) == "G4 S0\n");
}

// 1.5 s is exactly representable as a float, so neither form can drift when rounded.
TEST_CASE("Klipper waits in the wipe tower with a millisecond dwell", "[WipeTower][KlipperFlush]")
{
    CHECK(wait_command(gcfKlipper, 1.5f) == "G4 P1500\n");
}

TEST_CASE("Other flavors wait in the wipe tower with a seconds dwell", "[WipeTower][KlipperFlush]")
{
    const GCodeFlavor flavor = GENERATE(from_range(non_klipper_flavors()));
    INFO("gcode flavor: " << flavor_name(flavor));
    CHECK(wait_command(flavor, 1.5f) == "G4 S1.500\n");
}

// The G-code inside each WIPE_TOWER_START / WIPE_TOWER_END pair, concatenated, so an M400 emitted
// outside the tower (start / end G-code, the pre-heat injector) cannot create a false match.
static std::string wipe_tower_regions(const std::string &gcode)
{
    const std::string &start_tag = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_Start);
    const std::string &end_tag   = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_End);
    std::string regions;
    size_t      pos = 0;
    while (true) {
        const size_t start = gcode.find(start_tag, pos);
        if (start == std::string::npos)
            break;
        const size_t end = gcode.find(end_tag, start);
        if (end == std::string::npos)
            break;
        regions.append(gcode, start, end - start);
        pos = end + 1;
    }
    return regions;
}

// A WipeTower2 (non-Bambu) two-filament print: outer wall in filament 2, the rest in filament 1,
// so every layer runs the toolchange path and its flush_planner_queue(). Same shape as
// test_print.cpp's "prepare and slice agree on the plate filaments" case (one apply keeps the tower).
static std::string slice_two_filament_tower(const std::string &gcode_flavor, bool snapmaker_u1)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#00FF00"};
    config.option<ConfigOptionBool>("enable_prime_tower")->value   = true;
    config.option<ConfigOptionBool>("enable_support")->value       = false;
    config.option<ConfigOptionBool>("spiral_mode")->value          = false;
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value = false;
    config.option<ConfigOptionBool>("purge_in_prime_tower")->value           = false;
    config.option<ConfigOptionInt>("outer_wall_filament")->value    = 2;
    config.option<ConfigOptionInt>("wall_filament")->value          = 1;
    config.option<ConfigOptionInt>("sparse_infill_filament")->value = 1;
    config.option<ConfigOptionInt>("solid_infill_filament")->value  = 1;
    config.option<ConfigOptionFloats>("wipe_tower_x")->values       = {15.};
    config.option<ConfigOptionFloats>("wipe_tower_y")->values       = {15.};
    config.option<ConfigOptionFloat>("prime_tower_width")->value    = 35.;
    config.set_deserialize_strict({ { "gcode_flavor", gcode_flavor } });
    if (snapmaker_u1)
        config.set_key_value("printer_model", new ConfigOptionString("Snapmaker U1"));

    Print print;
    Model model;
    init_print({TestMesh::cube_20x20x20}, print, model, config);
    REQUIRE(print.has_wipe_tower());
    return Test::gcode(print);
}

TEST_CASE("The wipe tower's toolchange planner flush follows the gcode flavor", "[WipeTower][KlipperFlush]")
{
    SECTION("klipper") {
        const std::string tower = wipe_tower_regions(slice_two_filament_tower("klipper", false));
        REQUIRE_FALSE(tower.empty());
        CHECK_THAT(tower, Catch::Matchers::Contains("M400"));
        CHECK_THAT(tower, !Catch::Matchers::Contains("G4 S"));
    }
    SECTION("marlin") {
        const std::string tower = wipe_tower_regions(slice_two_filament_tower("marlin", false));
        REQUIRE_FALSE(tower.empty());
        CHECK_THAT(tower, Catch::Matchers::Contains("G4 S0"));
        CHECK_THAT(tower, !Catch::Matchers::Contains("M400"));
    }
}

// EDGE: the U1 is a Klipper printer whose tower takes Edge's U1-only writer paths (is_snapmaker_u1:
// M220 B/R, SET_PRESSURE_ADVANCE, stagger). Its flush must be M400 too, and no seconds dwell may remain.
TEST_CASE("The Snapmaker U1 tower flushes with M400 and never a seconds dwell", "[WipeTower][KlipperFlush]")
{
    const std::string tower = wipe_tower_regions(slice_two_filament_tower("klipper", true));
    REQUIRE_FALSE(tower.empty());
    CHECK_THAT(tower, Catch::Matchers::Contains("CP TOOLCHANGE"));
    CHECK_THAT(tower, Catch::Matchers::Contains("M400"));
    CHECK_THAT(tower, !Catch::Matchers::Contains("G4 S"));
}
