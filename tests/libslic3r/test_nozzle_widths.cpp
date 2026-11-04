// Per-nozzle process P6: every line width per nozzle on mixed-nozzle printers (NozzleProcess.hpp, "P6").
// The resolution rules, the reference nozzle, the per-nozzle table and its precedence against a nozzle's own
// process. The G-code side is in tests/fff_print/test_nozzle_widths_gcode.cpp.

#include <catch2/catch.hpp>

#include "libslic3r/NozzleProcess.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ToolSlots.hpp"

#include <cmath>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {

// A U1-like toolchanger: T1/T2 0.4, T3/T4 0.6, process widths in mm made for 0.4.
DynamicPrintConfig mixed_u1()
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_num_filaments(4);
    cfg.set_deserialize_strict({
        { "nozzle_diameter", "0.4,0.4,0.6,0.6" },
        { "single_extruder_multi_material", "0" },
        { "printer_variant", "0.4" },
        { "line_width", "0.42" },
        { "outer_wall_line_width", "0.42" },
        { "inner_wall_line_width", "0.45" },
        { "sparse_infill_line_width", "0.45" },
        { "internal_solid_infill_line_width", "0.42" },
        { "top_surface_line_width", "0.42" },
        { "initial_layer_line_width", "0.5" },
        { "support_line_width", "0.42" },
        { "bridge_line_width", "0.4" },
    });
    return cfg;
}

bool near(double a, double b) { return std::abs(a - b) < 1e-6; }

double width_of(const DynamicPrintConfig &cfg, const char *key, int tool, double diameter)
{
    return nozzle_width_for(cfg, key, tool, diameter).value.value;
}

const std::vector<FloatOrPercent> &array(const DynamicPrintConfig &cfg, const char *key)
{
    return cfg.option<ConfigOptionFloatsOrPercents>(key)->values;
}

} // namespace

TEST_CASE("The nozzle a process's widths were made for", "[NozzleWidths]")
{
    DynamicPrintConfig cfg = mixed_u1();
    CHECK(near(line_width_reference_diameter(cfg), 0.4)); // printer_variant
    cfg.set_deserialize_strict({ { "printer_variant", "0.4+0.6" } });
    CHECK(near(line_width_reference_diameter(cfg), 0.4)); // the mixed U1 machine: its first number
    cfg.set_deserialize_strict({ { "printer_variant", "" } });
    CHECK(near(line_width_reference_diameter(cfg), 0.4)); // the first extruder's nozzle
    cfg.set_deserialize_strict({ { "nozzle_diameter", "0.6,0.4,0.6,0.6" } });
    CHECK(near(line_width_reference_diameter(cfg), 0.6));
    cfg.set_deserialize_strict({ { "line_width_nozzle_diameter", "0.25" } });
    CHECK(near(line_width_reference_diameter(cfg), 0.25)); // set on the process: wins
}

