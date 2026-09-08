#ifndef libslic3r_FlexiJoint_hpp_
#define libslic3r_FlexiJoint_hpp_

// Flexi joint - a "print in place" articulated joint inserted at a planar cut.
//
// Everything here works in the CUT FRAME: the cut plane is z == 0, the joint axis is +Z,
// the MALE half of the object is the material below the plane (z < 0) and the FEMALE half
// is the material above it (z > 0).
//
// THE GAP. The two segments' cut faces are set back from the cut plane by gap/2 each, so the
// lower (male) segment ends at z == -gap/2 and the upper (female) segment starts at
// z == +gap/2 and the faces are `gap` apart. The joint bodies bridge that gap. A joint whose
// faces are only a clearance apart barely moves; the gap is what makes the segment actually
// flexible, which is why it is a parameter of its own with a floor at the clearance.
//
// DOUBLE RING / BALL & SOCKET. The male half carries a solid body that protrudes above the
// plane (an annular lip, or a ball on a neck). The female half carries the matching cavity,
// which is exactly the male body dilated by the clearance C. Because dilation by a ball
// distributes over union, "cavity = male dilated by C" holds component by component, and for a
// solid of revolution the 3D dilation is the revolution of the 2D dilation of the meridional
// section - so we build both bodies by revolving a (r, z) profile and its Clipper offset. That
// is what makes the measured clearance exactly C on every face.
//
// CHAIN LINK. Two interlocking closed loops, each a stadium centreline swept with a tube of
// radius `wire`, so the joint hinges AND swivels the way two links of a chain do.
//   * The VERTICAL loop belongs to the LOWER segment: it stands in the x-z plane (long axis
//     +Z), its far (bottom) end embedded `stem` deep in the lower body, its upper half free.
//     Printed bottom up it grows straight out of the lower segment and its top span is a short
//     self-supporting bridge.
//   * The HORIZONTAL loop belongs to the UPPER segment: it lies just above the upper face
//     (long axis +X), tilted up by `tilt_angle` about +Y so its far (+x) end rises into the
//     upper body while its near half stays free and threads the vertical loop. Lying nearly
//     flat, each of its layers is a closed ring resting on the one below - it prints on the
//     layer, no bridging.
// The two are LINKED (each threads the other's hole), so the joint is non-separable without
// either loop touching the other. Two sizing constraints make that possible: a loop's hole is
// a stadium of length L-2t and width W-2t, and the other loop's wire (radius t) has to pass
// through it with C to spare on both sides, i.e. L >= 4t + 2C and W >= 4t + 2C.
// Instead of a Clipper dilation, the chain link's cavity is the other loop's tube swept with
// radius t + C - a uniform offset of a tube IS a fatter tube, so the clearance is exact here
// too.
//
// The cut splits the object at z == -gap/2 for the male half and at z == +gap/2 for the
// female half (for the revolved kinds the female face additionally clears the male body by C).

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
    // Two interlocking closed loops, one lying in the cut plane and one standing along the
    // cut normal, hinging and swivelling like two links of a chain.
    ChainLink = 2,
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
    // ALL kinds: the thickness of the cut, i.e. how far apart the two segments' flat cut
    // faces end up. Each face is set back from the cut plane by gap/2. Floor = clearance.
    // A joint whose faces nearly touch does not bend in practice; this is what makes the
    // segment flexible. 0 means "use the kind's default" (see flexi_default_gap()).
    float gap{ 0.f };
    // Double ring only: radius of the solid central hub. <= 0 disables the hub.
    float hub_radius{ 0.0f };
    // Both: extra headroom carved into the cavity ceiling so the segment can rock.
    float tilt{ 0.20f };
    // Double ring only: stem width as a fraction of ring_width (the dovetail undercut).
    float neck_ratio{ 0.45f };
    // Ball & socket only: half angle of the socket mouth, in degrees.
    float open_angle{ 40.0f };

    // ------------------------------------------------------------------- Chain link only
    // Overall length of each loop's stadium CENTRELINE (along the loop's long axis).
    float link_length{ 7.0f };
    // Overall width of each loop's stadium centreline (across the loop).
    float link_width{ 5.5f };
    // Tube radius of the wire the centreline is swept with.
    float wire{ 1.0f };
    // How far the horizontal loop is tilted up out of the cut plane, in degrees, so its far
    // end rises into the upper segment while its near half stays free.
    float tilt_angle{ 7.0f };
    // How deep each loop's far end is embedded in its own segment.
    float stem{ 1.5f };

    bool operator==(const FlexiJointParams &o) const
    {
        return kind == o.kind && is_approx(outer_radius, o.outer_radius) && is_approx(ring_width, o.ring_width) &&
               is_approx(ring_height, o.ring_height) && is_approx(clearance, o.clearance) &&
               is_approx(gap, o.gap) && is_approx(hub_radius, o.hub_radius) && is_approx(tilt, o.tilt) &&
               is_approx(neck_ratio, o.neck_ratio) && is_approx(open_angle, o.open_angle) &&
               is_approx(link_length, o.link_length) && is_approx(link_width, o.link_width) &&
               is_approx(wire, o.wire) && is_approx(tilt_angle, o.tilt_angle) && is_approx(stem, o.stem);
    }
    bool operator!=(const FlexiJointParams &o) const { return !(*this == o); }

    template<class Archive> void serialize(Archive &ar)
    {
        ar(kind, outer_radius, ring_width, ring_height, clearance, hub_radius, tilt, neck_ratio, open_angle,
           gap, link_length, link_width, wire, tilt_angle, stem);
    }
};

