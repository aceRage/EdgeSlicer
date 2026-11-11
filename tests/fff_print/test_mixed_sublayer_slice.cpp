// Mixed color sublayer, phase 1 (tests/design_mixed_color_sublayer.md): a painted cube sliced with a Ratio row.
// The plan has one pass per component at its sub-Z (heights sum to the layer height), the first layer is not
// split, the start component alternates, the G-code has no extra layer-change markers and each component's
// extrusion follows the row's ratio. The same-diameter rule refuses a mix across nozzle sizes.

#include <catch2/catch.hpp>

#include "libslic3r/AdaptiveLayerMerge.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/ToolOrdering.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/MixedSublayer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/NozzleProcess.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include "test_data.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <tuple>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

// MixedFilamentManager's auto-generated pairs are a process-wide switch: off for a test, restored after it.
struct MixsubAutoGenerateOff
{
    bool saved { MixedFilamentManager::auto_generate_enabled() };
    MixsubAutoGenerateOff() { MixedFilamentManager::set_auto_generate_enabled(false); }
    ~MixsubAutoGenerateOff() { MixedFilamentManager::set_auto_generate_enabled(saved); }
};

// Two physical filaments on a two-head tool changer (no shared nozzle, no prime tower) and one mixed row
// A = 1, B = 2 with 30 % B. The row is virtual filament 3.
DynamicPrintConfig mixed_sublayer_config(bool enable_ratio, bool local_z, const std::vector<double> &nozzles = { 0.4, 0.4 })
{
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

// A 10 x 10 x 3 mm cube painted all over with the mixed row (filament 3 unless told otherwise).
const PrintObject *slice_painted_cube(Print &print, Model &model, const DynamicPrintConfig &config, bool process = true, int painted_state = 3)
{
    ModelObject *object = model.add_object();
    object->name        = "mixed_cube";
    ModelVolume *v      = object->add_volume(TriangleMesh(its_make_cube(10., 10., 3.)));
    v->config.set("extruder", 1);
    TriangleSelector selector(v->mesh());
    for (int f = 0; f < int(v->mesh().its.indices.size()); ++f)
        selector.set_facet(f, EnforcerBlockerType(painted_state));
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
    const MixsubAutoGenerateOff auto_generate_off;
    Print              print;
    Model              model;
    const PrintObject *object = slice_painted_cube(print, model, mixed_sublayer_config(true, false));
    const auto        &intervals = object->local_z_intervals();
    const auto        &plans     = object->local_z_sublayer_plan();
    REQUIRE(!intervals.empty());

    size_t       ratio_layers = 0;
    unsigned int last_top     = 0;
    double       total_a = 0., total_b = 0., total_h = 0.;
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
        total_a += a;
        total_b += b;
        total_h += interval.base_height;
        CHECK(plans[interval.first_sublayer_idx + interval.sublayer_count - 1].ratio_last_pass);
        // Alternating order: a layer printed by both components starts with the one that ended the layer below.
        // The top layer uses the ascending (Bambu Studio) order, so the higher id is on top. A layer whose thin
        // pass fell under the nozzle's minimum layer height is printed by one component (D5 carry-over).
        const bool top = interval.layer_id + 1 == object->layer_count();
        if (interval.sublayer_count >= 2) {
            if (last_top != 0 && !top)
                CHECK(first_extruder == last_top);
            if (top)
                CHECK(last_extruder == 2);
        }
        last_top = last_extruder;
    }
    CHECK(ratio_layers + 1 == object->layer_count());
    // Over the split layers each component prints its share to within one thin pass (the carry is bounded).
    REQUIRE(total_h > 0.);
    CHECK_THAT(total_b, WithinAbs(0.3 * total_h, 0.1));
    CHECK_THAT(total_a + total_b, WithinAbs(total_h, 1e-6));
}

