#ifndef libslic3r_FlexiJoint_hpp_
#define libslic3r_FlexiJoint_hpp_

// Flexi joint - a "print in place" articulated joint inserted at a planar cut.
//
// Everything here works in the CUT FRAME: the cut plane is z == 0, the joint axis is +Z,
// the MALE half of the object is the material below the plane (z < 0) and the FEMALE half
// is the material above it (z > 0).
//
// The male half carries a solid body that protrudes above the plane (an annular lip for the
// Double ring, a ball on a neck for the Ball & socket). The female half carries the matching
// cavity, which is exactly the male body dilated by the clearance C. Because dilation by a
// ball distributes over union, "cavity = male dilated by C" holds component by component, and
// for a solid of revolution the 3D dilation is the revolution of the 2D dilation of the
// meridional section - so we build both bodies by revolving a (r, z) profile and its Clipper
// offset. That is what makes the measured clearance exactly C on every face.
//
// The cut itself splits the object at z == 0 for the male half and at z == C for the female
// half, which is what opens the C-wide gap between the two flat mating faces.

#include <string>
#include <vector>

#include "libslic3r.h"
#include "Point.hpp"
#include "TriangleMesh.hpp"

namespace Slic3r {

enum class FlexiJointKind : int {
    // Primary: an annular dovetail lip nesting in an annular channel. THE standard flexi joint.
    DoubleRing = 0,
    // Secondary: a ball on a neck captured by a socket whose mouth is narrower than the ball.
    BallSocket = 1,
};

struct FlexiJointParams
{
    FlexiJointKind kind{ FlexiJointKind::DoubleRing };

    // Double ring: outer radius of the lip ring. Ball & socket: ball radius.
    // Auto default = 0.4 * the inscribed radius of the cut cross section.
    float outer_radius{ 4.0f };
    // Double ring only: radial thickness of the lip at its widest (the dovetail head).
    float ring_width{ 2.0f };
    // Double ring only: how far the lip protrudes above the cut plane.
    float ring_height{ 1.5f };
    // Both: the gap between every male face and the matching female face.
    float clearance{ 0.35f };
    // Double ring only: radius of the solid central hub. <= 0 disables the hub.
    float hub_radius{ 0.0f };
    // Both: extra headroom carved into the cavity ceiling so the segment can rock.
    float tilt{ 0.20f };
    // Double ring only: stem width as a fraction of ring_width (the dovetail undercut).
    float neck_ratio{ 0.45f };
    // Ball & socket only: half angle of the socket mouth, in degrees.
    float open_angle{ 40.0f };

    bool operator==(const FlexiJointParams &o) const
    {
        return kind == o.kind && is_approx(outer_radius, o.outer_radius) && is_approx(ring_width, o.ring_width) &&
               is_approx(ring_height, o.ring_height) && is_approx(clearance, o.clearance) &&
               is_approx(hub_radius, o.hub_radius) && is_approx(tilt, o.tilt) &&
               is_approx(neck_ratio, o.neck_ratio) && is_approx(open_angle, o.open_angle);
    }
    bool operator!=(const FlexiJointParams &o) const { return !(*this == o); }

    template<class Archive> void serialize(Archive &ar)
    {
        ar(kind, outer_radius, ring_width, ring_height, clearance, hub_radius, tilt, neck_ratio, open_angle);
    }
};

// ---------------------------------------------------------------------------------- defaults

// Default hub radius: outer_radius - ring_width - clearance - ring_width, clamped at 0
// (0 means "no hub", which is what a small cut cross section ends up with).
float flexi_default_hub_radius(const FlexiJointParams &p);

// Smallest clearance that still survives a nozzle of this diameter. The research
// (BambuStudio #182 and the maker guidance) puts the safe floor at 0.5-0.75x the nozzle;
// we use 0.75x so the 0.35 mm default stays reachable on the standard 0.4 mm nozzle.
float flexi_clearance_floor(double nozzle_diameter);

// Auto-size the joint to a cut cross section whose inscribed radius is `inscribed_radius`.
// Sets outer_radius = 0.4 * inscribed_radius (clamped to something printable) and recomputes
// the hub. Leaves every explicitly-set value alone when `size_auto` is false.
FlexiJointParams flexi_auto_size(FlexiJointParams p, double inscribed_radius);

// ------------------------------------------------------------------------------- key measures

// How far the male body protrudes above the cut plane.
float flexi_protrusion_height(const FlexiJointParams &p);
// Largest radius touched by the joint including the clearance - used for "fits in the contour".
float flexi_outer_extent(const FlexiJointParams &p);
// Half width of the female mouth at the female face (ring) / socket mouth radius (ball).
float flexi_mouth_half_width(const FlexiJointParams &p);
// Half width of the widest part of the male lip (ring) / the ball radius (ball).
float flexi_lip_half_width(const FlexiJointParams &p);
// True when the joint is captive, i.e. the mouth is narrower than the lip/ball.
inline bool flexi_is_captive(const FlexiJointParams &p) { return flexi_mouth_half_width(p) < flexi_lip_half_width(p); }

// ---------------------------------------------------------------------------------- geometry
// All in the cut frame (axis +Z, cut plane z == 0). Each call returns one entry per
// disconnected component (the ring and the optional hub are separate solids).

std::vector<indexed_triangle_set> flexi_male_bodies(const FlexiJointParams &p);
std::vector<indexed_triangle_set> flexi_female_cavities(const FlexiJointParams &p);

// The meridional (r, z) profiles the two above revolve; exposed for tests/preview.
std::vector<std::vector<Vec2d>> flexi_male_profiles(const FlexiJointParams &p);
std::vector<std::vector<Vec2d>> flexi_female_profiles(const FlexiJointParams &p);

// A single preview body: the male lip plus the female cavity shell, for the gizmo's
// connector-style preview on the cut plane.
indexed_triangle_set flexi_preview_body(const FlexiJointParams &p);

// ------------------------------------------------------------------------------------ guards

// The slicer's gap closing radius welds print-in-place clearances shut (BambuStudio #182).
// The clearance is at risk whenever slice_gap_closing_radius >= clearance / 2.
inline bool flexi_gap_closing_conflict(const FlexiJointParams &p, double slice_gap_closing_radius)
{
    return slice_gap_closing_radius >= 0.5 * double(p.clearance);
}
// Largest gap closing radius that is still safe for this joint.
inline double flexi_max_safe_gap_closing_radius(const FlexiJointParams &p) { return 0.5 * double(p.clearance); }

// Returns an empty string when the parameters make a printable, captive joint;
// otherwise a human readable reason.
std::string flexi_validate(const FlexiJointParams &p);

} // namespace Slic3r

#endif // libslic3r_FlexiJoint_hpp_
