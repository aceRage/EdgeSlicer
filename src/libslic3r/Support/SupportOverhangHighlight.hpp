#pragma once

// Support overhang highlight (tests/plan_support_highlight_by_layer.md).
//
// The paint-on-supports gizmo and the 3D view's "Show overhang" tint faces by one angle. The
// support generators do not use an angle directly: per layer they support what sticks out past
// the lower layer grown by an offset, and that offset depends on the support type, the layer
// height and the outer wall width. SupportOverhangRule turns that layer rule into the one angle a
// planar slope needs to get support, so the highlight shows what automatic supports will really
// hold up:
//   * normal supports, threshold angle 0 ("overlap mode"): the lower layer is grown by
//     outer wall width - support_threshold_overlap, so the angle changes with the layer height
//     (24 deg at 0.1 mm, 41.6 deg at 0.2 mm, 53.1 deg at 0.3 mm for a 0.45 mm wall at 50 %);
//   * tree supports (every style) ignore the overlap and use 30 deg when the angle is 0;
//   * normal supports drop an overhang narrower than 0.2 x the outer wall width (the sliver
//     filter), which caps steep thresholds (65.8 deg at 0.2 mm layers for a 0.45 mm wall).
//
// Mirrors detect_overhangs() in SupportMaterial.cpp (normal supports) and
// TreeSupport::detect_overhangs() (every tree style; organic calls it too). The
// [SupportHighlight] tests in tests/fff_print slice real objects and fail when the generators
// and this rule drift apart.

#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r {

class ConfigBase;
class DynamicPrintConfig;

struct SupportOverhangRule
{
    enum class Source {
        Angle,        // support_threshold_angle > 0: lower layer grown by h / tan(angle + 1)
        Overlap,      // normal supports, angle 0: lower layer grown by w - support_threshold_overlap
        TreeFallback, // tree supports, angle 0: a fixed 30 degrees, support_threshold_overlap ignored
    };

    bool   enable_support   { false }; // enable_support
    bool   auto_support     { false }; // enable_support && is_auto(support_type)
    bool   tree             { false }; // is_tree(support_type): tree rules, else normal rules
    Source source           { Source::Angle };
    int    threshold_angle  { 0 };     // support_threshold_angle as configured
    double angle_deg        { 0. };    // the angle the generator really uses (Angle: +1 inclusive, clamped to 89; TreeFallback: 30)
    double overlap_mm       { 0. };    // support_threshold_overlap resolved against outer_wall_width (Overlap only)
    double outer_wall_width { 0. };    // mm, external perimeter width normal supports measure with
    double line_width       { 0. };    // mm, line width tree supports measure with
    double layer_height     { 0. };    // mm, layer_height of the config the rule was made from
    int    enforce_layers   { 0 };     // enforce_support_layers

    // Offset (mm) the generator grows the lower layer by before taking the difference, for a
    // lower layer of the given height. <= 0 means every overhang is supported.
    double lower_layer_offset(double lower_layer_height) const;
    // Normal supports drop a layer's overhang when no part of it is wider than this (mm):
    // `offset(diff_polygons, -0.1 * fw).empty()` in detect_overhangs(). Tree supports have no
    // such filter (0).
    double min_band_width() const { return tree ? 0. : 0.2 * outer_wall_width; }
    // The equivalent overhang angle from horizontal for a planar slope at the given layer height:
    // a facet whose slope from horizontal is below this angle gets support.
    double effective_angle_deg(double layer_height) const;
    // The same at the config's own layer height.
    double effective_angle_deg() const { return this->effective_angle_deg(this->layer_height); }
    // True when the normal-support sliver filter, not the threshold, sets the angle.
    bool   capped_by_min_band(double layer_height) const;
};

// cfg: a config holding the keys listed by support_overhang_rule_keys() (a full print config, or
// the small one support_overhang_rule_config() builds), object overrides already applied. The rule
// is resolved whether or not supports are enabled, so a manual support type or supports off still
// get a "what auto supports would do" answer; check auto_support for whether anything will
// actually be made.
SupportOverhangRule support_overhang_rule(const ConfigBase &cfg);

// The keys support_overhang_rule() reads, besides "nozzle_diameter", "filament_diameter" (its
// size is the filament count) and "filament_map".
const std::vector<std::string> &support_overhang_rule_keys();

// Gather what support_overhang_rule() needs without building a full config (the 3D view asks
// every frame): the print preset's keys, the printer's nozzle_diameter, the project's
// filament_map and the number of filaments, then the object's overrides applied the way
// PrintObject applies them to a region ("extruder" sets the wall filaments, a 0 filament means
// "default"). object_config may be null.
DynamicPrintConfig support_overhang_rule_config(const DynamicPrintConfig &print_config,
                                                const DynamicPrintConfig &printer_config,
                                                const DynamicPrintConfig &project_config,
                                                size_t                    filament_count,
                                                const DynamicPrintConfig *object_config);

} // namespace Slic3r
