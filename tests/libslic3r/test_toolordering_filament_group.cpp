#include <catch2/catch.hpp>

#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/GCode/ToolOrdering.hpp"

using namespace Slic3r;

// Orca #14789 adapt: FilamentGroup indexes the grouping context's filament_info by filament id,
// so a short per-filament array must not shorten it. build_filament_group_context now walks
// filament_colour.size() and reads colour/type/is_support through get_at.

TEST_CASE("Grouping context spans the filament count with mis-sized config arrays", "[ToolOrdering][H2C]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    // Single 6-nozzle extruder: opens the grouping engine without needing a BBL multi-extruder.
    config.option<ConfigOptionFloats>("nozzle_diameter", true)->values           = {0.4};
    config.option<ConfigOptionInts>("extruder_max_nozzle_count", true)->values   = {6};
    config.option<ConfigOptionStrings>("extruder_nozzle_stats", true)->values    = {"Standard#6"};

    // Four filaments, with filament_type / filament_is_support left short on purpose.
    config.option<ConfigOptionStrings>("filament_colour", true)->values          = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    config.option<ConfigOptionStrings>("filament_type", true)->values            = {"PLA"};
    config.option<ConfigOptionBools>("filament_is_support", true)->values        = {0};
    config.option<ConfigOptionFloats>("filament_diameter", true)->values         = {1.75, 1.75, 1.75, 1.75};
    config.option<ConfigOptionInts>("filament_map", true)->values                = {1, 1, 1, 1};
    config.option<ConfigOptionFloats>("flush_volumes_matrix", true)->values      = std::vector<double>(16, 140.);

    Model model;
    model.add_object("cube", "", make_cube(20, 20, 20))->add_instance();

    Print print;
    print.apply(model, config);
    // apply() does not pad the per-filament arrays, so the mis-sizing survives into the engine.
    REQUIRE(print.config().filament_type.values.size() < print.config().filament_colour.values.size());

    std::vector<std::vector<unsigned int>> layer_filaments = {{0, 1}, {1, 2}, {2, 3}};

    SECTION("short per-filament arrays still yield one entry per filament")
    {
        auto result = ToolOrdering::get_recommended_filament_maps(&print, layer_filaments, FilamentMapMode::fmmAutoForFlush, {}, {}, {});
        REQUIRE(result.get_extruder_map(false).size() == 4);
        for (int f = 0; f < 4; ++f)
            REQUIRE(result.get_extruder_id(f) == 0);
    }

    SECTION("filament_ids longer than the filament count is truncated, not paired past the end")
    {
        config.option<ConfigOptionStrings>("filament_ids", true)->values = {"a", "b", "c", "d", "e", "f"};
        print.apply(model, config);
        auto result = ToolOrdering::get_recommended_filament_maps(&print, layer_filaments, FilamentMapMode::fmmAutoForFlush, {}, {}, {});
        REQUIRE(result.get_extruder_map(false).size() == 4);
    }
}
