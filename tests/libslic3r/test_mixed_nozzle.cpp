// Mixed nozzle sizes, Phase 1 (docs/superpowers/specs/2026-09-07-mixed-nozzle-sizes.md).
//
// Phase 1 is validation SCOPING: nothing about how an extrusion is sized changed, only which
// nozzle each check measures against.
//   (a) the layer-height ceiling of an object comes from the nozzles THAT object uses,
//   (b) each line-width bound is measured against the nozzle of the filament its role is routed to,
//   (c) a new per-feature bound refuses a role whose resolved width exceeds
//       MAX_FEATURE_WIDTH_TO_NOZZLE_RATIO x its own nozzle,
//   (d) the prime tower refuses mixed nozzle diameters outright instead of warning.
//
// Everything here is constructed PrintConfig / Model - no GUI, no preset bundle, no disk.

#include <catch2/catch.hpp>

#include <string>
#include <vector>

#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;

namespace {

// A two-filament FFF config on a toolchanger-shaped machine: one nozzle per filament, so the
// filament index IS the physical extruder and no filament_map indirection is involved.
DynamicPrintConfig mixed_nozzle_config(double nozzle_1, double nozzle_2)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = { nozzle_1, nozzle_2 };
    config.option<ConfigOptionFloats>("filament_diameter")->values = { 1.75, 1.75 };
    config.option<ConfigOptionStrings>("filament_colour")->values  = { "#FFFFFF", "#804020" };
    config.option<ConfigOptionInts>("filament_map")->values        = { 1, 2 };
    // Per-extruder layer height envelopes, so (a) has something machine-specific to read.
    config.option<ConfigOptionFloats>("min_layer_height")->values  = { 0.08, 0.04 };
    config.option<ConfigOptionFloats>("max_layer_height")->values  = { 0.45, 0.15 };
    // The prime tower is a separate axis (see the Phase 2 gate test); keep it out of the way.
    config.option<ConfigOptionBool>("enable_prime_tower")->value   = false;
    // A toolchanger is NOT single-extruder multi-material. The option registry defaults this key
    // to true (PrintConfig.cpp), which every real U1 / H2D / H2C preset overrides with 0; a config
    // built from full_print_config() does not, so say so here. Phase 3 reads it to decide whether
    // mixed diameters are even possible on this machine.
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value = false;
    // Pin the widths that would otherwise resolve from a bare option registry default of a
    // literal 0 - Flow treats 0 as "auto", but the validator has to see a real number to test.
    config.option<ConfigOptionFloatOrPercent>("line_width")->value                        = 0.42;
    config.option<ConfigOptionFloatOrPercent>("line_width")->percent                      = false;
    for (const char *key : { "inner_wall_line_width", "outer_wall_line_width", "sparse_infill_line_width",
                             "internal_solid_infill_line_width", "top_surface_line_width", "support_line_width",
                             "initial_layer_line_width" }) {
        auto *opt   = config.option<ConfigOptionFloatOrPercent>(key);
        opt->value   = 100.;
        opt->percent = true;   // % of the nozzle that prints it - the nozzle-agnostic profile style
    }
    config.option<ConfigOptionFloat>("layer_height")->value               = 0.1;
    config.option<ConfigOptionFloat>("initial_layer_print_height")->value = 0.1;
    return config;
}

ModelObject *add_cube(Model &model, const char *name, double size = 20.)
{
    ModelObject *object = model.add_object();
    object->name        = name;
    object->add_volume(make_cube(size, size, size));
    object->add_instance();
    object->ensure_on_bed();
    return object;
}

// Move the second object out of the first one's way so the clearance check does not fire first.
void offset_instance(ModelObject *object, double dx)
{
    object->instances.front()->set_offset(Vec3d(dx, 0., object->instances.front()->get_offset().z()));
}

std::string validate_message(Model &model, const DynamicPrintConfig &config)
{
    Print print;
    print.set_status_silent();
    // Twice on purpose: the first apply() runs normalize_fdm_2 while the Print still has no
    // objects, so `used_filaments` counts as 1 and the prime tower is switched off
    // (PrintApply.cpp:1317-1321). The GUI re-applies on every change, so the second call is what
    // a real session sees. Nothing else about the config changes between the two.
    print.apply(model, config);
    print.apply(model, config);
    StringObjectException warning;
    const std::string msg = print.validate(&warning).string;
    // A green validation returns an empty string, which makes a failing REQUIRE unreadable; report
    // the state the checks under test actually keyed off instead.
    return msg.empty() ? ("[accepted; filaments used=" + std::to_string(print.extruders().size())
                          + ", prime tower=" + std::to_string(int(print.has_wipe_tower())) + "]")
                       : msg;
}

