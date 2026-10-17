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

#include "../ExPolygon.hpp"
#include "admesh/stl.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r {

class ConfigBase;
class DynamicPrintConfig;
class PrintObject;

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

// The rule as the generators see it for a sliced object: the print config, the object config and
// its first region's config (outer wall width and filament).
SupportOverhangRule support_overhang_rule(const PrintObject &object);

// The layer-over-layer rule alone, run on an object's layer slices: lslices[i] (layer 0 on the
// bed), heights[i] the layer heights. Returns the per-layer overhang areas either generator would
// detect before its extras (no sharp tails, small-overhang removal, cantilevers, bridge removal,
// build-plate-only trimming, enforcers or blockers). Normal supports: the full overhang (grown back)
// and the sliver filter; trees: the extrudable areas, no sliver filter.
std::vector<ExPolygons> support_overhangs_from_slices(const std::vector<ExPolygons> &lslices,
                                                      const std::vector<double>     &heights,
                                                      const SupportOverhangRule     &rule);

// Phase 2: the preview overlay of one sliced object (posSupportMaterial done), per object layer.
struct SupportOverhangOverlayLayer
{
    double              print_z { 0. };   // the layer's top, as in Layer::print_z (raft included)
    ExPolygons          supported;        // what the generator holds up (Layer::support_overhangs)
    ExPolygons          unsupported;      // what the rule calls an overhang and nothing holds up
    // Both triangulated (object XY coordinates in mm, three points per triangle, counter-clockwise
    // from above). Instances add PrintInstance::shift.
    std::vector<Vec2f>  supported_triangles;
    std::vector<Vec2f>  unsupported_triangles;
};

// Unsupported = the rule's overhang (support_overhangs_from_slices()) minus the supported areas
// grown by 0.1 x the outer wall width, opened by the same amount so slivers along the edge of a
// supported area do not show. Tree supports with "on build plate only" drop their branches over
// the object later, so for them what lies above lower layers moves to the unsupported set, as the
// normal generator does up front. Layers with neither are left out.
std::vector<SupportOverhangOverlayLayer> support_overhang_overlay(const PrintObject &object);

// ---------------------------------------------------------------------------------------------
// Phase 3: before slicing. A quick slice of the object's parts at its real layer heights, the
// rule above plus the slice-only extras, mapped back onto the facets.
// ---------------------------------------------------------------------------------------------

// Map per-layer areas back onto the facets of `its` placed by `trafo` (world: z = 0 on the bed,
// the same frame as the areas). slice_z[i] is the slicing plane of layer i; layer i's overhang is
// made by the facets between slice_z[i-1] and slice_z[i]. grow[i] (mm, may be empty) is added to
// the areas before the test (the tree lower-layer offset, so an overhang band covers its whole
// facet strip). A downward facet is flagged when at least `min_coverage` of its projected area in
// the slabs it crosses lies inside the grown areas. Layer 0 (the bed) is never flagged. One flag per
// facet.
std::vector<uint8_t> overhang_facets_from_layers(const indexed_triangle_set    &its,
                                                 const Transform3d             &trafo,
                                                 const std::vector<float>      &slice_z,
                                                 const std::vector<ExPolygons> &areas,
                                                 const std::vector<double>     &grow,
                                                 double                         min_coverage = 0.5);

struct SupportOverhangPreviewInput
{
    struct Part {
        std::shared_ptr<const indexed_triangle_set> its;      // the volume's mesh (immutable, shared)
        Transform3d                                 trafo { Transform3d::Identity() }; // volume -> world
        bool                                        negative { false }; // a negative volume: cut away, not mapped
    };
    std::vector<Part>   parts;
    // Object layers as from generate_object_layers(): low, high pairs from z = 0 (no raft).
    std::vector<double> layer_zs;
    SupportOverhangRule rule;
    bool                build_plate_only { false };
    float               closing_radius   { 0.049f };
};

struct SupportOverhangPreview
{
    std::vector<float>       slice_z;     // per object layer
    std::vector<double>      print_z;     // top of each layer (world z, raft not included)
    std::vector<ExPolygons>  supported;   // per layer, world XY: what automatic supports would hold up
    std::vector<ExPolygons>  unsupported; // per layer: overhangs that would get none (supports off or
                                          // manual, build plate only)
    // Per input part (empty for negative parts), per facet: 0 none, 1 supported, 2 unsupported.
    std::vector<std::vector<uint8_t>> part_facets;
    size_t layers_with_overhangs() const;
    double slice_ms { 0. }, detect_ms { 0. }, map_ms { 0. };
};

// throw_on_cancel is called between the stages and inside the slicer; whatever it throws ends
// the run. Extras modelled: floating islands (sharp tails) are supported, "on build plate only"
// moves overhangs above lower layers to `unsupported`, supports off or a manual type move
// everything to `unsupported`. Not modelled: bridges, small-overhang removal, cantilevers,
// critical regions only, painted enforcers / blockers (the gizmo shows those itself).
SupportOverhangPreview support_overhang_preview(const SupportOverhangPreviewInput &input,
                                                const std::function<void()>       &throw_on_cancel = {});

} // namespace Slic3r