TEST_CASE("Mixed sublayer: Ratio G-code keeps one layer change per layer", "[MixedSublayer][Slice]")
{
    const MixsubAutoGenerateOff auto_generate_off;
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
    const MixsubAutoGenerateOff auto_generate_off;
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
    const MixsubAutoGenerateOff auto_generate_off;
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

// ---- Phase 3 ----------------------------------------------------------------------------------------------

namespace {

// Four heads, 0.4 0.4 0.8 0.8 (an H3C-like mixed-diameter plate), one filament each, 0.2 mm layers. The mixed row
// 3 + 4 at 30 % (virtual filament 5) prints through the two 0.8 nozzles, so AdaptiveLayerMerge bands its region.
DynamicPrintConfig alm_mixed_config(bool enable_ratio = true)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(4);
    config.set_num_filaments(4);
    config.set_deserialize_strict({
        { "nozzle_diameter", "0.4,0.4,0.8,0.8" },
        { "filament_diameter", "1.75,1.75,1.75,1.75" },
        { "filament_map", "1,2,3,4" },
        { "single_extruder_multi_material", "0" },
        { "enable_prime_tower", "0" },
        { "enable_support", "0" },
        { "enable_arc_fitting", "0" },
        { "wall_generator", "classic" },
        { "layer_height", "0.2" },
        { "initial_layer_print_height", "0.2" },
        { "min_layer_height", "0.08,0.08,0.1,0.1" },
        { "max_layer_height", "0.28,0.28,0.56,0.56" },
        { "brim_type", "no_brim" },
        { "skirt_loops", "0" },
        { "use_relative_e_distances", "1" },
        { "enable_mixed_color_sublayer", enable_ratio ? "1" : "0" },
        { "dithering_local_z_mode", "0" },
    });
    config.option<ConfigOptionStrings>("filament_colour")->values = { "#FF0000", "#00FF00", "#0000FF", "#FFFF00" };
    MixedFilamentManager mgr;
    mgr.add_custom_filament(3, 4, 30, { "#FF0000", "#00FF00", "#0000FF", "#FFFF00" });
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(mgr.serialize_custom_entries()));
    return config;
}

// One extrusion move of the G-code: its layer (by layer-change tag), its Z, the top of the layer below (;Z: of the
// previous layer), the tool, the role and the E.
struct GExtrusion
{
    size_t      layer { 0 };
    double      z { 0. };
    double      prev_top { 0. };
    int         tool { -1 };
    std::string role;
    double      e { 0. };
};

std::vector<GExtrusion> gcode_extrusions(const std::string &gcode)
{
    const std::string layer_tag = ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Layer_Change);
    const std::string role_tag  = ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role);
    std::vector<GExtrusion> out;
    long        layer = -1;
    double      z = 0., top = 0., prev_top = 0.;
    int         tool = -1;
    std::string role;
    std::istringstream in(gcode);
    std::string        line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind(layer_tag, 0) == 0) {
            ++layer;
            continue;
        }
        if (line.rfind(";Z:", 0) == 0) {
            prev_top = top;
            top      = std::atof(line.c_str() + 3);
            continue;
        }
        if (line.rfind(role_tag, 0) == 0) {
            role = line.substr(role_tag.size());
            continue;
        }
        if (line.size() >= 2 && line[0] == 'T' && std::isdigit(static_cast<unsigned char>(line[1]))) {
            tool = std::atoi(line.c_str() + 1);
            continue;
        }
        if (line.size() < 3 || line[0] != 'G' || !(line[1] == '0' || line[1] == '1' || line[1] == '2' || line[1] == '3') || line[2] != ' ')
            continue;
        const std::string code = line.substr(0, line.find(';'));
        bool              xy   = false;
        double            e    = 0.;
        std::istringstream words(code);
        std::string        w;
        while (words >> w) {
            if (w.empty())
                continue;
            if (w[0] == 'Z')
                z = std::atof(w.c_str() + 1);
            else if (w[0] == 'X' || w[0] == 'Y')
                xy = true;
            else if (w[0] == 'E')
                e = std::atof(w.c_str() + 1);
        }
        if (xy && e > 0. && layer >= 0)
            out.push_back({ size_t(layer), z, prev_top, tool, role, e });
    }
    return out;
}

std::map<size_t, double> e_per_layer(const std::vector<GExtrusion> &moves)
{
    std::map<size_t, double> out;
    for (const GExtrusion &m : moves)
        out[m.layer] += m.e;
    return out;
}

bool is_infill_role(const std::string &role)
{
    std::string r = role;
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return r.find("infill") != std::string::npos || r.find("bridge") != std::string::npos || r.find("surface") != std::string::npos ||
           r.find("ironing") != std::string::npos;
}

// Every layer of `a` extrudes what the same layer of `b` does, within 3 % (or 0.05 mm of filament).
void check_same_e_per_layer(const std::map<size_t, double> &a, const std::map<size_t, double> &b)
{
    REQUIRE(a.size() == b.size());
    for (const auto &[layer, e] : b) {
        INFO("layer " << layer);
        const auto it = a.find(layer);
        REQUIRE(it != a.end());
        CHECK_THAT(it->second, WithinAbs(e, std::max(0.05, 0.03 * e)));
    }
}

// Every extrusion move prints at or above the top of the layer below: a band pass never ploughs into what the layer
// under it printed.
void check_no_extrusion_below_previous_layer(const std::vector<GExtrusion> &moves)
{
    size_t below = 0;
    for (const GExtrusion &m : moves)
        if (m.layer > 0 && m.z < m.prev_top - 1e-3) {
            if (below++ < 5)
                UNSCOPED_INFO("layer " << m.layer << " z " << m.z << " below " << m.prev_top << " role " << m.role);
        }
    CHECK(below == 0);
}

