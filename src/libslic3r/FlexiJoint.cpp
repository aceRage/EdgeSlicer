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

// How far the male body reaches DOWN into the male half. It only has to be deep enough for a
// robust union with the male half (no coplanar faces), so it scales with the joint.
static double flexi_anchor_depth(const FlexiJointParams &p)
{
    const double h = double(flexi_protrusion_height(p));
    return std::max(0.6, 0.4 * h);
}

// ---------------------------------------------------------------------------------- defaults

float flexi_default_hub_radius(const FlexiJointParams &p)
{
    const float r = p.outer_radius - p.ring_width - p.clearance - p.ring_width;
    return r > 0.2f ? r : 0.f;
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
    }
    return p;
}

// ------------------------------------------------------------------------------- key measures

float flexi_protrusion_height(const FlexiJointParams &p)
{
    if (p.kind == FlexiJointKind::DoubleRing)
        return p.ring_height;
    // Ball & socket: the ball centre sits above the mouth, plus the ball radius.
    const double C  = double(p.clearance);
    const double Rb = double(p.outer_radius);
    const double th = double(p.open_angle) * M_PI / 180.;
    const double zb = C + (Rb + C) * std::cos(th) + 0.2;
    return float(zb + Rb);
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
    if (p.kind == FlexiJointKind::DoubleRing)
        return p.outer_radius + p.clearance;
    return p.outer_radius + p.clearance;
}

float flexi_mouth_half_width(const FlexiJointParams &p)
{
    if (p.kind == FlexiJointKind::DoubleRing)
        return 0.5f * p.ring_width * p.neck_ratio + p.clearance;
    return float(flexi_ball_neck_radius(p)) + p.clearance;
}

float flexi_lip_half_width(const FlexiJointParams &p)
{
    if (p.kind == FlexiJointKind::DoubleRing)
        return 0.5f * p.ring_width;
    return p.outer_radius;
}

// ---------------------------------------------------------------------------------- profiles

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
    const double A  = flexi_anchor_depth(p);

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
    const double A  = flexi_anchor_depth(p);
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
    const double A  = flexi_anchor_depth(p);
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
    return revolve_all(flexi_male_profiles(p));
}

std::vector<indexed_triangle_set> flexi_female_cavities(const FlexiJointParams &p)
{
    return revolve_all(flexi_female_profiles(p));
}

indexed_triangle_set flexi_preview_body(const FlexiJointParams &p)
{
    // The preview shows the female cavity envelope: it is the male body plus the clearance,
    // so it is what actually has to fit inside the cut cross section.
    indexed_triangle_set out;
    for (const indexed_triangle_set &its : flexi_female_cavities(p))
        its_merge(out, its);
    return out;
}

// ------------------------------------------------------------------------------------ guards

std::string flexi_validate(const FlexiJointParams &p)
{
    if (p.outer_radius <= 0.5f)
        return "Joint radius is too small.";
    if (p.clearance <= 0.f)
        return "Clearance must be greater than zero, otherwise the joint prints as one solid.";
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