PrintConfig static_print_config(const DynamicPrintConfig &config)
{
    PrintConfig out;
    out.apply(config, true);
    return out;
}

PrintObjectConfig static_object_config(const DynamicPrintConfig &config)
{
    PrintObjectConfig out;
    out.apply(config, true);
    return out;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// The filament -> physical extruder map that every per-feature nozzle lookup now goes through.
// Ported from OrcaSlicer PR #13782 (LixNix).
// ---------------------------------------------------------------------------------------------
SCENARIO("Mixed nozzle: a filament resolves to the nozzle it is loaded into", "[MixedNozzle]")
{
    GIVEN("a toolchanger - as many nozzles as filaments")
    {
        const PrintConfig config = static_print_config(mixed_nozzle_config(0.6, 0.2));
        THEN("the filament index is the physical extruder")
        {
            REQUIRE(physical_extruder_for_filament(config, 1) == 0);
            REQUIRE(physical_extruder_for_filament(config, 2) == 1);
            REQUIRE(config.nozzle_diameter.get_at(physical_extruder_for_filament(config, 1)) == Approx(0.6));
            REQUIRE(config.nozzle_diameter.get_at(physical_extruder_for_filament(config, 2)) == Approx(0.2));
        }
        THEN("filament 0 - 'keep using the current tool' - resolves to the first extruder")
        {
            REQUIRE(physical_extruder_for_filament(config, 0) == 0);
        }
    }

    GIVEN("more filaments than nozzles, as on an H2D with an AMS")
    {
        DynamicPrintConfig dyn = mixed_nozzle_config(0.6, 0.2);
        dyn.set_num_filaments(4);
        dyn.option<ConfigOptionFloats>("filament_diameter")->values = { 1.75, 1.75, 1.75, 1.75 };
        dyn.option<ConfigOptionStrings>("filament_colour")->values  = { "#1", "#2", "#3", "#4" };
        // Filaments 1 and 3 sit on the left nozzle, 2 and 4 on the right one.
        dyn.option<ConfigOptionInts>("filament_map")->values = { 1, 2, 1, 2 };
        const PrintConfig config = static_print_config(dyn);

        THEN("filament_map decides, not the filament number")
        {
            REQUIRE(physical_extruder_for_filament(config, 3) == 0);
            REQUIRE(physical_extruder_for_filament(config, 4) == 1);
            // The behaviour this replaces: nozzle_diameter.get_at(3) on a 2-entry vector fell back
            // to the FIRST value, so filament 4 was sized for the 0.6 nozzle it never touches.
            REQUIRE(config.nozzle_diameter.get_at(physical_extruder_for_filament(config, 4)) == Approx(0.2));
            REQUIRE(config.nozzle_diameter.get_at(3) == Approx(0.6));
        }
    }
}

// ---------------------------------------------------------------------------------------------
// (a) Layer-height limits per object, from the nozzles that object's regions actually use.
// ---------------------------------------------------------------------------------------------
SCENARIO("Mixed nozzle (a): layer height limits come from the object's own nozzles", "[MixedNozzle]")
{
    const DynamicPrintConfig dyn        = mixed_nozzle_config(0.6, 0.2);
    const PrintConfig        print_cfg  = static_print_config(dyn);
    const PrintObjectConfig  object_cfg = static_object_config(dyn);

    GIVEN("an object whose regions only use filament 1, the 0.6 nozzle")
    {
        // object_extruders() hands out 0-BASED filament indices.
        const std::vector<unsigned int> object_extruders { 0 };
        const SlicingParameters params = SlicingParameters::create_from_config(
            print_cfg, object_cfg, 20., object_extruders, Vec3d(1., 1., 1.));

        THEN("its envelope is the 0.6 nozzle's, not the finest nozzle on the printer")
        {
            REQUIRE(params.max_layer_height == Approx(0.45));
            REQUIRE(params.min_layer_height == Approx(0.08));
        }
    }

    GIVEN("an object whose regions only use filament 2, the 0.2 nozzle")
    {
        const std::vector<unsigned int> object_extruders { 1 };
        const SlicingParameters params = SlicingParameters::create_from_config(
            print_cfg, object_cfg, 20., object_extruders, Vec3d(1., 1., 1.));

        THEN("its envelope is the 0.2 nozzle's")
        {
            // Before the fix this read extruder index 1-1 = 0 and answered 0.45 - the fine nozzle
            // was handed the coarse nozzle's ceiling.
            REQUIRE(params.max_layer_height == Approx(0.15));
            REQUIRE(params.min_layer_height == Approx(0.04));
        }
    }

    GIVEN("an object that uses both filaments")
    {
        const std::vector<unsigned int> object_extruders { 0, 1 };
        const SlicingParameters params = SlicingParameters::create_from_config(
            print_cfg, object_cfg, 20., object_extruders, Vec3d(1., 1., 1.));

        THEN("the ceiling is the finest nozzle's and the floor the coarsest nozzle's")
        {
            REQUIRE(params.max_layer_height == Approx(0.15));
            REQUIRE(params.min_layer_height == Approx(0.08));
        }
    }
}

SCENARIO("Mixed nozzle (a): one object's fine nozzle does not clamp another object", "[MixedNozzle]")
{
    DynamicPrintConfig config = mixed_nozzle_config(0.6, 0.2);
    Model model;
    ModelObject *coarse = add_cube(model, "coarse.stl");
    ModelObject *fine   = add_cube(model, "fine.stl");
    offset_instance(fine, 40.);

    // The coarse object prints entirely on filament 1 (0.6) at a layer height only that nozzle can
    // do; the fine object prints entirely on filament 2 (0.2) at a height the 0.2 can do.
    for (const char *key : { "wall_filament", "sparse_infill_filament", "solid_infill_filament" })
        coarse->config.set_key_value(key, new ConfigOptionInt(1));
    coarse->config.set_key_value("layer_height", new ConfigOptionFloat(0.3));
    for (const char *key : { "wall_filament", "sparse_infill_filament", "solid_infill_filament" })
        fine->config.set_key_value(key, new ConfigOptionInt(2));
    fine->config.set_key_value("layer_height", new ConfigOptionFloat(0.1));

    THEN("validation accepts both")
    {
        // Before the scoping fix, `min_nozzle_diameter` was the minimum over every extruder used
        // ANYWHERE in the print (0.2), so the 0.3 mm coarse object was rejected with
        // "Layer height cannot exceed nozzle diameter." even though its own nozzle is 0.6.
        const std::string message = validate_message(model, config);
        INFO(message);
        REQUIRE(message.find("Layer height cannot exceed nozzle diameter") == std::string::npos);
    }
}

SCENARIO("Mixed nozzle (a): an object still cannot out-run its own nozzle", "[MixedNozzle]")
{
    DynamicPrintConfig config = mixed_nozzle_config(0.6, 0.2);
    Model model;
    ModelObject *fine = add_cube(model, "fine.stl");
    for (const char *key : { "wall_filament", "sparse_infill_filament", "solid_infill_filament" })
        fine->config.set_key_value(key, new ConfigOptionInt(2));
    fine->config.set_key_value("layer_height", new ConfigOptionFloat(0.3));

    THEN("the layer height check still fires for the object that owns the fine nozzle")
    {
        const std::string message = validate_message(model, config);
        INFO(message);
        REQUIRE(message.find("Layer height cannot exceed nozzle diameter") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------------------------
// (b) + (c) Per-feature line-width bounds against the feature's own nozzle.
// ---------------------------------------------------------------------------------------------
SCENARIO("Mixed nozzle (c): a feature may not out-width its own nozzle", "[MixedNozzle]")
{
    GIVEN("an outer wall routed to the 0.2 nozzle but still asking for 0.62 mm lines")
    {
        DynamicPrintConfig config = mixed_nozzle_config(0.6, 0.2);
        config.option<ConfigOptionInt>("outer_wall_filament")->value = 2;
        auto *outer   = config.option<ConfigOptionFloatOrPercent>("outer_wall_line_width");
        outer->value   = 0.62;
        outer->percent = false;

        Model model;
        add_cube(model, "cube.stl");

        THEN("validation names the feature and the filament")
        {
            // Nothing rejected this before: the only upper bound was 5 x the COARSEST nozzle of the
            // print (0.6 -> 3.0 mm), so a 0.2 mm head was quietly asked for a 0.62 mm line.
            const std::string message = validate_message(model, config);
            INFO(message);
            REQUIRE(message.find("Outer wall") != std::string::npos);
            REQUIRE(message.find("filament 2") != std::string::npos);
            REQUIRE(message.find("0.20 mm") != std::string::npos);
        }
    }

    GIVEN("the same 0.62 mm outer wall left on the 0.6 nozzle")
    {
        DynamicPrintConfig config = mixed_nozzle_config(0.6, 0.2);
        auto *outer   = config.option<ConfigOptionFloatOrPercent>("outer_wall_line_width");
        outer->value   = 0.62;
        outer->percent = false;

        Model model;
        add_cube(model, "cube.stl");

        THEN("it is accepted - 0.62 is a normal line for a 0.6 nozzle")
        {
            const std::string message = validate_message(model, config);
            INFO(message);
            REQUIRE(message.find("more than") == std::string::npos);
            REQUIRE(message.find("Too large line width") == std::string::npos);
        }
    }
}

SCENARIO("Mixed nozzle (b): percent widths follow the feature's own nozzle", "[MixedNozzle]")
{
    // Every width is 100% of "its" nozzle in mixed_nozzle_config, i.e. the nozzle-agnostic profile
    // style upstream OrcaSlicer's Mixed Nozzle Sizes guide recommends. With the walls on the coarse
    // head and the top surface on the fine one, nothing may be flagged: 0.6 and 0.2 are each 100%
    // of their own nozzle. Under the old whole-print bounds the SAME config resolved
    // top_surface_line_width against the print's 0.6 max for the "too large" half and against the
    // 0.2 min for the "too small" half, which is not what either nozzle will actually lay down.
    DynamicPrintConfig config = mixed_nozzle_config(0.6, 0.2);
    config.option<ConfigOptionInt>("wall_filament")->value         = 1;
    config.option<ConfigOptionInt>("sparse_infill_filament")->value = 1;
    config.option<ConfigOptionInt>("solid_infill_filament")->value  = 2;
    config.option<ConfigOptionInt>("outer_wall_filament")->value    = 2;
    config.option<ConfigOptionFloat>("layer_height")->value               = 0.1;
    config.option<ConfigOptionFloat>("initial_layer_print_height")->value = 0.1;

    Model model;
    add_cube(model, "cube.stl");

    THEN("the mixed assignment validates clean")
    {
        const std::string message = validate_message(model, config);
        INFO(message);
        REQUIRE(message.find("line width") == std::string::npos);
        REQUIRE(message.find("Layer height cannot exceed") == std::string::npos);
    }
}

// ---------------------------------------------------------------------------------------------
// (d) The prime tower gate - Phase 3 territory, refused rather than silently mis-sized.
// ---------------------------------------------------------------------------------------------
SCENARIO("Mixed nozzle (d): the prime tower refuses mixed diameters", "[MixedNozzle]")
{
    GIVEN("a two-filament job with 0.6 and 0.2 nozzles and the prime tower on")
    {
        DynamicPrintConfig config = mixed_nozzle_config(0.6, 0.2);
        config.option<ConfigOptionBool>("enable_prime_tower")->value = true;
        config.option<ConfigOptionInt>("solid_infill_filament")->value = 2;

        Model model;
        add_cube(model, "cube.stl");

        THEN("validation refuses it and says what to do")
        {
            const std::string message = validate_message(model, config);
            INFO(message);
            REQUIRE(message.find("prime tower does not support mixed nozzle diameters") != std::string::npos);
        }
    }

    GIVEN("the same job on two 0.4 nozzles")
    {
        DynamicPrintConfig config = mixed_nozzle_config(0.4, 0.4);
        config.option<ConfigOptionBool>("enable_prime_tower")->value = true;
        config.option<ConfigOptionInt>("solid_infill_filament")->value = 2;

        Model model;
        add_cube(model, "cube.stl");

        THEN("the prime tower is fine - matching diameters change nothing")
        {
            const std::string message = validate_message(model, config);
            INFO(message);
            REQUIRE(message.find("prime tower does not support mixed nozzle diameters") == std::string::npos);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Phase 3 (UI): the config-level rule the Printer settings pages and the sidebar are built on.
//
// The dialogs themselves are wxWidgets and are not exercised here (nobody clicks them - see the
// spec's owner-test list). What IS testable, and what every one of those code paths asks first, is:
//   - may this machine hold different nozzles at once (supports_mixed_nozzle_diameters),
//   - does it currently (has_mixed_nozzle_diameters),
//   - what does the sidebar print in place of a single printer_variant (nozzle_diameter_summary),
//   - and which nozzle does a given filament slot print through (nozzle_diameter_for_filament).
// ---------------------------------------------------------------------------------------------

SCENARIO("mixed nozzle sizes: per-extruder diameters are independent", "[MixedNozzle]")
{
    GIVEN("a toolchanger whose extruder 2 is given a finer nozzle")
    {
        DynamicPrintConfig config = mixed_nozzle_config(0.6, 0.6);

        THEN("it starts out uniform")
        {
            REQUIRE(supports_mixed_nozzle_diameters(config));
            REQUIRE_FALSE(has_mixed_nozzle_diameters(config));
            REQUIRE(nozzle_diameter_summary(config) == "0.6");
        }

        // This is the edit the Extruder 2 page makes: one element of the vector, nothing else.
        config.option<ConfigOptionFloats>("nozzle_diameter")->values[1] = 0.2;

        THEN("extruder 1 is untouched and only extruder 2 changed")
        {
            const std::vector<double> &nozzles = config.option<ConfigOptionFloats>("nozzle_diameter")->values;
            REQUIRE(nozzles.size() == 2);
            REQUIRE(nozzles[0] == Approx(0.6));
            REQUIRE(nozzles[1] == Approx(0.2));
        }

        THEN("the machine now reads as mixed and the sidebar summary names both heads")
        {
            REQUIRE(has_mixed_nozzle_diameters(config));
            REQUIRE(nozzle_diameter_summary(config) == "0.6 / 0.2");
        }

        THEN("each filament slot resolves to its own nozzle")
        {
            PrintConfig print_config;
            print_config.apply(config, true);
            REQUIRE(nozzle_diameter_for_filament(print_config, 1) == Approx(0.6));
            REQUIRE(nozzle_diameter_for_filament(print_config, 2) == Approx(0.2));
        }
    }

    GIVEN("a four-head U1-shaped toolchanger with one fine head")
    {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_num_extruders(4);
        config.option<ConfigOptionBool>("single_extruder_multi_material")->value = false;
        config.option<ConfigOptionFloats>("nozzle_diameter")->values = { 0.6, 0.2, 0.6, 0.6 };

        THEN("the summary keeps head order and keeps duplicates so heads can be counted")
        {
            REQUIRE(has_mixed_nozzle_diameters(config));
            REQUIRE(nozzle_diameter_summary(config) == "0.6 / 0.2 / 0.6 / 0.6");
        }
    }

    GIVEN("a single-nozzle printer")
    {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_num_extruders(1);
        config.option<ConfigOptionFloats>("nozzle_diameter")->values = { 0.4 };

        THEN("there is nothing to mix, and the summary is the plain diameter")
        {
            REQUIRE_FALSE(supports_mixed_nozzle_diameters(config));
            REQUIRE_FALSE(has_mixed_nozzle_diameters(config));
            REQUIRE(nozzle_diameter_summary(config) == "0.4");
        }
    }

    GIVEN("a single-extruder multi-material printer with two declared extruders")
    {
        DynamicPrintConfig config = mixed_nozzle_config(0.4, 0.4);
        config.option<ConfigOptionBool>("single_extruder_multi_material")->value = true;

        THEN("mixing is refused: every filament goes through the same physical nozzle")
        {
            REQUIRE_FALSE(supports_mixed_nozzle_diameters(config));
        }

        THEN("so the old forced-sync behaviour is what the Extruder pages must keep")
        {
            PrintConfig print_config;
            print_config.apply(config, true);
            REQUIRE_FALSE(supports_mixed_nozzle_diameters(print_config));
        }
    }

    GIVEN("an AMS-shaped machine: four filaments sharing two nozzles of different sizes")
    {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_num_extruders(2);
        config.set_num_filaments(4);
        config.option<ConfigOptionBool>("single_extruder_multi_material")->value = false;
        config.option<ConfigOptionFloats>("nozzle_diameter")->values   = { 0.6, 0.2 };
        config.option<ConfigOptionFloats>("filament_diameter")->values = { 1.75, 1.75, 1.75, 1.75 };
        // filaments 1 and 3 on the coarse head, 2 and 4 on the fine one (filament_map is 1-based).
        config.option<ConfigOptionInts>("filament_map")->values        = { 1, 2, 1, 2 };

        THEN("each slot resolves through filament_map, not by slot index")
        {
            PrintConfig print_config;
            print_config.apply(config, true);
            REQUIRE(nozzle_diameter_for_filament(print_config, 1) == Approx(0.6));
            REQUIRE(nozzle_diameter_for_filament(print_config, 2) == Approx(0.2));
            REQUIRE(nozzle_diameter_for_filament(print_config, 3) == Approx(0.6));
            REQUIRE(nozzle_diameter_for_filament(print_config, 4) == Approx(0.2));
        }

        THEN("and the summary still describes the two physical heads, not the four slots")
        {
            REQUIRE(nozzle_diameter_summary(config) == "0.6 / 0.2");
        }
    }
}
