#pragma once

// The support generators' overhang detection, shared (support highlight phase 4,
// tests/plan_support_highlight_by_layer.md).
//
// Both generators decide per layer what sticks out past the lower layer grown by an offset. These
// functions are that decision, taken out of detect_overhangs() in SupportMaterial.cpp (normal
// supports) and TreeSupport::detect_overhangs() (every tree style; organic calls it too) without
// changing a single operation, so the generators produce the same G-code. The support painting
// highlight (Support/SupportOverhangHighlight.hpp) calls the same functions, so what it shows
// cannot drift from what the generators do. The extras stay in the generators: sharp tails,
// cantilevers, small-overhang removal, blockers, bridges, enforcers, critical regions only.

#include "../Config.hpp"
#include "../ExPolygon.hpp"
#include "../Polygon.hpp"

namespace Slic3r {

// The threshold angle (degrees) the generators really use: "+1 makes the threshold inclusive",
// below 90. Normal supports: 0 means "overlap mode". Tree supports: 0 means 30 degrees.
double normal_support_threshold_angle(int support_threshold_angle);
double tree_support_threshold_angle(int support_threshold_angle);

// ---- Normal supports (SupportMaterial.cpp) ----

// The scaled offset the lower layer is grown by: 0 for the first enforce_support_layers layers
// (everything is supported), lower_layer_height / tan(threshold) with a threshold angle, else
// fw - support_threshold_overlap (fw: the scaled external perimeter width).
float normal_support_lower_layer_offset(size_t layer_id, int enforce_support_layers, double threshold_rad, double lower_layer_height,
                                        float fw, const ConfigOptionFloatOrPercent &support_threshold_overlap);

// One region's overhang on one layer, before sharp tails, blockers, bridges and the sliver filter:
// with offset 0 everything past the lower layer; else (automatic normal supports only) what
// sticks out past the grown lower layer, grown back to the full overhang. buildplate_covered is
// subtracted when buildplate_only.
Polygons normal_support_region_overhangs(const Polygons &layerm_polygons, const Polygons &lower_layer_polygons, float lower_layer_offset,
                                         bool auto_normal_support, bool buildplate_only, const Polygons &buildplate_covered);

// The sliver filter: an overhang with no part wider than 0.2 x fw is dropped.
bool normal_support_overhang_too_thin(const Polygons &overhang, float fw);

// ---- Tree supports (TreeSupport.cpp) ----

// "Filter out areas whose diameter that is smaller than extrusion_width, but we don't want to
// lose any details": Layer::lslices_extrudable.
ExPolygons tree_support_extrudable(const ExPolygons &lslices, coordf_t extrusion_width_scaled);

// The (unscaled) lower-layer offset: -0.15 x the line width for the first enforce_support_layers
// layers, else lower_layer_height / tan(threshold).
coordf_t tree_support_lower_layer_offset(size_t layer_nr, int enforce_support_layers, coordf_t extrusion_width, coordf_t lower_layer_height,
                                         double threshold_rad);

// The "normal overhang" of a layer: its extrudable area past the lower layer's grown by
// support_offset_scaled. lower_offseted receives the grown lower layer (the generator reuses it).
ExPolygons tree_support_layer_overhangs(const ExPolygons &curr_polys, const ExPolygons &lower_polys, coordf_t support_offset_scaled,
                                        ExPolygons &lower_offseted);

} // namespace Slic3r
