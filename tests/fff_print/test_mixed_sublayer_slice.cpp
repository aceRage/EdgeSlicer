// Mixed color sublayer, phase 1 (tests/design_mixed_color_sublayer.md): a painted cube sliced with a Ratio row.
// The plan has one pass per component at its sub-Z (heights sum to the layer height), the first layer is not
// split, the start component alternates, the G-code has no extra layer-change markers and each component's
// extrusion follows the row's ratio. The same-diameter rule refuses a mix across nozzle sizes.

#include <catch2/catch.hpp>

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/MixedSublayer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include "test_data.hpp"

#include <cctype>
#include <cmath>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

// Two physical filaments on a two-head tool changer (no shared nozzle, no prime tower) and one mixed row
// A = 1, B = 2 with 30 % B. The row is virtual filament 3.
DynamicPrintConfig mixed_sublayer_config(bool enable_ratio, bool local_z, const std::vector<double> &nozzles = { 0.4, 0.4 })
{
    MixedFilamentManager::set_auto_generate_enabled(false);
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("filament_diameter")->values = { 1.75, 1.75 };
    config.option<ConfigOptionStrings>("filament_colour")->values  = { "#FF0000", "#0000FF" };
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = nozzles;
    config.option<ConfigOptionFloats>("min_layer_height")->values  = { 0.04, 0.04 };
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value = false;
    config.option<ConfigOptionBool>("enable_prime_tower")->value             = false;
    for (const char *key : { "line_width", "initial_layer_line_width", "outer_wall_line_width", "inner_wall_line_width",
                             "top_surface_line_width", "sparse_infill_line_width", "internal_solid_infill_line_width" })
        if (auto *opt = config.option<ConfigOptionFloatOrPercent>(key); opt != nullptr) {
            opt->value   = 0.42;
            opt->percent = false;
        }
    config.set_deserialize_strict({
        { "layer_height", "0.2" },
        { "initial_layer_print_height", "0.2" },
        { "use_relative_e_distances", "1" },
        { "enable_mixed_color_sublayer", enable_ratio ? "1" : "0" },
        { "dithering_local_z_mode", local_z ? "1" : "0" },
    });

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 30, { "#FF0000", "#0000FF" });
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(mgr.serialize_custom_entries()));
    return config;
}

// A 10 x 10 x 3 mm cube painted all over with the mixed row (filament 3).
const PrintObject *slice_painted_cube(Print &print, Model &model, const DynamicPrintConfig &config, bool process = true)
{
    ModelObject *object = model.add_object();
    object->name        = "mixed_cube";
    ModelVolume *v      = object->add_volume(TriangleMesh(its_make_cube(10., 10., 3.)));
    v->config.set("extruder", 1);
    TriangleSelector selector(v->mesh());
    for (int f = 0; f < int(v->mesh().its.indices.size()); ++f)
        selector.set_facet(f, EnforcerBlockerType(3));
    REQUIRE(v->mmu_segmentation_facets.set(selector));
    object->add_instance();
    object->ensure_on_bed();
    print.apply(model, config);
    print.set_status_silent();
    if (process)
        print.process();
    REQUIRE(!print.objects().empty());
    return print.objects().front();
}

} // namespace

TEST_CASE("Mixed sublayer: Ratio plan on a painted cube", "[MixedSublayer][Slice]")
{
    Print              print;
    Model              model;
    const PrintObject *object = slice_painted_cube(print, model, mixed_sublayer_config(true, false));
    const auto        &intervals = object->local_z_intervals();
    const auto        &plans     = object->local_z_sublayer_plan();
    REQUIRE(!intervals.empty());

    size_t       ratio_layers = 0;
    unsigned int last_top     = 0;
    for (const LocalZInterval &interval : intervals) {
        INFO("layer " << interval.layer_id);
        if (interval.layer_id == 0) {
            // The first layer is never split.
            CHECK_FALSE(local_z_interval_has_passes(interval, plans));
            continue;
        }
        if (!local_z_interval_has_passes(interval, plans))
            continue;
        ++ratio_layers;
        double       sum = 0.;
        double       a = 0., b = 0.;
        unsigned int first_extruder = 0, last_extruder = 0;
        for (size_t i = interval.first_sublayer_idx; i < interval.first_sublayer_idx + interval.sublayer_count; ++i) {
            const SubLayerPlan &plan = plans[i];
            REQUIRE(plan.ratio_pass);
            CHECK_THAT(plan.flow_height, WithinAbs(plan.z_hi - plan.z_lo, 1e-9));
            CHECK_THAT(plan.print_z, WithinAbs(plan.z_hi, 1e-9));
            sum += plan.flow_height;
            unsigned int extruder = 0;
            for (size_t e = 0; e < plan.painted_masks_by_extruder.size(); ++e)
                if (!plan.painted_masks_by_extruder[e].empty())
                    extruder = unsigned(e + 1);
            REQUIRE(extruder != 0);
            (extruder == 1 ? a : b) += plan.flow_height;
            if (first_extruder == 0)
                first_extruder = extruder;
            last_extruder = extruder;
        }
        CHECK_THAT(sum, WithinAbs(interval.base_height, 1e-9));
        CHECK_THAT(a, WithinAbs(0.7 * interval.base_height, 1e-6));
        CHECK_THAT(b, WithinAbs(0.3 * interval.base_height, 1e-6));
        CHECK(plans[interval.first_sublayer_idx + interval.sublayer_count - 1].ratio_last_pass);
        // Alternating order, except on the top layer, which uses the ascending (Bambu Studio) order.
        const bool top = interval.layer_id + 1 == object->layer_count();
        if (last_top != 0 && !top)
            CHECK(first_extruder == last_top);
        if (top)
            CHECK(last_extruder == 2);
        last_top = last_extruder;
    }
    CHECK(ratio_layers + 1 == object->layer_count());
}

