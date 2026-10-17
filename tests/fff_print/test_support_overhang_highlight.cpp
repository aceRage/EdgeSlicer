// Support overhang highlight (tests/plan_support_highlight_by_layer.md).
//
// SupportOverhangRule::effective_angle_deg() must predict whether the support generators put
// support under a planar slope, across layer heights, overlap mode, the tree 30 degree fallback
// and the normal-support sliver cap. Each case slices a real object, so a change in either
// generator's overhang detection fails here.

#include <catch2/catch.hpp>

#include <chrono>
#include <cmath>
#include <iostream>

#include "libslic3r/Config.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Support/SupportOverhangDetect.hpp"
#include "libslic3r/Support/SupportOverhangHighlight.hpp"
#include "libslic3r/Support/TreeSupport.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;
using Catch::Matchers::WithinAbs;

namespace {

// An upside-down square frustum: a 10 x 10 mm foot on the bed, every side face overhanging at
// slope_deg from horizontal for 10 mm of height.
indexed_triangle_set inverted_frustum_its(double slope_deg, double foot = 10., double height = 10.)
{
    const double g = height / std::tan(slope_deg * M_PI / 180.);
    const float  a = float(foot / 2.), b = float(foot / 2. + g), h = float(height);
    indexed_triangle_set its;
    its.vertices = {
        { -a, -a, 0.f }, { a, -a, 0.f }, { a, a, 0.f }, { -a, a, 0.f },
        { -b, -b, h },   { b, -b, h },   { b, b, h },   { -b, b, h },
    };
    its.indices = {
        { 0, 2, 1 }, { 0, 3, 2 },   // bottom
        { 4, 5, 6 }, { 4, 6, 7 },   // top
        { 0, 1, 5 }, { 0, 5, 4 },   // -y
        { 1, 2, 6 }, { 1, 6, 5 },   // +x
        { 2, 3, 7 }, { 2, 7, 6 },   // +y
        { 3, 0, 4 }, { 3, 4, 7 },   // -x
    };
    return its;
}

DynamicPrintConfig support_config(const char *type, int angle, double layer_height,
                                  std::initializer_list<ConfigBase::SetDeserializeItem> extra = {})
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support",              1 },
        { "support_type",                type },
        { "support_style",               std::string(type).find("tree") != std::string::npos ? "tree_slim" : "default" },
        { "support_threshold_angle",     angle },
        { "support_threshold_overlap",   "50%" },
        { "layer_height",                layer_height },
        { "initial_layer_print_height",  layer_height },
        { "max_bridge_length",           0 },
    });
    config.set_deserialize_strict(extra);
    return config;
}

size_t support_extrusion_points(const Print &print)
{
    Points points;
    for (const SupportLayer *layer : print.objects().front()->support_layers())
        layer->support_fills.collect_points(points);
    return points.size();
}

struct SlopeCase { double slope; double layer_height; };

// Slice the frustum and report whether the generator made any support, next to the rule's
// prediction and the old gizmo's (support_threshold_angle as is, 0 = nothing highlighted).
void check_slope_cases(const char *type, int angle, std::initializer_list<SlopeCase> cases)
{
    for (const SlopeCase &c : cases) {
        DynamicPrintConfig config = support_config(type, angle, c.layer_height);
        const SupportOverhangRule rule = support_overhang_rule(config);
        const double eff        = rule.effective_angle_deg(c.layer_height);
        const bool   predicted  = c.slope < eff;
        const bool   old_gizmo  = angle > 0 && c.slope < angle;

        Print print;
        Model model;
        init_print({ TriangleMesh(inverted_frustum_its(c.slope)) }, print, model, config);
        print.process();
        const bool supported = support_extrusion_points(print) > 0;

        INFO(type << " threshold " << angle << " slope " << c.slope << " layer " << c.layer_height
                  << ": effective angle " << eff << ", rule says " << predicted << ", old gizmo says " << old_gizmo
                  << ", generator made support " << supported);
        CHECK(predicted == supported);
    }
}

} // namespace

TEST_CASE("Support highlight rule: normal supports in overlap mode depend on the layer height", "[SupportHighlight]")
{
    // outer wall 1.125 * 0.4 = 0.45 mm, overlap 50% -> lower layer grown by 0.225 mm:
    // 0.1 mm layers -> 24.0 deg, 0.2 -> 41.6 deg, 0.3 -> 53.1 deg.
    const SupportOverhangRule rule = support_overhang_rule(support_config("normal(auto)", 0, 0.2));
    CHECK(rule.source == SupportOverhangRule::Source::Overlap);
    CHECK_THAT(rule.outer_wall_width, WithinAbs(0.45, 1e-6));
    CHECK_THAT(rule.effective_angle_deg(0.1), WithinAbs(23.96, 0.05));
    CHECK_THAT(rule.effective_angle_deg(0.2), WithinAbs(41.63, 0.05));
    CHECK_THAT(rule.effective_angle_deg(0.3), WithinAbs(53.13, 0.05));
    CHECK_THAT(rule.effective_angle_deg(), WithinAbs(41.63, 0.05));

    // The same 45 degree chamfer is unsupported at 0.1 / 0.2 mm and supported at 0.3 mm; the
    // old gizmo highlighted nothing at all in overlap mode.
    check_slope_cases("normal(auto)", 0, { { 45., 0.1 }, { 45., 0.3 }, { 30., 0.2 }, { 55., 0.2 } });
}

TEST_CASE("Support highlight rule: tree supports ignore the overlap and fall back to 30 degrees", "[SupportHighlight]")
{
    const SupportOverhangRule rule = support_overhang_rule(support_config("tree(auto)", 0, 0.3));
    CHECK(rule.source == SupportOverhangRule::Source::TreeFallback);
    CHECK_THAT(rule.effective_angle_deg(0.1), WithinAbs(30., 1e-6));
    CHECK_THAT(rule.effective_angle_deg(0.3), WithinAbs(30., 1e-6));
    // A 45 degree chamfer at 0.3 mm: normal(auto) supports it (above), tree(auto) does not.
    check_slope_cases("tree(auto)", 0, { { 45., 0.3 }, { 20., 0.2 } });
}

TEST_CASE("Support highlight rule: explicit angle is inclusive (+1)", "[SupportHighlight]")
{
    const SupportOverhangRule rule = support_overhang_rule(support_config("normal(auto)", 40, 0.2));
    CHECK(rule.source == SupportOverhangRule::Source::Angle);
    CHECK_THAT(rule.effective_angle_deg(0.2), WithinAbs(41., 1e-6));
    CHECK_FALSE(rule.capped_by_min_band(0.2));
    // 55 deg faces: the frustum's corner edges slope at atan(tan(55) / sqrt(2)) = 45.3 deg, still
    // above 41 (see the corner case below for why the corners matter).
    check_slope_cases("normal(auto)", 40, { { 35., 0.2 }, { 55., 0.2 } });
    check_slope_cases("tree(auto)", 40, { { 35., 0.2 }, { 55., 0.2 } });
}