// The planned Ratio passes of an object, as comparable records.
std::vector<std::tuple<size_t, double, double, double, int, bool>> ratio_plan_records(const PrintObject &object)
{
    std::vector<std::tuple<size_t, double, double, double, int, bool>> out;
    for (const SubLayerPlan &plan : object.local_z_sublayer_plan()) {
        int extruder = -1;
        for (size_t e = 0; e < plan.painted_masks_by_extruder.size(); ++e)
            if (!plan.painted_masks_by_extruder[e].empty())
                extruder = int(e);
        out.emplace_back(plan.layer_id, plan.z_lo, plan.z_hi, plan.flow_height, extruder, plan.ratio_pass);
    }
    return out;
}

// Slices the plate: a plain cube on filament 1 (0.4 nozzle, so the merge is live) and the given volumes painted with
// `state`. Returns the painted object.
const PrintObject *slice_alm_plate(Print &print, Model &model, const DynamicPrintConfig &config, int state,
                                   const std::vector<std::pair<Vec3d, Vec3d>> &boxes)
{
    ModelObject *plain = model.add_object();
    plain->name        = "plain_04";
    plain->add_volume(TriangleMesh(its_make_cube(10., 10., 3.)));
    plain->volumes.front()->config.set("extruder", 1);
    plain->add_instance()->set_offset(Vec3d(40., 40., 0.));
    plain->ensure_on_bed();

    ModelObject *object = model.add_object();
    object->name        = "mixed_part";
    for (const auto &box : boxes) {
        TriangleMesh mesh(its_make_cube(box.second.x(), box.second.y(), box.second.z()));
        mesh.translate(box.first.cast<float>());
        ModelVolume *v = object->add_volume(std::move(mesh));
        v->config.set("extruder", 1);
        TriangleSelector selector(v->mesh());
        for (int f = 0; f < int(v->mesh().its.indices.size()); ++f)
            selector.set_facet(f, EnforcerBlockerType(state));
        REQUIRE(v->mmu_segmentation_facets.set(selector));
    }
    object->add_instance()->set_offset(Vec3d(80., 80., 0.));
    object->ensure_on_bed();
    print.apply(model, config);
    print.set_status_silent();
    print.process();
    for (const PrintObject *po : print.objects())
        if (po->model_object()->name == "mixed_part")
            return po;
    FAIL("no mixed part");
    return nullptr;
}

} // namespace

TEST_CASE("Mixed sublayer: Ratio passes conserve each layer's extrusion, bridges included", "[MixedSublayer][Slice][P3]")
{
    const MixsubAutoGenerateOff auto_generate_off;
    // The same painted 10 x 10 x 6 cube printed whole-layer (key off: cadence) and as Ratio passes: the passes re-extrude
    // the same paths with flow x pass / layer height, so every layer extrudes the same filament.
    Print print_on, print_off;
    Model model_on, model_off;
    auto  tall_cube = [](Print &print, Model &model, const DynamicPrintConfig &config) {
        ModelObject *object = model.add_object();
        object->name        = "mixed_cube";
        ModelVolume *v      = object->add_volume(TriangleMesh(its_make_cube(10., 10., 6.)));
        v->config.set("extruder", 1);
        TriangleSelector selector(v->mesh());
        for (int f = 0; f < int(v->mesh().its.indices.size()); ++f)
            selector.set_facet(f, EnforcerBlockerType(3));
        REQUIRE(v->mmu_segmentation_facets.set(selector));
        object->add_instance();
        object->ensure_on_bed();
        print.apply(model, config);
        print.set_status_silent();
    };
    tall_cube(print_on, model_on, mixed_sublayer_config(true, false));
    tall_cube(print_off, model_off, mixed_sublayer_config(false, false));
    const std::vector<GExtrusion> on  = gcode_extrusions(Test::gcode(print_on));
    const std::vector<GExtrusion> off = gcode_extrusions(Test::gcode(print_off));
    REQUIRE(!on.empty());
    // A bridge is among the paths the passes re-extrude.
    CHECK(std::any_of(on.begin(), on.end(), [](const GExtrusion &m) { return m.role.find("ridge") != std::string::npos; }));
    check_same_e_per_layer(e_per_layer(on), e_per_layer(off));
    check_no_extrusion_below_previous_layer(on);
}

