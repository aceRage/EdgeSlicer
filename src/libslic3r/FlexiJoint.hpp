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
// BOTH ring planes contain the cut normal n, and the two planes are PERPENDICULAR to each
// other - that is what makes it a chain link rather than a ring sitting on a ring. Writing d
// for a unit reference direction in the cut plane (d = the plane's +X turned by `rotation`):
//   * The LOWER ring belongs to the LOWER segment: it stands in the plane spanned by (n, d),
//     long axis along n, its far (bottom) end embedded `stem` deep in the lower body and its
//     upper half free, reaching up through the cut plane.
//   * The UPPER ring belongs to the UPPER segment: it stands in the plane spanned by
//     (n, n x d) - perpendicular to the lower ring's plane - long axis along n, its far (top)
//     end embedded `stem` deep in the upper body and its lower half free, reaching down
//     through the cut plane. `tilt_angle` tips it slightly about n x d so its free end leans
//     clear of the lower ring rather than sitting concentric with it.
// Their free halves overlap around the cut plane, and because the planes are perpendicular
// each ring threads the other's hole there. Printed bottom up, each ring grows out of its own
// segment and its top arc is a short self-supporting bridge - the same print behaviour the
// standing ring already had, now on both.
// The two are LINKED (each threads the other's hole), so the joint is non-separable without
// either loop touching the other. Two sizing constraints make that possible: a loop's hole is
// a stadium of length L-2t and width W-2t, and the other loop's wire (radius t) has to pass
// through it with C to spare on both sides, i.e. L >= 4t + 2C and W >= 4t + 2C.
// Instead of a Clipper dilation, the chain link's cavity is the other loop's tube swept with
// radius t + C - a uniform offset of a tube IS a fatter tube, so the clearance is exact here
// too.
//
// HINGE. A print-in-place pin hinge: N alternating knuckles strung along an in-plane axis,
// with one continuous pin running through all of them.
//   * The in-plane axis is +X in the cut frame; the connector's own Rotation (z_angle) spins
//     the whole joint about the cut normal, so nothing here has to know about it.
//   * Knuckle i occupies a slot of length K along +X; the run of N slots is centred on the
//     joint origin. Even-indexed knuckles belong to the LOWER segment, odd-indexed ones to the
//     UPPER segment (swapped by `hinge_fold_upper`). Each cylinder is generated gap/2 short at
//     each end of its slot, so every pair of facing knuckle end faces is exactly `gap` apart -
//     pure placement arithmetic, no Clipper pass, because a cylinder shortened uniformly is
//     still a cylinder.
//   * The PIN is one continuous cylinder of radius R_p spanning the whole run, integral to the
//     half that owns the even knuckles. The other half's knuckles are bored to R_p + clearance,
//     so the pin turns inside them with exactly the clearance all round.
//   * The barrel sits at the EDGE of the cut face, offset along -e (e = n x d = +Y in the cut
//     frame) by `hinge_edge_offset`, so that when the two halves fold shut there is no material
//     behind the hinge line for them to collide with. The gizmo fills that offset in from the
//     cut contour; the geometry here just honours it.
// Phase 1 is horizontal-pin-axis only in the sense that a non-horizontal axis is WARNED about
// (not blocked) in the gizmo: a vertical pin axis needs a teardrop bore, which is phase 2.
//
// THREAD. A helical screw thread about the cut normal, so the two halves screw together.
//   * The LID half carries the MALE thread: a core cylinder of the minor radius with N
//     helical strands (an N-start thread) standing proud of it out to the major radius. The
//     other half carries a BORE of the major radius plus the clearance, with the SAME helix
//     re-swept at the clearance-offset radius cut into its wall as the groove.
//   * The profile is a fixed trapezoid - ACME-style, 30 degree flanks, crest and root
//     flattened - because a 60 degree V comes to a knife edge that either will not print (the
//     crest) or notches the root.
//   * The clearance is NOT a Clipper dilation of the male profile here. A thread is not a
//     solid of revolution - its meridional section changes with the axial position - so the
//     "offset the 2D profile then revolve" shortcut the double ring relies on does not apply.
//     Instead the female groove is its own helical sweep: the same profile at radius + C with
//     the same pitch, starts, turns and phase, so male and female facets stay parallel and the
//     measured clearance is exactly C - the same discipline reached by a different route.
//   * `rotation` is the THREAD START ANGLE. It turns every strand about the cut normal at
//     once, which is what decides where the lid ends up pointing when it is done up.
//   * The lead-in taper fades each strand's radial depth to zero over the first and last
//     fraction of a turn, so the halves catch at any relative angle - and it is also what
//     closes the swept ribbon's ends, so no cap mesh is needed.
//
// BAYONET. A quarter-turn lock, and the more print-robust route to the same twist-lock UX:
//   * The LID half carries a plug cylinder with N radial LUGS (blocks) standing out of it. The
//     other half carries a bore, and cut into the bore's wall an L-shaped SLOT per lug: an
//     AXIAL entry channel down from the mouth, then a CIRCUMFERENTIAL track turning by the
//     lock angle, ending in a small DETENT bump the lug has to ride over.
//   * Insert straight down the entry channels, turn by the lock angle, and the lug sits under
//     the track's roof: pulling axially now hits solid material, which is the whole lock.
//   * Every slot face is the lug's own swept shape grown by the clearance, so the fit is C all
//     round the same way the hinge's bore is exactly pin + C.
//   * Nothing here is a helix and nothing overhangs more than a lug's own width, so both
//     halves print with the axis vertical as plain layer-wise geometry - which is why it is
//     the more robust choice even though a thread is what "screw top" literally means.
//
// The cut splits the object at z == -gap/2 for the male half and at z == +gap/2 for the
// female half (for the revolved kinds the female face additionally clears the male body by C).