TEST_CASE("Support highlight rule: convex corners are a blind spot of any per-facet angle", "[SupportHighlight]")
{
    // Every face of this frustum slopes at 50 deg, above the 41 deg the rule asks for, so a
    // facet-normal highlight shows nothing. But where two faces meet at a 90 degree corner the
    // edge slopes at atan(tan(50) / sqrt(2)) = 40.1 deg: each layer's corner sticks out diagonally
    // past the grown lower layer, and both generators support the corners. Documented in the
    // gizmo tooltip; a layer-based highlight is needed to show it.
    for (const char *type : { "normal(auto)", "tree(auto)" }) {
        DynamicPrintConfig config = support_config(type, 40, 0.2);
        Print print;
        Model model;
        init_print({ TriangleMesh(inverted_frustum_its(50.)) }, print, model, config);
        print.process();
        INFO(type);
        CHECK(support_extrusion_points(print) > 0);
        CHECK(50. > support_overhang_rule(config).effective_angle_deg(0.2));
    }
}

TEST_CASE("Support highlight rule: normal supports cap steep thresholds with the sliver filter", "[SupportHighlight]")
{
    // Threshold 80: the lower layer grows by only 0.2 / tan(81) = 0.03 mm, but normal support
    // then drops any overhang band narrower than 0.2 * 0.45 = 0.09 mm, so at 0.2 mm layers
    // nothing steeper than atan(0.2 / 0.09) = 65.8 deg is supported. Tree supports have no such
    // filter.
    const SupportOverhangRule normal = support_overhang_rule(support_config("normal(auto)", 80, 0.2));
    CHECK_THAT(normal.effective_angle_deg(0.2), WithinAbs(65.77, 0.05));
    CHECK(normal.capped_by_min_band(0.2));
    const SupportOverhangRule tree = support_overhang_rule(support_config("tree(auto)", 80, 0.2));
    CHECK_THAT(tree.effective_angle_deg(0.2), WithinAbs(81., 1e-6));
    CHECK_FALSE(tree.capped_by_min_band(0.2));
    check_slope_cases("normal(auto)", 80, { { 72., 0.2 }, { 60., 0.2 } });
    check_slope_cases("tree(auto)", 80, { { 72., 0.2 } });
}

TEST_CASE("Support highlight rule: supports off or manual still answer what auto would do", "[SupportHighlight]")
{
    SupportOverhangRule off = support_overhang_rule(support_config("normal(auto)", 0, 0.2, { { "enable_support", 0 } }));
    CHECK_FALSE(off.enable_support);
    CHECK_FALSE(off.auto_support);
    CHECK_THAT(off.effective_angle_deg(), WithinAbs(41.63, 0.05));
    SupportOverhangRule manual = support_overhang_rule(support_config("tree(manual)", 0, 0.2));
    CHECK(manual.enable_support);
    CHECK_FALSE(manual.auto_support);
    CHECK(manual.source == SupportOverhangRule::Source::TreeFallback);
}

TEST_CASE("Support highlight rule: the outer wall width follows the wall filament's physical nozzle", "[SupportHighlight]")
{
    // An H2D-like machine: two nozzles (0.4, 0.8), four filaments, filament_map sends 2 and 3 to
    // the 0.8 nozzle. The external perimeter flow of a region printed with filament 3 is 0.9 mm
    // wide (1.125 x 0.8), not 1.125 x nozzle_diameter[2] (which does not exist).
    auto cfg = [](std::initializer_list<ConfigBase::SetDeserializeItem> extra) {
        DynamicPrintConfig c = support_config("normal(auto)", 0, 0.2, {
            { "nozzle_diameter",   "0.4,0.8" },
            { "filament_diameter", "1.75,1.75,1.75,1.75" },
            { "filament_map",      "1,2,2,1" },
        });
        c.set_deserialize_strict(extra);
        return c;
    };
    CHECK_THAT(support_overhang_rule(cfg({ { "wall_filament", 3 } })).outer_wall_width, WithinAbs(0.9, 1e-6));
    CHECK_THAT(support_overhang_rule(cfg({ { "wall_filament", 4 } })).outer_wall_width, WithinAbs(0.45, 1e-6));
    // outer_wall_filament (0 = follow wall_filament) wins for the outer wall.
    CHECK_THAT(support_overhang_rule(cfg({ { "wall_filament", 1 }, { "outer_wall_filament", 2 } })).outer_wall_width, WithinAbs(0.9, 1e-6));
    // Out of range: PrintObject clamps it to filament 1.
    CHECK_THAT(support_overhang_rule(cfg({ { "wall_filament", 9 } })).outer_wall_width, WithinAbs(0.45, 1e-6));
    // The overlap angle follows: 0.9 mm wall at 50 % -> grown by 0.45 mm -> atan(0.2 / 0.45).
    CHECK_THAT(support_overhang_rule(cfg({ { "wall_filament", 3 } })).effective_angle_deg(0.2), WithinAbs(23.96, 0.05));
    // Tree supports measure with line_width of the first nozzle, as TreeSupport::detect_overhangs() does.
    CHECK_THAT(support_overhang_rule(cfg({ { "wall_filament", 3 }, { "line_width", "0.42" } })).line_width, WithinAbs(0.42, 1e-6));

    // The raw-value overload agrees with the PrintConfig one.
    PrintConfig pc;
    pc.apply(cfg({}), true);
    for (unsigned int f = 0; f <= 5; ++ f)
        CHECK(physical_extruder_for_filament(pc, f) ==
              physical_extruder_for_filament(pc.nozzle_diameter.values.size(), pc.filament_diameter.values.size(), pc.filament_map.values, f));
}