TEST_CASE("Mixed sublayer: Ratio G-code keeps one layer change per layer", "[MixedSublayer][Slice]")
{
    Print       print;
    Model       model;
    slice_painted_cube(print, model, mixed_sublayer_config(true, false), false);
    const std::string gcode = Test::gcode(print);
    REQUIRE(!gcode.empty());
    const PrintObject *object = print.objects().front();

    const std::string layer_tag = ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Layer_Change);
    size_t            layer_changes = 0;
    size_t            m73_layer     = 0;
    size_t            toolchanges   = 0;
    int               tool          = -1;
    std::map<int, double> extruded_after_first_layer;
    std::istringstream    in(gcode);
    std::string           line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind(layer_tag, 0) == 0) {
            ++layer_changes;
            continue;
        }
        if (line.rfind("M73 L", 0) == 0) {
            ++m73_layer;
            continue;
        }
        if (line.size() >= 2 && line[0] == 'T' && std::isdigit(static_cast<unsigned char>(line[1]))) {
            tool = std::atoi(line.c_str() + 1);
            ++toolchanges;
            continue;
        }
        if (layer_changes < 2 || tool < 0 || line.rfind("G1 ", 0) != 0)
            continue;
        if (line.find(" X") == std::string::npos && line.find(" Y") == std::string::npos)
            continue;
        const size_t epos = line.find(" E");
        if (epos == std::string::npos)
            continue;
        const double e = std::atof(line.c_str() + epos + 2);
        if (e > 0.)
            extruded_after_first_layer[tool] += e;
    }
    // Sub-passes move Z inside the layer; they never announce a layer.
    CHECK(layer_changes == object->layer_count());
    CHECK((m73_layer == 0 || m73_layer <= object->layer_count() + 1));
    // Alternating order: one change per layer (the next layer starts with the filament that ended this one, and
    // the layer's nominal extruder is that filament), plus the start and the first layer's cadence pick.
    CHECK(toolchanges <= object->layer_count() + 3);
    REQUIRE(extruded_after_first_layer.count(0) == 1);
    REQUIRE(extruded_after_first_layer.count(1) == 1);
    const double share_b = extruded_after_first_layer[1] / (extruded_after_first_layer[0] + extruded_after_first_layer[1]);
    CHECK_THAT(share_b, WithinAbs(0.3, 0.05));
}

TEST_CASE("Mixed sublayer: off path plans as before", "[MixedSublayer][Slice]")
{
    SECTION("both keys off: no sub-layer plan")
    {
        Print              print;
        Model              model;
        const PrintObject *object = slice_painted_cube(print, model, mixed_sublayer_config(false, false));
        CHECK(object->local_z_intervals().empty());
    }
    SECTION("Subdivide Mix Layer only: Local-Z passes, none of them Ratio")
    {
        Print              print;
        Model              model;
        const PrintObject *object = slice_painted_cube(print, model, mixed_sublayer_config(false, true));
        const auto        &plans  = object->local_z_sublayer_plan();
        CHECK(!plans.empty());
        for (const SubLayerPlan &plan : plans)
            CHECK_FALSE(plan.ratio_pass);
    }
    SECTION("both on: Subdivide wins")
    {
        Print              print;
        Model              model;
        const PrintObject *object = slice_painted_cube(print, model, mixed_sublayer_config(true, true));
        for (const SubLayerPlan &plan : object->local_z_sublayer_plan())
            CHECK_FALSE(plan.ratio_pass);
    }
}

TEST_CASE("Mixed sublayer: a mix across nozzle diameters is refused", "[MixedSublayer][Slice]")
{
    Print print;
    Model model;
    slice_painted_cube(print, model, mixed_sublayer_config(true, false, { 0.4, 0.6 }), false);
    const StringObjectException err = print.validate();
    CHECK(err.string.find("same nozzle diameter") != std::string::npos);

    // The same printer without the sub-layer setting is not refused for it.
    Print print_off;
    Model model_off;
    slice_painted_cube(print_off, model_off, mixed_sublayer_config(false, false, { 0.4, 0.6 }), false);
    CHECK(print_off.validate().string.find("same nozzle diameter") == std::string::npos);
}
