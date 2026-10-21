// The support generators' overhang detection, shared. See SupportOverhangDetect.hpp.
//
// Every expression below is the generators' own, moved here unchanged (types and casts included),
// so the G-code stays byte for byte the same; the G-code identity gate checks it.

#include "SupportOverhangDetect.hpp"

#include "../ClipperUtils.hpp"
#include "../libslic3r.h"

#include <algorithm>
#include <cmath>

namespace Slic3r {

// Same as SupportMaterial.cpp / TreeSupport.cpp.
#define SUPPORT_SURFACES_OFFSET_PARAMETERS ClipperLib::jtSquare, 0.

double normal_support_threshold_angle(int support_threshold_angle)
{
    // If user specified a custom angle threshold, convert it to radians.
    // Zero means automatic overhang detection.
    // +1 makes the threshold inclusive
    double thresh_angle = support_threshold_angle > 0 ? support_threshold_angle + 1 : 0;
    thresh_angle = std::min(thresh_angle, 89.); // BBS should be smaller than 90
    return thresh_angle;
}

double tree_support_threshold_angle(int support_threshold_angle)
{
    // +1 makes the threshold inclusive
    double thresh_angle = support_threshold_angle > EPSILON ? support_threshold_angle + 1 : 30;
    thresh_angle = std::min(thresh_angle, 89.); // should be smaller than 90
    return thresh_angle;
}

float normal_support_lower_layer_offset(size_t layer_id, int enforce_support_layers, double threshold_rad, double lower_layer_height,
                                        float fw, const ConfigOptionFloatOrPercent &support_threshold_overlap)
{
    return
        (layer_id < (size_t)enforce_support_layers) ?
            // Enforce a full possible support, ignore the overhang angle.
            0.f :
        (threshold_rad > 0. ?
            // Overhang defined by an angle.
            float(scale_(lower_layer_height / tan(threshold_rad))) :
            // Overhang defined by overlap.
            fw - float(scale_(support_threshold_overlap.get_abs_value(unscale_(fw)))));
}

Polygons normal_support_region_overhangs(const Polygons &layerm_polygons, const Polygons &lower_layer_polygons, float lower_layer_offset,
                                         bool auto_normal_support, bool buildplate_only, const Polygons &buildplate_covered)
{
    Polygons diff_polygons;
    if (lower_layer_offset == 0.f) {
        // Support everything.
        diff_polygons = diff(layerm_polygons, lower_layer_polygons);
        if (buildplate_only) {
            // Don't support overhangs above the top surfaces.
            // This step is done before the contact surface is calculated by growing the overhang region.
            diff_polygons = diff(diff_polygons, buildplate_covered);
        }
    } else if (auto_normal_support) {
        // Get the regions needing a suport, collapse very tiny spots.
        //FIXME cache the lower layer offset if this layer has multiple regions.
        diff_polygons =
            diff(layerm_polygons,
                    expand(lower_layer_polygons, lower_layer_offset, SUPPORT_SURFACES_OFFSET_PARAMETERS));
        if (buildplate_only && ! buildplate_covered.empty()) {
            // Don't support overhangs above the top surfaces.
            // This step is done before the contact surface is calculated by growing the overhang region.
            diff_polygons = diff(diff_polygons, buildplate_covered);
        }
        if (! diff_polygons.empty()) {
            // Offset the support regions back to a full overhang, restrict them to the full overhang.
            // This is done to increase size of the supporting columns below, as they are calculated by
            // propagating these contact surfaces downwards.
            diff_polygons = diff(intersection(expand(diff_polygons, lower_layer_offset, SUPPORT_SURFACES_OFFSET_PARAMETERS), layerm_polygons), lower_layer_polygons);
        }
    }
    return diff_polygons;
}

bool normal_support_overhang_too_thin(const Polygons &overhang, float fw)
{
    return overhang.empty() || offset(overhang, -0.1 * fw).empty();
}

ExPolygons tree_support_extrudable(const ExPolygons &lslices, coordf_t extrusion_width_scaled)
{
    return intersection_ex(lslices, offset2_ex(lslices, -extrusion_width_scaled / 2, extrusion_width_scaled));
}

coordf_t tree_support_lower_layer_offset(size_t layer_nr, int enforce_support_layers, coordf_t extrusion_width, coordf_t lower_layer_height,
                                         double threshold_rad)
{
    return layer_nr < enforce_support_layers ? -0.15 * extrusion_width : (float)lower_layer_height / tan(threshold_rad);
}

ExPolygons tree_support_layer_overhangs(const ExPolygons &curr_polys, const ExPolygons &lower_polys, coordf_t support_offset_scaled,
                                        ExPolygons &lower_offseted)
{
    // normal overhang
    lower_offseted = offset_ex(lower_polys, support_offset_scaled, SUPPORT_SURFACES_OFFSET_PARAMETERS);
    return diff_ex(curr_polys, lower_offseted);
}

} // namespace Slic3r