TEST_CASE("Support highlight rule: the small rule config matches the full config, object overrides applied", "[SupportHighlight]")
{
    DynamicPrintConfig full = support_config("normal(auto)", 0, 0.2, {
        { "nozzle_diameter",   "0.4,0.8" },
        { "filament_diameter", "1.75,1.75,1.75" },
        { "filament_map",      "1,2,2" },
        { "wall_filament",     1 },
    });
    // The 3D view and the gizmo build the rule from the presets without a full config.
    const SupportOverhangRule a = support_overhang_rule(full);
    const SupportOverhangRule b = support_overhang_rule(support_overhang_rule_config(full, full, full, 3, nullptr));
    CHECK(a.source == b.source);
    CHECK(a.tree == b.tree);
    CHECK(a.auto_support == b.auto_support);
    CHECK_THAT(a.outer_wall_width, WithinAbs(b.outer_wall_width, 1e-9));
    CHECK_THAT(a.effective_angle_deg(), WithinAbs(b.effective_angle_deg(), 1e-9));

    // Object overrides: layer height, support type and the object's filament ("extruder").
    DynamicPrintConfig object;
    object.set_deserialize_strict({ { "layer_height", 0.3 }, { "extruder", 2 } });
    SupportOverhangRule o = support_overhang_rule(support_overhang_rule_config(full, full, full, 3, &object));
    CHECK_THAT(o.layer_height, WithinAbs(0.3, 1e-9));
    CHECK_THAT(o.outer_wall_width, WithinAbs(0.9, 1e-6));
    // 0.9 mm wall at 50 % -> 0.45 mm; atan(0.3 / 0.45) = 33.7 deg.
    CHECK_THAT(o.effective_angle_deg(), WithinAbs(33.69, 0.05));

    object.set_deserialize_strict({ { "support_type", "tree(auto)" }, { "wall_filament", 0 } });
    o = support_overhang_rule(support_overhang_rule_config(full, full, full, 3, &object));
    CHECK(o.source == SupportOverhangRule::Source::TreeFallback);
    CHECK_THAT(o.effective_angle_deg(), WithinAbs(30., 1e-9));
    // wall_filament 0 means "default" and does not undo the object's "extruder".
    CHECK_THAT(o.outer_wall_width, WithinAbs(0.9, 1e-6));
}

// ---------------------------------------------------------------------------------------------
// Phase 2: Layer::support_overhangs (the generator's decision, kept for the preview) and the
// preview overlay built from it (support_overhang_overlay()).
// ---------------------------------------------------------------------------------------------

namespace {

indexed_triangle_set box_its(double x0, double y0, double z0, double dx, double dy, double dz)
{
    indexed_triangle_set its = its_make_cube(dx, dy, dz);
    for (stl_vertex &v : its.vertices)
        v += Vec3f(float(x0), float(y0), float(z0));
    return its;
}

struct OverlayTotals
{
    double supported   { 0. }; // mm2 over all layers
    double unsupported { 0. };
    size_t supported_layers { 0 };
};

double area_mm2(const ExPolygons &expolys)
{
    double a = 0.;
    for (const ExPolygon &e : expolys)
        a += e.area();
    return a * SCALING_FACTOR * SCALING_FACTOR;
}

OverlayTotals overlay_totals(const PrintObject &object, double z_min = 0., double z_max = 1e9)
{
    OverlayTotals t;
    for (const SupportOverhangOverlayLayer &l : support_overhang_overlay(object)) {
        if (l.print_z < z_min || l.print_z > z_max)
            continue;
        t.supported   += area_mm2(l.supported);
        t.unsupported += area_mm2(l.unsupported);
        t.supported_layers += l.supported.empty() ? 0 : 1;
        // Triangles come in threes and cover the polygons.
        CHECK(l.supported_triangles.size() % 3 == 0);
        CHECK(l.unsupported_triangles.size() % 3 == 0);
        CHECK(l.supported.empty() == l.supported_triangles.empty());
    }
    return t;
}

size_t layers_with_support_overhangs(const PrintObject &object)
{
    size_t n = 0;
    for (const Layer *layer : object.layers())
        n += layer->support_overhangs.empty() ? 0 : 1;
    return n;
}

} // namespace

TEST_CASE("Support highlight overlay: the generators keep what they hold up", "[SupportHighlight]")
{
    for (const char *type : { "normal(auto)", "tree(auto)" }) {
        DynamicPrintConfig config = support_config(type, 40, 0.2);
        Print print;
        Model model;
        init_print({ TriangleMesh(inverted_frustum_its(30.)) }, print, model, config);
        print.process();
        const PrintObject &object = *print.objects().front();
        INFO(type);
        REQUIRE(support_extrusion_points(print) > 0);
        // Every layer of a 30 degree frustum overhangs: (nearly) all of them are held up.
        CHECK(layers_with_support_overhangs(object) > object.layer_count() / 2);
        CHECK(object.layers().front()->support_overhangs.empty());
        const OverlayTotals t = overlay_totals(object);
        INFO("supported " << t.supported << " mm2, unsupported " << t.unsupported << " mm2");
        CHECK(t.supported > 0.);
        // What the rule calls an overhang is held up: little or nothing is left amber.
        CHECK(t.unsupported < 0.1 * t.supported);
    }
}

TEST_CASE("Support highlight overlay: supports off leave every overhang unsupported", "[SupportHighlight]")
{
    DynamicPrintConfig config = support_config("normal(auto)", 40, 0.2, { { "enable_support", 0 } });
    Print print;
    Model model;
    init_print({ TriangleMesh(inverted_frustum_its(30.)) }, print, model, config);
    print.process();
    const PrintObject &object = *print.objects().front();
    CHECK(layers_with_support_overhangs(object) == 0);
    const OverlayTotals t = overlay_totals(object);
    CHECK(t.supported == 0.);
    CHECK(t.unsupported > 0.);

    // Turning supports on fills the cache; turning them off again clears it with the support step.
    config.set_deserialize_strict({ { "enable_support", 1 } });
    print.apply(model, config);
    print.process();
    CHECK(layers_with_support_overhangs(*print.objects().front()) > 0);
    config.set_deserialize_strict({ { "enable_support", 0 } });
    print.apply(model, config);
    print.process();
    CHECK(layers_with_support_overhangs(*print.objects().front()) == 0);
}

TEST_CASE("Support highlight overlay: on build plate only, an overhang above the part is unsupported", "[SupportHighlight]")
{
    // A 20 x 20 x 4 mm base, a 4 x 4 mm column up to 10 mm and a 16 x 16 x 2 mm cap on it: the
    // cap's underside hangs over the base, not over the plate.
    indexed_triangle_set its = box_its(-10., -10., 0., 20., 20., 4.);
    its_merge(its, box_its(-2., -2., 4., 4., 4., 6.));
    its_merge(its, box_its(-8., -8., 10., 16., 16., 2.));
    for (const bool plate_only : { false, true }) {
        DynamicPrintConfig config = support_config("normal(auto)", 40, 0.2, { { "support_on_build_plate_only", plate_only ? 1 : 0 } });
        Print print;
        Model model;
        init_print({ TriangleMesh(its) }, print, model, config);
        print.process();
        // The cap's first layers.
        const OverlayTotals t = overlay_totals(*print.objects().front(), 10.05, 10.65);
        INFO("plate only " << plate_only << ": supported " << t.supported << " mm2, unsupported " << t.unsupported << " mm2");
        if (plate_only) {
            CHECK(t.supported == 0.);
            CHECK(t.unsupported > 100.);
        } else {
            CHECK(t.supported > 100.);
            CHECK(t.unsupported < 0.1 * t.supported);
        }
    }
}

