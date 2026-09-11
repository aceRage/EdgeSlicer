#include "FillBedPack.hpp"

#include "Polygon.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r {
namespace fill_bed {

int estimate_count(double bbox_w, double bbox_h, double gap, double free_area)
{
    if (bbox_w <= 0. || bbox_h <= 0. || free_area <= 0.)
        return 0;

    const double g    = std::max(0., gap);
    const double cell = (bbox_w + g) * (bbox_h + g);
    if (cell <= 0.)
        return 0;

    const double n = std::floor(free_area / cell);
    if (n <= 0.)
        return 0;
    // Clamp before the cast: a tiny part on a huge bed can overflow int. This is a guard against
    // undefined behaviour, not the fill's cap - that is COUNT_CAP, applied by
    // estimate_count_with_overshoot(), so the raw tiling number stays honest for the dialog label.
    return int(std::min(n, 1.e9));
}

int estimate_count_with_overshoot(double bbox_w, double bbox_h, double gap, double free_area)
{
    const int base = estimate_count(bbox_w, bbox_h, gap, free_area);
    if (base <= 0)
        return 0;
    const double over = std::ceil(double(base) * COUNT_OVERSHOOT);
    return int(std::min(over, double(COUNT_CAP)));
}

Points shrink_bed_per_side(const Points &bedpts,
                           double        edge_margin,
                           double        front_margin,
                           bool          front_margin_enabled,
                           double        already_shrunk_x,
                           double        already_shrunk_y)
{
    Points out = bedpts;
    if (out.empty())
        return out;

    const double edge  = std::max(0., edge_margin);
    // Never closer to the front than the general margin: the override only ever widens it.
    const double front = front_margin_enabled ? std::max(edge, std::max(0., front_margin)) : edge;

    // What arrange's own shrink already took off each side; only the remainder is ours to move.
    const double dx       = std::max(0., edge - already_shrunk_x);
    const double dy_back  = std::max(0., edge - already_shrunk_y);
    const double dy_front = std::max(0., front - already_shrunk_y);

    if (dx <= 0. && dy_back <= 0. && dy_front <= 0.)
        return out;

    const Point center = Polygon(out).bounding_box().center();

    for (Point &pt : out) {
        // X: move every point towards the centre, exactly as get_shrink_bedpts does.
        if (dx > 0.) {
            const coord_t sgn = (center.x() - pt.x()) >= 0 ? 1 : -1;
            pt.x() += coord_t(scaled(dx)) * sgn;
        }
        // Y: the same, except that points on the front half (y below the centre) use the
        // - possibly larger - front margin.
        const bool   is_front = pt.y() < center.y();
        const double dy       = is_front ? dy_front : dy_back;
        if (dy > 0.) {
            const coord_t sgn = (center.y() - pt.y()) >= 0 ? 1 : -1;
            pt.y() += coord_t(scaled(dy)) * sgn;
        }
    }

    return out;
}

} // namespace fill_bed
} // namespace Slic3r
