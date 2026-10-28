// EdgeSlicer: support interface interlock ("woven band"), phase 1. See SupportInterlock.hpp.

#include "SupportInterlock.hpp"

#include "../BoundingBox.hpp"
#include "../ClipperUtils.hpp"

#include <cmath>
#include <limits>

namespace Slic3r {

static inline int64_t floor_div(double v, coord_t w)
{
    return int64_t(std::floor(v / double(w)));
}

bool support_interlock_is_body(const Point &pt, double angle, coord_t beam_width)
{
    assert(beam_width > 0);
    const int64_t idx = floor_div(support_interlock_stripe_coordinate(pt, angle), beam_width);
    return (idx & 1) == 0;
}

Polygons support_interlock_body_mask(const BoundingBox &bbox_in, double angle, coord_t beam_width)
{
    assert(beam_width > 0);
    Polygons out;
    if (! bbox_in.defined)
        return out;
    BoundingBox bbox = bbox_in;
    bbox.offset(beam_width);

    // Work in the frame where the stripes are vertical: u = along the stripe normal (the stripe
    // coordinate), v = along the stripe.
    const double nx = - std::sin(angle), ny = std::cos(angle); // stripe normal
    const double dx =   std::cos(angle), dy = std::sin(angle); // stripe direction
    double umin = std::numeric_limits<double>::max(), umax = - umin, vmin = umin, vmax = - umin;
    for (const Point &c : { bbox.min, bbox.max, Point(bbox.min.x(), bbox.max.y()), Point(bbox.max.x(), bbox.min.y()) }) {
        const double u = nx * double(c.x()) + ny * double(c.y());
        const double v = dx * double(c.x()) + dy * double(c.y());
        umin = std::min(umin, u); umax = std::max(umax, u);
        vmin = std::min(vmin, v); vmax = std::max(vmax, v);
    }
    const int64_t i0 = floor_div(umin, beam_width);
    const int64_t i1 = floor_div(umax, beam_width);
    auto to_xy = [nx, ny, dx, dy](double u, double v) {
        return Point(coord_t(std::round(u * nx + v * dx)), coord_t(std::round(u * ny + v * dy)));
    };
    for (int64_t i = i0; i <= i1; ++ i) {
        if ((i & 1) != 0)
            continue;
        const double u0 = double(i) * double(beam_width);
        const double u1 = u0 + double(beam_width);
        Polygon rect;
        rect.points = { to_xy(u0, vmin), to_xy(u1, vmin), to_xy(u1, vmax), to_xy(u0, vmax) };
        if (rect.is_clockwise())
            rect.reverse();
        out.emplace_back(std::move(rect));
    }
    return out;
}

SupportInterlockSplit support_interlock_split(const ExPolygons &footprint, double angle, coord_t beam_width, coord_t min_line_width)
{
    SupportInterlockSplit out;
    if (footprint.empty() || beam_width <= 0)
        return out;

    // An island has to hold a disc two beams across, otherwise one stripe of each kind is just a comb.
    ExPolygons wide;
    for (const ExPolygon &island : footprint) {
        if (opening_ex(ExPolygons{ island }, 0.99f * float(beam_width)).empty())
            polygons_append(out.unsplit, to_polygons(island));
        else
            wide.emplace_back(island);
    }
    if (wide.empty())
        return out;

    const Polygons mask = support_interlock_body_mask(get_extents(wide), angle, beam_width);
    ExPolygons body = intersection_ex(wide, mask);
    ExPolygons iface = diff_ex(wide, mask);

    // Slivers along the island outline that no fill line fits into. Hand them to the other kind so the
    // footprint stays covered. Only one direction is needed: a body sliver becomes interface.
    if (min_line_width > 0) {
        ExPolygons keep, slivers;
        for (ExPolygon &piece : body)
            (opening_ex(ExPolygons{ piece }, 0.49f * float(min_line_width)).empty() ? slivers : keep).emplace_back(std::move(piece));
        if (! slivers.empty()) {
            body  = std::move(keep);
            iface = union_ex(iface, to_polygons(slivers));
        }
    }
    out.body      = to_polygons(body);
    out.interface = to_polygons(iface);
    return out;
}

} // namespace Slic3r