TEST_CASE("Support highlight overlay: a short bridge is left to bridging", "[SupportHighlight]")
{
    // Two 5 x 5 x 10 mm pillars 8 mm apart and a 18 x 5 x 2 mm slab across them. Tree supports with
    // max_bridge_length 10 bridge the 8 mm span (amber); with 0 they support it (red).
    indexed_triangle_set its = box_its(0., 0., 0., 5., 5., 10.);
    its_merge(its, box_its(13., 0., 0., 5., 5., 10.));
    its_merge(its, box_its(0., 0., 10., 18., 5., 2.));
    const double gap_area = 8. * 5.;
    for (const int max_bridge : { 0, 10 }) {
        DynamicPrintConfig config = support_config("tree(auto)", 40, 0.2, { { "max_bridge_length", max_bridge } });
        Print print;
        Model model;
        init_print({ TriangleMesh(its) }, print, model, config);
        print.process();
        const OverlayTotals t = overlay_totals(*print.objects().front(), 10.05, 10.25);
        INFO("max_bridge_length " << max_bridge << ": supported " << t.supported << " mm2, unsupported " << t.unsupported << " mm2");
        if (max_bridge == 0) {
            CHECK(t.supported > 0.5 * gap_area);
        } else {
            CHECK(t.unsupported > 0.5 * gap_area);
            CHECK(t.supported < 0.5 * gap_area);
        }
    }
}