// ---------------------------------------------------------------------------------- defaults

// Default hub radius: outer_radius - ring_width - clearance - ring_width, clamped at 0
// (0 means "no hub", which is what a small cut cross section ends up with).
float flexi_default_hub_radius(const FlexiJointParams &p);

// Default cut thickness for a kind, in mm: 1.5 for Chain link, 0.6 for the revolved kinds.
// The chain link's loops have to swing THROUGH each other, so the two segments need real
// room between them; the ring and the ball only rock and rotate in place, so a gap just wide
// enough to stop the faces rubbing (about 3 x a 0.2 mm layer) is enough there.
float flexi_default_gap(FlexiJointKind kind);
// The gap actually used: p.gap when set, else the kind default; never below the clearance.
float flexi_effective_gap(const FlexiJointParams &p);

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
// Smallest link_length / link_width that lets the other loop's wire through with the
// clearance to spare on both sides: 4 * wire + 2 * clearance.
float flexi_min_link_size(const FlexiJointParams &p);
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

// For the revolved kinds these are the male protrusion and the female cavity. For the chain
// link, `male` is the vertical loop (which belongs to the LOWER segment) and `female` is the
// horizontal loop (which belongs to the UPPER segment); each segment is then relieved by the
// OTHER loop inflated by the clearance - see flexi_lower_reliefs() / flexi_upper_reliefs().
std::vector<indexed_triangle_set> flexi_male_bodies(const FlexiJointParams &p);
std::vector<indexed_triangle_set> flexi_female_cavities(const FlexiJointParams &p);

// True when the kind builds a body on BOTH segments (chain link) rather than a protrusion on
// the male one and a cavity on the female one.
inline bool flexi_is_two_sided(const FlexiJointParams &p) { return p.kind == FlexiJointKind::ChainLink; }

// The bodies to UNION into the lower / upper segment.
std::vector<indexed_triangle_set> flexi_lower_bodies(const FlexiJointParams &p);
std::vector<indexed_triangle_set> flexi_upper_bodies(const FlexiJointParams &p);
// The bodies to SUBTRACT from the lower / upper segment (the other side's body, inflated by
// the clearance). For the revolved kinds the lower list is empty and the upper one is the
// dilated male body.
std::vector<indexed_triangle_set> flexi_lower_reliefs(const FlexiJointParams &p);
std::vector<indexed_triangle_set> flexi_upper_reliefs(const FlexiJointParams &p);

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