TEST_CASE("Mixed sublayer: Ratio passes split AdaptiveLayerMerge bands (D9)", "[MixedSublayer][Slice][P3]")
{
    const MixsubAutoGenerateOff auto_generate_off;
    Print print;
    Model model;
    const PrintObject *object = slice_alm_plate(print, model, alm_mixed_config(), 5, { { Vec3d::Zero(), Vec3d(10., 10., 3.) } });
    REQUIRE(print.uses_multiple_nozzle_diameters());

    const auto &intervals = object->local_z_intervals();
    const auto &plans     = object->local_z_sublayer_plan();
    REQUIRE(!intervals.empty());
    size_t banded = 0, swallowed = 0;
    for (const LocalZInterval &interval : intervals) {
        INFO("layer " << interval.layer_id << " z_lo " << interval.z_lo << " z_hi " << interval.z_hi);
        if (interval.layer_id == 0)
            continue;
        double span = 0., first_top = -1., lowest = interval.z_hi;
        size_t ratio_passes = 0;
        for (size_t i = interval.first_sublayer_idx; i < interval.first_sublayer_idx + interval.sublayer_count && i < plans.size(); ++i) {
            const SubLayerPlan &plan = plans[i];
            if (!plan.ratio_pass)
                continue;
            ++ratio_passes;
            span += plan.flow_height;
            lowest = std::min(lowest, plan.z_lo);
            if (first_top < 0.)
                first_top = plan.z_hi;
            CHECK_THAT(plan.flow_height, WithinAbs(plan.z_hi - plan.z_lo, 1e-9));
        }
        if (ratio_passes == 0) {
            if (interval.has_mixed_paint)
                ++swallowed;
            continue;
        }
        CHECK_THAT(lowest, WithinAbs(interval.z_hi - span, 1e-6));
        if (span > interval.base_height + 1e-6) {
            ++banded;
            CHECK(first_top >= interval.z_lo - 1e-6);
        }
    }
    INFO("banded " << banded << " swallowed " << swallowed);
    CHECK(banded > 0);
    CHECK(swallowed > 0);

    // G-code: one layer change per layer, nothing extruded below the layer under it, and every layer extrudes what the
    // same plate extrudes with the region painted in one 0.8 filament (the same merge bands, printed whole).
    const std::vector<GExtrusion> moves = gcode_extrusions(Test::gcode(print));
    REQUIRE(!moves.empty());
    check_no_extrusion_below_previous_layer(moves);
    Print print_ref;
    Model model_ref;
    slice_alm_plate(print_ref, model_ref, alm_mixed_config(), 3, { { Vec3d::Zero(), Vec3d(10., 10., 3.) } });
    check_same_e_per_layer(e_per_layer(moves), e_per_layer(gcode_extrusions(Test::gcode(print_ref))));

    // Planning again (a perimeter-only re-run) gives the same plan: the band split starts from the slice-time plan.
    const auto before = ratio_plan_records(*object);
    const_cast<PrintObject *>(object)->adapt_local_z_ratio_plan_to_merge_bands();
    CHECK(ratio_plan_records(*object) == before);
}

TEST_CASE("Mixed sublayer: a stepped part on a 0.4 + 0.8 plate splits every band patch on its own height", "[MixedSublayer][Slice][P3]")
{
    const MixsubAutoGenerateOff auto_generate_off;
    // A 10 x 10 x 2.1 base with a 5 x 5 tower to 3.7: the step and the odd heights leave band tops, single steps and
    // swallowed footprints next to each other on the same layers.
    const std::vector<std::pair<Vec3d, Vec3d>> boxes { { Vec3d::Zero(), Vec3d(10., 10., 2.1) }, { Vec3d(0., 0., 2.1), Vec3d(5., 5., 1.6) } };
    Print print, print_ref;
    Model model, model_ref;
    const PrintObject *object = slice_alm_plate(print, model, alm_mixed_config(), 5, boxes);
    const std::vector<GExtrusion> moves = gcode_extrusions(Test::gcode(print));
    REQUIRE(!moves.empty());
    check_no_extrusion_below_previous_layer(moves);
    slice_alm_plate(print_ref, model_ref, alm_mixed_config(), 3, boxes);
    check_same_e_per_layer(e_per_layer(moves), e_per_layer(gcode_extrusions(Test::gcode(print_ref))));
    // Every Ratio pass group of a layer ends at the layer's top.
    for (const LocalZInterval &interval : object->local_z_intervals())
        for (size_t i = interval.first_sublayer_idx; i < interval.first_sublayer_idx + interval.sublayer_count; ++i) {
            const SubLayerPlan &plan = object->local_z_sublayer_plan()[i];
            if (plan.ratio_pass && plan.ratio_last_pass)
                CHECK_THAT(plan.z_hi, WithinAbs(interval.z_hi, 1e-6));
        }
}

