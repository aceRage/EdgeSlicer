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
// The frame, in the cut frame (cut plane z == 0, faces at -+ gap/2):
//   VERTICAL loop  (LOWER segment): in the x-z plane (normal +Y), long axis +Z, centre
//       (x_shift, 0, vcz) with vcz = -gap/2 - stem + L/2, so its bottom centreline extremity
//       sits `stem` below the lower face and the loop is anchored in the lower body.
//   HORIZONTAL loop (UPPER segment): long axis +X, tilted up by tilt_angle about +Y, plane
//       at z = +gap/2 + wire so it lies immediately on top of the upper face, centre
//       (x_shift + L/2, 0, hcz). The +L/2 offset is what nests its near cap inside the
//       vertical loop's hole; the far (+x) end rises into the upper body.
//   x_shift centres the whole assembly on the joint axis, so the joint is not lopsided and
//       the gizmo's "fits in the cross section" test means what it says.

struct ChainLinkFrame
{
    double L{ 0. }, W{ 0. }, t{ 0. };
    double gap{ 0. }, face_lo{ 0. }, face_hi{ 0. };
    double vcz{ 0. };          // vertical loop centre z
    double hcz{ 0. }, hcx{ 0. };  // horizontal loop centre
    double shift{ 0. };        // x shift that centres the assembly
    double vert_top{ 0. };     // topmost vertical centreline z
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
    f.vcz     = f.face_lo - double(p.stem) + 0.5 * f.L;
    f.hcz     = f.face_hi + f.t;
    f.hcx     = 0.5 * f.L;
    const double ca = std::cos(double(p.tilt_angle) * M_PI / 180.);
    // The union spans x from -W/2 - t (the vertical loop's near side) to hcx + L/2*ca + t.
    const double xlo = -0.5 * f.W - f.t;
    const double xhi = f.hcx + 0.5 * f.L * ca + f.t;
    f.shift   = -0.5 * (xlo + xhi);
    f.vert_top = f.vcz + 0.5 * f.L;
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

// The vertical loop's centreline: the stadium's long axis (a) maps to +Z, its width (b) to X.
static std::vector<Vec3d> chain_vertical_path(const FlexiJointParams &p)
{
    const ChainLinkFrame f = chain_frame(p);
    std::vector<Vec3d>   out;
    for (const Vec2d &ab : stadium_path(f.L, f.W, FLEXI_CAP_STEPS))
        out.emplace_back(ab.y() + f.shift, 0., f.vcz + ab.x());
    return out;
}

// The horizontal loop's centreline: long axis (a) to +X tilted up about +Y, width (b) to Y.
static std::vector<Vec3d> chain_horizontal_path(const FlexiJointParams &p)
{
    const ChainLinkFrame f  = chain_frame(p);
    const double         ca = std::cos(double(p.tilt_angle) * M_PI / 180.);
    const double         sa = std::sin(double(p.tilt_angle) * M_PI / 180.);
    std::vector<Vec3d>   out;
    for (const Vec2d &ab : stadium_path(f.L, f.W, FLEXI_CAP_STEPS))
        out.emplace_back(f.hcx + ab.x() * ca + f.shift, ab.y(), f.hcz + ab.x() * sa);
    return out;
}

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
    if (p.kind == FlexiJointKind::ChainLink) {
        // The tallest thing above the cut plane is the vertical loop's top.
        const ChainLinkFrame f = chain_frame(p);
        return float(f.vert_top + double(p.wire));
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
    if (p.kind == FlexiJointKind::ChainLink) {
        double best = 0.;
        for (const std::vector<Vec3d> &path : { chain_vertical_path(p), chain_horizontal_path(p) })
            for (const Vec3d &v : path)
                best = std::max(best, std::hypot(v.x(), v.y()));
        return float(best + double(p.wire) + double(p.clearance));
    }
    return p.outer_radius + p.clearance;
}

float flexi_mouth_half_width(const FlexiJointParams &p)
{
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

std::vector<indexed_triangle_set> flexi_male_bodies(const FlexiJointParams &p)
{
    if (p.kind == FlexiJointKind::ChainLink)
        // The chain link's "male" body is the vertical loop, which belongs to the LOWER
        // segment. There is no revolved profile for it.
        return { its_make_swept_loop(chain_vertical_path(p), double(p.wire), FLEXI_WIRE_SECTORS) };
    return revolve_all(flexi_male_profiles(p));
}

std::vector<indexed_triangle_set> flexi_female_cavities(const FlexiJointParams &p)
{
    if (p.kind == FlexiJointKind::ChainLink)
        // ... and its "female" body is the horizontal loop, on the UPPER segment. It is a
        // solid body, not a cavity; flexi_upper_reliefs() carries what gets subtracted.
        return { its_make_swept_loop(chain_horizontal_path(p), double(p.wire), FLEXI_WIRE_SECTORS) };
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
    if (p.kind == FlexiJointKind::ChainLink)
        return flexi_female_cavities(p);
    return {};
}

std::vector<indexed_triangle_set> flexi_lower_reliefs(const FlexiJointParams &p)
{
    // The lower segment has to make room for the OTHER loop (the horizontal one) wherever it
    // dips near the lower body. A tube offset uniformly by C is simply a fatter tube, so the
    // relief is the same centreline swept with radius wire + C - which is why the measured
    // clearance comes out exactly C here too, with no Clipper dilation involved.
    if (p.kind == FlexiJointKind::ChainLink)
        return { its_make_swept_loop(chain_horizontal_path(p), double(p.wire) + double(p.clearance),
                                     FLEXI_WIRE_SECTORS) };
    return {};
}

std::vector<indexed_triangle_set> flexi_upper_reliefs(const FlexiJointParams &p)
{
    if (p.kind == FlexiJointKind::ChainLink)
        return { its_make_swept_loop(chain_vertical_path(p), double(p.wire) + double(p.clearance),
                                     FLEXI_WIRE_SECTORS) };
    // Revolved kinds: the cavity IS the relief.
    return flexi_female_cavities(p);
}

indexed_triangle_set flexi_preview_body(const FlexiJointParams &p)
{
    indexed_triangle_set out;
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

// ------------------------------------------------------------------------------------ guards

std::string flexi_validate(const FlexiJointParams &p)
{
    if (p.kind != FlexiJointKind::ChainLink && p.outer_radius <= 0.5f)
        return "Joint radius is too small.";
    if (p.clearance <= 0.f)
        return "Clearance must be greater than zero, otherwise the joint prints as one solid.";
    if (p.gap > 0.f && p.gap < p.clearance)
        return "Gap cannot be smaller than the clearance.";
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
            return "The horizontal loop's tilt must be between 0 and 30 degrees.";
        if (p.stem <= 0.f)
            return "Stem depth must be greater than zero, or the loop is not attached.";
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