TEST_CASE("Support highlight benchmark: preview overlay build", "[.][SupportOverlayBench]")
{
    auto run = [](const char *name, Model &model, const char *type) {
        DynamicPrintConfig config = support_config(type, 30, 0.2);
        DynamicPrintConfig full   = DynamicPrintConfig::full_print_config();
        full.apply(config);
        for (ModelObject *mo : model.objects) {
            if (mo->instances.empty())
                mo->add_instance();
            mo->ensure_on_bed();
        }
        Print print;
        print.apply(model, full);
        print.set_status_silent();
        const auto t0 = std::chrono::steady_clock::now();
        print.process();
        const auto t1 = std::chrono::steady_clock::now();
        size_t layers = 0, tris = 0, facets = 0;
        double ms = 0.;
        for (const PrintObject *object : print.objects()) {
            const auto a = std::chrono::steady_clock::now();
            const std::vector<SupportOverhangOverlayLayer> overlay = support_overhang_overlay(*object);
            ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
            layers += object->layer_count();
            for (const SupportOverhangOverlayLayer &l : overlay)
                tris += (l.supported_triangles.size() + l.unsupported_triangles.size()) / 3;
            facets += object->model_object()->facets_count();
        }
        std::cout << name << " (" << facets << " facets, " << layers << " layers), " << type << ": slice " << std::chrono::duration<double, std::milli>(t1 - t0).count()
                  << " ms, overlay build " << ms << " ms, " << tris << " triangles (" << (tris * 3 * 3 * 4 + tris * 3 * 4) / 1024 << " KiB on the GPU)" << std::endl;
    };
    for (const char *type : { "normal(auto)", "tree(auto)" }) {
        {
            Model model;
            ModelObject *mo = model.add_object();
            indexed_triangle_set its = its_make_sphere(40., 2. * PI / 1000.);
            mo->add_volume(TriangleMesh(std::move(its)));
            run("sphere r40", model, type);
        }
        {
            Model model;
            DynamicPrintConfig cfg;
            ConfigSubstitutionContext ctxt(ForwardCompatibilitySubstitutionRule::EnableSilent);
            PlateDataPtrs plate_data;
            std::vector<Preset*> presets;
            bool is_bbl = false;
            Semver version;
            const std::string path = std::string(TEST_DATA_DIR) + "/../../resources/handy_models/3DBenchy.3mf";
            if (load_bbs_3mf(path.c_str(), &cfg, &ctxt, &model, &plate_data, &presets, &is_bbl, &version, nullptr, LoadStrategy::LoadModel) &&
                ! model.objects.empty())
                run("3DBenchy", model, type);
            else
                std::cout << "3DBenchy: could not load " << path << std::endl;
            release_PlateData_list(plate_data);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Phase 3: before slicing (support_overhang_preview(), overhang_facets_from_layers()).
// ---------------------------------------------------------------------------------------------

namespace {

// Object layers as generate_object_layers() returns them: low, high pairs from z = 0.
std::vector<double> uniform_layer_zs(const indexed_triangle_set &its, double first_layer_height, double layer_height)
{
    float zmax = 0.f;
    for (const stl_vertex &v : its.vertices)
        zmax = std::max(zmax, v.z());
    std::vector<double> zs;
    double bottom = 0., h = first_layer_height;
    while (bottom + 0.5 * h < double(zmax)) {
        zs.push_back(bottom);
        zs.push_back(bottom + h);
        bottom += h;
        h = layer_height;
    }
    return zs;
}

SupportOverhangPreview preview_of(const indexed_triangle_set &its, const SupportOverhangRule &rule, double layer_height,
                                  bool plate_only = false, const indexed_triangle_set *negative = nullptr)
{
    SupportOverhangPreviewInput in;
    in.parts.push_back({ std::make_shared<const indexed_triangle_set>(its), Transform3d::Identity(), false });
    if (negative != nullptr)
        in.parts.push_back({ std::make_shared<const indexed_triangle_set>(*negative), Transform3d::Identity(), true });
    in.layer_zs         = uniform_layer_zs(its, layer_height, layer_height);
    in.rule             = rule;
    in.build_plate_only = plate_only;
    return support_overhang_preview(in);
}

// Option (b) per facet, for comparison: a downward facet flatter than the rule's angle, not on the bed.
std::vector<uint8_t> angle_overhang_facets(const indexed_triangle_set &its, double first_layer_height, double layer_height,
                                           const SupportOverhangRule &rule)
{
    std::vector<uint8_t> flags(its.indices.size(), 0);
    const double limit = -std::cos(rule.effective_angle_deg(layer_height) * M_PI / 180.);
    for (size_t f = 0; f < its.indices.size(); ++ f) {
        const stl_triangle_vertex_indices &idx = its.indices[f];
        const Vec3f a = its.vertices[idx(0)], b = its.vertices[idx(1)], c = its.vertices[idx(2)];
        if (std::max({ a.z(), b.z(), c.z() }) <= 0.5 * first_layer_height)
            continue;
        const Vec3f nrm = (b - a).cross(c - a);
        const float len = nrm.norm();
        if (len > 0.f && double(nrm.z() / len) < limit)
            flags[f] = 1;
    }
    return flags;
}

} // namespace

TEST_CASE("Support highlight core: overhangs from the object's slices match TreeSupport::detect_overhangs", "[SupportHighlight]")
{
    struct ModelCase { const char *name; TriangleMesh mesh; };
    std::vector<ModelCase> models;
    models.push_back({ "frustum 30 deg", TriangleMesh(inverted_frustum_its(30.)) });
    models.push_back({ "frustum 50 deg", TriangleMesh(inverted_frustum_its(50.)) });
    models.push_back({ "sphere r15", make_sphere(15., 2. * PI / 120.) });

    for (ModelCase &m : models) {
        DynamicPrintConfig config = support_config("tree(auto)", 40, 0.2);
        Print print;
        Model model;
        init_print({ m.mesh }, print, model, config);
        print.process();

        PrintObject *object = print.get_object(size_t(0));
        // The generator's own overhangs: run its detection again on the finished slices (after
        // print.process() Layer::loverhangs holds the auto-lift overhangs instead).
        TreeSupport tree_support(*object, object->slicing_parameters());
        tree_support.support_type = stTreeAuto;
        tree_support.detect_overhangs();

        std::vector<ExPolygons> lslices;
        std::vector<double>     heights;
        for (const Layer *layer : object->layers()) {
            lslices.push_back(layer->lslices);
            heights.push_back(layer->height);
        }
        const std::vector<ExPolygons> ours = support_overhangs_from_slices(lslices, heights, support_overhang_rule(*object));

        double area_theirs = 0., area_ours = 0., area_xor = 0.;
        for (size_t i = 1; i < lslices.size(); ++ i) {
            const ExPolygons &theirs = object->get_layer(int(i))->loverhangs;
            area_theirs += area_mm2(theirs);
            area_ours   += area_mm2(ours[i]);
            area_xor    += area_mm2(diff_ex(theirs, ours[i])) + area_mm2(diff_ex(ours[i], theirs));
        }
        const double denom    = std::max(area_theirs, area_ours);
        const double mismatch = denom > 0. ? area_xor / denom : 0.;
        INFO(m.name << ": generator " << area_theirs << " mm2, ours " << area_ours << " mm2, mismatch " << mismatch);
        if (std::string(m.name).find("sphere") == std::string::npos)
            CHECK(mismatch < 0.01);
        else
            // The generator adds sharp tails at the sphere's foot; the rest must agree.
            CHECK(mismatch < 0.15);
    }
}

TEST_CASE("Support highlight preview: convex corners are found by the layer core", "[SupportHighlight]")
{
    // The 50 degree frustum of the rule test: no facet is flatter than the 41 degree rule, but
    // the corner edges slope at 40.1 degrees and both generators support them. The layer-based
    // preview finds those layers.
    for (const char *type : { "normal(auto)", "tree(auto)" }) {
        const SupportOverhangRule    rule = support_overhang_rule(support_config(type, 40, 0.2));
        const SupportOverhangPreview p    = preview_of(inverted_frustum_its(50.), rule, 0.2);
        INFO(type);
        CHECK(p.layers_with_overhangs() > 10);
    }
}

TEST_CASE("Support highlight preview: facets mapped from the layer overhangs", "[SupportHighlight]")
{
    for (const double slope : { 30., 50. }) {
        const indexed_triangle_set   its  = inverted_frustum_its(slope);
        const SupportOverhangRule    rule = support_overhang_rule(support_config("tree(auto)", 40, 0.2));
        const SupportOverhangPreview p    = preview_of(its, rule, 0.2);
        REQUIRE(p.part_facets.size() == 1);
        const std::vector<uint8_t> &by_layer = p.part_facets.front();
        const std::vector<uint8_t>  by_angle = angle_overhang_facets(its, 0.2, 0.2, rule);
        REQUIRE(by_layer.size() == 12);
        const bool expect_sides = slope < 41.;
        for (size_t f = 0; f < 12; ++ f) {
            INFO("slope " << slope << " facet " << f);
            const bool side = f >= 4;
            CHECK(bool(by_layer[f]) == (side && expect_sides));
            CHECK(bool(by_layer[f]) == bool(by_angle[f]));
        }
    }
}

TEST_CASE("Support highlight preview: layer mapping vs angle on a sphere", "[SupportHighlight]")
{
    // How far the two per-facet answers drift apart on a curved surface. The layer answer follows
    // the generator; the angle answer is phase 1.
    indexed_triangle_set its = its_make_sphere(15., 2. * PI / 180.);
    for (stl_vertex &v : its.vertices) v.z() += 15.f;
    for (const char *type : { "normal(auto)", "tree(auto)" }) {
        const SupportOverhangRule    rule     = support_overhang_rule(support_config(type, 40, 0.2));
        const std::vector<uint8_t>   by_layer = preview_of(its, rule, 0.2).part_facets.front();
        const std::vector<uint8_t>   by_angle = angle_overhang_facets(its, 0.2, 0.2, rule);
        size_t n_layer = 0, n_angle = 0, n_diff = 0;
        for (size_t f = 0; f < by_layer.size(); ++ f) {
            n_layer += by_layer[f] != 0;
            n_angle += by_angle[f] != 0;
            n_diff  += (by_layer[f] != 0) != (by_angle[f] != 0);
        }
        INFO(type << ": facets " << by_layer.size() << ", by layer " << n_layer << ", by angle " << n_angle << ", differ " << n_diff);
        CHECK(n_layer > 0);
        CHECK(double(n_diff) < 0.1 * double(std::max(n_layer, n_angle)));
    }
}

TEST_CASE("Support highlight preview: build plate only, supports off, negative volumes, transforms", "[SupportHighlight]")
{
    // The base / column / cap of the overlay test: the cap's underside hangs over the base.
    indexed_triangle_set its = box_its(-10., -10., 0., 20., 20., 4.);
    its_merge(its, box_its(-2., -2., 4., 4., 4., 6.));
    its_merge(its, box_its(-8., -8., 10., 16., 16., 2.));
    const SupportOverhangRule on = support_overhang_rule(support_config("normal(auto)", 40, 0.2));

    auto count = [](const std::vector<uint8_t> &flags, uint8_t v) { return size_t(std::count(flags.begin(), flags.end(), v)); };
    {
        const SupportOverhangPreview p = preview_of(its, on, 0.2);
        CHECK(count(p.part_facets.front(), 1) > 0);
        CHECK(count(p.part_facets.front(), 2) == 0);
    }
    {
        // On build plate only: the cap is over the base, so amber.
        const SupportOverhangPreview p = preview_of(its, on, 0.2, true);
        CHECK(count(p.part_facets.front(), 1) == 0);
        CHECK(count(p.part_facets.front(), 2) > 0);
    }
    {
        // Supports off: the same overhang, amber.
        const SupportOverhangRule off = support_overhang_rule(support_config("normal(auto)", 40, 0.2, { { "enable_support", 0 } }));
        const SupportOverhangPreview p = preview_of(its, off, 0.2);
        CHECK(count(p.part_facets.front(), 1) == 0);
        CHECK(count(p.part_facets.front(), 2) > 0);
    }
    {
        // A negative volume taking the whole cap away leaves nothing to hold up.
        const indexed_triangle_set cut = box_its(-9., -9., 9.9, 18., 18., 3.);
        const SupportOverhangPreview p = preview_of(its, on, 0.2, false, &cut);
        CHECK(count(p.part_facets.front(), 1) == 0);
        CHECK(p.part_facets.back().empty());
    }
    {
        // Placed by a mirroring transformation (X): the windings flip, the same facets overhang.
        Transform3d mirror = Transform3d::Identity();
        mirror.scale(Vec3d(-1., 1., 1.));
        SupportOverhangPreviewInput in;
        in.parts.push_back({ std::make_shared<const indexed_triangle_set>(inverted_frustum_its(30.)), mirror, false });
        in.layer_zs = uniform_layer_zs(inverted_frustum_its(30.), 0.2, 0.2);
        in.rule     = on;
        const SupportOverhangPreview p = support_overhang_preview(in);
        const std::vector<uint8_t> &f = p.part_facets.front();
        REQUIRE(f.size() == 12);
        for (size_t k = 0; k < 12; ++ k) {
            INFO("facet " << k);
            CHECK(bool(f[k]) == (k >= 4));
        }
    }
}

TEST_CASE("Support highlight preview: cancel stops the run", "[SupportHighlight]")
{
    SupportOverhangPreviewInput in;
    in.parts.push_back({ std::make_shared<const indexed_triangle_set>(inverted_frustum_its(30.)), Transform3d::Identity(), false });
    in.layer_zs = uniform_layer_zs(inverted_frustum_its(30.), 0.2, 0.2);
    in.rule     = support_overhang_rule(support_config("normal(auto)", 40, 0.2));
    struct Cancelled {};
    CHECK_THROWS_AS(support_overhang_preview(in, []() { throw Cancelled(); }), Cancelled);
}

TEST_CASE("Support highlight benchmark: pre-slice preview (slice + detect + map)", "[.][SupportHighlightBench]")
{
    auto run = [](const char *name, const SupportOverhangPreviewInput &in) {
        const auto t0 = std::chrono::steady_clock::now();
        const SupportOverhangPreview p = support_overhang_preview(in);
        const double total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        size_t facets = 0, flagged = 0;
        for (const std::vector<uint8_t> &f : p.part_facets) {
            facets += f.size();
            for (uint8_t v : f) flagged += v != 0;
        }
        std::cout << name << ": " << facets << " facets, " << p.slice_z.size() << " layers: slice " << p.slice_ms << " ms, detect " << p.detect_ms
                  << " ms, map " << p.map_ms << " ms, total " << total << " ms, flagged " << flagged << std::endl;
    };
    for (const char *type : { "normal(auto)", "tree(auto)" }) {
        for (const double h : { 0.2, 0.1 }) {
            const SupportOverhangRule rule = support_overhang_rule(support_config(type, 30, h));
            for (const int steps : { 180, 1000 }) {
                indexed_triangle_set its = its_make_sphere(40., 2. * PI / double(steps));
                for (stl_vertex &v : its.vertices) v.z() += 40.f;
                SupportOverhangPreviewInput in;
                in.parts.push_back({ std::make_shared<const indexed_triangle_set>(std::move(its)), Transform3d::Identity(), false });
                in.layer_zs = uniform_layer_zs(*in.parts.front().its, h, h);
                in.rule     = rule;
                run((std::string("sphere r40 ") + type + " " + std::to_string(h)).c_str(), in);
            }
            // 3DBenchy from the handy models, on the bed.
            Model model;
            DynamicPrintConfig cfg;
            ConfigSubstitutionContext ctxt(ForwardCompatibilitySubstitutionRule::EnableSilent);
            PlateDataPtrs plate_data;
            std::vector<Preset*> presets;
            bool is_bbl = false;
            Semver version;
            const std::string path = std::string(TEST_DATA_DIR) + "/../../resources/handy_models/3DBenchy.3mf";
            if (load_bbs_3mf(path.c_str(), &cfg, &ctxt, &model, &plate_data, &presets, &is_bbl, &version, nullptr, LoadStrategy::LoadModel) &&
                ! model.objects.empty()) {
                ModelObject *mo = model.objects.front();
                if (mo->instances.empty())
                    mo->add_instance();
                mo->ensure_on_bed();
                SupportOverhangPreviewInput in;
                for (const ModelVolume *mv : mo->volumes)
                    if (mv->is_model_part())
                        in.parts.push_back({ std::make_shared<const indexed_triangle_set>(mv->mesh().its),
                                             mo->instances.front()->get_matrix() * mv->get_matrix(), false });
                indexed_triangle_set world;
                for (const auto &part : in.parts) {
                    indexed_triangle_set t = *part.its;
                    for (stl_vertex &v : t.vertices) v = (part.trafo * v.cast<double>()).cast<float>();
                    its_merge(world, t);
                }
                in.layer_zs = uniform_layer_zs(world, h, h);
                in.rule     = rule;
                run((std::string("3DBenchy ") + type + " " + std::to_string(h)).c_str(), in);
            }
            release_PlateData_list(plate_data);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Phase 4: one detector (Support/SupportOverhangDetect.hpp) for both generators and the highlight.
// ---------------------------------------------------------------------------------------------

TEST_CASE("Support highlight detector: the highlight and the generators detect the same overhangs", "[SupportHighlight]")
{
    // With the generators' extras out of the way (no small-overhang removal, no bridges, no
    // expansion; a 20 degree frustum has no sharp tails or cantilevers) what each generator holds up is
    // exactly its detector's output, and the highlight runs the same detector on the same slices.
    for (const char *type : { "normal(auto)", "tree(auto)" }) {
        for (const int angle : { 0, 40 }) {
            DynamicPrintConfig config = support_config(type, angle, 0.2, {
                { "support_remove_small_overhang", 0 },
                { "support_expansion",             0 },
                { "bridge_no_support",             0 },
            });
            Print print;
            Model model;
            init_print({ TriangleMesh(inverted_frustum_its(20.)) }, print, model, config);
            print.process();
            const PrintObject &object = *print.objects().front();

            std::vector<ExPolygons> lslices;
            std::vector<double>     heights;
            for (const Layer *layer : object.layers()) {
                lslices.push_back(layer->lslices);
                heights.push_back(layer->height);
            }
            const std::vector<ExPolygons> ours = support_overhangs_from_slices(lslices, heights, support_overhang_rule(object));
            double area_theirs = 0., area_xor = 0.;
            for (size_t i = 1; i < lslices.size(); ++ i) {
                const ExPolygons &theirs = object.get_layer(int(i))->support_overhangs;
                area_theirs += area_mm2(theirs);
                area_xor    += area_mm2(diff_ex(theirs, ours[i])) + area_mm2(diff_ex(ours[i], theirs));
            }
            INFO(type << " threshold " << angle << ": generator " << area_theirs << " mm2, difference " << area_xor << " mm2");
            CHECK(area_theirs > 0.);
            // Trees run on the very same lslices; normal supports on the region's slices, which for
            // one region are the same areas (lslices is their union).
            CHECK(area_xor < (std::string(type).find("tree") != std::string::npos ? 1e-6 : 1e-4) * area_theirs + 1e-6);
        }
    }
}

TEST_CASE("Support highlight detector: the shared rule pieces", "[SupportHighlight]")
{
    CHECK(normal_support_threshold_angle(0) == 0.);
    CHECK(normal_support_threshold_angle(40) == 41.);
    CHECK(normal_support_threshold_angle(90) == 89.);
    CHECK(tree_support_threshold_angle(0) == 30.);
    CHECK(tree_support_threshold_angle(40) == 41.);
    const ConfigOptionFloatOrPercent half(50., true);
    const float fw = float(scale_(0.45));
    // The first enforce_support_layers layers: everything (offset 0).
    CHECK(normal_support_lower_layer_offset(1, 3, 0., 0.2, fw, half) == 0.f);
    // Overlap mode: the wall minus the overlap.
    CHECK_THAT(double(normal_support_lower_layer_offset(5, 0, 0., 0.2, fw, half)), WithinAbs(scale_(0.225), 2.));
    // Angle mode: h / tan(angle).
    CHECK_THAT(double(normal_support_lower_layer_offset(5, 0, M_PI / 4., 0.2, fw, half)), WithinAbs(scale_(0.2), 2.));
    CHECK_THAT(tree_support_lower_layer_offset(1, 3, 0.4, 0.2, M_PI / 4.), WithinAbs(-0.06, 1e-9));
    CHECK_THAT(tree_support_lower_layer_offset(5, 3, 0.4, 0.2, M_PI / 4.), WithinAbs(0.2, 1e-6));
    // The sliver filter drops a band narrower than 0.2 x fw.
    Polygons band { Polygon({ { 0, 0 }, { scaled<coord_t>(10.), 0 }, { scaled<coord_t>(10.), scaled<coord_t>(0.05) }, { 0, scaled<coord_t>(0.05) } }) };
    CHECK(normal_support_overhang_too_thin(band, fw));
    band.front().points[2].y() = band.front().points[3].y() = scaled<coord_t>(1.);
    CHECK_FALSE(normal_support_overhang_too_thin(band, fw));
}

// ---------------------------------------------------------------------------------------------
// Phase 5: the pre-slice highlight runs the generator's own detection
// (support_overhang_preview_exact()) and must match the slice exactly.
// ---------------------------------------------------------------------------------------------

namespace {

// 3DBenchy from the handy models, on the bed, one instance. False when the file is not there.
bool load_benchy(Model &model)
{
    DynamicPrintConfig cfg;
    ConfigSubstitutionContext ctxt(ForwardCompatibilitySubstitutionRule::EnableSilent);
    PlateDataPtrs plate_data;
    std::vector<Preset*> presets;
    bool is_bbl = false;
    Semver version;
    const std::string path = std::string(TEST_DATA_DIR) + "/../../resources/handy_models/3DBenchy.3mf";
    const bool ok = load_bbs_3mf(path.c_str(), &cfg, &ctxt, &model, &plate_data, &presets, &is_bbl, &version, nullptr, LoadStrategy::LoadModel) &&
                    ! model.objects.empty();
    release_PlateData_list(plate_data);
    if (! ok)
        return false;
    while (model.objects.size() > 1)
        model.delete_object(model.objects.size() - 1);
    ModelObject *mo = model.objects.front();
    if (mo->instances.empty())
        mo->add_instance();
    while (mo->instances.size() > 1)
        mo->delete_instance(mo->instances.size() - 1);
    mo->config.reset();
    mo->ensure_on_bed();
    return true;
}

struct LayerAreas { std::vector<ExPolygons> supported; double total { 0. }; };

// What the slice holds up per layer, in world XY like the preview: the after-slicing overlay's
// supported areas (Layer::support_overhangs, with trees on the build plate only trimmed as there).
LayerAreas sliced_support_overhangs(const Model &model, const DynamicPrintConfig &config)
{
    DynamicPrintConfig full = DynamicPrintConfig::full_print_config();
    full.apply(config);
    Print print;
    print.apply(model, full);
    print.set_status_silent();
    print.process();
    LayerAreas out;
    const PrintObject &object = *print.objects().front();
    const Point shift = object.instances().front().shift;
    out.supported.assign(object.layer_count(), {});
    for (SupportOverhangOverlayLayer &l : support_overhang_overlay(object))
        for (size_t i = 0; i < object.layer_count(); ++ i)
            if (object.get_layer(int(i))->print_z == l.print_z) {
                for (ExPolygon &e : l.supported)
                    e.translate(shift);
                out.total += area_mm2(l.supported);
                out.supported[i] = std::move(l.supported);
                break;
            }
    return out;
}

SupportOverhangPreview exact_preview(const Model &model, const DynamicPrintConfig &config)
{
    DynamicPrintConfig full = DynamicPrintConfig::full_print_config();
    full.apply(config);
    Print print;
    return support_overhang_preview_exact(print, model, full);
}

// The per-layer difference between the highlight and the slice, relative to the slice's area.
double layer_mismatch(const LayerAreas &slice, const std::vector<ExPolygons> &highlight)
{
    double xor_area = 0.;
    const size_t n = std::max(slice.supported.size(), highlight.size());
    for (size_t i = 0; i < n; ++ i) {
        const ExPolygons empty;
        const ExPolygons &a = i < slice.supported.size() ? slice.supported[i] : empty;
        const ExPolygons &b = i < highlight.size() ? highlight[i] : empty;
        xor_area += area_mm2(diff_ex(a, b)) + area_mm2(diff_ex(b, a));
    }
    return slice.total > 0. ? xor_area / slice.total : xor_area;
}

size_t count_facets(const SupportOverhangPreview &p, uint8_t cls)
{
    size_t n = 0;
    for (const std::vector<uint8_t> &f : p.part_facets)
        n += size_t(std::count(f.begin(), f.end(), cls));
    return n;
}

} // namespace

TEST_CASE("Support highlight exact: the highlight holds up exactly what the slice does on a Benchy", "[SupportHighlight]")
{
    Model model;
    if (! load_benchy(model)) {
        WARN("3DBenchy.3mf not found, skipped");
        return;
    }
    const std::vector<std::pair<const char*, DynamicPrintConfig>> cases = {
        { "normal(auto)", support_config("normal(auto)", 30, 0.2, { { "max_bridge_length", 10 } }) },
        { "normal(auto) bridge_no_support", support_config("normal(auto)", 30, 0.2, { { "bridge_no_support", 1 } }) },
        { "tree slim", support_config("tree(auto)", 30, 0.2, { { "support_style", "tree_slim" }, { "max_bridge_length", 10 } }) },
        { "organic", support_config("tree(auto)", 30, 0.2, { { "support_style", "tree_organic" }, { "max_bridge_length", 10 } }) },
        { "organic angle 0, build plate only", support_config("tree(auto)", 0, 0.2, { { "support_style", "tree_organic" }, { "max_bridge_length", 10 },
                                                                                       { "support_on_build_plate_only", 1 } }) },
    };
    double normal_area = 0., tree_area = 0.;
    for (const auto &[name, config] : cases) {
        const LayerAreas             slice     = sliced_support_overhangs(model, config);
        const SupportOverhangPreview highlight = exact_preview(model, config);
        const double                 mismatch  = layer_mismatch(slice, highlight.supported);
        INFO(name << ": the slice holds up " << slice.total << " mm2 over " << slice.supported.size() << " layers, the highlight "
                  << highlight.supported.size() << " layers, mismatch " << mismatch << ", red faces " << count_facets(highlight, 1)
                  << ", amber faces " << count_facets(highlight, 2));
        CHECK(slice.supported.size() == highlight.supported.size());
        CHECK(slice.total > 0.);
        CHECK(mismatch < 1e-6);
        CHECK(count_facets(highlight, 1) > 0);
        if (std::string(name) == "normal(auto)")
            normal_area = slice.total;
        if (std::string(name) == "tree slim")
            tree_area = slice.total;
    }
    // Normal and tree supports decide differently (trees bridge short gaps such as the door and
    // window tops): the highlight follows each.
    INFO("normal " << normal_area << " mm2, tree " << tree_area << " mm2");
    CHECK(std::abs(normal_area - tree_area) > 0.05 * std::max(normal_area, tree_area));
}

TEST_CASE("Support highlight exact: object overrides and supports switched off", "[SupportHighlight]")
{
    // The process says normal supports; the object says tree. The highlight follows the object,
    // as the slice does.
    Model model;
    model.add_object()->add_volume(TriangleMesh(inverted_frustum_its(30.)));
    model.objects.front()->add_instance();
    model.objects.front()->ensure_on_bed();
    {
        DynamicPrintConfig object_config;
        object_config.set_deserialize_strict({ { "support_type", "tree(auto)" }, { "support_style", "tree_slim" } });
        model.objects.front()->config.assign_config(std::move(object_config));
    }
    const DynamicPrintConfig config = support_config("normal(auto)", 40, 0.2);
    {
        const LayerAreas slice = sliced_support_overhangs(model, config);
        const SupportOverhangPreview highlight = exact_preview(model, config);
        INFO("slice " << slice.total << " mm2, mismatch " << layer_mismatch(slice, highlight.supported));
        CHECK(slice.total > 0.);
        CHECK(layer_mismatch(slice, highlight.supported) < 1e-6);
    }
    {
        // Supports off for the object: the highlight simulates automatic supports, which then hold
        // up what the slice with supports on would.
        Model off = model;
        DynamicPrintConfig object_config = off.objects.front()->config.get();
        object_config.set_deserialize_strict({ { "enable_support", 0 } });
        off.objects.front()->config.assign_config(std::move(object_config));
        const LayerAreas on_slice = sliced_support_overhangs(model, config);
        const SupportOverhangPreview highlight = exact_preview(off, config);
        CHECK(layer_mismatch(on_slice, highlight.supported) < 1e-6);
    }
}

TEST_CASE("Support highlight exact: cancel ends the run", "[SupportHighlight]")
{
    Model model;
    model.add_object()->add_volume(TriangleMesh(inverted_frustum_its(30.)));
    model.objects.front()->add_instance();
    model.objects.front()->ensure_on_bed();
    DynamicPrintConfig full = DynamicPrintConfig::full_print_config();
    full.apply(support_config("normal(auto)", 40, 0.2));
    Print print;
    print.cancel();
    CHECK_THROWS_AS(support_overhang_preview_exact(print, model, full), CanceledException);
}

TEST_CASE("Support highlight benchmark: exact pre-slice preview", "[.][SupportHighlightBench]")
{
    auto run = [](const char *name, const Model &model, const char *type, const char *style) {
        DynamicPrintConfig config = support_config(type, 30, 0.2, { { "support_style", style }, { "max_bridge_length", 10 } });
        const auto t0 = std::chrono::steady_clock::now();
        const SupportOverhangPreview p = exact_preview(model, config);
        const double total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::cout << "exact " << name << " " << type << " " << style << ": " << p.slice_z.size() << " layers, steps " << p.slice_ms << " ms, detect "
                  << p.detect_ms << " ms, map " << p.map_ms << " ms, total " << total << " ms, red " << count_facets(p, 1) << ", amber "
                  << count_facets(p, 2) << std::endl;
    };
    Model benchy;
    const bool have_benchy = load_benchy(benchy);
    Model sphere;
    {
        indexed_triangle_set its = its_make_sphere(40., 2. * PI / 1000.);
        sphere.add_object()->add_volume(TriangleMesh(std::move(its)));
        sphere.objects.front()->add_instance();
        sphere.objects.front()->ensure_on_bed();
    }
    using TypeStyle = std::pair<const char*, const char*>;
    for (const auto &[type, style] : std::initializer_list<TypeStyle>{ { "normal(auto)", "default" }, { "tree(auto)", "tree_slim" }, { "tree(auto)", "tree_organic" } }) {
        if (have_benchy)
            run("3DBenchy", benchy, type, style);
        run("sphere r40 998k", sphere, type, style);
    }
}