TEST_CASE("Mixed sublayer: feature off on a 0.4 + 0.8 plate leaves the mixed region unmerged, as before", "[MixedSublayer][Slice][P3]")
{
    const MixsubAutoGenerateOff auto_generate_off;
    // Key off and the row on Auto: the region keeps its virtual id for the merge (no band patch on it), as before P3.
    Print print;
    Model model;
    const PrintObject *object = slice_alm_plate(print, model, alm_mixed_config(false), 5, { { Vec3d::Zero(), Vec3d(10., 10., 3.) } });
    CHECK(object->local_z_intervals().empty());
    size_t band_patches = 0;
    for (size_t region_id = 0; region_id < object->num_printing_regions(); ++region_id) {
        if (object->printing_region(region_id).config().wall_filament.value != 5)
            continue;
        for (const Layer *layer : object->layers())
            if (region_id < layer->regions().size())
                for (const AdaptiveMergePatch &patch : layer->get_region(int(region_id))->adaptive_merge_patches)
                    if (patch.group_top && patch.merged_height > layer->height + 1e-6)
                        ++band_patches;
    }
    CHECK(band_patches == 0);
}

TEST_CASE("Mixed sublayer: Ratio and Subdivide rows on one layer", "[MixedSublayer][Slice][P3]")
{
    const MixsubAutoGenerateOff auto_generate_off;
    DynamicPrintConfig config = mixed_sublayer_config(false, false, { 0.4, 0.4, 0.4 });
    config.set_num_extruders(3);
    config.set_num_filaments(3);
    config.option<ConfigOptionFloats>("filament_diameter")->values = { 1.75, 1.75, 1.75 };
    config.option<ConfigOptionStrings>("filament_colour")->values  = { "#FF0000", "#0000FF", "#00FF00" };
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = { 0.4, 0.4, 0.4 };
    config.option<ConfigOptionFloats>("min_layer_height")->values  = { 0.04, 0.04, 0.04 };
    // Row 4 = 1 + 2 at 30 % as Ratio, row 5 = 2 + 3 at 50 % as Subdivide (per-row methods, process keys off).
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 30, { "#FF0000", "#0000FF", "#00FF00" });
    mgr.add_custom_filament(2, 3, 50, { "#FF0000", "#0000FF", "#00FF00" });
    REQUIRE(mgr.mixed_filaments().size() == 2);
    mgr.mixed_filaments()[0].sublayer_method = int(MixedSublayerMethod::Ratio);
    mgr.mixed_filaments()[1].sublayer_method = int(MixedSublayerMethod::Subdivide);
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(mgr.serialize_custom_entries()));

    Print        print;
    Model        model;
    ModelObject *object_m = model.add_object();
    object_m->name        = "two_rows";
    ModelVolume *v        = object_m->add_volume(TriangleMesh(its_make_cube(10., 10., 3.)));
    v->config.set("extruder", 1);
    TriangleSelector selector(v->mesh());
    const indexed_triangle_set &its = v->mesh().its;
    for (int f = 0; f < int(its.indices.size()); ++f) {
        const stl_triangle_vertex_indices &tri = its.indices[size_t(f)];
        const Vec3f n = (its.vertices[tri[1]] - its.vertices[tri[0]]).cross(its.vertices[tri[2]] - its.vertices[tri[0]]).normalized();
        // The -X half of the side walls takes the Ratio row, everything else the Subdivide row.
        selector.set_facet(f, EnforcerBlockerType(n.x() < -0.5f ? 4 : 5));
    }
    REQUIRE(v->mmu_segmentation_facets.set(selector));
    object_m->add_instance();
    object_m->ensure_on_bed();
    print.apply(model, config);
    print.set_status_silent();
    print.process();
    const PrintObject *object = print.objects().front();
    const auto        &intervals = object->local_z_intervals();
    const auto        &plans     = object->local_z_sublayer_plan();

    size_t shared_layers = 0;
    std::map<size_t, std::vector<double>> ratio_tops; // by layer
    for (const LocalZInterval &interval : intervals) {
        if (!local_z_interval_has_passes(interval, plans))
            continue;
        bool   has_ratio = false, has_other = false;
        double ratio_sum = 0.;
        for (size_t i = interval.first_sublayer_idx; i < interval.first_sublayer_idx + interval.sublayer_count; ++i) {
            if (plans[i].ratio_pass) {
                has_ratio = true;
                ratio_sum += plans[i].flow_height;
                ratio_tops[interval.layer_id].push_back(plans[i].print_z);
            } else
                has_other = true;
        }
        if (has_ratio)
            CHECK_THAT(ratio_sum, WithinAbs(interval.base_height, 1e-9));
        if (has_ratio && has_other)
            ++shared_layers;
    }
    INFO("layers with both rows split: " << shared_layers);
    CHECK(shared_layers > 0);

    const std::string gcode = Test::gcode(print);
    REQUIRE(!gcode.empty());
    const std::string layer_tag = ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Layer_Change);
    size_t            layer_changes = 0;
    for (size_t pos = gcode.find(layer_tag); pos != std::string::npos; pos = gcode.find(layer_tag, pos + 1))
        ++layer_changes;
    CHECK(layer_changes == object->layer_count());

    // Infill policy: "Local-Z infill" is off, so infill (and top / bottom surfaces, bridges) prints either at the layer
    // top (the Subdivide row and the plain base, whole) or at a Ratio pass's Z (the Ratio row, split), never at a
    // Subdivide pass's Z.
    const std::vector<GExtrusion> moves = gcode_extrusions(gcode);
    size_t infill_checked = 0, off_policy = 0;
    for (const GExtrusion &m : moves) {
        if (!is_infill_role(m.role) || m.layer == 0 || m.layer >= object->layer_count())
            continue;
        ++infill_checked;
        const double top = object->get_layer(int(m.layer))->print_z;
        bool ok = std::abs(m.z - top) < 2e-3;
        for (double z : ratio_tops[m.layer])
            ok |= std::abs(m.z - z) < 2e-3;
        if (!ok && off_policy++ < 5)
            UNSCOPED_INFO("layer " << m.layer << " infill at z " << m.z << " role " << m.role);
    }
    CHECK(infill_checked > 0);
    CHECK(off_policy == 0);
    check_no_extrusion_below_previous_layer(moves);
}