TEST_CASE("Each width category scales from the process's nozzle to a 0.6 nozzle", "[NozzleWidths]")
{
    const DynamicPrintConfig cfg = mixed_u1();
    SECTION("mm widths: x1.5 on the 0.6, as they are on the 0.4")
    {
        CHECK(near(width_of(cfg, "outer_wall_line_width", 3, 0.6), 0.63));
        CHECK(near(width_of(cfg, "inner_wall_line_width", 3, 0.6), 0.675));
        CHECK(near(width_of(cfg, "sparse_infill_line_width", 3, 0.6), 0.675));
        CHECK(near(width_of(cfg, "internal_solid_infill_line_width", 3, 0.6), 0.63));
        CHECK(near(width_of(cfg, "top_surface_line_width", 3, 0.6), 0.63));
        CHECK(near(width_of(cfg, "initial_layer_line_width", 3, 0.6), 0.75));
        CHECK(near(width_of(cfg, "support_line_width", 3, 0.6), 0.63));
        CHECK(near(width_of(cfg, "bridge_line_width", 3, 0.6), 0.6));
        CHECK(near(width_of(cfg, "line_width", 3, 0.6), 0.63));
        CHECK(nozzle_width_for(cfg, "outer_wall_line_width", 3, 0.6).source == NozzleWidthSource::Scaled);
        for (const std::string &key : nozzle_width_option_keys()) {
            const auto *plate = cfg.option<ConfigOptionFloatOrPercent>(key);
            const NozzleWidth w = nozzle_width_for(cfg, key, 1, 0.4);
            CHECK(w.source == NozzleWidthSource::Plate);
            CHECK(near(w.value.value, plate->value));
        }
    }
    SECTION("percentages and auto resolve against the nozzle as they are")
    {
        DynamicPrintConfig c = cfg;
        c.set_deserialize_strict({ { "outer_wall_line_width", "105%" }, { "skin_infill_line_width", "100%" } });
        const NozzleWidth w = nozzle_width_for(c, "outer_wall_line_width", 3, 0.6);
        CHECK(w.source == NozzleWidthSource::Plate);
        CHECK(w.value.percent);
        CHECK(near(w.value.value, 105.));
        CHECK(nozzle_width_for(c, "skin_infill_line_width", 3, 0.6).value.percent);
    }
    SECTION("a 0 role width follows line_width; 0 first-layer and bridge widths keep their meaning")
    {
        DynamicPrintConfig c = cfg;
        c.set_deserialize_strict({ { "sparse_infill_line_width", "0" }, { "initial_layer_line_width", "0" }, { "bridge_line_width", "0" },
                                   { "line_width", "0.45" } });
        CHECK(near(width_of(c, "sparse_infill_line_width", 3, 0.6), 0.675));
        CHECK(near(width_of(c, "sparse_infill_line_width", 1, 0.4), 0.)); // unchanged: the object's line_width
        CHECK(near(width_of(c, "initial_layer_line_width", 3, 0.6), 0.));
        CHECK(near(width_of(c, "bridge_line_width", 3, 0.6), 0.));
        c.set_deserialize_strict({ { "line_width", "0" } });
        CHECK(near(width_of(c, "sparse_infill_line_width", 3, 0.6), 0.)); // auto from the nozzle
    }
    SECTION("rack nozzles count as sizes: a 0.2 rack nozzle of a 0.4 printer scales down")
    {
        DynamicPrintConfig c = DynamicPrintConfig::full_print_config();
        c.set_num_filaments(2);
        c.set_deserialize_strict({ { "nozzle_diameter", "0.4,0.4" },
                                   { "printer_variant", "0.4" },
                                   { "outer_wall_line_width", "0.42" } });
        // extruder_rack_nozzle_diameter is one text per extruder.
        c.option<ConfigOptionStrings>("extruder_rack_nozzle_diameter", true)->values = { "0.4,0.2", "" };
        REQUIRE(printer_nozzle_sizes(c).size() == 2);
        CHECK(near(width_of(c, "outer_wall_line_width", 1, 0.2), 0.21));
    }
}

TEST_CASE("One nozzle size: nothing is scaled and nothing resolved", "[NozzleWidths][Identity]")
{
    DynamicPrintConfig cfg = mixed_u1();
    cfg.set_deserialize_strict({ { "nozzle_diameter", "0.6,0.6,0.6,0.6" } }); // a 0.4 printer preset with 0.6 nozzles fitted
    CHECK(nozzle_width_for(cfg, "outer_wall_line_width", 3, 0.6).source == NozzleWidthSource::Plate);
    CHECK(near(width_of(cfg, "outer_wall_line_width", 3, 0.6), 0.42));
    CHECK_FALSE(resolve_nozzle_line_widths(cfg));
    CHECK(array(cfg, "nozzle_outer_wall_line_width").empty());
    CHECK(array(cfg, "nozzle_initial_layer_line_width").empty());

    // Mixed sizes, but every width a percentage: nothing changes, so nothing is resolved either.
    DynamicPrintConfig pct = mixed_u1();
    for (const std::string &key : nozzle_width_option_keys())
        pct.set_key_value(key, new ConfigOptionFloatOrPercent(105., true));
    CHECK_FALSE(resolve_nozzle_line_widths(pct));
    CHECK(array(pct, "nozzle_outer_wall_line_width").empty());
}

