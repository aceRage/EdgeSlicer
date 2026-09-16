#ifndef slic3r_ScaleToVolume_hpp_
#define slic3r_ScaleToVolume_hpp_

#include "Point.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r {

// The pure arithmetic behind "Scale to build volume": the per-axis factors that make a selection's
// bounding box fill a gap-shrunk build volume, and the minimal translation that brings a box back
// inside that volume when auto-centering is off.
//
// It lives here rather than in src/slic3r/GUI/Selection.cpp so that the dialog's live warning,
// Selection::scale_to_fit_print_volume() and the unit tests all use one implementation, and so the
// tests need neither wx nor a GLCanvas3D.
namespace scale_to_volume {

enum class Mode : unsigned char { Uniform, NonUniform };

// What the dialog collects. Edge gap is the clearance kept from every XY bed edge (or from the bed
// circle's rim); top gap is the clearance kept below the maximum print height.
struct Settings
{
    Mode   mode        = Mode::Uniform;
    double edge_gap_mm = 0.;
    double top_gap_mm  = 0.;
    bool   auto_center = true;
};

// The usable extent of a (W,D,H) build volume once the gaps are taken off. X and Y lose the edge gap
// on BOTH sides, Z loses the top gap once - the object always sits on the bed, so there is no bottom
// gap to subtract.
inline Vec3d usable_size(const Vec3d &volume_size, double edge_gap, double top_gap)
{
    return Vec3d(volume_size.x() - 2. * edge_gap,
                 volume_size.y() - 2. * edge_gap,
                 volume_size.z() - top_gap);
}

// True when the gaps leave nothing to scale into on at least one axis. The caller shows a warning
// and disables OK rather than letting the scale silently no-op.
inline bool target_degenerate(const Vec3d &volume_size, double edge_gap, double top_gap)
{
    const Vec3d t = usable_size(volume_size, edge_gap, top_gap);
    return t.x() <= 0. || t.y() <= 0. || t.z() <= 0.;
}

// The per-axis scale factors that make `box_size` fill `usable_size(volume_size, ...)`.
//
// `box_size` is expected to already carry the caller's anti-rounding slack (today's 0.02 mm, i.e.
// 0.01 mm per side), so that a fitted object does not trip the out-of-volume test on a float
// rounding.
//
// Uniform mode returns min(fx,fy,fz) on all three axes - the largest factor that keeps every axis
// inside. Non-uniform returns the three factors unchanged, so the box fills the volume exactly.
//
// A zero extent on any axis (a perfectly flat selection) yields a zero factor on that axis, which
// the caller must treat as "do not scale" - the same guard today's fit() lambda applies.
inline Vec3d factors(const Vec3d &box_size, const Vec3d &volume_size, const Settings &s)
{
    const Vec3d target = usable_size(volume_size, s.edge_gap_mm, s.top_gap_mm);

    const double fx = (box_size.x() != 0.) ? target.x() / box_size.x() : 0.;
    const double fy = (box_size.y() != 0.) ? target.y() / box_size.y() : 0.;
    const double fz = (box_size.z() != 0.) ? target.z() / box_size.z() : 0.;

    if (fx <= 0. || fy <= 0. || fz <= 0.)
        return Vec3d::Zero();

    if (s.mode == Mode::Uniform) {
        const double f = std::min(fx, std::min(fy, fz));
        return Vec3d(f, f, f);
    }
    return Vec3d(fx, fy, fz);
}

// The minimal XY translation that brings a box of half-extent `box_half` centred at `box_center`
// inside [vol_min + edge_gap, vol_max - edge_gap] on each axis. Zero on an axis already inside.
//
// This is the auto-center-off case: the object keeps where the user put it, and only moves if it
// would otherwise hang over the (gap-shrunk) edge. When the box is wider than the usable extent on
// an axis - which only happens if the caller declined to scale - the min-side clamp wins, so the
// overhang is pushed to the max side rather than oscillating.
inline Vec2d clamp_translation(const Vec2d &box_min, const Vec2d &box_max,
                               const Vec2d &vol_min, const Vec2d &vol_max,
                               double edge_gap)
{
    Vec2d d = Vec2d::Zero();
    for (int i = 0; i < 2; ++i) {
        const double lo = vol_min[i] + edge_gap;
        const double hi = vol_max[i] - edge_gap;
        if (box_max[i] > hi)
            d[i] = hi - box_max[i];
        // The min side is applied second so that a box too big to fit ends up flush with the min
        // edge rather than the max one - the corner the user can see the overhang from.
        if (box_min[i] + d[i] < lo)
            d[i] = lo - box_min[i];
    }
    return d;
}

// The circular-bed counterpart of clamp_translation(): the minimal shift along the bed-centre to
// box-centre vector that brings a circle of radius `box_radius` centred at `box_center` inside a bed
// circle of radius `bed_radius` centred at `bed_center`, minus the edge gap. Zero when it already
// fits, and zero when the box is bigger than the shrunk bed (nothing to be gained by moving).
inline Vec2d clamp_translation_circle(const Vec2d &box_center, double box_radius,
                                      const Vec2d &bed_center, double bed_radius,
                                      double edge_gap)
{
    const double effective = bed_radius - edge_gap - box_radius;
    if (effective <= 0.)
        return Vec2d::Zero();
    const Vec2d  delta = box_center - bed_center;
    const double dist  = delta.norm();
    if (dist <= effective || dist == 0.)
        return Vec2d::Zero();
    return delta * (effective / dist - 1.);
}

// The uniform factor for a circular bed: the selection is described by the radius of its smallest
// enclosing circle and its height, and must fit inside (bed_radius - edge_gap) and
// (printable_height - top_gap). Returns 0 when either input extent is degenerate or a gap has eaten
// the target.
inline double circle_factor(double circle_radius, double max_z,
                            double bed_radius, double printable_height,
                            double edge_gap, double top_gap)
{
    if (circle_radius <= 0. || max_z <= 0.)
        return 0.;
    const double r = bed_radius - edge_gap;
    const double h = printable_height - top_gap;
    if (r <= 0. || h <= 0.)
        return 0.;
    return std::min(r / circle_radius, h / max_z);
}

// The XY target a non-uniform fit uses on a circular bed. A circle has no per-axis extent, so the
// largest axis-aligned box that is guaranteed to stay inside it is its inscribed square, of side
// r*sqrt(2). The edge gap shrinks the radius first, exactly as the uniform path does.
inline Vec3d inscribed_square_volume_size(double bed_radius, double printable_height, double edge_gap)
{
    const double r    = std::max(0., bed_radius - edge_gap);
    const double side = r * std::sqrt(2.);
    // edge_gap has already been taken off the radius here, so hand back a size that
    // usable_size(size, edge_gap, top_gap) will not shrink a second time on XY.
    return Vec3d(side + 2. * edge_gap, side + 2. * edge_gap, printable_height);
}

} // namespace scale_to_volume
} // namespace Slic3r

#endif // slic3r_ScaleToVolume_hpp_