TEST_CASE("Mixed sublayer: a Ratio component's heater stays on while its passes still print", "[MixedSublayer][Slice][P3]")
{
    const MixsubAutoGenerateOff auto_generate_off;
    // Three heads with ooze prevention (a non-Bambu tool changer turns a tool off after its last layer). Filament 1
    // prints a plain base, then only inside the Ratio passes of the 1 + 2 row above it, in Bambu Studio order so it
    // never ends a layer again: its last use is the last Ratio pass, not the base.
    DynamicPrintConfig config = mixed_sublayer_config(true, false, { 0.4, 0.4, 0.4 });
    config.set_num_extruders(3);
    config.set_num_filaments(3);
    config.option<ConfigOptionFloats>("filament_diameter")->values = { 1.75, 1.75, 1.75 };
    config.option<ConfigOptionStrings>("filament_colour")->values  = { "#FF0000", "#0000FF", "#00FF00" };
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = { 0.4, 0.4, 0.4 };
    config.option<ConfigOptionFloats>("min_layer_height")->values  = { 0.04, 0.04, 0.04 };
    config.set_deserialize_strict({ { "ooze_prevention", "1" } });
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 30, { "#FF0000", "#0000FF", "#00FF00" });
    mgr.mixed_filaments()[0].sublayer_bs_order = true;
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(mgr.serialize_custom_entries()));

    Print        print;
    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "base_and_mix";
    ModelVolume *base   = object->add_volume(TriangleMesh(its_make_cube(10., 10., 1.)));
    base->config.set("extruder", 1);
    TriangleMesh top_mesh(its_make_cube(10., 10., 2.));
    top_mesh.translate(Vec3f(0.f, 0.f, 1.f));
    ModelVolume *top = object->add_volume(std::move(top_mesh));
    top->config.set("extruder", 3);
    TriangleSelector selector(top->mesh());
    for (int f = 0; f < int(top->mesh().its.indices.size()); ++f)
        selector.set_facet(f, EnforcerBlockerType(4));
    REQUIRE(top->mmu_segmentation_facets.set(selector));
    object->add_instance();
    object->ensure_on_bed();
    print.apply(model, config);
    print.set_status_silent();
    const std::string gcode = Test::gcode(print);
    REQUIRE(!gcode.empty());

    // The last extrusion of each tool, and any "turned off" (S0 cooldown) command for it before that.
    std::map<int, size_t> last_extrusion;
    std::vector<std::pair<size_t, int>> cooldowns;
    std::istringstream in(gcode);
    std::string        line;
    int                tool = -1;
    for (size_t idx = 0; std::getline(in, line); ++idx) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.size() >= 2 && line[0] == 'T' && std::isdigit(static_cast<unsigned char>(line[1])))
            tool = std::atoi(line.c_str() + 1);
        else if (line.rfind("G1 ", 0) == 0 && line.find(" E") != std::string::npos && std::atof(line.c_str() + line.find(" E") + 2) > 0. &&
                 (line.find(" X") != std::string::npos || line.find(" Y") != std::string::npos) && tool >= 0)
            last_extrusion[tool] = idx;
        else if ((line.rfind("M104", 0) == 0 || line.rfind("G10", 0) == 0) && line.find("cooldown") != std::string::npos &&
                 line.find(" S0") != std::string::npos) {
            const size_t t = line.find(" T");
            cooldowns.emplace_back(idx, t == std::string::npos ? tool : std::atoi(line.c_str() + t + 2));
        }
    }
    REQUIRE(last_extrusion.count(0) == 1);
    for (const auto &[idx, t] : cooldowns) {
        INFO("tool " << t << " turned off at line " << idx);
        if (last_extrusion.count(t))
            CHECK(idx > last_extrusion[t]);
    }
}

