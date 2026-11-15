#ifndef slic3r_VariableWidth_hpp_
#define slic3r_VariableWidth_hpp_

#include "Polygon.hpp"
#include "ExtrusionEntity.hpp"
#include "Flow.hpp"

namespace Slic3r {
    ExtrusionMultiPath thick_polyline_to_multi_path(const ThickPolyline& thick_polyline, ExtrusionRole role, const Flow& flow, const float tolerance, const float merge_tolerance);
    // Drops the closed gap fill loops that nearly duplicate an earlier one (#375); open polylines are all kept.
    // variable_width() applies it to more than one gap fill polyline; callers that pass them one by one call it first.
    ThickPolylines remove_near_duplicate_gap_fill_loops(const ThickPolylines& polylines);
    void variable_width(const ThickPolylines& polylines, ExtrusionRole role, const Flow& flow, std::vector<ExtrusionEntity*>& out);
}

#endif
