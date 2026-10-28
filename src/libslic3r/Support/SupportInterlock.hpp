#pragma once

// EdgeSlicer: support interface interlock ("woven band"), phase 1.
// Design: tests/design_support_interface_interlock.md (Option 1, owner decisions D1-D7).
//
// With a separate interface filament, the interface layers just under the top contact layer are cut
// into stripes that alternate the support body filament and the interface filament. The next layer
// down has its stripes turned 90 degrees, so every interface stripe runs under body stripes that are
// fused to the body on both sides: the interface cannot peel off the body without breaking beams.
// The top contact layer (the one that touches the model) is never changed.
//
// This header holds the pure geometry: the stripe grid and the split of a footprint into body and
// interface stripes. The layer bookkeeping lives in SupportCommon.cpp
// (support_interlock_weave_layers, called at the end of generate_interface_layers, and the woven
// extrusion in generate_support_toolpaths).

#include "../libslic3r.h"
#include "../Point.hpp"
#include "../Polygon.hpp"
#include "../ExPolygon.hpp"

namespace Slic3r {

struct SupportInterlockSplit
{
    // Stripes printed with the support body filament (erSupportMaterial).
    Polygons body;
    // Stripes printed with the interface filament (erSupportMaterialInterface).
    Polygons interface;
    // Islands too narrow to weave (less than two beams across). They keep today's behaviour.
    Polygons unsplit;
};

// The stripe coordinate of a point: its distance along the stripe normal of stripes running in
// direction `angle` (radians). It is measured from the object origin, so the stripes of every
// island and of every layer share one global grid. It is the same coordinate FillRectilinear uses to
// place its lines (x of the point rotated by -(angle + pi/2)), which is what lets a stripe of
// `beam_lines` line spacings hold exactly `beam_lines` lines.
inline double support_interlock_stripe_coordinate(const Point &pt, double angle)
{
    return - std::sin(angle) * double(pt.x()) + std::cos(angle) * double(pt.y());
}

// True when the point lies in a body stripe (even stripe index) of the grid of stripes running in
// direction `angle`, `beam_width` (scaled) wide each.
bool support_interlock_is_body(const Point &pt, double angle, coord_t beam_width);

// Mask of the body stripes covering `bbox` (inflated by one beam), as polygons in object coordinates.
Polygons support_interlock_body_mask(const BoundingBox &bbox, double angle, coord_t beam_width);

// Split `footprint` into body and interface stripes of `beam_width` (scaled) running in direction
// `angle`. Islands that cannot hold a disc two beams across are returned whole in `unsplit`.
// Slivers of a stripe narrower than `min_line_width` (scaled, usually one line spacing) cannot be
// filled; they are handed to the neighbouring stripe of the other kind so no area is lost.
SupportInterlockSplit support_interlock_split(const ExPolygons &footprint, double angle, coord_t beam_width, coord_t min_line_width);

} // namespace Slic3r