TEST_CASE("Mixed nozzle sizes resolve every filament's widths", "[NozzleWidths][Resolve]")
{
    DynamicPrintConfig cfg = mixed_u1();
    REQUIRE(resolve_nozzle_line_widths(cfg));
    // Filaments 1/2 on the 0.4 tools keep the plate's; 3/4 on the 0.6 tools are scaled.
    const auto &outer = array(cfg, "nozzle_outer_wall_line_width");
    REQUIRE(outer.size() == 4);
    CHECK(near(outer[0].value, 0.42));
    CHECK(near(outer[1].value, 0.42));
    CHECK(near(outer[2].value, 0.63));
    CHECK(near(outer[3].value, 0.63));
    CHECK(near(array(cfg, "nozzle_inner_wall_line_width")[2].value, 0.675));
    CHECK(near(array(cfg, "nozzle_sparse_infill_line_width")[3].value, 0.675));
    CHECK(near(array(cfg, "nozzle_internal_solid_infill_line_width")[2].value, 0.63));
    CHECK(near(array(cfg, "nozzle_top_surface_line_width")[2].value, 0.63));
    CHECK(near(array(cfg, "nozzle_support_line_width")[2].value, 0.63));
    CHECK(near(array(cfg, "nozzle_bridge_line_width")[2].value, 0.6));
    CHECK(near(array(cfg, "nozzle_initial_layer_line_width")[2].value, 0.75));
    CHECK(near(array(cfg, "nozzle_initial_layer_line_width")[0].value, 0.5));
    CHECK(near(array(cfg, "nozzle_line_width")[3].value, 0.63));
    // The plate's own keys are not touched: the stamp happens per region / object.
    CHECK(near(cfg.option<ConfigOptionFloatOrPercent>("outer_wall_line_width")->value, 0.42));

    // A filament assigned to another tool takes that tool's size.
    DynamicPrintConfig assigned = mixed_u1();
    assigned.option<ConfigOptionInts>("filament_nozzle_assignment", true)->values = { 3, 0, 0, 1 };
    REQUIRE(resolve_nozzle_line_widths(assigned));
    CHECK(near(array(assigned, "nozzle_outer_wall_line_width")[0].value, 0.63));
    CHECK(near(array(assigned, "nozzle_outer_wall_line_width")[3].value, 0.42));

    // Stamping a region / object puts each role's nozzle width in.
    PrintRegionConfig region;
    region.apply(cfg, true);
    region.wall_filament.value          = 3;
    region.sparse_infill_filament.value = 1;
    region.solid_infill_filament.value  = 4;
    stamp_nozzle_line_widths(region);
    CHECK(near(region.outer_wall_line_width.value, 0.63));
    CHECK(near(region.inner_wall_line_width.value, 0.675));
    CHECK(near(region.sparse_infill_line_width.value, 0.45));
    CHECK(near(region.internal_solid_infill_line_width.value, 0.63));
    CHECK(near(region.top_surface_line_width.value, 0.63));
    CHECK(near(region.bridge_line_width.value, 0.6));
    PrintObjectConfig object;
    object.apply(cfg, true);
    object.support_filament.value = 4;
    stamp_nozzle_line_widths(object);
    CHECK(near(object.support_line_width.value, 0.63));
}

TEST_CASE("Widths set per nozzle win over scaling, the most specific entry first", "[NozzleWidths][Table]")
{
    DynamicPrintConfig cfg = mixed_u1();
    cfg.set_deserialize_strict({ { "process_nozzle_line_widths", "0.6: outer_wall_line_width=0.7, sparse_infill_line_width=120% | "
                                                                 "T4: outer_wall_line_width=0.66 | 0.4: top_surface_line_width=0.4 | "
                                                                 "0.6 high_flow: inner_wall_line_width=0.9" } });
    CHECK(near(width_of(cfg, "outer_wall_line_width", 3, 0.6), 0.7));
    CHECK(nozzle_width_for(cfg, "outer_wall_line_width", 3, 0.6).source == NozzleWidthSource::Override);
    CHECK(near(width_of(cfg, "outer_wall_line_width", 4, 0.6), 0.66)); // the tool entry beats the size entry
    const NozzleWidth sparse = nozzle_width_for(cfg, "sparse_infill_line_width", 3, 0.6);
    CHECK(sparse.value.percent);
    CHECK(near(sparse.value.value, 120.));
    CHECK(near(width_of(cfg, "top_surface_line_width", 1, 0.4), 0.4));
    CHECK(near(width_of(cfg, "inner_wall_line_width", 3, 0.6), 0.675)); // a flow-type entry is not a width entry
    // A line_width entry carries the roles left at 0.
    DynamicPrintConfig zero = mixed_u1();
    zero.set_deserialize_strict({ { "inner_wall_line_width", "0" }, { "process_nozzle_line_widths", "0.6: line_width=0.61" } });
    CHECK(near(width_of(zero, "inner_wall_line_width", 3, 0.6), 0.61));

    // Entries apply on a printer with one nozzle size too (per tool, e.g. an H2D left / right).
    DynamicPrintConfig h2d = DynamicPrintConfig::full_print_config();
    h2d.set_num_filaments(2);
    h2d.set_deserialize_strict({ { "nozzle_diameter", "0.4,0.4" }, { "outer_wall_line_width", "0.42" },
                                 { "process_nozzle_line_widths", "T2: outer_wall_line_width=0.45" } });
    REQUIRE(resolve_nozzle_line_widths(h2d));
    CHECK(near(array(h2d, "nozzle_outer_wall_line_width")[0].value, 0.42));
    CHECK(near(array(h2d, "nozzle_outer_wall_line_width")[1].value, 0.45));

    SECTION("editing and validation")
    {
        std::string text = set_process_nozzle_width("", 0, 0.6, "outer_wall_line_width", "0.62");
        text             = set_process_nozzle_width(text, 2, 0., "support_line_width", "110%");
        CHECK(validate_process_nozzle_line_widths(text).empty());
        FloatOrPercent w;
        DynamicPrintConfig p;
        p.set_key_value("process_nozzle_line_widths", new ConfigOptionString(text));
        REQUIRE(process_nozzle_width_override(p, "outer_wall_line_width", 3, 0.6, w));
        CHECK(near(w.value, 0.62));
        REQUIRE(process_nozzle_width_override(p, "support_line_width", 2, 0.4, w));
        CHECK(w.percent);
        CHECK_FALSE(process_nozzle_width_override(p, "support_line_width", 1, 0.4, w));
        text = set_process_nozzle_width(text, 0, 0.6, "outer_wall_line_width", "");
        p.set_key_value("process_nozzle_line_widths", new ConfigOptionString(text));
        CHECK_FALSE(process_nozzle_width_override(p, "outer_wall_line_width", 3, 0.6, w));

        CHECK_FALSE(validate_process_nozzle_line_widths("0.6: outer_wall_speed=50").empty());
        CHECK_FALSE(validate_process_nozzle_line_widths("0.6: outer_wall_line_width=wide").empty());
        CHECK_FALSE(validate_process_nozzle_line_widths("0.6 high_flow: outer_wall_line_width=0.6").empty());
        CHECK_FALSE(validate_nozzle_width_value("-1").empty());
        CHECK_FALSE(validate_nozzle_width_value("9").empty());
        CHECK(validate_nozzle_width_value("0.62").empty());
        CHECK(validate_nozzle_width_value("105%").empty());
    }
}

