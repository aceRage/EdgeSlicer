// Support overhang highlight (tests/plan_support_highlight_by_layer.md).
//
// SupportOverhangRule::effective_angle_deg() must predict whether the support generators put
// support under a planar slope, across layer heights, overlap mode, the tree 30 degree fallback
// and the normal-support sliver cap. Each case slices a real object, so a change in either
// generator's overhang detection fails here.

#include <catch2/catch.hpp>

#include <cmath>

#include "libslic3r/Config.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
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