#include <cmath>
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
    // Two interlocking closed loops, both standing along the cut normal in planes
    // perpendicular to each other, hinging and swivelling like two links of a chain.
    ChainLink = 2,
    // A print-in-place pin hinge: N alternating knuckles along an in-plane axis with one
    // continuous pin through them, placed at the edge of the cut face so the halves fold shut.
    Hinge = 3,
    // A helical screw thread about the cut normal: male thread on the lid half, bore plus
    // matching groove on the other, so the two halves screw together.
    Thread = 4,
    // A quarter-turn bayonet lock: lugs on the lid half's plug, L-shaped slots in the other
    // half's bore. Insert axially, twist, captured.
    Bayonet = 5,
};

// The kinds that are TWIST LOCKS: a lid screwing or twisting onto a body about the cut normal.
// They share the lid-side convention, the "print with the axis vertical" warning and the
// bore-wall footprint. Neither is a Flexi joint in the articulated sense - they do not flex,
// they fasten - but they ride the same cut pipeline, so they live in the same family.
inline bool flexi_kind_is_twist(FlexiJointKind k)
{
    return k == FlexiJointKind::Thread || k == FlexiJointKind::Bayonet;
}

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
    // How far the UPPER ring is tipped about n x d, in degrees, so its free (lower) end leans
    // clear of the lower ring instead of sitting concentric with it. Stays within the ring's
    // own plane family; 0 is legal and puts both rings on the same axis.
    float tilt_angle{ 7.0f };
    // How deep each loop's far end is embedded in its own segment.
    float stem{ 1.5f };

    // ----------------------------------------------------------------------- Hinge only
    // Number of knuckles along the hinge axis. Odd counts are self-centring (the middle
    // knuckle sits on the joint origin), which is why the default is 3. 1..9.
    int hinge_knuckles{ 3 };
    // Diameter of the continuous pin.
    float hinge_pin_dia{ 2.0f };
    // Outer diameter of each knuckle barrel. Has to clear the pin, its bore clearance and a
    // printable wall: see flexi_validate().
    float hinge_barrel_dia{ 4.0f };
    // Overall length of the knuckle run along the hinge axis, end face to end face.
    float hinge_length{ 12.0f };
    // How far the barrel centreline is pushed out from the cut face's edge along -e.
    // The gizmo computes this from the cut contour; 0 leaves the barrel on the joint origin.
    float hinge_edge_offset{ 0.0f };
    // Which half carries the pin (and therefore the even-indexed knuckles). false = the
    // lower half, which is the default; true swaps the two parities.
    bool hinge_fold_upper{ false };

    // ------------------------------------------------------------- Thread and Bayonet only
    // Major diameter of the thread / outer diameter of the bayonet's lug circle. Auto default
    // = 1.4 x the joint's outer_radius, i.e. 0.7 x the cut section's inscribed radius.
    float thread_major_dia{ 12.0f };
    // Which half carries the MALE half of the fastener (the thread, or the lugged plug). The
    // mental model is "the cap screws onto the body": the cap is the UPPER half by default and
    // it is the one that comes off, so the male thread lives up there.
    // true = the upper half is the lid (default); false moves the male side to the lower half.
    bool thread_lid_upper{ true };

    // ------------------------------------------------------------------------- Thread only
    // Axial rise per full turn. Note that it is the CREST SPACING - pitch / starts - that has
    // to be coarse enough to print, not the pitch: an N-start thread puts N crests in every
    // pitch. The default is 6 mm because the default start count is 2, which makes the crest
    // spacing 3 mm - the research's recommended profile, done up in half a turn.
    // See thread_crest_spacing() and flexi_validate().
    float thread_pitch{ 6.0f };
    // Intertwined strands. An N-start thread seats in 360/N degrees of turn, which is what
    // makes a 2-start thread a half-turn lid rather than a fastener. 1..4.
    int   thread_starts{ 2 };
    // Total revolutions of engagement. 1.25 is the commercial jar / pill-bottle range.
    float thread_turns{ 1.25f };
    // Right-hand (clockwise to tighten, looking down the axis at the lid) unless this is set.
    bool  thread_left_hand{ false };
    // How much of a turn, at EACH end of each strand, the thread's radial depth fades to zero
    // over. This is the lead-in chamfer: it lets the two halves catch at any relative angle,
    // and it is also what closes the swept ribbon's ends. 0 caps the ends flat instead.
    float thread_lead_turns{ 0.5f };

    // ------------------------------------------------------------------------ Bayonet only
    // Number of lugs around the plug, evenly spaced. 2..4.
    int   bayonet_lugs{ 3 };
    // How far the lug stands out of the plug wall, radially.
    float bayonet_lug_height{ 1.6f };
    // The lug's axial thickness (the height of the block).
    float bayonet_lug_thickness{ 2.4f };
    // Angular width of one lug, in degrees. It has to leave room for the track to turn through
    // without running into the next lug's entry channel: see flexi_validate().
    float bayonet_lug_arc{ 30.0f };
    // How far the lug turns along the circumferential track before it stops, in degrees.
    // 60-90 is the usual quarter-turn range.
    float bayonet_lock_angle{ 75.0f };
    // Axial depth of the entry channel from the bore mouth down to the track. The plug's
    // insertion depth follows it.
    float bayonet_entry_depth{ 4.0f };
    // Height of the detent bump at the end of the track, radially into the lug's path. The lug
    // rides over it and is held past it. 0 disables the detent.
    float bayonet_detent{ 0.35f };

    // ------------------------------------------ kinds that have a direction in the cut plane
    // Rotation of the joint about the cut normal n, in degrees. The reference direction d0 is
    // the cut plane's own +X axis, and the joint is built along d = rotate(d0, n, rotation).
    // Only kinds that HAVE a direction in the cut plane honour it: the chain link does - it
    // turns both rings together, so their planes stay perpendicular to each other and only
    // the pair's orientation within the plane changes - and so does the HINGE, for which d is
    // the pin axis itself and the whole point of the control. Double ring and ball & socket
    // are solids of revolution about n, so rotating them changes nothing and they IGNORE this
    // value. 0-180 is the whole range: a chain link at 180 degrees presents the same two ring
    // planes as at 0, and a hinge presents the same pin axis. Default 0 - which is also what
    // an older 3MF without the field loads as.
    float rotation{ 0.f };

    bool operator==(const FlexiJointParams &o) const
    {
        return kind == o.kind && is_approx(outer_radius, o.outer_radius) && is_approx(ring_width, o.ring_width) &&
               is_approx(ring_height, o.ring_height) && is_approx(clearance, o.clearance) &&
               is_approx(gap, o.gap) && is_approx(hub_radius, o.hub_radius) && is_approx(tilt, o.tilt) &&
               is_approx(neck_ratio, o.neck_ratio) && is_approx(open_angle, o.open_angle) &&
               is_approx(link_length, o.link_length) && is_approx(link_width, o.link_width) &&
               is_approx(wire, o.wire) && is_approx(tilt_angle, o.tilt_angle) && is_approx(stem, o.stem) &&
               hinge_knuckles == o.hinge_knuckles && is_approx(hinge_pin_dia, o.hinge_pin_dia) &&
               is_approx(hinge_barrel_dia, o.hinge_barrel_dia) && is_approx(hinge_length, o.hinge_length) &&
               is_approx(hinge_edge_offset, o.hinge_edge_offset) && hinge_fold_upper == o.hinge_fold_upper &&
               is_approx(thread_major_dia, o.thread_major_dia) && thread_lid_upper == o.thread_lid_upper &&
               is_approx(thread_pitch, o.thread_pitch) && thread_starts == o.thread_starts &&
               is_approx(thread_turns, o.thread_turns) && thread_left_hand == o.thread_left_hand &&
               is_approx(thread_lead_turns, o.thread_lead_turns) && bayonet_lugs == o.bayonet_lugs &&
               is_approx(bayonet_lug_height, o.bayonet_lug_height) &&
               is_approx(bayonet_lug_thickness, o.bayonet_lug_thickness) &&
               is_approx(bayonet_lug_arc, o.bayonet_lug_arc) &&
               is_approx(bayonet_lock_angle, o.bayonet_lock_angle) &&
               is_approx(bayonet_entry_depth, o.bayonet_entry_depth) &&
               is_approx(bayonet_detent, o.bayonet_detent) &&
               is_approx(rotation, o.rotation);
    }
    bool operator!=(const FlexiJointParams &o) const { return !(*this == o); }

    template<class Archive> void serialize(Archive &ar)
    {
        ar(kind, outer_radius, ring_width, ring_height, clearance, hub_radius, tilt, neck_ratio, open_angle,
           gap, link_length, link_width, wire, tilt_angle, stem, rotation,
           hinge_knuckles, hinge_pin_dia, hinge_barrel_dia, hinge_length, hinge_edge_offset, hinge_fold_upper,
           thread_major_dia, thread_lid_upper, thread_pitch, thread_starts, thread_turns,
           thread_left_hand, thread_lead_turns, bayonet_lugs, bayonet_lug_height,
           bayonet_lug_thickness, bayonet_lug_arc, bayonet_lock_angle, bayonet_entry_depth,
           bayonet_detent);
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
// link, `male` is the LOWER ring (in the plane spanned by n and d) and `female` is the UPPER
// ring (in the plane spanned by n and n x d); each segment is then relieved by the
// OTHER loop inflated by the clearance - see flexi_lower_reliefs() / flexi_upper_reliefs().
std::vector<indexed_triangle_set> flexi_male_bodies(const FlexiJointParams &p);
std::vector<indexed_triangle_set> flexi_female_cavities(const FlexiJointParams &p);

// True when the kind builds a body on BOTH segments (chain link) rather than a protrusion on
// the male one and a cavity on the female one.
inline bool flexi_is_two_sided(const FlexiJointParams &p)
{
    return p.kind == FlexiJointKind::ChainLink || p.kind == FlexiJointKind::Hinge ||
           flexi_kind_is_twist(p.kind);
}

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

// Chain link only: the two rings' CENTRELINES in the cut frame, after `rotation`. Empty for
// the other kinds. Exposed so the gizmo and the tests can reason about the ring planes
// without re-deriving the frame.
std::vector<Vec3d> flexi_chain_lower_centreline(const FlexiJointParams &p);
std::vector<Vec3d> flexi_chain_upper_centreline(const FlexiJointParams &p);

// A single preview body: the male lip plus the female cavity shell, for the gizmo's
// connector-style preview on the cut plane.
indexed_triangle_set flexi_preview_body(const FlexiJointParams &p);

// ------------------------------------------------------------------------------------- hinge

// Number of knuckles actually used: p.hinge_knuckles clamped into 1..9.
int   hinge_knuckle_count(const FlexiJointParams &p);
// Length of one knuckle SLOT along the hinge axis: hinge_length / N. Each cylinder is built
// gap/2 short at each end of its slot, so the solid part is slot - gap long.
double hinge_slot_length(const FlexiJointParams &p);
// The solid length of one knuckle cylinder: slot - gap, floored so it never inverts.
double hinge_knuckle_length(const FlexiJointParams &p);
// Centre of knuckle slot i along the hinge axis (+X in the cut frame), the run centred on 0.
double hinge_slot_centre(const FlexiJointParams &p, int i);
// True when knuckle i belongs to the LOWER segment (and therefore carries the pin).
bool  hinge_knuckle_is_lower(const FlexiJointParams &p, int i);
// Radius of the bore drilled through the knuckles that do NOT carry the pin.
inline double hinge_bore_radius(const FlexiJointParams &p)
{
    return 0.5 * double(p.hinge_pin_dia) + double(p.clearance);
}
// The barrel centreline's offset from the joint origin along -e (e = n x d = +Y here), i.e.
// how far out towards the cut face's edge the hinge sits.
inline double hinge_axis_y(const FlexiJointParams &p) { return -double(p.hinge_edge_offset); }

// The hinge's real footprint on the cut plane: the four corners of the knuckle run's oriented
// rectangle - hinge_length along the axis by the barrel diameter across it, centred on the
// barrel centreline - each grown by `pad`. In the JOINT's own frame (axis = +X); the caller
// rotates by the connector's z_angle and translates to the connector position. This is what
// the gizmo's "fits inside the cut contour" test has to sample instead of a circle: a long
// thin hinge fits contours that its circumscribing circle does not.
std::vector<Vec2d> hinge_footprint_corners(const FlexiJointParams &p, double pad = 0.);

// The same idea for the chain link: the two loops sweep a SLOT through the cut plane, not a
// disc. Returns the corners of that slot's bounding rectangle in the joint frame, padded.
std::vector<Vec2d> chain_footprint_corners(const FlexiJointParams &p, double pad = 0.);

// The footprint the gizmo should test for ANY flexi kind: the real rectangle for the hinge
// and the chain link, and for the revolved kinds a 60-gon on the disc of radius
// flexi_outer_extent() - exactly the circle the gizmo used to build by hand. Always at least
// three points, always in the joint frame.
std::vector<Vec2d> flexi_footprint_corners(const FlexiJointParams &p, double pad = 0.);

// PRINTABILITY OF THE PIN AXIS. A pin whose axis is HORIZONTAL (parallel to the bed) is the
// easy case: nothing about the bore has to bridge more than the pin's own width, and the
// ordinary overhang/bridging settings carry it. A pin whose axis is VERTICAL is the hard one -
// every layer of the bore is a plain ring, but the SEAM between the two halves then runs
// across the printed layers and the bore's roof is a full unsupported circle. `axis_world` is
// the hinge axis d in WORLD coordinates (the +X of the joint frame, spun by the connector's
// rotation and the cut plane's own orientation). Returns true when the axis is more than about
// 6 degrees off horizontal, which is what the gizmo warns - never blocks - on.
inline bool hinge_axis_needs_care(const Vec3d &axis_world)
{
    const double n = axis_world.norm();
    return n > EPSILON && std::abs(axis_world.z()) / n > 0.1;
}

// ------------------------------------------------------------------------ thread / bayonet

// Major radius of the thread (or of the bayonet's lug circle): half thread_major_dia.
inline double thread_major_radius(const FlexiJointParams &p) { return 0.5 * double(p.thread_major_dia); }
// Axial distance from one thread crest to the next: pitch / starts, because strand s sits
// pitch x s/starts above strand 0 and an N-start thread puts N crests in every pitch. Every
// other dimension of the thread has to fit inside this.
double thread_crest_spacing(const FlexiJointParams &p);
// Radial depth of the thread profile: the ACME convention, 0.35 x pitch, capped so that the
// profile AND the clearance the female groove adds to it still fit between one crest and the
// next. A groove whose coils merge is a plain annular cavity that holds nothing, so this cap is
// what makes the thread a thread; flexi_validate() refuses parameters that need it to bite hard.
double thread_depth(const FlexiJointParams &p);
// The trapezoid's own AXIAL height at a given depth: the crest flat (a quarter of the crest
// spacing) plus one flank's axial run at each end. thread_depth() solves this backwards to find
// the deepest thread that still fits between two crests.
double thread_profile_height(const FlexiJointParams &p, double depth);
// How much TALLER the female groove is than the thread it clears: the clearance measured
// perpendicular to a 30 degree flank, at each end. profile height + this has to fit inside
// thread_crest_spacing() or the groove's turns merge and the thread holds nothing.
double thread_groove_growth(const FlexiJointParams &p);
// Minor radius = major radius - depth. This is the male core cylinder's radius.
inline double thread_minor_radius(const FlexiJointParams &p) { return thread_major_radius(p) - thread_depth(p); }
// Strand count actually used: clamped into 1..4.
int    thread_start_count(const FlexiJointParams &p);
// Axial length of the threaded region: the helix's own rise plus the last strand's offset.
double thread_axial_length(const FlexiJointParams &p);
// The female bore's inner radius: major radius + clearance.
inline double thread_bore_radius(const FlexiJointParams &p) { return thread_major_radius(p) + double(p.clearance); }
// The wall left around the bore so the female half is not a paper tube: the research's
// "1-1.5 mm of core wall" rule, applied to the outside of the bore.
inline double thread_bore_wall() { return 1.2; }
// The trapezoidal thread profile in the strand's own (radial, axial) frame, as
// its_make_helical_sweep() wants it: 30 degree flanks, flats at crest and root, centred on the
// helix centreline. `radial_offset` shifts the whole profile outward, which is how the female
// groove is built at the clearance radius.
std::vector<Vec2d> thread_profile(const FlexiJointParams &p, double radial_offset = 0.);
// The same profile DILATED by `grow` - each edge moved out along its own normal, which is what
// a clearance actually is. Sliding the profile radially instead leaves the two flanks on the
// same pair of parallel lines and gives no clearance at all where the thread bears, which is
// why the female groove is built with this rather than with a radial offset.
std::vector<Vec2d> thread_profile_grown(const FlexiJointParams &p, double radial_offset, double grow);
// Helix angle of the thread at the major radius, measured FROM VERTICAL, in degrees: a steep
// helix means the crest overhangs badly even with the axis upright.
double thread_helix_angle_deg(const FlexiJointParams &p);

// Lug count actually used: clamped into 2..4.
int    bayonet_lug_count(const FlexiJointParams &p);
// The bayonet plug's radius: the lug circle's radius less the lug height.
inline double bayonet_plug_radius(const FlexiJointParams &p)
{
    return std::max(0.5, thread_major_radius(p) - double(p.bayonet_lug_height));
}
// The lock angle actually used: clamped into 30..120 degrees, and never so wide that one lug's
// track would run into the next lug's entry channel.
double bayonet_effective_lock_angle(const FlexiJointParams &p);
// Overall axial length of the bayonet plug: the entry depth plus the lug's own thickness plus
// a little seating clearance.
double bayonet_plug_length(const FlexiJointParams &p);
// The centre angle of lug i, in degrees, INCLUDING the joint's own rotation.
double bayonet_lug_angle_deg(const FlexiJointParams &p, int i);

// PRINTABILITY OF THE TWIST AXIS. A thread or a bayonet wants its axis VERTICAL in print
// orientation: with the axis lying down, each turn's upper flank is a horizontal overhang that
// sags into the mating clearance and the fit is gone. `axis_world` is the cut normal in WORLD
// coordinates. Returns true when it is more than about 6 degrees off vertical, which the gizmo
// warns - never blocks - on. Exactly the mirror image of hinge_axis_needs_care(), which wants
// its axis HORIZONTAL.
inline bool twist_axis_needs_care(const Vec3d &axis_world)
{
    const double n = axis_world.norm();
    return !(n > EPSILON && std::abs(axis_world.z()) / n > 0.995);
}

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