TEST_CASE("A nozzle's own process (Speeds from) wins over the plate's per-nozzle widths and scaling", "[NozzleWidths][Precedence]")
{
    DynamicPrintConfig cfg = mixed_u1();
    cfg.set_deserialize_strict({ { "process_nozzle_line_widths", "0.6: outer_wall_line_width=0.7" } });
    DynamicPrintConfig p06;
    p06.set_key_value("outer_wall_line_width", new ConfigOptionFloatOrPercent(0.62, false));
    p06.set_key_value("line_width", new ConfigOptionFloatOrPercent(0.64, false));
    p06.set_key_value("process_nozzle_line_widths", new ConfigOptionString("T3: line_width=0.66"));
    cfg.option<ConfigOptionStrings>("nozzle_process_presets", true)->values = { "", "", "0.6 process", "" };
    REQUIRE(fill_nozzle_process_slots(cfg, { nullptr, nullptr, &p06, nullptr }));
    const auto &outer = array(cfg, "nozzle_outer_wall_line_width");
    REQUIRE(outer.size() == 4);
    CHECK(near(outer[2].value, 0.62)); // T3: its own process's width, not the plate's 0.7 or 0.63
    CHECK(near(outer[3].value, 0.7));  // T4 (0.6, no process of its own): the plate's table entry
    CHECK(near(outer[0].value, 0.42)); // T1 (0.4): the plate's width
    // T3's process leaves the inner walls at 0: its line_width, from its own table entry for T3.
    CHECK(near(array(cfg, "nozzle_inner_wall_line_width")[2].value, 0.66));
    CHECK(near(array(cfg, "nozzle_inner_wall_line_width")[3].value, 0.675)); // T4: scaled
    // Print::apply's resolver leaves arrays a per-nozzle process wrote alone.
    DynamicPrintConfig again = cfg;
    CHECK_FALSE(resolve_nozzle_line_widths(again));
    CHECK(array(again, "nozzle_outer_wall_line_width") == outer);
}

TEST_CASE("A process split by nozzle size keeps each size's speeds for nozzles without a process of their own", "[NozzleWidths][Precedence][Speeds]")
{
    DynamicPrintConfig cfg = mixed_u1();
    cfg.option<ConfigOptionFloats>("outer_wall_speed", true)->values = { 100. };
    REQUIRE(split_process_per_diameter(cfg, { 0.4, 0.6 }));
    const int slot06 = process_diameter_slot(cfg, "outer_wall_speed", 0.6, fvtStandard);
    REQUIRE(slot06 >= 0);
    cfg.option<ConfigOptionFloats>("outer_wall_speed")->values[size_t(slot06)] = 60.;

    DynamicPrintConfig other;
    other.set_key_value("outer_wall_speed", new ConfigOptionFloats({ 33. }));
    cfg.option<ConfigOptionStrings>("nozzle_process_presets", true)->values = { "other", "", "", "" };
    REQUIRE(fill_nozzle_process_slots(cfg, { &other, nullptr, nullptr, nullptr }));
    CHECK(nozzle_process_value(cfg, "outer_wall_speed", 0, false) == "33"); // T1: its own process
    CHECK(nozzle_process_value(cfg, "outer_wall_speed", 1, false) == "100"); // T2 (0.4): the 0.4 size's
    CHECK(nozzle_process_value(cfg, "outer_wall_speed", 2, false) == "60");  // T3 (0.6): the 0.6 size's, not the 0.4's
    CHECK(nozzle_process_value(cfg, "outer_wall_speed", 3, false) == "60");
}
