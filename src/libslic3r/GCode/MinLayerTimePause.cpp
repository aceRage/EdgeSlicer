#include "MinLayerTimePause.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace Slic3r {
namespace MinLayerTimePause {

namespace {

using Box = BoundingBoxBase<Vec2d>;

// The four corners and the four edge midpoints of `box`.
std::vector<Vec2d> ring(const Box &box)
{
    const Vec2d c = box.center();
    return { box.min, Vec2d(box.max.x(), box.min.y()), box.max, Vec2d(box.min.x(), box.max.y()),
             Vec2d(c.x(), box.min.y()), Vec2d(box.max.x(), c.y()), Vec2d(c.x(), box.max.y()), Vec2d(box.min.x(), c.y()) };
}

bool on_bed(const Sites &sites, const Vec2d &p)
{
    if (!sites.bed.defined)
        return false;
    if (!sites.bed.inflated(-EDGE_MARGIN).contains(p))
        return false;
    return sites.bed_polygon.points.size() < 3 || sites.bed_polygon.contains(Point::new_scale(p.x(), p.y()));
}

bool clear_of(const std::vector<BoundingBoxf> &boxes, const Vec2d &p, double margin)
{
    return std::none_of(boxes.begin(), boxes.end(), [&p, margin](const BoundingBoxf &b) { return b.defined && b.inflated(margin).contains(p); });
}

// Usable for a park: on the bed, reachable, clear of the excluded areas, the objects and the tower.
bool free_spot(const Sites &sites, const Vec2d &p, const std::function<bool(const Vec2d &)> &reachable)
{
    return on_bed(sites, p) && (!reachable || reachable(p)) && clear_of(sites.exclusions, p, OBJECT_MARGIN * 0.5) &&
           clear_of(sites.objects, p, OBJECT_MARGIN - EPSILON) && (!sites.tower || !sites.tower->inflated(OBJECT_MARGIN - EPSILON).contains(p));
}

std::optional<Vec2d> nearest_free(const Sites &sites, const std::vector<Vec2d> &candidates, const Vec2d &from,
                                  const std::function<bool(const Vec2d &)> &reachable)
{
    std::optional<Vec2d> best;
    double               best_d2 = std::numeric_limits<double>::max();
    for (const Vec2d &p : candidates)
        if (free_spot(sites, p, reachable)) {
            const double d2 = (p - from).squaredNorm();
            if (d2 < best_d2 - EPSILON) {
                best_d2 = d2;
                best    = p;
            }
        }
    return best;
}

std::optional<Vec2d> away_from_print(const Sites &sites, const Vec2d &from, const std::function<bool(const Vec2d &)> &reachable)
{
    if (!sites.bed.defined)
        return std::nullopt;
    const Box inner = sites.bed.inflated(-EDGE_MARGIN);
    // The print's box: the objects and the tower.
    Box print;
    for (const BoundingBoxf &b : sites.objects)
        if (b.defined)
            print.merge(b);
    if (sites.tower)
        print.merge(*sites.tower);
    std::vector<Vec2d> candidates = ring(inner);
    if (print.defined) {
        // The bed edge points closest to the print, then the free strips just beyond each side of it.
        const Vec2d c = print.center();
        const Box   around = print.inflated(OBJECT_MARGIN);
        for (const Vec2d &p : { Vec2d(c.x(), inner.min.y()), Vec2d(c.x(), inner.max.y()), Vec2d(inner.min.x(), c.y()), Vec2d(inner.max.x(), c.y()),
                                Vec2d(c.x(), around.min.y()), Vec2d(c.x(), around.max.y()), Vec2d(around.min.x(), c.y()), Vec2d(around.max.x(), c.y()) })
            candidates.push_back(p);
        // Prefer the bed edge: only fall back to the strips right beside the print when no edge spot is free.
        std::vector<Vec2d> edge(candidates.begin(), candidates.begin() + 12);
        if (std::optional<Vec2d> p = nearest_free(sites, edge, from, reachable))
            return p;
        std::vector<Vec2d> beside(candidates.begin() + 12, candidates.end());
        return nearest_free(sites, beside, from, reachable);
    }
    return nearest_free(sites, candidates, from, reachable);
}

} // namespace

std::optional<Vec2d> park_point(const Sites &sites, MinLayerTimePausePark mode, const Vec2d &custom, const Vec2d &from,
                                const BoundingBoxf *current_object, const std::function<bool(const Vec2d &)> &reachable)
{
    if (mode == mltpCustom) {
        if (!sites.bed.defined)
            return custom;
        const Vec2d p = custom.cwiseMax(sites.bed.min).cwiseMin(sites.bed.max);
        if (!reachable || reachable(p))
            return p;
        // Out of the active extruder's reach: fall through to the automatic choice.
    }
    if (current_object != nullptr && current_object->defined) {
        // By object: finished objects may stand taller than the head, so stay beside the one being printed.
        // Printing by object keeps the objects much further apart than OBJECT_MARGIN.
        return nearest_free(sites, ring(current_object->inflated(OBJECT_MARGIN)), from, reachable);
    }
    if (mode == mltpPrimeTower && sites.tower && sites.tower->defined) {
        if (std::optional<Vec2d> p = nearest_free(sites, ring(sites.tower->inflated(OBJECT_MARGIN)), from, reachable))
            return p;
    }
    return away_from_print(sites, from, reachable);
}

std::string dwell_gcode(float target, float layer_time)
{
    const float missing = target - std::max(0.f, layer_time);
    if (!(missing >= MIN_PAUSE_S))
        return {};
    // G4 P (milliseconds): Marlin, RepRapFirmware, Klipper and Bambu all take it; Klipper has no G4 S.
    const long ms = std::lround(double(missing) * 1000.);
    char       buf[128];
    std::snprintf(buf, sizeof(buf), "%s %.2fs\nG4 P%ld\n", COMMENT_TAG, double(ms) / 1000., ms);
    return buf;
}

} // namespace MinLayerTimePause
} // namespace Slic3r
