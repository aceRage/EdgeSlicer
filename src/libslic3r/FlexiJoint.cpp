#include "FlexiJoint.hpp"

#include <algorithm>
#include <cmath>

#include "ClipperUtils.hpp"
#include "Polygon.hpp"

namespace Slic3r {

// Sector count of every revolved body. Male and female use the SAME count and the same
// angular phase, so their facets stay parallel and the measured clearance is exactly C
// instead of C +/- the faceting error.
static const int FLEXI_SECTORS = 120;
// Meridional steps used for the ball's spherical arc.
static const int FLEXI_ARC_STEPS = 32;
// Facets around the chain link's wire, and steps per 180 degree end cap of its centreline.
// The two loops and their clearance-inflated copies all use the SAME counts, so a loop and
// the relief carved for it stay facet-parallel and the measured clearance is exactly C.
static const int FLEXI_WIRE_SECTORS = 32;
static const int FLEXI_CAP_STEPS    = 24;

// How far the male body reaches DOWN into the male half. It only has to be deep enough for a
// robust union with the male half (no coplanar faces), so it scales with the joint.
static double flexi_anchor_depth(const FlexiJointParams &p)
{
    // The revolved kinds derive this from the protrusion height; keep that, but compute the
    // height inline to avoid recursing through flexi_protrusion_height for the chain link
    // (which does not use an anchor depth at all).
    const double h = p.kind == FlexiJointKind::DoubleRing ? double(p.ring_height) : [&p]() {
        const double Cc = double(p.clearance);
        const double Rb = double(p.outer_radius);
        const double th = double(p.open_angle) * M_PI / 180.;
        return Cc + (Rb + Cc) * std::cos(th) + 0.2 + Rb;
    }();
    return std::max(0.6, 0.4 * h);
}

// THE GAP, for the revolved kinds: how far the whole (r, z) profile is lifted so that the
// stem starts at the FEMALE FACE rather than at the cut plane. See the profile section.
static double flexi_revolved_z_shift(const FlexiJointParams &p)
{
    return 0.5 * double(flexi_effective_gap(p));
}

// ------------------------------------------------------------------------- chain link
//
// Both loops are STADIUM centrelines: two parallel straights of length (L - W) joined by two
// semicircular caps of radius W/2, so the loop has overall length L and width W and no
// corners for the swept tube to pinch at. Each is then swept with a tube of radius `wire`.
//
// THE TWO PLANES. A chain link is two rings that each pass THROUGH the cut plane, so BOTH
// ring planes have to contain the cut normal n, and the two planes have to be perpendicular
// to each other - that is the whole reason the joint hinges in two directions instead of one.
// Writing d for a unit reference direction lying in the cut plane:
//   LOWER ring (A): its plane is spanned by (n, d)     -> the x-z plane, centre pushed to -n.
//   UPPER ring (B): its plane is spanned by (n, n x d) -> the y-z plane, centre pushed to +n.
// Both long axes run along n. Their spans overlap around z == 0, and because the planes are
// perpendicular each ring threads the other's hole there. Phase 2 laid ring B down IN the cut
// plane (spanned by d and n x d) instead - which is why the owner saw one ring standing and
// one lying flat, and why the joint hinged on only one axis. See the phase 3 spec section.
//
// THE OFFSET. Each ring is centred `off` from the cut plane on ITS OWN side: the lower ring
// at z == -off, the upper one at z == +off. That is what puts the two centroids on opposite
// sides of the plane while both rings still cross it, and it is the single number that has to
// satisfy all three of the joint's constraints at once:
//   (1) The ring's centre has to clear its own segment's face, or the ring would be centred in
//       the gap rather than in its own half:      off >= gap/2 + wire.
//   (2) The ring's far end has to be embedded at least `stem` deep in its own segment, which
//       is what attaches it:                      off >= stem + gap/2 - L/2.
//   (3) The two rings still have to THREAD each other: the upper ring's free (bottom) end has
//       to reach below the top of the lower ring's hole with the clearance to spare, i.e.
//       off - L/2 <= -off + (L/2 - wire) - C, so  off <= (L - wire)/2 - C.
// The preferred value is L/4 - a quarter of the ring length puts the centroids a half length
// apart and leaves each ring's free half threaded through the other; it is clamped into
// [max of (1) and (2), (3)]. When (3) is below the lower bounds the parameters cannot make an
// interlocked joint at all and flexi_validate() refuses them.
//
// The frame, in the cut frame (cut plane z == 0, faces at -+ gap/2):
//   LOWER ring: in the x-z plane (normal +Y), long axis +Z, centre (0, 0, -off). Its bottom
//       extremity is embedded in the lower body; its top half is free, reaching up through
//       the cut plane.
//   UPPER ring: in the y-z plane (normal +X), long axis +Z, centre (0, 0, +off) - the mirror
//       image in the perpendicular plane. Its top extremity is embedded in the upper body;
//       its bottom half is free, reaching down through the cut plane and threading the
//       lower ring's hole.
//   `tilt_angle` tips the UPPER ring about +Y so its free end leans clear of the lower ring
//       instead of sitting dead concentric with it; 0 is a legal value.
//
// PRINTABILITY. Both rings now stand vertical, so each has a self-supporting bridge at the
// top of its arc and prints the way the phase 2 "vertical" ring already did; see the spec's
// phase 3 printability note.

struct ChainLinkFrame
{
    double L{ 0. }, W{ 0. }, t{ 0. };
    double gap{ 0. }, face_lo{ 0. }, face_hi{ 0. };
    double off{ 0. };          // each ring's centre offset from the plane, on its own side
    double off_min{ 0. }, off_max{ 0. };
    double lcz{ 0. };          // lower ring centre z (x-z plane)
    double ucz{ 0. };          // upper ring centre z (y-z plane)
    double tilt{ 0. };         // upper ring tilt about +Y, radians
    double lower_top{ 0. };    // topmost lower-ring centreline z
    double upper_bot{ 0. };    // bottommost upper-ring centreline z
    double embed{ 0. };        // how deep each ring's far end sits inside its own segment
    double interlock{ 0. };    // how far the upper ring's free end reaches past the top of
                               // the lower ring's hole (must stay positive for a real link)
};

static ChainLinkFrame chain_frame(const FlexiJointParams &p)
{
    ChainLinkFrame f;
    f.L       = double(p.link_length);
    f.W       = double(p.link_width);
    f.t       = double(p.wire);
    f.gap     = double(flexi_effective_gap(p));
    f.face_lo = -0.5 * f.gap;
    f.face_hi = +0.5 * f.gap;

    f.off_min = std::max(0.5 * f.gap + f.t, double(p.stem) + 0.5 * f.gap - 0.5 * f.L);
    f.off_max = 0.5 * (f.L - f.t) - double(p.clearance);
    f.off     = std::min(std::max(0.25 * f.L, f.off_min), std::max(f.off_min, f.off_max));

    f.lcz     = -f.off;
    f.ucz     = +f.off;
    f.tilt    = double(p.tilt_angle) * M_PI / 180.;
    f.lower_top = f.lcz + 0.5 * f.L;
    f.upper_bot = f.ucz - 0.5 * f.L;
    f.embed     = f.off + 0.5 * f.L - 0.5 * f.gap;
    // The lower ring's hole reaches up to lower_top - t; the upper ring's free end reaches
    // down to upper_bot. The link is real only while the latter is genuinely below the former.
    f.interlock = (f.lower_top - f.t) - f.upper_bot;
    return f;
}

// Closed stadium centreline of overall length L and width W in a local (a, b) plane, `a` the
// long axis. Wound so that the loop is traversed once.
static std::vector<Vec2d> stadium_path(double L, double W, int cap_steps)
{
    const double r        = 0.5 * W;
    const double straight = std::max(EPSILON, L - W);
    const double a0 = -0.5 * straight, a1 = +0.5 * straight;

    std::vector<Vec2d> pts;
    pts.emplace_back(a0, -r);
    pts.emplace_back(a1, -r);
    for (int i = 1; i < cap_steps; ++ i) {
        const double th = M_PI * (-0.5 + double(i) / double(cap_steps));
        pts.emplace_back(a1 + r * std::cos(th), r * std::sin(th));
    }
    pts.emplace_back(a1, r);
    pts.emplace_back(a0, r);
    for (int i = 1; i < cap_steps; ++ i) {
        const double th = M_PI * (0.5 + double(i) / double(cap_steps));
        pts.emplace_back(a0 + r * std::cos(th), r * std::sin(th));
    }
    return pts;
}

// THE ROTATION. `p.rotation` turns the whole two-ring assembly about the cut normal n, so
// d = rotate(d0, n, rotation) with d0 = +X, the cut plane's own X axis. Both rings turn
// together, which keeps their planes perpendicular; the parameter only chooses where in the
// cut plane the pair sits. Applied last, as a rotation about +Z in the cut frame.
static void chain_apply_rotation(std::vector<Vec3d> &path, const FlexiJointParams &p)
{
    const double a = double(p.rotation) * M_PI / 180.;
    if (std::abs(a) <= EPSILON)
        return;
    const double ca = std::cos(a), sa = std::sin(a);
    for (Vec3d &v : path)
        v = Vec3d(ca * v.x() - sa * v.y(), sa * v.x() + ca * v.y(), v.z());
}

// The LOWER ring's centreline: the stadium's long axis (a) maps to +Z and its width (b) to X,
// so the ring stands in the x-z plane - the plane spanned by (n, d).
static std::vector<Vec3d> chain_lower_path(const FlexiJointParams &p)
{
    const ChainLinkFrame f = chain_frame(p);
    std::vector<Vec3d>   out;
    for (const Vec2d &ab : stadium_path(f.L, f.W, FLEXI_CAP_STEPS))
        out.emplace_back(ab.y(), 0., f.lcz + ab.x());
    chain_apply_rotation(out, p);
    return out;
}

// The UPPER ring's centreline: long axis (a) to +Z and width (b) to Y, so the ring stands in
// the y-z plane - the plane spanned by (n, n x d), perpendicular to the lower ring's. It is
// then tipped by tilt_angle INSIDE that plane, about +X (the ring's own plane normal), which
// leans its free (lower) end clear of the lower ring.
//
// THE TILT AXIS MATTERS. Tipping about +Y instead would swing the ring's free tip out along
// x - straight towards the LOWER ring's plane - and eat into the clearance: at the defaults
// the two wires' closest approach drops from 0.75 mm to 0.32 mm at only 7 degrees, i.e. below
// the 0.35 mm clearance. Tipping about the ring's own normal keeps every point in the y-z
// plane, so the lateral margin (which is what the `link_width >= 4*wire + 2*C` rule buys) is
// untouched whatever the tilt.
static std::vector<Vec3d> chain_upper_path(const FlexiJointParams &p)
{
    const ChainLinkFrame f  = chain_frame(p);
    const double         ca = std::cos(f.tilt), sa = std::sin(f.tilt);
    std::vector<Vec3d>   out;
    for (const Vec2d &ab : stadium_path(f.L, f.W, FLEXI_CAP_STEPS)) {
        // In the ring's own frame: x = 0 (its plane's normal), y = width, z = long axis.
        const double y = ab.y(), z = ab.x();
        // Tip about +X, inside the y-z plane: (y, z) -> (y cos - z sin, y sin + z cos).
        out.emplace_back(0., y * ca - z * sa, f.ucz + y * sa + z * ca);
    }
    chain_apply_rotation(out, p);
    return out;
}

// ---------------------------------------------------------------------------------- hinge
//
// The hinge lives in the cut frame with its axis along +X. `e = n x d` is +Y, and the barrel
// centreline sits at y = -hinge_edge_offset, z = 0 - straddling the cut plane exactly the way
// every other flexi body does, so the segment split at -+ gap/2 cuts nothing but the object.
//
// The run of N knuckles is centred on x == 0. Slot i spans [x_i - S/2, x_i + S/2) with
// S = hinge_length / N, and the cylinder inside it is only S - gap long, centred in the slot -
// which puts exactly `gap` between every pair of facing knuckle end faces, including the two
// at the ends of the run (where there is nothing facing them, so it just shortens the run by
// gap/2 at each end). No Clipper offset is involved: a cylinder shortened uniformly along its
// own axis is still a cylinder, so the arithmetic IS the clearance.

int hinge_knuckle_count(const FlexiJointParams &p)
{
    return std::max(1, std::min(9, p.hinge_knuckles));
}

double hinge_slot_length(const FlexiJointParams &p)
{
    return std::max(EPSILON, double(p.hinge_length) / double(hinge_knuckle_count(p)));
}

double hinge_knuckle_length(const FlexiJointParams &p)
{
    // Never let the gap eat the whole knuckle: leave at least a tenth of the slot solid, so a
    // silly gap/length combination degrades into a stubby hinge instead of an empty mesh.
    const double S = hinge_slot_length(p);
    return std::max(0.1 * S, S - double(flexi_effective_gap(p)));
}

double hinge_slot_centre(const FlexiJointParams &p, int i)
{
    const int    N = hinge_knuckle_count(p);
    const double S = hinge_slot_length(p);
    // Slot i's centre, with the whole run of N slots centred on 0.
    return (double(i) - 0.5 * double(N - 1)) * S;
}

bool hinge_knuckle_is_lower(const FlexiJointParams &p, int i)
{
    // Even index = lower by default; the fold-side flag swaps both parities at once, which is
    // the whole of what it does (it moves the pin to the other half with its knuckles).
    const bool even = (i % 2) == 0;
    return p.hinge_fold_upper ? !even : even;
}

// One knuckle barrel: a cylinder of radius `r` and length `len` along +X, centred at x == cx
// on the barrel centreline. its_make_cylinder builds along +Z from z == 0, so it is rotated
// +Y-wards by 90 degrees and then translated.
static indexed_triangle_set hinge_cylinder(const FlexiJointParams &p, double cx, double r, double len)
{
    indexed_triangle_set its = its_make_cylinder(r, len, 2. * M_PI / double(FLEXI_SECTORS));
    // THE ROTATION. `p.rotation` turns the whole hinge about the cut normal, exactly as it
    // turns the chain link's ring pair - the same field, the same sense. For the hinge that
    // angle is not a secondary orientation, it IS the pin axis d: the run is built along the
    // cut plane's +X and then swung to d = rotate(+X, n, rotation).
    // Building order, read right to left: a cylinder along +Z becomes one along +X, lands at
    // x == cx on the barrel centreline (out along -e by the edge offset), and the whole thing
    // then turns in the cut plane.
    Transform3d m = Transform3d::Identity();
    m.rotate(Eigen::AngleAxisd(double(p.rotation) * M_PI / 180., Vec3d::UnitZ()));
    m.translate(Vec3d(cx - 0.5 * len, hinge_axis_y(p), 0.));
    m.rotate(Eigen::AngleAxisd(0.5 * M_PI, Vec3d::UnitY()));
    its_transform(its, m);
    return its;
}

// The continuous pin: one cylinder of radius `r` spanning the WHOLE knuckle run, ends flush
// with the outermost knuckles' outer faces so nothing sticks out past the barrel.
static indexed_triangle_set hinge_pin(const FlexiJointParams &p, double r)
{
    const int    N  = hinge_knuckle_count(p);
    const double x0 = hinge_slot_centre(p, 0)     - 0.5 * hinge_knuckle_length(p);
    const double x1 = hinge_slot_centre(p, N - 1) + 0.5 * hinge_knuckle_length(p);
    return hinge_cylinder(p, 0.5 * (x0 + x1), r, std::max(EPSILON, x1 - x0));
}

// The knuckles of one half, each already merged with the pin (lower) or left plain (upper).
// `lower` picks the parity; the caller adds the pin or the bore.
static std::vector<indexed_triangle_set> hinge_knuckles(const FlexiJointParams &p, bool lower)
{
    const int    N   = hinge_knuckle_count(p);
    const double Ro  = 0.5 * double(p.hinge_barrel_dia);
    const double len = hinge_knuckle_length(p);
    std::vector<indexed_triangle_set> out;
    for (int i = 0; i < N; ++ i)
        if (hinge_knuckle_is_lower(p, i) == lower)
            out.emplace_back(hinge_cylinder(p, hinge_slot_centre(p, i), Ro, len));
    return out;
}

// The same knuckles grown by the clearance in every direction: radius + C, and C longer at
// each end face. This is the relief the OTHER half is carved with.
static std::vector<indexed_triangle_set> hinge_inflated_knuckles(const FlexiJointParams &p, bool lower)
{
    const int    N   = hinge_knuckle_count(p);
    const double C   = double(p.clearance);
    const double Ro  = 0.5 * double(p.hinge_barrel_dia) + C;
    const double len = hinge_knuckle_length(p) + 2. * C;
    std::vector<indexed_triangle_set> out;
    for (int i = 0; i < N; ++ i)
        if (hinge_knuckle_is_lower(p, i) == lower)
            out.emplace_back(hinge_cylinder(p, hinge_slot_centre(p, i), Ro, len));
    return out;
}

std::vector<Vec2d> hinge_footprint_corners(const FlexiJointParams &p, double pad)
{
    const double hx = 0.5 * double(p.hinge_length) + pad;
    const double hy = 0.5 * double(p.hinge_barrel_dia) + pad;
    const double cy = hinge_axis_y(p);
    std::vector<Vec2d> out = { Vec2d(-hx, cy - hy), Vec2d(hx, cy - hy),
                               Vec2d(hx, cy + hy), Vec2d(-hx, cy + hy) };
    // Turned by the joint's own Rotation, the same way the bodies are - so a rotated hinge
    // tests against the cut contour as a ROTATED rectangle rather than a bigger one.
    const double a = double(p.rotation) * M_PI / 180.;
    if (std::abs(a) > EPSILON) {
        const double ca = std::cos(a), sa = std::sin(a);
        for (Vec2d &v : out)
            v = Vec2d(ca * v.x() - sa * v.y(), sa * v.x() + ca * v.y());
    }
    return out;
}

// ------------------------------------------------------------------ thread and bayonet
//
// THE FRAME. Everything below is in the cut frame (axis +Z, cut plane z == 0), and both kinds
// obey THE GAP the same way every other kind does: the lower segment's face is at -gap/2 and
// the upper one's at +gap/2. What is different is that a twist lock is not symmetric about the
// plane - one half carries a plug that reaches INTO the other half's bore - so the two halves
// are named LID (the one with the male fastener) and BODY, and `thread_lid_upper` says which
// is which. `twist_lid_is_upper()` is the only place that decision is read.
//
// THE SIGN. All the geometry below is built for a lid on top: the plug hangs DOWN from the
// upper face into a bore sunk DOWN from the lower face. When the lid is the lower half the
// whole assembly is simply mirrored through z == 0, which is what twist_flip() does - a
// mirror also flips a right-hand thread into a left-hand one, so the hand is flipped back at
// the same time and "right hand" keeps meaning right hand however the lid is chosen.

static bool twist_lid_is_upper(const FlexiJointParams &p) { return p.thread_lid_upper; }

// Mirror a body through the cut plane, for the lid-is-lower case. Mirroring inverts the
// winding, so the triangles are flipped back.
static void twist_mirror_z(indexed_triangle_set &its)
{
    for (Vec3f &v : its.vertices)
        v.z() = -v.z();
    for (Vec3i32 &f : its.indices)
        std::swap(f[1], f[2]);
}

static std::vector<indexed_triangle_set> twist_orient(const FlexiJointParams &p,
                                                     std::vector<indexed_triangle_set> in)
{
    if (!twist_lid_is_upper(p))
        for (indexed_triangle_set &its : in)
            twist_mirror_z(its);
    return in;
}

int thread_start_count(const FlexiJointParams &p)
{
    return std::max(1, std::min(4, p.thread_starts));
}

double thread_crest_spacing(const FlexiJointParams &p)
{
    // ONE CREST TO THE NEXT, along the axis. Strand s sits pitch x s/starts above strand 0, so
    // an N-start thread puts N crests in every pitch and the spacing is pitch / starts. This is
    // the single number every other dimension has to fit inside.
    return std::max(EPSILON, double(p.thread_pitch) / double(thread_start_count(p)));
}

double thread_profile_height(const FlexiJointParams &p, double depth)
{
    return 0.25 * thread_crest_spacing(p) + 2. * depth * std::tan(30. * M_PI / 180.);
}

double thread_groove_growth(const FlexiJointParams &p)
{
    // How much TALLER the groove is than the thread it clears, end to end: the mitred extension
    // of the crest flat (C / cos(30 deg)) at the crest end, and that plus the root corners' extra
    // 0.6 x C at the root end - see thread_profile_grown() for why the root end needs more.
    const double C = double(p.clearance);
    return 2. * C / std::cos(30. * M_PI / 180.) + 0.6 * C;
}

double thread_depth(const FlexiJointParams &p)
{
    // The ACME convention is a depth of about 0.35 x pitch...
    double d = 0.35 * std::max(EPSILON, double(p.thread_pitch));
    // ... capped so the thread and, crucially, the GROOVE CUT FOR IT still fit between one crest
    // and the next. The groove is the profile grown by the clearance, so it is
    // 2 x C / cos(30 deg) taller than the thread; if that exceeds the crest spacing the groove's
    // own coils merge into one annular cavity and the "thread" holds nothing at all - the lid
    // pulls straight out. Leave a fifth of the spacing as wall between coils.
    const double S    = thread_crest_spacing(p);
    const double grow = thread_groove_growth(p);
    // A tenth of the crest spacing left as wall between one groove turn and the next. Anything
    // more is paid for out of the thread's DEPTH, which is what makes it hold - and how thick
    // that wall needs to be to print is a question the PITCH answers (see the guard), not one
    // the depth cap should keep paying for.
    const double room = 0.9 * S - grow;      // axial room the trapezoid itself may take
    if (thread_profile_height(p, d) > room) {
        // Solve thread_profile_height(d) == room for d, and floor it at something buildable.
        const double dd = (room - 0.25 * S) / (2. * std::tan(30. * M_PI / 180.));
        d = std::max(0.05, dd);
    }
    // Never eat the whole core: leave the minor radius at least a third of the major one.
    d = std::min(d, 0.66 * thread_major_radius(p));
    return std::max(0.05, d);
}

std::vector<Vec2d> thread_profile(const FlexiJointParams &p, double radial_offset)
{
    return thread_profile_grown(p, radial_offset, 0.);
}

std::vector<Vec2d> thread_profile_grown(const FlexiJointParams &p, double radial_offset, double grow)
{
    // THE TRAPEZOID, in (dr, dz) offsets from the helix centreline. 30 degree flanks measured
    // from the radial direction (the ACME/60-degree-included convention), and a flat at the
    // crest AND at the root so nothing comes to a knife edge that will not print.
    //
    //          <-- flat_crest -->        (outer, dr = +depth/2)
    //         /                  \
    //        /                    \      flank, 30 degrees off radial
    //       <------ flat_root ------>    (inner, dr = -depth/2)
    //
    // The axial run of one flank is depth/2 x tan(30 deg) + ... expressed the other way round:
    // the crest flat is 0.25 x pitch and each flank rises depth x tan(30 deg) axially, so the
    // root flat comes out to crest + 2 x that. Written this way the profile stays a proper
    // trapezoid for any depth/pitch the guard allows.
    const double d  = thread_depth(p);
    const double hr = 0.5 * d;                       // half the radial extent
    // The ACME "crest flat = a quarter of the pitch" is written for a single-start thread, where
    // the pitch IS the distance from one crest to the next. With N starts there are N crests in
    // every pitch, so the rule is a quarter of the CREST SPACING - which is the same number on a
    // single start and the only one that leaves room between the turns on a multi-start.
    const double crest = 0.25 * thread_crest_spacing(p);
    const double flank = d * std::tan(30. * M_PI / 180.);   // axial run of one flank
    const double hc = 0.5 * crest;
    const double hf = hc + flank;                    // half the root flat
    const double o  = radial_offset;
    // THE DILATION. `grow` is a uniform offset of the whole trapezoid, i.e. the profile's own
    // outward normal offset by that much - which is what a real clearance is, and is NOT the
    // same as sliding the profile radially. Sliding it leaves the two FLANKS lying on the same
    // pair of parallel lines, so a "clearance" built that way is exactly zero along the flanks,
    // which is where a thread actually bears. Growing it moves the crest and root out radially
    // by `grow` and moves each flank out along its own normal, which lengthens both flats by
    // grow / cos(30 deg) at each end - the standard offset of a convex polygon.
    const double g   = std::max(0., grow);
    const double hrg = hr + g;
    // THE MITRE. Moving the crest/root edge out by g and the flank out by g (measured
    // perpendicular to itself) puts their intersection g / cos(30 deg) further along the axis -
    // that is the flat's extension, and it does leave g between the two EDGES. What it does not
    // leave is g at the corner POINT itself, where the male's own mitred corner sits: a true
    // offset rounds that corner with an arc of radius g, and a four-point polygon cannot carry
    // one. The shortfall is small (a couple of microns at the sizes here) but it is a contact,
    // and a contact is what fuses in print. Half a nozzle-independent hair of extra extension
    // clears it without measurably changing the fit anywhere else.
    // The mitre: offsetting the crest/root edge and the flank each by g and taking where they
    // cross extends the flat by g / cos(30 deg). That is exact along both EDGES.
    const double ext = g / std::cos(30. * M_PI / 180.);
    const double hcg = hc + ext;
    // ... but not at the ROOT CORNERS, which are the sharp ones (60 degrees interior against the
    // crest's 120). A true offset would round them with an arc of radius g; a four-point polygon
    // cannot, and the mitre leaves the male's own root corner short of g. Extending the ROOT
    // flat further reaches it, and costs nothing - that flat sits at the groove's inner radius,
    // inside the bore, so it only removes material the bore was taking anyway.
    const double hfg = hf + ext + 0.6 * g;
    // Counter-clockwise in (dr, dz): root left, root right, crest right, crest left.
    return { Vec2d(o - hrg, -hfg), Vec2d(o - hrg, hfg), Vec2d(o + hrg, hcg), Vec2d(o + hrg, -hcg) };
}

double thread_profile_height(const FlexiJointParams &p, double depth);

double thread_axial_length(const FlexiJointParams &p)
{
    // The helix itself rises pitch x turns; the last strand starts (starts-1)/starts of a pitch
    // higher, so the whole threaded band is that much longer. On top of that the band has to
    // cover the PROFILE, not just its centreline, so half the profile's own height is added at
    // each end - the profile's height, not half a pitch, which at N starts would be N times too
    // much and would inflate the plug, the bore and the depth of lid the thread needs with it.
    const double P = double(p.thread_pitch);
    const int    N = thread_start_count(p);
    return P * double(p.thread_turns) + P * double(N - 1) / double(N)
         + thread_profile_height(p, thread_depth(p));
}

double thread_helix_angle_deg(const FlexiJointParams &p)
{
    // The lead - how far the thread advances per turn of the LID - is pitch x starts. Measured
    // from vertical, the crest's overhang is atan(lead / circumference).
    const double R = thread_major_radius(p);
    if (R <= EPSILON)
        return 90.;
    const double lead = double(p.thread_pitch) * double(thread_start_count(p));
    return std::atan2(lead, 2. * M_PI * R) * 180. / M_PI;
}

// The male thread's OWN axial extent below the lid face: the threaded band plus a short collar
// so the core cylinder meets the lid face squarely rather than at a thread crest.
static double thread_plug_length(const FlexiJointParams &p)
{
    return thread_axial_length(p) + 0.6;
}

// The bore has to be deeper than the plug is long, or the lid bottoms out before the flats
// seat - the axial analogue of the revolved kinds' tilt allowance.
static double thread_bore_depth(const FlexiJointParams &p)
{
    // Deep enough for the plug, its seating clearance, and the half turn of groove run-out that
    // is swept below the thread's own bottom coil.
    return thread_plug_length(p) + double(p.clearance) + 0.4 + 0.5 * double(p.thread_pitch);
}

// A cylinder of radius r spanning z in [z0, z1] in the cut frame.
static indexed_triangle_set twist_cylinder(double r, double z0, double z1)
{
    indexed_triangle_set its = its_make_cylinder(std::max(EPSILON, r), std::max(EPSILON, z1 - z0),
                                                 2. * M_PI / double(FLEXI_SECTORS));
    its_translate(its, Vec3f(0.f, 0.f, float(z0)));
    return its;
}

// The male thread's strands, already lifted so the band sits under the lid face. `radius` is
// the helix centreline radius and `offset` shifts the profile outward (the female groove uses
// the same call with offset = clearance, which is what makes the fit exactly C).
// The thread's strands. `offset` shifts the profile outward - the female GROOVE is the same
// call with offset == clearance, which is what makes the fit exactly C - and `extra_turns` runs
// the helix on past the male thread's own ends.
//
// TWO THINGS HAVE TO LINE UP or the groove does not clear the thread it is cut for.
//  * THE Z ORIGIN. Both sweeps have to start at the same height, whatever `extra_turns` does to
//    their length: the extra turns extend the groove PAST the thread at the top, they do not
//    slide it. So the translation is computed from the MALE band's own length, not from this
//    call's, and the extra half turn is bought at the start (by beginning half a turn of phase
//    and pitch earlier) as well as spent at the end.
//  * THE LEAD-IN. The male thread's depth fades to zero over the lead-in at each end, which is
//    the chamfer that lets the halves catch. The GROOVE must NOT do that: a groove that fades
//    out where the thread is still full depth is a groove that pinches the thread. So the
//    groove is swept at FULL depth end to end (lead == 0) and simply runs longer.
static std::vector<indexed_triangle_set> thread_strands(const FlexiJointParams &p, double offset,
                                                        double extra_turns)
{
    const double P  = double(p.thread_pitch);
    const int    N  = thread_start_count(p);
    const double Rc = thread_major_radius(p) - 0.5 * thread_depth(p);   // the centreline radius
    const bool   is_groove = extra_turns > 0.;
    // THE GROOVE RUNS ALL THE WAY OUT OF THE MOUTH. Below the thread it needs only `extra_turns`
    // of run-out; ABOVE it, it has to cover the whole unscrewing travel, because a lid backing
    // out lifts its topmost coil by the full engagement - and a groove that stops short leaves a
    // solid rim between itself and the bore mouth for that coil to jam against. A real tapped
    // hole has no such rim: the thread runs right out of the mouth, and so does this one.
    const double run_out = extra_turns;                                    // below
    const double run_up  = is_groove ? double(p.thread_turns) + extra_turns : 0.;   // above
    const double turns = std::max(0.05, double(p.thread_turns) + run_out + run_up);
    // Segments per turn: fine enough that the facet chord at the major radius stays well under
    // a tenth of a millimetre at the sizes this is used at, and the male and female sweeps use
    // the SAME count and the SAME phase, so their facets stay parallel and the measured
    // clearance is exactly C rather than C plus or minus the faceting error.
    const int seg = FLEXI_SECTORS;
    // Starting the groove `extra_turns` earlier means starting it that much further round AND
    // that much lower, which is exactly one point of the same helix - so the phase and the
    // height move together and the two sweeps stay on one screw.
    const double sgn   = p.thread_left_hand ? -1. : 1.;
    const double phase = double(p.rotation) - (is_groove ? sgn * 360. * run_out : 0.);
    const double lead  = is_groove ? 0. : double(p.thread_lead_turns);
    // The groove is the male profile GROWN by the clearance on the SAME centreline, not a copy
    // pushed outward: growing it puts C between the two along the flanks as well as at the
    // crest, which is where a thread bears and where a radial shift leaves nothing at all.
    std::vector<indexed_triangle_set> strands =
        its_make_helical_sweep(thread_profile_grown(p, 0., offset), Rc, P, N, turns, seg,
                               p.thread_left_hand, phase, lead);
    // The sweep starts at z == 0 and rises; the thread has to hang DOWN from the lid face, so
    // the whole band drops to sit just under it. The lid face is at +gap/2 (lid on top), and
    // the drop is measured from the MALE band's length so the groove lands on the same screw.
    const double top       = 0.5 * double(flexi_effective_gap(p));
    const double male_band = P * double(p.thread_turns) + P * double(N - 1) / double(N);
    const double dz        = top - 0.3 - male_band - (is_groove ? P * run_out : 0.);
    for (indexed_triangle_set &its : strands)
        its_translate(its, Vec3f(0.f, 0.f, float(dz)));
    return strands;
}

// ------------------------------------------------------------------------------- bayonet

int bayonet_lug_count(const FlexiJointParams &p)
{
    return std::max(2, std::min(4, p.bayonet_lugs));
}

double bayonet_effective_lock_angle(const FlexiJointParams &p)
{
    const int    N    = bayonet_lug_count(p);
    const double span = 360. / double(N);       // the angular pitch of the lug pattern
    // The track has to turn the lug through the lock angle and stop before it reaches the next
    // lug's entry channel: entry + lug + track < span, with a little wall left over.
    const double room = span - double(p.bayonet_lug_arc) - 8.;
    double a = double(p.bayonet_lock_angle);
    a = std::max(30., std::min(120., a));
    return std::max(20., std::min(a, room));
}

double bayonet_plug_length(const FlexiJointParams &p)
{
    // Down from the lid face: the entry run, the lug's own thickness and a seating margin.
    return double(p.bayonet_entry_depth) + double(p.bayonet_lug_thickness) + 1.0;
}

double bayonet_lug_angle_deg(const FlexiJointParams &p, int i)
{
    const int N = bayonet_lug_count(p);
    return double(p.rotation) + 360. * double(i) / double(N);
}

// The axial position of the lug's centre when the lid is fully inserted, i.e. the height of
// the circumferential track. Measured in the cut frame with the lid on top: the lid face is at
// +gap/2 and the lug sits `entry_depth` below it.
static double bayonet_track_z(const FlexiJointParams &p)
{
    return 0.5 * double(flexi_effective_gap(p)) - double(p.bayonet_entry_depth);
}

// ONE LUG, as an annular sector: the wedge between r0 and r1, spanning `arc` degrees centred on
// `centre_deg`, and `thickness` tall centred on z == cz. Built as a prism over a polygon so a
// grown copy (the slot) is the same shape with bigger numbers - which is what keeps the
// measured clearance exactly C, the same way an offset cylinder is still a cylinder.
static indexed_triangle_set bayonet_sector(double r0, double r1, double centre_deg, double arc_deg,
                                           double cz, double thickness)
{
    indexed_triangle_set its;
    r0 = std::max(EPSILON, r0);
    r1 = std::max(r0 + EPSILON, r1);
    arc_deg = std::max(1., std::min(350., arc_deg));
    const double a0 = (centre_deg - 0.5 * arc_deg) * M_PI / 180.;
    const double a1 = (centre_deg + 0.5 * arc_deg) * M_PI / 180.;
    // One facet per 3 degrees of arc, at least 2 - fine enough that the sector's outer wall is
    // a good circle and coarse enough not to bloat the boolean.
    const int    n  = std::max(2, int(std::ceil(arc_deg / 3.)));
    const double z0 = cz - 0.5 * thickness, z1 = cz + 0.5 * thickness;

    // The (r, theta) rectangle's outline, walked outer-arc then back along the inner arc.
    std::vector<Vec2d> ring;
    ring.reserve(2 * (n + 1));
    for (int i = 0; i <= n; ++ i) {
        const double a = a0 + (a1 - a0) * double(i) / double(n);
        ring.emplace_back(r1 * std::cos(a), r1 * std::sin(a));
    }
    for (int i = n; i >= 0; -- i) {
        const double a = a0 + (a1 - a0) * double(i) / double(n);
        ring.emplace_back(r0 * std::cos(a), r0 * std::sin(a));
    }
    const int m = int(ring.size());
    its.vertices.reserve(2 * m);
    for (const Vec2d &q : ring)
        its.vertices.emplace_back(Vec3f(float(q.x()), float(q.y()), float(z0)));
    for (const Vec2d &q : ring)
        its.vertices.emplace_back(Vec3f(float(q.x()), float(q.y()), float(z1)));
    // Side wall.
    its.indices.reserve(2 * m + 2 * m);
    for (int i = 0; i < m; ++ i) {
        const int j = (i + 1) % m;
        its.indices.emplace_back(Vec3i32(i, j, m + j));
        its.indices.emplace_back(Vec3i32(i, m + j, m + i));
    }
    // Caps: the outline is a simple ring of 2(n+1) points whose two halves are matched arcs, so
    // the quad strip between point i and point (m-1-i) triangulates it exactly.
    for (int i = 0; i < n; ++ i) {
        const int a = i,           b = i + 1;
        const int c = m - 1 - i,   d = m - 2 - i;
        its.indices.emplace_back(Vec3i32(a, c, b));
        its.indices.emplace_back(Vec3i32(b, c, d));
        its.indices.emplace_back(Vec3i32(m + a, m + b, m + c));
        its.indices.emplace_back(Vec3i32(m + b, m + d, m + c));
    }
    if (its_volume(its) < 0.f)
        for (Vec3i32 &f : its.indices)
            std::swap(f[1], f[2]);
    return its;
}

// The lugs standing out of the plug wall.
static std::vector<indexed_triangle_set> bayonet_lugs_bodies(const FlexiJointParams &p, double pad)
{
    const int    N  = bayonet_lug_count(p);
    const double r0 = bayonet_plug_radius(p) - 0.2;              // bite into the plug wall
    const double r1 = thread_major_radius(p) + pad;
    const double cz = bayonet_track_z(p);
    std::vector<indexed_triangle_set> out;
    for (int i = 0; i < N; ++ i)
        out.emplace_back(bayonet_sector(r0, r1, bayonet_lug_angle_deg(p, i),
                                        double(p.bayonet_lug_arc) + 2. * pad / std::max(1., r1) * 180. / M_PI,
                                        cz, double(p.bayonet_lug_thickness) + 2. * pad));
    return out;
}

// Cut the inboard part of the track across the detent's own arc: the bump stands proud only
// from r1 - det outward, so everything inside that radius is still cut away and the lug's root
// runs free while its tip clicks over.
static void sector_between_inner(std::vector<indexed_triangle_set> &out, double r0, double r1_in,
                                 double from, double to, double cz, double th)
{
    const double span = std::abs(to - from);
    if (span < 0.5 || r1_in <= r0 + EPSILON)
        return;
    out.emplace_back(bayonet_sector(r0, r1_in, 0.5 * (from + to), span, cz, th));
}

// The L-shaped slot for lug i, as a list of solids to subtract from the female half: the AXIAL
// entry channel from the bore mouth down to the track, then the CIRCUMFERENTIAL track the lug
// turns along, then (optionally) the room past the detent the lug clicks into.
static std::vector<indexed_triangle_set> bayonet_slot(const FlexiJointParams &p, int i)
{
    const double C    = double(p.clearance);
    const double r0   = bayonet_plug_radius(p) - 0.3;
    const double r1   = thread_major_radius(p) + C;
    const double cz   = bayonet_track_z(p);
    const double th   = double(p.bayonet_lug_thickness) + 2. * C;
    const double rad  = std::max(1., r1);
    // The angular padding that turns a radial clearance into an angular one at this radius.
    const double apad = (C / rad) * 180. / M_PI;
    const double arc  = double(p.bayonet_lug_arc) + 2. * apad;
    const double lock = bayonet_effective_lock_angle(p);
    // Turning sense: right-hand (clockwise seen from the lid, i.e. -theta) unless left-handed,
    // so "twist the way you would screw it shut" holds for both kinds.
    const double sgn  = p.thread_left_hand ? 1. : -1.;
    const double a0   = bayonet_lug_angle_deg(p, i);
    // Where the entry channel starts, in z: the bore mouth, which is the body's own face.
    const double mouth = 0.5 * double(flexi_effective_gap(p)) + C;

    std::vector<indexed_triangle_set> out;
    // THE REST POSE IS LOCKED. The lugs are built where they sit when the lid is done up - so
    // the cut comes out of the gizmo as a closed container, not a half-assembled one - which
    // puts the ENTRY channel a lock angle BACK from them, at a0 - sgn x lock.
    const double a_entry = a0 - sgn * lock;
    // 1. ENTRY: a channel of the lug's cross-section running from the bore mouth down past the
    //    track, so the lug can drop straight in.
    {
        const double z0 = cz - 0.5 * th;
        out.emplace_back(bayonet_sector(r0, r1, a_entry, arc, 0.5 * (z0 + mouth), mouth - z0));
    }
    // 2. TRACK: the same cross-section swept from the entry angle round to the locked one. One
    //    sector spanning arc + lock is exactly that sweep, because the shape does not change as
    //    it turns.
    //
    // 3. DETENT, and why it lives here. The bump the lug clicks over cannot be added as a BODY:
    //    the cut pipeline unions each half's bodies in first and subtracts its reliefs after, so
    //    a bump crossed by this very track would be carved away again. It has to be left
    //    STANDING by the relief instead - so the track is cut as two sectors with an uncut
    //    sliver between them, at the bump's angle and only over the OUTER part of the lug's
    //    path. That sliver is the female half's own material; the lug has to squeeze past it.
    const double det = std::max(0., std::min(double(p.bayonet_detent), 0.8 * double(p.bayonet_lug_height)));
    if (det <= EPSILON) {
        const double span = arc + lock;
        out.emplace_back(bayonet_sector(r0, r1, a_entry + sgn * 0.5 * lock, span, cz, th));
    } else {
        // The bump sits just short of the LOCKED end (a0), on the side the lug arrives from,
        // and is a few degrees wide.
        // A few degrees, and no more. The lug is `arc` degrees wide, so it is in contact with a
        // bump of `b` degrees over arc + b degrees of travel: a wide bump is a brake, not a
        // click. 6 degrees is about one perimeter's width of bump at the diameters this is
        // used at, which is what makes it click rather than drag.
        const double bump_arc = std::max(3., std::min(6., 0.12 * lock));
        const double a_bump   = a0 - sgn * (0.5 * arc + 0.5 * bump_arc);
        // The track, in full, from the entry angle to the locked one...
        const double a_lo = a_entry - sgn * 0.5 * arc;      // the far edge of the entry channel
        const double a_hi = a0      + sgn * 0.5 * arc;      // the far edge of the locked position
        // ... cut as the run up to the bump, and the run past it.
        const double b_lo = a_bump - sgn * 0.5 * bump_arc;
        const double b_hi = a_bump + sgn * 0.5 * bump_arc;
        auto sector_between = [&](double from, double to) {
            const double span = std::abs(to - from);
            if (span < 0.5)
                return;
            out.emplace_back(bayonet_sector(r0, r1, 0.5 * (from + to), span, cz, th));
        };
        sector_between(a_lo, b_lo);
        sector_between(b_hi, a_hi);
        // The bump only stands proud over the OUTER part of the path: inboard of it the track
        // stays clear, so the lug's root is never pinched and only its tip has to click past.
        sector_between_inner(out, r0, r1 - det, b_lo, b_hi, cz, th);
    }
    return out;
}

// NOTE ON THE DETENT. It is NOT a body. The cut pipeline unions each half's bodies in first and
// subtracts its reliefs after, so a bump added as a body and then crossed by the very track
// relief that runs past it would simply be carved away again - which is exactly what happened
// the first time round, and the detent never bit at all. It is left STANDING by the track relief
// instead: bayonet_slot() cuts the track as two sectors with an uncut sliver between them, and
// that sliver is the female half's own material. See there.

// ---------------------------------------------------------------------------------- defaults

float flexi_default_hub_radius(const FlexiJointParams &p)
{
    const float r = p.outer_radius - p.ring_width - p.clearance - p.ring_width;
    return r > 0.2f ? r : 0.f;
}

float flexi_default_gap(FlexiJointKind kind)
{
    // Chain link: the loops swing right through each other, so the segments need real space
    // between them - 1.5 mm is about one loop wire diameter and reads as an obvious hinge.
    // Ring / ball: those only rotate and rock in place, so the gap just has to keep the flat
    // faces from rubbing; 0.6 mm is three 0.2 mm layers, enough to stay open after slicing
    // and small enough that the joint does not look like a mistake.
    // Hinge: the knuckle end faces are `gap` apart along the axis and the two segments' flat
    // faces are `gap` apart across it, and a pin hinge only has to turn, not swing through
    // itself - so it takes the same 0.6 mm the ring and the ball do, not the chain's 1.5.
    // Thread / bayonet: the two faces are meant to MEET when the lid is done up, not to bridge
    // a flexing gap - the fastener's clearance is radial, in the thread or the slot, and lives
    // on its own. The gap here is only the saw kerf: 0.4 mm, two layers, enough that the faces
    // do not fuse and small enough that a screwed-down lid looks shut.
    if (flexi_kind_is_twist(kind))
        return 0.4f;
    return kind == FlexiJointKind::ChainLink ? 1.5f : 0.6f;
}

float flexi_effective_gap(const FlexiJointParams &p)
{
    float g = p.gap > 0.f ? p.gap : flexi_default_gap(p.kind);
    // The faces can never be closer than the clearance: below that the slicer's gap closing
    // would weld them and the "gap" would not exist at all.
    return std::max(g, p.clearance);
}

float flexi_min_link_size(const FlexiJointParams &p)
{
    return 4.f * p.wire + 2.f * p.clearance;
}

float flexi_clearance_floor(double nozzle_diameter)
{
    if (nozzle_diameter <= 0.)
        return 0.2f;
    return float(0.75 * nozzle_diameter);
}

FlexiJointParams flexi_auto_size(FlexiJointParams p, double inscribed_radius)
{
    if (inscribed_radius > 0.) {
        p.outer_radius = float(std::max(1.2, 0.4 * inscribed_radius));
        // Never let the joint (plus its clearance) poke out of the cross section.
        const float max_r = float(inscribed_radius) - p.clearance - 0.4f;
        if (max_r > 1.0f && p.outer_radius > max_r)
            p.outer_radius = max_r;
    }
    if (p.kind == FlexiJointKind::DoubleRing) {
        // The lip has to fit inside its own outer radius.
        p.ring_width = std::min(p.ring_width, 0.8f * p.outer_radius);
        p.hub_radius = flexi_default_hub_radius(p);
    } else if (p.kind == FlexiJointKind::Hinge && inscribed_radius > 0.) {
        // The hinge is sized by the cut face it has to lie along: the run gets most of the
        // available width, and the barrel scales with it but stays printable and stays well
        // clear of the pin (barrel >= pin + 2 x clearance + 2 x a nozzle-ish wall).
        const double span = std::max(2.0, 1.6 * inscribed_radius);
        p.hinge_length    = float(std::min(span, 60.));
        double barrel     = std::max(2.0, std::min(0.35 * inscribed_radius, 10.));
        double pin        = std::max(1.0, 0.45 * barrel);
        // Keep a real wall around the bore whatever the clearance ends up being.
        const double wall = 0.8;
        if (barrel < pin + 2. * double(p.clearance) + 2. * wall)
            barrel = pin + 2. * double(p.clearance) + 2. * wall;
        p.hinge_barrel_dia = float(barrel);
        p.hinge_pin_dia    = float(pin);
    } else if (flexi_kind_is_twist(p.kind) && inscribed_radius > 0.) {
        // A twist lock is sized by its MAJOR DIAMETER, not by outer_radius: the research's
        // default is 0.7 x the cut section's inscribed radius, which leaves the bore's own wall
        // and a perimeter or two of part around it.
        double major = 1.4 * inscribed_radius;
        // The bore plus its wall has to stay inside the section.
        const double room = 2. * (inscribed_radius - double(p.clearance) - thread_bore_wall() - 0.4);
        major = std::min(major, std::max(3., room));
        p.thread_major_dia = float(std::max(4., major));
        if (p.kind == FlexiJointKind::Thread) {
            // Pitch scales gently with the diameter but never below the printability floor: a
            // fine pitch on a big cap is exactly the crest-too-thin failure the research warns
            // about, and a coarse one on a small cap makes a helix that is all overhang.
            // The pitch has to carry the START COUNT: an N-start thread puts N crests in every
            // pitch, so the same crest spacing - and therefore the same thread depth and the
            // same printable crest - needs N times the pitch. Sizing against the spacing rather
            // than the pitch is what keeps a 2-start auto-sized thread as deep as a 1-start one.
            const int    N       = thread_start_count(p);
            const double spacing = std::max(2.0, std::min(0.25 * double(p.thread_major_dia), 6.0));
            p.thread_pitch = float(spacing * double(N));
        } else {
            // The bayonet's lug scales with the diameter and stays printable.
            p.bayonet_lug_height    = float(std::max(1.0, std::min(0.14 * double(p.thread_major_dia), 3.0)));
            p.bayonet_lug_thickness = float(std::max(1.6, std::min(0.20 * double(p.thread_major_dia), 4.0)));
        }
    } else if (p.kind == FlexiJointKind::ChainLink && inscribed_radius > 0.) {
        // The chain link is sized by its own outer extent, not by outer_radius: scale the
        // loops so the whole interlocked assembly fits inside the cross section with a
        // perimeter's worth of wall to spare. The proportions (length : width : wire) are
        // kept, because they are what makes the two loops thread each other.
        const FlexiJointParams ref  = FlexiJointParams{};
        const double           want = std::max(1.0, inscribed_radius - double(p.clearance) - 0.4);
        FlexiJointParams probe = p;
        probe.link_length = ref.link_length;
        probe.link_width  = ref.link_width;
        probe.wire        = ref.wire;
        probe.stem        = ref.stem;
        const double have = double(flexi_outer_extent(probe));
        double       s    = have > EPSILON ? want / have : 1.;
        s = std::min(s, 2.5);           // do not blow the joint up on a huge cut
        s = std::max(s, 0.45);          // below this the wire stops being printable
        p.link_length = float(s * double(ref.link_length));
        p.link_width  = float(s * double(ref.link_width));
        p.wire        = float(s * double(ref.wire));
        p.stem        = float(s * double(ref.stem));
        // The sizing constraint (a loop's hole has to pass the other's wire with C to spare)
        // does not scale with s because the clearance does not: widen the loops if needed.
        const float need = flexi_min_link_size(p);
        p.link_length = std::max(p.link_length, need);
        p.link_width  = std::max(p.link_width,  need);
    }
    return p;
}

// ------------------------------------------------------------------------------- key measures

float flexi_protrusion_height(const FlexiJointParams &p)
{
    if (flexi_kind_is_twist(p.kind))
        // How far the male fastener reaches out of its own half: the plug's whole length,
        // which is what the gizmo scales its preview and its picking radius by.
        return float((p.kind == FlexiJointKind::Thread ? thread_plug_length(p) : bayonet_plug_length(p)) +
                     0.5 * double(flexi_effective_gap(p)));
    if (p.kind == FlexiJointKind::Hinge)
        // The barrel straddles the cut plane, so it reaches its own outer radius above it.
        return 0.5f * p.hinge_barrel_dia;
    if (p.kind == FlexiJointKind::ChainLink) {
        // Both rings straddle the plane now, so the tallest thing above it is whichever ring
        // reaches higher: the lower ring's free top, or the upper ring's anchored top. The
        // upper ring is tipped inside its own plane, which lifts its far corner by
        // (W/2) sin(tilt) on top of the (L/2) cos(tilt) the long axis reaches.
        const ChainLinkFrame f = chain_frame(p);
        const double         utop = f.ucz + 0.5 * f.L * std::cos(f.tilt) + 0.5 * f.W * std::sin(f.tilt);
        return float(std::max(f.lower_top, utop) + double(p.wire));
    }
    const double shift = flexi_revolved_z_shift(p);
    if (p.kind == FlexiJointKind::DoubleRing)
        return float(double(p.ring_height) + shift);
    // Ball & socket: the ball centre sits above the mouth, plus the ball radius.
    const double C  = double(p.clearance);
    const double Rb = double(p.outer_radius);
    const double th = double(p.open_angle) * M_PI / 180.;
    const double zb = C + (Rb + C) * std::cos(th) + 0.2;
    return float(zb + Rb + shift);
}

// Ball & socket: neck radius derived from the socket opening half angle.
static double flexi_ball_neck_radius(const FlexiJointParams &p)
{
    const double C  = double(p.clearance);
    const double Rb = double(p.outer_radius);
    const double th = double(p.open_angle) * M_PI / 180.;
    double rn = (Rb + C) * std::sin(th) - C;
    rn = std::max(rn, 0.25 * Rb);
    rn = std::min(rn, 0.85 * Rb);
    return rn;
}

// Ball & socket: the height of the ball centre above the cut plane.
static double flexi_ball_centre_z(const FlexiJointParams &p)
{
    const double C  = double(p.clearance);
    const double Rb = double(p.outer_radius);
    const double th = double(p.open_angle) * M_PI / 180.;
    return C + (Rb + C) * std::cos(th) + 0.2;
}

float flexi_outer_extent(const FlexiJointParams &p)
{
    if (flexi_kind_is_twist(p.kind))
        // The widest thing a twist lock puts on the cut plane is the OUTER WALL of the female
        // bore: the bore itself is major/2 + clearance, and it needs a wall of its own around
        // it or the body half prints as a paper tube. That is the circle the footprint test
        // has to fit inside the cut contour.
        return float(thread_bore_radius(p) + thread_bore_wall());
    if (p.kind == FlexiJointKind::Hinge) {
        // The farthest corner of the knuckle run's rectangle from the joint origin. The gizmo
        // still uses this for the connector's picking radius and preview scale; the CONTOUR
        // test uses hinge_footprint_corners() instead, which is the point of that function.
        double best = 0.;
        for (const Vec2d &c : hinge_footprint_corners(p, double(p.clearance)))
            best = std::max(best, c.norm());
        return float(best);
    }
    if (p.kind == FlexiJointKind::ChainLink) {
        double best = 0.;
        for (const std::vector<Vec3d> &path : { chain_lower_path(p), chain_upper_path(p) })
            for (const Vec3d &v : path)
                best = std::max(best, std::hypot(v.x(), v.y()));
        return float(best + double(p.wire) + double(p.clearance));
    }
    return p.outer_radius + p.clearance;
}

float flexi_mouth_half_width(const FlexiJointParams &p)
{
    if (flexi_kind_is_twist(p.kind))
        // A twist lock is NOT captive in the flexi sense - it is meant to come apart, that is
        // the whole point of a lid. Report a mouth narrower than the lip so the generic captive
        // test (which flexi_validate() only applies to the articulated kinds anyway) does not
        // claim the opposite.
        return float(thread_minor_radius(p));
    if (p.kind == FlexiJointKind::Hinge)
        // A pin hinge is captive because the bore wraps the pin all the way round: the "mouth"
        // is the bore and the "lip" is the barrel's outer wall, so the generic captive test
        // below asks exactly the right question - is there wall left outside the bore?
        return float(hinge_bore_radius(p));
    if (p.kind == FlexiJointKind::ChainLink)
        // A chain link is not captive by a narrow mouth but by the two loops being LINKED.
        // Report the hole a loop's wire has to pass, and the wire it has to pass, so the
        // generic captive test below still says the right thing.
        return 2.f * p.wire + 2.f * p.clearance;
    if (p.kind == FlexiJointKind::DoubleRing)
        return 0.5f * p.ring_width * p.neck_ratio + p.clearance;
    return float(flexi_ball_neck_radius(p)) + p.clearance;
}

float flexi_lip_half_width(const FlexiJointParams &p)
{
    if (flexi_kind_is_twist(p.kind))
        return float(thread_major_radius(p));
    if (p.kind == FlexiJointKind::Hinge)
        return 0.5f * p.hinge_barrel_dia;
    if (p.kind == FlexiJointKind::ChainLink)
        return std::min(p.link_length, p.link_width) - 2.f * p.wire;
    if (p.kind == FlexiJointKind::DoubleRing)
        return 0.5f * p.ring_width;
    return p.outer_radius;
}

// ---------------------------------------------------------------------------------- profiles

// THE GAP, for the revolved kinds. The profiles below are written in a frame whose origin is
// the FEMALE FACE (z == +gap/2 in the cut frame): the lip's straight stem starts there and the
// flare is above it, exactly as in phase 1 where the gap was zero and the female face sat at
// z == 0. `flexi_revolved_z_shift()` then moves the whole profile up into the cut frame, and
// the anchor is deepened by the gap so the body still reaches down into the male segment.
// Consequence: the undercut geometry - which is what makes the joint captive - is completely
// unaffected by the gap; the gap only lengthens the stem that crosses it.
// The dovetail lip cross section, in (r, z), for a ring of mean radius Rm.
// `top_z` lets the female profile raise the ceiling by the tilt allowance while keeping the
// flare (and therefore the undercut) identical to the male's.
static std::vector<Vec2d> ring_profile(const FlexiJointParams &p, double top_z)
{
    const double Rm = double(p.outer_radius) - 0.5 * double(p.ring_width);
    const double W  = double(p.ring_width);
    const double Wn = W * double(p.neck_ratio);
    const double H  = double(p.ring_height);
    const double Hd = 0.6 * H;              // flare height
    const double zn = H - Hd;               // top of the straight stem
    // The anchor has to cross the gap AND bite into the male segment.
    const double A  = flexi_anchor_depth(p) + double(flexi_effective_gap(p));

    std::vector<Vec2d> prof;
    prof.emplace_back(Rm - 0.5 * Wn, -A);
    prof.emplace_back(Rm + 0.5 * Wn, -A);
    prof.emplace_back(Rm + 0.5 * Wn, zn);
    prof.emplace_back(Rm + 0.5 * W,  H);
    if (top_z > H + EPSILON) {
        prof.emplace_back(Rm + 0.5 * W, top_z);
        prof.emplace_back(Rm - 0.5 * W, top_z);
    }
    prof.emplace_back(Rm - 0.5 * W,  H);
    prof.emplace_back(Rm - 0.5 * Wn, zn);
    return prof;
}

// The hub cross section (a plain cylinder touching the axis).
static std::vector<Vec2d> hub_profile(const FlexiJointParams &p, double top_z)
{
    const double Rh = double(p.hub_radius);
    const double A  = flexi_anchor_depth(p) + double(flexi_effective_gap(p));
    std::vector<Vec2d> prof;
    prof.emplace_back(0.,  -A);
    prof.emplace_back(Rh, -A);
    prof.emplace_back(Rh, top_z);
    prof.emplace_back(0.,  top_z);
    return prof;
}

// The ball-on-a-neck cross section (touches the axis at both ends).
static std::vector<Vec2d> ball_profile(const FlexiJointParams &p)
{
    const double Rb = double(p.outer_radius);
    const double Rn = flexi_ball_neck_radius(p);
    const double zb = flexi_ball_centre_z(p);
    const double A  = flexi_anchor_depth(p) + double(flexi_effective_gap(p));
    // Where the neck cylinder meets the sphere.
    const double zj = zb - std::sqrt(std::max(0., Rb * Rb - Rn * Rn));

    std::vector<Vec2d> prof;
    prof.emplace_back(0.,  -A);
    prof.emplace_back(Rn, -A);
    prof.emplace_back(Rn, zj);
    // Spherical arc from the junction up over the pole. beta is measured from +Z.
    const double beta_j = M_PI - std::asin(std::min(1., Rn / Rb));
    for (int i = 0; i <= FLEXI_ARC_STEPS; ++ i) {
        const double beta = beta_j * (1. - double(i) / double(FLEXI_ARC_STEPS));
        prof.emplace_back(Rb * std::sin(beta), zb + Rb * std::cos(beta));
    }
    return prof;
}

// ------------------------------------------------------------------------------- 2D dilation

// Dilate a meridional profile by `delta` in the (r, z) half plane. For a solid of revolution
// the nearest point of the solid to any query point lies in the query point's own meridional
// half plane, so the 3D dilation by a ball of radius delta IS the revolution of this 2D
// dilation - which is why the measured clearance comes out exactly equal to delta.
//
// `touches_axis` profiles are mirrored to r < 0 first so the axis end caps get offset too,
// then clipped back to r >= 0.
static std::vector<Vec2d> dilate_profile(const std::vector<Vec2d> &prof, double delta, bool touches_axis)
{
    Polygon src;
    if (touches_axis) {
        // prof runs (0, z0) ... (0, z1); mirror the interior back down to close the loop.
        src.points.reserve(2 * prof.size());
        for (const Vec2d &v : prof)
            src.points.emplace_back(Point(coord_t(scale_(v.x())), coord_t(scale_(v.y()))));
        for (size_t i = prof.size() - 1; i > 0; -- i) {
            if (prof[i].x() <= EPSILON)
                continue;
            src.points.emplace_back(Point(coord_t(scale_(-prof[i].x())), coord_t(scale_(prof[i].y()))));
        }
    } else {
        src.points.reserve(prof.size());
        for (const Vec2d &v : prof)
            src.points.emplace_back(Point(coord_t(scale_(v.x())), coord_t(scale_(v.y()))));
    }
    if (src.area() < 0)
        src.reverse();

    Polygons out = offset(src, float(scale_(delta)), ClipperLib::jtMiter, 3.);
    if (out.empty())
        return prof;

    // Keep the biggest contour.
    size_t best = 0;
    double best_a = std::abs(out[0].area());
    for (size_t i = 1; i < out.size(); ++ i) {
        const double a = std::abs(out[i].area());
        if (a > best_a) { best_a = a; best = i; }
    }
    Polygon res = out[best];

    if (touches_axis) {
        // Clip to r >= 0 and snap the cut edge exactly onto the axis.
        const coord_t big = coord_t(scale_(1000.));
        Polygon half;
        half.points = { Point(0, -big), Point(big, -big), Point(big, big), Point(0, big) };
        Polygons clipped = intersection(Polygons{ res }, Polygons{ half });
        if (!clipped.empty()) {
            size_t b = 0;
            double ba = std::abs(clipped[0].area());
            for (size_t i = 1; i < clipped.size(); ++ i) {
                const double a = std::abs(clipped[i].area());
                if (a > ba) { ba = a; b = i; }
            }
            res = clipped[b];
        }
    }
    if (res.area() < 0)
        res.reverse();

    std::vector<Vec2d> ret;
    ret.reserve(res.points.size());
    for (const Point &pt : res.points) {
        double r = unscale<double>(pt.x());
        if (r < EPSILON)
            r = 0.;
        ret.emplace_back(r, unscale<double>(pt.y()));
    }
    return ret;
}

// ------------------------------------------------------------------------------------ bodies

// Lift a profile from the "female face at z == 0" frame into the cut frame.
static void shift_profiles(std::vector<std::vector<Vec2d>> &profiles, double dz)
{
    if (std::abs(dz) <= EPSILON)
        return;
    for (std::vector<Vec2d> &prof : profiles)
        for (Vec2d &v : prof)
            v.y() += dz;
}

std::vector<std::vector<Vec2d>> flexi_male_profiles(const FlexiJointParams &p)
{
    std::vector<std::vector<Vec2d>> out;
    if (p.kind == FlexiJointKind::DoubleRing) {
        out.push_back(ring_profile(p, double(p.ring_height)));
        if (p.hub_radius > 0.2f)
            out.push_back(hub_profile(p, double(p.ring_height)));
    } else {
        out.push_back(ball_profile(p));
    }
    shift_profiles(out, flexi_revolved_z_shift(p));
    return out;
}

std::vector<std::vector<Vec2d>> flexi_female_profiles(const FlexiJointParams &p)
{
    const double C = double(p.clearance);
    std::vector<std::vector<Vec2d>> out;
    if (p.kind == FlexiJointKind::DoubleRing) {
        out.push_back(dilate_profile(ring_profile(p, double(p.ring_height) + double(p.tilt)), C, false));
        if (p.hub_radius > 0.2f)
            out.push_back(dilate_profile(hub_profile(p, double(p.ring_height) + double(p.tilt)), C, true));
    } else {
        out.push_back(dilate_profile(ball_profile(p), C, true));
    }
    shift_profiles(out, flexi_revolved_z_shift(p));
    return out;
}

static std::vector<indexed_triangle_set> revolve_all(const std::vector<std::vector<Vec2d>> &profiles)
{
    std::vector<indexed_triangle_set> out;
    out.reserve(profiles.size());
    for (const std::vector<Vec2d> &prof : profiles)
        out.emplace_back(its_make_revolved(prof, FLEXI_SECTORS));
    return out;
}

std::vector<Vec3d> flexi_chain_lower_centreline(const FlexiJointParams &p)
{
    return p.kind == FlexiJointKind::ChainLink ? chain_lower_path(p) : std::vector<Vec3d>{};
}

std::vector<Vec3d> flexi_chain_upper_centreline(const FlexiJointParams &p)
{
    return p.kind == FlexiJointKind::ChainLink ? chain_upper_path(p) : std::vector<Vec3d>{};
}

// The LID half's bodies for a twist lock: the plug, and the thread strands or the lugs on it.
// Built lid-on-top and mirrored by twist_orient() when the lid is the lower half.
static std::vector<indexed_triangle_set> twist_lid_bodies(const FlexiJointParams &p)
{
    const double top = 0.5 * double(flexi_effective_gap(p));
    std::vector<indexed_triangle_set> out;
    if (p.kind == FlexiJointKind::Thread) {
        // The core cylinder: the minor radius, hanging down from ABOVE the lid face (so the
        // union with the lid half has real overlap to work with, never a coplanar kiss) to the
        // bottom of the threaded band.
        const double len = thread_plug_length(p);
        out.emplace_back(twist_cylinder(thread_minor_radius(p), top - len, top + 0.6));
        for (indexed_triangle_set &s : thread_strands(p, 0., 0.))
            out.emplace_back(std::move(s));
    } else {
        const double len = bayonet_plug_length(p);
        out.emplace_back(twist_cylinder(bayonet_plug_radius(p), top - len, top + 0.6));
        for (indexed_triangle_set &s : bayonet_lugs_bodies(p, 0.))
            out.emplace_back(std::move(s));
    }
    return twist_orient(p, std::move(out));
}

// The BODY half's bodies: only the bayonet has any - the detent bumps that stand back into the
// track after it was cut. The thread's body half is all relief, no added solid.
static std::vector<indexed_triangle_set> twist_body_bodies(const FlexiJointParams &)
{
    // Neither kind puts a solid body on the BODY half. The thread's is all relief, and the
    // bayonet's detent is left standing by its own track relief rather than added back as a
    // body - see bayonet_slot() for why a body would not survive.
    return {};
}

// What the BODY half has to be relieved by: the bore, and the groove or the slots in its wall.
static std::vector<indexed_triangle_set> twist_body_reliefs(const FlexiJointParams &p)
{
    const double C   = double(p.clearance);
    const double top = 0.5 * double(flexi_effective_gap(p));
    std::vector<indexed_triangle_set> out;
    if (p.kind == FlexiJointKind::Thread) {
        // THE BORE. It is the MINOR radius plus the clearance, NOT the major one. A bore wide
        // enough to swallow the male CREST swallows the groove with it - the groove spans from
        // the minor radius out to the major one, so a major-radius bore contains it entirely
        // and the female half comes out as a plain tube with no thread in it, which the lid
        // lifts straight out of. Sunk from a hair above the body's own face (so the mouth is
        // open, not skinned over) down past the end of the plug.
        out.emplace_back(twist_cylinder(thread_minor_radius(p) + C, top - thread_bore_depth(p), top + 0.6));
        // THE GROOVE: the SAME helix re-swept at the clearance-offset radius. Not a dilation of
        // the male mesh - a thread's meridional section changes with height, so the revolved
        // shortcut does not hold - but the same profile, pitch, starts, turns and phase, half a
        // turn longer at each end so the lead-in has somewhere to run out into.
        for (indexed_triangle_set &s : thread_strands(p, C, 0.5))
            out.emplace_back(std::move(s));
    } else {
        // The bore takes the plug plus its clearance...
        out.emplace_back(twist_cylinder(bayonet_plug_radius(p) + C, top - bayonet_plug_length(p) - C - 0.4,
                                        top + 0.6));
        // ... and each lug gets its L-shaped slot cut into the bore's wall.
        const int N = bayonet_lug_count(p);
        for (int i = 0; i < N; ++ i)
            for (indexed_triangle_set &s : bayonet_slot(p, i))
                out.emplace_back(std::move(s));
    }
    return twist_orient(p, std::move(out));
}

// What the LID half has to be relieved by: nothing for the thread (the male side is all solid),
// and for the bayonet the detent bumps grown by the clearance, so the lug's own path past them
// stays open in the lid's material rather than fusing.
static std::vector<indexed_triangle_set> twist_lid_reliefs(const FlexiJointParams &)
{
    return {};
}

std::vector<indexed_triangle_set> flexi_male_bodies(const FlexiJointParams &p)
{
    if (flexi_kind_is_twist(p.kind))
        // "Male" is the LOWER half by the family's convention, and for a twist lock the lower
        // half is the lid only when the lid is not upper.
        return twist_lid_is_upper(p) ? twist_body_bodies(p) : twist_lid_bodies(p);
    if (p.kind == FlexiJointKind::Hinge) {
        // The hinge's "male" body is whatever the LOWER segment owns: its own knuckles, plus
        // the ONE continuous pin when the fold side leaves the pin down here. They are
        // returned as separate components because the cut pipeline unions the list entry by
        // entry - the same way the ring and its hub are separate - and that union is what
        // fuses the pin to its knuckles in the finished half.
        std::vector<indexed_triangle_set> out = hinge_knuckles(p, true);
        // The pin travels with the half that owns knuckle 0 - which the fold-side flag is
        // exactly what decides. hinge_knuckle_is_lower() already folds the flag in, so asking
        // it about knuckle 0 is the whole test.
        if (hinge_knuckle_is_lower(p, 0))
            out.emplace_back(hinge_pin(p, 0.5 * double(p.hinge_pin_dia)));
        return out;
    }
    if (p.kind == FlexiJointKind::ChainLink)
        // The chain link's "male" body is the LOWER ring, in the plane spanned by (n, d).
        // There is no revolved profile for it.
        return { its_make_swept_loop(chain_lower_path(p), double(p.wire), FLEXI_WIRE_SECTORS) };
    return revolve_all(flexi_male_profiles(p));
}

std::vector<indexed_triangle_set> flexi_female_cavities(const FlexiJointParams &p)
{
    if (flexi_kind_is_twist(p.kind))
        return twist_lid_is_upper(p) ? twist_lid_bodies(p) : twist_body_bodies(p);
    if (p.kind == FlexiJointKind::Hinge) {
        // ... and its "female" body is what the UPPER segment owns: the other parity of
        // knuckle, plus the pin if the fold side put it up here. Like the chain link's, these
        // are SOLID bodies to union in, not a cavity to subtract. The BORE is not cut here:
        // it falls out of the relief pass' inflated pin, which the cut pipeline subtracts from
        // this half right after unioning these in. That keeps "the body" and "the clearance"
        // two independent, separately testable steps, exactly as the ring's lip and groove.
        std::vector<indexed_triangle_set> out = hinge_knuckles(p, false);
        if (!hinge_knuckle_is_lower(p, 0))
            out.emplace_back(hinge_pin(p, 0.5 * double(p.hinge_pin_dia)));
        return out;
    }
    if (p.kind == FlexiJointKind::ChainLink)
        // ... and its "female" body is the UPPER ring, in the perpendicular plane spanned by
        // (n, n x d). It is a solid body, not a cavity; flexi_upper_reliefs() carries what
        // gets subtracted.
        return { its_make_swept_loop(chain_upper_path(p), double(p.wire), FLEXI_WIRE_SECTORS) };
    return revolve_all(flexi_female_profiles(p));
}

std::vector<indexed_triangle_set> flexi_lower_bodies(const FlexiJointParams &p)
{
    // Both families put a solid body on the lower (male) segment.
    return flexi_male_bodies(p);
}

std::vector<indexed_triangle_set> flexi_upper_bodies(const FlexiJointParams &p)
{
    // Only the chain link puts a solid body on the upper segment; the revolved kinds put a
    // cavity there instead.
    if (p.kind == FlexiJointKind::ChainLink || p.kind == FlexiJointKind::Hinge ||
        flexi_kind_is_twist(p.kind))
        return flexi_female_cavities(p);
    return {};
}

std::vector<indexed_triangle_set> flexi_lower_reliefs(const FlexiJointParams &p)
{
    if (flexi_kind_is_twist(p.kind))
        return twist_lid_is_upper(p) ? twist_body_reliefs(p) : twist_lid_reliefs(p);
    // The lower segment has to make room for the OTHER ring (the upper one) wherever its free
    // end dips near the lower body. A tube offset uniformly by C is simply a fatter tube, so the
    // relief is the same centreline swept with radius wire + C - which is why the measured
    // clearance comes out exactly C here too, with no Clipper dilation involved.
    if (p.kind == FlexiJointKind::Hinge) {
        // The lower segment has to make room for everything the UPPER half owns: that half's
        // knuckles grown by the clearance in every direction - radius + C, and C longer at
        // each end face - plus the pin, when the fold-side flag has put the pin up there. A
        // cylinder offset uniformly is still a cylinder, so the measured clearance comes out
        // exactly C here too, with no Clipper dilation involved.
        std::vector<indexed_triangle_set> out = hinge_inflated_knuckles(p, false);
        if (!hinge_knuckle_is_lower(p, 0))
            out.emplace_back(hinge_pin(p, 0.5 * double(p.hinge_pin_dia) + double(p.clearance)));
        return out;
    }
    if (p.kind == FlexiJointKind::ChainLink)
        return { its_make_swept_loop(chain_upper_path(p), double(p.wire) + double(p.clearance),
                                     FLEXI_WIRE_SECTORS) };
    return {};
}

std::vector<indexed_triangle_set> flexi_upper_reliefs(const FlexiJointParams &p)
{
    if (flexi_kind_is_twist(p.kind))
        return twist_lid_is_upper(p) ? twist_lid_reliefs(p) : twist_body_reliefs(p);
    if (p.kind == FlexiJointKind::Hinge) {
        // The upper segment clears everything the LOWER half owns: that half's knuckles
        // inflated by C, plus - when the pin lives down there - the pin inflated by C along
        // its whole length, which IS the bore, drilled through every knuckle on this side at
        // exactly pin radius + C. The knuckle inflation is also what holds the inter-knuckle
        // gap open: each cylinder was built `gap` short of its slot and is now C fatter.
        std::vector<indexed_triangle_set> out = hinge_inflated_knuckles(p, true);
        if (hinge_knuckle_is_lower(p, 0))
            out.emplace_back(hinge_pin(p, 0.5 * double(p.hinge_pin_dia) + double(p.clearance)));
        return out;
    }
    if (p.kind == FlexiJointKind::ChainLink)
        return { its_make_swept_loop(chain_lower_path(p), double(p.wire) + double(p.clearance),
                                     FLEXI_WIRE_SECTORS) };
    // Revolved kinds: the cavity IS the relief.
    return flexi_female_cavities(p);
}

indexed_triangle_set flexi_preview_body(const FlexiJointParams &p)
{
    indexed_triangle_set out;
    if (flexi_kind_is_twist(p.kind)) {
        // Show the male fastener - the threaded plug, or the lugged one - because that is the
        // envelope the user is placing against the cut face.
        for (const indexed_triangle_set &its : twist_lid_bodies(p))
            its_merge(out, its);
        return out;
    }
    if (p.kind == FlexiJointKind::Hinge) {
        // Show the whole barrel run - every knuckle of both halves, plus the pin - because
        // that is the envelope the user is positioning against the cut face's edge.
        for (const indexed_triangle_set &its : flexi_male_bodies(p))
            its_merge(out, its);
        for (const indexed_triangle_set &its : flexi_female_cavities(p))
            its_merge(out, its);
        return out;
    }
    if (p.kind == FlexiJointKind::ChainLink) {
        // Show both interlocked loops, at their clearance-inflated size, because that is the
        // envelope that actually has to fit inside the cut cross section.
        for (const indexed_triangle_set &its : flexi_lower_reliefs(p))
            its_merge(out, its);
        for (const indexed_triangle_set &its : flexi_upper_reliefs(p))
            its_merge(out, its);
        return out;
    }
    // The revolved kinds: the female cavity envelope is the male body plus the clearance.
    for (const indexed_triangle_set &its : flexi_female_cavities(p))
        its_merge(out, its);
    return out;
}

// ------------------------------------------------------------------------------- footprints
//
// What the joint actually occupies ON THE CUT PLANE, so the gizmo can ask "does this fit
// inside the cut contour?" about the real shape instead of a circumscribing circle. The old
// circle test rejects a long thin joint that fits the contour perfectly well - which is what
// the chain link was hitting: its outer extent is the distance to the far end of a 7 mm loop,
// so a disc of that radius sticks out of contours the loops themselves clear easily.

std::vector<Vec2d> chain_footprint_corners(const FlexiJointParams &p, double pad)
{
    // Both rings' centrelines projected onto the cut plane, grown by the wire radius and the
    // clearance: that is the slot the interlocked pair sweeps through the plane. A rectangle
    // around it is a tight, safe approximation - it is what the pair actually needs, and it is
    // emphatically not a disc. It already carries the pair's Rotation, because the paths do.
    double xlo = 0., xhi = 0., ylo = 0., yhi = 0.;
    bool   first = true;
    for (const std::vector<Vec3d> &path : { chain_lower_path(p), chain_upper_path(p) })
        for (const Vec3d &v : path) {
            if (first) { xlo = xhi = v.x(); ylo = yhi = v.y(); first = false; continue; }
            xlo = std::min(xlo, v.x()); xhi = std::max(xhi, v.x());
            ylo = std::min(ylo, v.y()); yhi = std::max(yhi, v.y());
        }
    const double m = double(p.wire) + double(p.clearance) + pad;
    xlo -= m; xhi += m; ylo -= m; yhi += m;
    return { Vec2d(xlo, ylo), Vec2d(xhi, ylo), Vec2d(xhi, yhi), Vec2d(xlo, yhi) };
}

std::vector<Vec2d> flexi_footprint_corners(const FlexiJointParams &p, double pad)
{
    // A twist lock IS round - and the circle that has to fit is not the male thread's but the
    // OUTER WALL of the female bore, which flexi_outer_extent() already reports, so it falls
    // through to the disc case below with the right radius.
    if (p.kind == FlexiJointKind::Hinge)
        return hinge_footprint_corners(p, pad);
    if (p.kind == FlexiJointKind::ChainLink)
        return chain_footprint_corners(p, pad);
    // The revolved kinds really are round: sample the disc the gizmo used to sample by hand.
    const double r = double(flexi_outer_extent(p)) + pad;
    std::vector<Vec2d> out;
    const int          n = 60;
    out.reserve(n);
    for (int i = 0; i < n; ++ i) {
        const double a = 2. * M_PI * double(i) / double(n);
        out.emplace_back(r * std::cos(a), r * std::sin(a));
    }
    return out;
}

// ------------------------------------------------------------------------------------ guards

std::string flexi_validate(const FlexiJointParams &p)
{
    if (p.kind != FlexiJointKind::ChainLink && p.kind != FlexiJointKind::Hinge &&
        !flexi_kind_is_twist(p.kind) && p.outer_radius <= 0.5f)
        return "Joint radius is too small.";
    if (p.clearance <= 0.f)
        return "Clearance must be greater than zero, otherwise the joint prints as one solid.";
    if (p.gap > 0.f && p.gap < p.clearance)
        return "Gap cannot be smaller than the clearance.";
    if (flexi_kind_is_twist(p.kind)) {
        if (p.thread_major_dia < 4.f)
            return "Major diameter is too small: a twist lock under 4 mm across has no wall "
                   "left once the clearance is taken out.";
        if (p.rotation < 0.f || p.rotation > 360.f)
            return "Rotation must be between 0 and 360 degrees.";
        if (p.kind == FlexiJointKind::Thread) {
            if (p.thread_starts < 1 || p.thread_starts > 4)
                return "A thread needs between 1 and 4 starts.";
            if (p.thread_turns <= 0.f)
                return "Turns must be greater than zero.";
            // THE PRINTABILITY FLOOR. Below about 2 mm of pitch the crest is thinner than two
            // perimeters on a 0.4 mm nozzle and prints weak or not at all.
            if (p.thread_pitch < 1.f)
                return "Pitch is too fine to print: keep it at 1 mm or more (2 mm or more is "
                       "the recommended floor on a 0.4 mm nozzle).";
            // THE CREST-SPACING GUARD. One crest sits pitch / starts above the next, and the
            // groove cut for the thread is the profile grown by the clearance at each end. If
            // the two together do not fit in that spacing, the groove's coils merge into a
            // plain annular cavity, the thread holds nothing and the lid pulls straight out -
            // so refuse it here rather than shipping a lid that falls off. thread_depth() caps
            // itself to keep this true; what is refused is the case where even the smallest
            // buildable depth will not fit.
            const double S     = thread_crest_spacing(p);
            const double grow  = thread_groove_growth(p);
            const double flats = 0.25 * S;
            if (flats + grow >= 0.9 * S)
                return "This pitch, start count and clearance leave no wall between one thread "
                       "and the next: the groove's turns would run into each other and the lid "
                       "would pull straight out. Raise the pitch, reduce the starts, or reduce "
                       "the clearance.";
            // The male core has to be left with real wall under the thread root.
            if (thread_minor_radius(p) < 1.2)
                return "Major diameter is too small for this pitch: the thread eats the whole "
                       "core. Raise the diameter, or lower the pitch.";
            if (p.thread_lead_turns < 0.f || double(p.thread_lead_turns) > 0.5 * double(p.thread_turns))
                return "The lead-in cannot be longer than half the thread: it is tapered at "
                       "both ends.";
            return std::string();
        }
        // Bayonet.
        if (p.bayonet_lugs < 2 || p.bayonet_lugs > 4)
            return "A bayonet needs between 2 and 4 lugs.";
        if (p.bayonet_lug_height <= 2.f * p.clearance)
            return "Lug height is too small for this clearance: the lug would vanish into its "
                   "own slot.";
        if (p.bayonet_lug_thickness <= 2.f * p.clearance)
            return "Lug thickness is too small for this clearance.";
        if (bayonet_plug_radius(p) <= 1.0)
            return "The lugs are taller than the plug is wide: reduce the lug height, or raise "
                   "the major diameter.";
        // Entry channel, lug and track all live on the same 360/N slice of the bore wall, and
        // they have to leave wall between them or the female half is a slotted ring.
        {
            // The REQUESTED lock angle, not the one bayonet_effective_lock_angle() has already
            // clamped down to fit: if the clamp is doing work, the user is not getting the lock
            // they asked for, and that is exactly what this refuses rather than silently
            // delivering something else.
            const double span = 360. / double(bayonet_lug_count(p));
            const double want = std::max(30., std::min(120., double(p.bayonet_lock_angle)));
            if (double(p.bayonet_lug_arc) + want + 8. > span)
                return "These lugs are too wide, or the lock angle is too large, for this lug "
                       "count: each lug's slot and track have to fit in its own share of the "
                       "bore with wall to spare.";
        }
        if (p.bayonet_entry_depth < p.bayonet_lug_thickness)
            return "The entry channel has to be at least as deep as the lug is thick, or the "
                   "lug cannot drop in far enough to turn.";
        if (p.bayonet_detent < 0.f)
            return "The detent height cannot be negative.";
        if (double(p.bayonet_detent) >= double(p.bayonet_lug_height))
            return "The detent is as tall as the lug: it would block the track instead of "
                   "clicking.";
        return std::string();
    }
    if (p.kind == FlexiJointKind::Hinge) {
        if (p.hinge_knuckles < 1 || p.hinge_knuckles > 9)
            return "A hinge needs between 1 and 9 knuckles.";
        if (p.hinge_pin_dia <= 0.f)
            return "Pin diameter must be greater than zero.";
        // The barrel has to hold the bore AND leave a wall around it, or the knuckle prints
        // as a ring of nothing.
        if (double(p.hinge_barrel_dia) <= 2. * hinge_bore_radius(p) + 0.4)
            return "Barrel diameter is too small for this pin and clearance: it has to clear "
                   "the pin, twice the clearance and a wall on each side.";
        if (p.hinge_length <= 0.f)
            return "Hinge length must be greater than zero.";
        // Every knuckle loses `gap` of its slot to the end-face clearances; below that the
        // knuckles vanish and the hinge is just a loose pin.
        if (hinge_slot_length(p) <= double(flexi_effective_gap(p)) + 0.2)
            return "Too many knuckles for this hinge length and gap: each knuckle needs more "
                   "than the gap plus 0.2 mm of length.";
        if (p.rotation < 0.f || p.rotation > 180.f)
            return "Rotation must be between 0 and 180 degrees.";
        return std::string();
    }
    if (p.kind == FlexiJointKind::ChainLink) {
        if (p.wire <= 0.f)
            return "Wire thickness must be greater than zero.";
        const float need = flexi_min_link_size(p);
        if (p.link_length < need || p.link_width < need)
            return "The loops are too small for this wire and clearance: each loop's opening "
                   "has to pass the other loop's wire, which needs a length and a width of at "
                   "least 4 x wire + 2 x clearance.";
        if (p.link_length < p.link_width)
            return "Link length must be at least the link width.";
        if (p.tilt_angle < 0.f || p.tilt_angle > 30.f)
            return "The upper ring's tilt must be between 0 and 30 degrees.";
        if (p.stem <= 0.f)
            return "Stem depth must be greater than zero, or the ring is not attached.";
        if (p.rotation < 0.f || p.rotation > 180.f)
            return "Rotation must be between 0 and 180 degrees.";
        // Both rings straddle the cut plane, each centred `off` into its own half. The three
        // constraints on `off` (clear its own face, embed `stem` deep, still thread the other
        // ring) have to leave a value that satisfies them all - see chain_frame().
        const ChainLinkFrame f = chain_frame(p);
        if (f.off_max < f.off_min || f.interlock <= 0.)
            return "The rings cannot interlock with these proportions: each has to sit clear of "
                   "its own cut face and still reach through the other ring's hole. Lengthen "
                   "the link, or reduce the gap, the stem or the wire.";
        return std::string();
    }
    if (p.kind == FlexiJointKind::DoubleRing) {
        if (p.ring_width <= 2.f * p.clearance)
            return "Ring width is too small for this clearance.";
        if (p.ring_height <= 2.f * p.clearance)
            return "Ring height is too small for this clearance.";
        if (p.ring_width >= p.outer_radius)
            return "Ring width must be smaller than the outer radius.";
        if (p.neck_ratio <= 0.05f || p.neck_ratio >= 0.95f)
            return "Neck ratio must be between 0.05 and 0.95.";
        if (p.hub_radius > 0.f && p.hub_radius + p.clearance >= p.outer_radius - p.ring_width)
            return "Hub radius leaves no room between the hub and the ring.";
    } else {
        if (p.open_angle < 10.f || p.open_angle > 75.f)
            return "Socket opening angle must be between 10 and 75 degrees.";
    }
    if (!flexi_is_captive(p))
        return "These parameters do not capture the joint: the mouth is as wide as the lip.";
    return std::string();
}

} // namespace Slic3r
