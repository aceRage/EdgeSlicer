#include <catch2/catch.hpp>

#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

// A machine with several INDEPENDENT, IDENTICAL toolheads - the Snapmaker U1 family and the
// Flashforge Creator 5 - prints filament i from toolhead i. Nothing in this fork used to say so:
// ToolOrdering's filament->nozzle grouping engine is gated on the printer carrying more than one
// extruder VARIANT (DynamicPrintConfig::support_different_extruders), which is true of the
// dual-nozzle machines (H2D/H2C/X2D) and false of a toolchanger, so filament_map stayed at its
// preset default of {1} and every reported artefact claimed all filaments sat in toolhead 1.
//
// That is invisible to the U1, whose firmware follows the T commands, but the Creator 5 runs
// Bambu-derived firmware that resolves a filament to a physical nozzle through this map: a plate
// that says "everything is in nozzle 1" and then drives T3 is what the printer choked on.
//
// identity_filament_map() reports the real assignment. It is deliberately NOT written into the live
// PrintConfig - see its header comment - because filament_map doubles as this fork's dual-nozzle
// routing signal, and a differing map zeroes the purge volume on every filament change. Reporting
// only keeps the emitted G-code byte-identical.

namespace {

// The shape of a toolchanger preset: `extruders` identical toolheads, one filament each.
DynamicPrintConfig make_toolchanger_config(size_t extruders, const char *variant = "Direct Drive Standard")
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("nozzle_diameter", new ConfigOptionFloats(std::vector<double>(extruders, 0.4)));
    cfg.set_key_value("single_extruder_multi_material", new ConfigOptionBool(false));
    if (variant != nullptr)
        cfg.set_key_value("extruder_variant_list", new ConfigOptionStrings(std::vector<std::string>(extruders, variant)));
    return cfg;
}

} // namespace

TEST_CASE("Identical multi-extruder printers are told apart from the grouping machines", "[FilamentMap]")
{
    // The Creator 5 / U1 shape: four identical toolheads, each with its own filament.
    CHECK(is_identical_multi_extruder_printer(make_toolchanger_config(4)));
    CHECK(is_identical_multi_extruder_printer(make_toolchanger_config(2)));

    // A single-nozzle machine has nothing to map.
    CHECK_FALSE(is_identical_multi_extruder_printer(make_toolchanger_config(1)));

    // An AMS-style machine has one toolhead and many spools, so filament index is not a toolhead.
    {
        DynamicPrintConfig cfg = make_toolchanger_config(4);
        cfg.set_key_value("single_extruder_multi_material", new ConfigOptionBool(true));
        CHECK_FALSE(is_identical_multi_extruder_printer(cfg));
    }

    // A dual-nozzle grouping machine (H2D/H2C/X2D) carries more than one extruder variant. Its
    // assignment is ToolOrdering's to compute and this identity map must not pre-empt it.
    {
        DynamicPrintConfig cfg = make_toolchanger_config(2);
        cfg.set_key_value("extruder_variant_list",
                          new ConfigOptionStrings({"Direct Drive Standard", "Bowden Standard"}));
        CHECK_FALSE(is_identical_multi_extruder_printer(cfg));
    }

    // A preset that names no variants at all is still a toolchanger by its nozzles (the U1's own
    // profile declares no extruder_variant_list).
    CHECK(is_identical_multi_extruder_printer(make_toolchanger_config(4, nullptr)));
}

TEST_CASE("A toolchanger's reported filament map is the identity, wrapping past the last toolhead",
          "[FilamentMap]")
{
    const DynamicPrintConfig c5 = make_toolchanger_config(4);

    // One filament per toolhead - what upstream OrcaSlicer reports for a Creator 5 plate
    // ("; filament_map = 1,2,3,4"), against our old "1,1,1,1".
    CHECK(identity_filament_map(c5, 4) == std::vector<int>{1, 2, 3, 4});

    // Fewer filaments than toolheads.
    CHECK(identity_filament_map(c5, 2) == std::vector<int>{1, 2});
    CHECK(identity_filament_map(c5, 1) == std::vector<int>{1});

    // More filaments than toolheads wraps, which is exactly what Flash Studio writes for a
    // five-filament plate on the four-head Creator 5: "1 2 3 4 1".
    CHECK(identity_filament_map(c5, 5) == std::vector<int>{1, 2, 3, 4, 1});
    CHECK(identity_filament_map(c5, 6) == std::vector<int>{1, 2, 3, 4, 1, 2});

    // No filaments, no map.
    CHECK(identity_filament_map(c5, 0).empty());

    // Every entry is a real 1-based toolhead.
    for (int e : identity_filament_map(c5, 9)) {
        CHECK(e >= 1);
        CHECK(e <= 4);
    }
}

TEST_CASE("Printers that are not identical multi-extruder machines get no reported map", "[FilamentMap]")
{
    // A single-nozzle machine: the empty result is what leaves every existing caller's behaviour
    // exactly as it was, which is how this change stays confined to toolchangers.
    CHECK(identity_filament_map(make_toolchanger_config(1), 4).empty());

    {
        DynamicPrintConfig ams = make_toolchanger_config(4);
        ams.set_key_value("single_extruder_multi_material", new ConfigOptionBool(true));
        CHECK(identity_filament_map(ams, 4).empty());
    }

    {
        DynamicPrintConfig h2d = make_toolchanger_config(2);
        h2d.set_key_value("extruder_variant_list",
                          new ConfigOptionStrings({"Direct Drive Standard", "Bowden Standard"}));
        CHECK(identity_filament_map(h2d, 4).empty());
    }

    // A config with no nozzle_diameter at all must not crash or invent a map.
    {
        DynamicPrintConfig empty;
        CHECK_FALSE(is_identical_multi_extruder_printer(empty));
        CHECK(identity_filament_map(empty, 4).empty());
    }
}