namespace {

// Painted mixed cubes of the given heights side by side, with a Type2 prime tower and precise Z height on.
// The mixed row's two heads are separate nozzles, so a Ratio layer's pass changes are planned on the tower
// as Local-Z tool changes (D10 gives them the prime volume).
std::string slice_mixed_on_tower(Print &print, Model &model, const std::vector<double> &heights)
{
    DynamicPrintConfig config = mixed_sublayer_config(true, false);
    config.option<ConfigOptionBool>("enable_prime_tower")->value = true;
    config.set_deserialize_strict({
        { "wipe_tower_type", "type2" },
        { "precise_z_height", "1" },
        { "wipe_tower_x", "15" },
        { "wipe_tower_y", "15" },
        { "skirt_loops", "0" },
        { "brim_type", "no_brim" },
    });
    double x = 100.;
    for (double height : heights) {
        ModelObject *object = model.add_object();
        object->name        = "mixed_cube";
        ModelVolume *v      = object->add_volume(TriangleMesh(its_make_cube(10., 10., height)));
        v->config.set("extruder", 1);
        TriangleSelector selector(v->mesh());
        for (int f = 0; f < int(v->mesh().its.indices.size()); ++f)
            selector.set_facet(f, EnforcerBlockerType(3));
        REQUIRE(v->mmu_segmentation_facets.set(selector));
        object->add_instance()->set_offset(Vec3d(x, 100., 0.));
        object->ensure_on_bed();
        x += 30.;
    }
    // A second apply, as the wipe tower tests do: the first one can still see one filament and drop the tower.
    print.apply(model, config);
    print.apply(model, config);
    print.set_status_silent();
    const std::string gcode = Test::gcode(print);
    REQUIRE(print.has_wipe_tower());
    return gcode;
}

// Z of every extruding move under the tower's feature tag.
std::vector<double> mixed_tower_extrusion_z(const std::string &gcode)
{
    std::vector<double> zs;
    bool                in_tower = false;
    GCodeReader         parser;
    parser.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string_view raw = line.raw();
        if (raw.rfind(";TYPE:", 0) == 0)
            in_tower = raw.find("Prime tower") != std::string_view::npos;
        else if (in_tower && line.extruding(self) && (line.has_x() || line.has_y()))
            zs.emplace_back(self.z());
    });
    return zs;
}

size_t planned_local_z_tool_changes(const Print &print)
{
    size_t count = 0;
    for (const auto &layer : print.wipe_tower_data().local_z_tool_changes)
        count += layer.size();
    return count;
}

} // namespace

TEST_CASE("Mixed sublayer: Ratio layers on a prime tower print the tower at its own layers", "[MixedSublayer][Slice][WipeTower]")
{
    const MixsubAutoGenerateOff auto_generate_off;
    // A Ratio layer's passes live inside one tool-ordering layer, so they never look like a precise-Z sliver
    // (Orca #17204) to the tower. With a second cube 3.15 mm tall, precise Z height leaves the 6 mm cube's
    // layers 0.01 to 0.05 mm above the short one's near its top, under the 0.04 mm minimum for some: those
    // join the tower layer below, and their Local-Z pass changes must print down on it too.
    const double short_height = GENERATE(0., 3.15);
    CAPTURE(short_height);
    std::vector<double> heights { 6. };
    if (short_height > 0.)
        heights.emplace_back(short_height);

    Print             print;
    Model             model;
    const std::string gcode = slice_mixed_on_tower(print, model, heights);
    REQUIRE(!gcode.empty());
    CHECK(planned_local_z_tool_changes(print) > 0);

    size_t              joined = 0;
    std::vector<double> tower_layer_z;
    for (const LayerTools &lt : print.wipe_tower_data().tool_ordering) {
        if (lt.joins_wipe_tower_layer_below()) {
            ++joined;
            CHECK(lt.print_z - lt.wipe_tower_print_z < 0.04);
        } else if (lt.has_wipe_tower)
            tower_layer_z.emplace_back(lt.print_z);
    }
    if (short_height == 0.)
        CHECK(joined == 0);
    else
        CHECK(joined > 0);

    const std::vector<double> zs = mixed_tower_extrusion_z(gcode);
    REQUIRE_FALSE(zs.empty());
    size_t             off_layer = 0;
    std::ostringstream off_layer_z;
    for (double z : zs)
        if (std::none_of(tower_layer_z.begin(), tower_layer_z.end(), [z](double t) { return std::abs(z - t) < 0.002; })) {
            if (off_layer++ < 10)
                off_layer_z << z << " ";
        }
    INFO("tower extrusions off the tower layers, first Z values: " << off_layer_z.str());
    CHECK(off_layer == 0);
}

