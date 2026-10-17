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
#include "libslic3r/Support/SupportOverhangHighlight.hpp"
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