// ---- A mixed filament's nozzle in validation and flows (owner hand test on #937) -------------------------

namespace {

// The owner's H3C plate in small: one extruder with a rack 0.4 / 0.8 / 0.8 / 0.4, filament 2 and 3 pinned to the
// given slots, the mixed filament 2 + 3 at 30 % (filament 5), and a cube assigned to it. A mm width is set for the
// plate's 0.4 nozzle and scaled to the size of the nozzle that prints it (per-nozzle process P6).
const PrintObject *apply_rack_mixed_cube(Print &print, Model &model, const std::vector<int> &slot_pins, bool ratio,
                                         const char *inner_wall_width)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_filaments(4);
    const std::vector<std::string> colours { "#FF0000", "#00FF00", "#0000FF", "#FFFF00" };
    config.option<ConfigOptionStrings>("filament_colour", true)->values                = colours;
    config.option<ConfigOptionFloats>("nozzle_diameter", true)->values                  = { 0.4 };
    config.option<ConfigOptionStrings>("extruder_rack_nozzle_diameter", true)->values = { "0.4,0.8,0.8,0.4" };
    config.option<ConfigOptionInts>("filament_rack_nozzle_slot", true)->values          = slot_pins;
    config.set_deserialize_strict({
        { "enable_prime_tower", "0" },
        { "layer_height", "0.2" },
        { "initial_layer_print_height", "0.2" },
        { "inner_wall_line_width", inner_wall_width },
        { "enable_mixed_color_sublayer", ratio ? "1" : "0" },
    });
    MixedFilamentManager mgr;
    mgr.add_custom_filament(2, 3, 30, colours);
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(mgr.serialize_custom_entries()));

    ModelObject *object = model.add_object();
    object->name        = "mixed_cube";
    object->add_volume(TriangleMesh(its_make_cube(10., 10., 3.)));
    object->volumes.front()->config.set("extruder", 5);
    object->add_instance();
    object->ensure_on_bed();
    print.apply(model, config);
    REQUIRE(!print.objects().empty());
    return print.objects().front();
}

// The nozzle the region of filament 5 sizes its walls with.
double mixed_region_wall_nozzle(const PrintObject &object)
{
    for (const PrintRegion &region : object.all_regions())
        if (region.config().wall_filament.value == 5)
            return region.flow(object, frPerimeter, 0.2, false).nozzle_diameter();
    return 0.;
}

} // namespace

TEST_CASE("Mixed sublayer: a mixed filament's line widths are checked against its components' nozzle", "[MixedSublayer][Nozzle][Slice]")
{
    const MixsubAutoGenerateOff auto_generate_off;
    SECTION("components on the 0.8 slots: the inner wall is sized for and checked against the 0.8 nozzle")
    {
        Print              print;
        Model              model;
        const PrintObject *object = apply_rack_mixed_cube(print, model, { 0, 2, 3, 0 }, false, "0.41");
        const StringObjectException err = print.validate();
        INFO(err.string);
        CHECK(err.string.find("Line width") == std::string::npos);
        CHECK(err.string.find("too wide") == std::string::npos);
        CHECK_THAT(mixed_region_wall_nozzle(*object), WithinAbs(0.8, 1e-6));
        // 0.41 mm for the 0.4 nozzle is 0.82 mm on the 0.8 one: the owner's width, accepted.
        for (const PrintRegion &region : object->all_regions())
            if (region.config().wall_filament.value == 5)
                CHECK_THAT(region.flow(*object, frPerimeter, 0.2, false).width(), WithinAbs(0.82, 0.011));
    }
    SECTION("components on the 0.4 slots: the width error names the real nozzle and jumps to the components' tool")
    {
        Print print;
        Model model;
        apply_rack_mixed_cube(print, model, { 0, 1, 4, 0 }, false, "0.82");
        const StringObjectException err = print.validate();
        INFO(err.string << " | " << err.opt_key);
        REQUIRE(!err.string.empty());
        CHECK((err.string.find("0.4") != std::string::npos));
        CHECK(err.opt_key.rfind("inner_wall_line_width", 0) == 0);
        if (const size_t at = err.opt_key.find('@'); at != std::string::npos)
            CHECK(err.opt_key.substr(at + 1) == nozzle_filament_tool_id(print.config(), 2));
    }
    SECTION("components on different diameters, Ratio: refused by the same-diameter rule, no crash")
    {
        Print print;
        Model model;
        apply_rack_mixed_cube(print, model, { 0, 2, 4, 0 }, true, "0.41");
        const StringObjectException err = print.validate();
        INFO(err.string);
        // The cube is assigned whole, so the Ratio row may not be "used" by paint; either the D8 error or the width
        // check against the finest component's nozzle refuses the plate.
        CHECK(!err.string.empty());
    }
}
