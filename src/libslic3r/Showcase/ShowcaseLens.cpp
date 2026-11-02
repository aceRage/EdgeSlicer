#include "ShowcaseLens.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r {
namespace Showcase {

namespace {

constexpr double LENS_PI = 3.14159265358979323846;

double  lens_dot(const LensVec &a, const LensVec &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
LensVec lens_sub(const LensVec &a, const LensVec &b) { return { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; }
LensVec lens_add(const LensVec &a, const LensVec &b) { return { a[0] + b[0], a[1] + b[1], a[2] + b[2] }; }
LensVec lens_mul(const LensVec &a, double k) { return { a[0] * k, a[1] * k, a[2] * k }; }

} // namespace

// ---- The floor ----------------------------------------------------------------------------------

bool ground_projection_active(const Scene &s)
{
    return s.background.type == BackgroundType::Environment && s.background.ground && !s.lighting.environment.empty();
}

FloorType effective_floor_type(const Scene &s)
{
    if (s.floor.type == FloorType::None)
        return FloorType::None;
    if (s.background.type == BackgroundType::Backplate || ground_projection_active(s))
        return FloorType::ShadowCatcher;
    return s.floor.type;
}

// ---- The environment backdrop -------------------------------------------------------------------

double GroundDome::floor_radius() const { return std::sqrt(std::max(0., radius * radius - height * height)); }

double ground_auto_height(double size_x, double size_y, double size_z)
{
    const double extent = std::max(std::max(size_z, 0.), 0.5 * std::sqrt(size_x * size_x + size_y * size_y));
    const double h      = 3. * extent;
    return std::isfinite(h) ? std::clamp(h, 60., 5000.) : 60.;
}

GroundDome ground_dome(const Background &bg, double hdri_height_mm, double hdri_radius_mm, const LensVec &model_min, const LensVec &model_max)
{
    GroundDome d;
    const double h0 = bg.ground_height > 0.f ?
                          double(bg.ground_height) :
                          ground_auto_height(model_max[0] - model_min[0], model_max[1] - model_min[1], model_max[2] - model_min[2]);
    const double ratio = hdri_height_mm > 0. && hdri_radius_mm > 0. ? hdri_radius_mm / hdri_height_mm : 10.;
    const double r0    = bg.ground_radius > 0.f ? double(bg.ground_radius) : h0 * ratio;
    const double scale = bg.ground_scale > 0.f ? double(bg.ground_scale) : 1.;
    d.height    = h0 * scale;
    d.radius    = std::max(r0 * scale, 1.25 * d.height);
    d.center    = { 0.5 * (model_min[0] + model_max[0]), 0.5 * (model_min[1] + model_max[1]), d.height };
    return d;
}

LensVec ground_lookup(const GroundDome &dome, const LensVec &o, const LensVec &d, double *distance)
{
    const LensVec &c = dome.center;
    // The floor disc first: from above the floor, a ray going down that lands inside it.
    if (d[2] < 0. && o[2] > 0.) {
        const double tg = -o[2] / d[2];
        const LensVec p = lens_add(o, lens_mul(d, tg));
        const double qx = p[0] - c[0], qy = p[1] - c[1];
        const double fr = dome.floor_radius();
        if (qx * qx + qy * qy <= fr * fr) {
            if (distance != nullptr)
                *distance = tg;
            return lens_sub(p, c);
        }
    }
    // Then the dome: the far side of the sphere (the camera is normally inside it), above the floor.
    const LensVec oc   = lens_sub(o, c);
    const double  b    = lens_dot(oc, d);
    const double  k    = lens_dot(oc, oc) - dome.radius * dome.radius;
    const double  disc = b * b - k;
    if (disc >= 0.) {
        const double t = -b + std::sqrt(disc);
        if (t > 0.) {
            const LensVec p = lens_add(o, lens_mul(d, t));
            if (p[2] >= 0.) {
                if (distance != nullptr)
                    *distance = t;
                return lens_sub(p, c);
            }
        }
    }
    if (distance != nullptr)
        *distance = std::numeric_limits<double>::infinity();
    return d;
}

std::array<double, 9> backdrop_pitch_matrix(double pitch_deg, const LensVec &forward)
{
    std::array<double, 9> m{ { 1., 0., 0., 0., 1., 0., 0., 0., 1. } };
    // The right axis: forward x Z (horizontal).
    LensVec      r{ { forward[1], -forward[0], 0. } };
    const double len = std::sqrt(r[0] * r[0] + r[1] * r[1]);
    if (!std::isfinite(pitch_deg) || pitch_deg == 0. || len < 1e-9)
        return m;
    r = lens_mul(r, 1. / len);
    // Rodrigues: a rotation by +angle about r (right-handed) turns forward towards +Z.
    const double a = pitch_deg * LENS_PI / 180.;
    const double c = std::cos(a), s = std::sin(a), t = 1. - c;
    const double x = r[0], y = r[1], z = r[2];
    m = { t * x * x + c,     t * x * y - s * z, t * x * z + s * y,
          t * x * y + s * z, t * y * y + c,     t * y * z - s * x,
          t * x * z - s * y, t * y * z + s * x, t * z * z + c };
    return m;
}

LensVec apply_matrix(const std::array<double, 9> &m, const LensVec &v)
{
    return { m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2], m[6] * v[0] + m[7] * v[1] + m[8] * v[2] };
}

// ---- Camera match -------------------------------------------------------------------------------

double half_sensor_height_mm(double aspect)
{
    if (!std::isfinite(aspect) || aspect <= 0.)
        aspect = 1.5;
    return FULL_FRAME_HALF_DIAGONAL_MM / std::sqrt(aspect * aspect + 1.);
}

double fov_y_from_focal(double focal_mm, double aspect)
{
    focal_mm = std::isfinite(focal_mm) ? std::clamp(focal_mm, 1., 100000.) : 50.;
    return 2. * std::atan(half_sensor_height_mm(aspect) / focal_mm);
}

double focal_from_fov_y(double fov_y, double aspect)
{
    const double t = std::tan(0.5 * std::clamp(fov_y, 1e-6, LENS_PI - 1e-6));
    return half_sensor_height_mm(aspect) / t;
}

double pitch_for_horizon(double horizon, double fov_y)
{
    return std::atan((2. * horizon - 1.) * std::tan(0.5 * fov_y));
}

double horizon_for_pitch(double pitch, double fov_y)
{
    const double t = std::tan(0.5 * fov_y);
    return t > 0. ? 0.5 + 0.5 * std::tan(pitch) / t : 0.5;
}

double distance_for_fov(double frame_h_px, double zoom_px_per_mm, double fov_y)
{
    const double t = std::tan(0.5 * fov_y);
    if (!(frame_h_px > 0.) || !(zoom_px_per_mm > 0.) || !(t > 0.))
        return 0.;
    return frame_h_px / (2. * zoom_px_per_mm * t);
}

LensVec matched_eye(const LensVec &target, const LensVec &forward, double pitch, double distance)
{
    double       hx = forward[0], hy = forward[1];
    const double hl = std::sqrt(hx * hx + hy * hy);
    if (hl < 1e-9) {
        hx = 0.;
        hy = 1.;
    } else {
        hx /= hl;
        hy /= hl;
    }
    const LensVec f{ { std::cos(pitch) * hx, std::cos(pitch) * hy, -std::sin(pitch) } };
    return lens_sub(target, lens_mul(f, distance));
}

// ---- Depth of field -----------------------------------------------------------------------------

int dof_taps(BokehQuality q)
{
    switch (q) {
    case BokehQuality::Draft: return 32;
    case BokehQuality::Best: return 128;
    default:
    case BokehQuality::Good: return 64;
    }
}

DofSetup dof_setup(const DepthOfField &dof, double p11_frame, double frame_aspect, double frame_h_px, double focus_distance)
{
    DofSetup s;
    if (!dof.enabled || !(p11_frame > 0.) || !(frame_h_px > 0.) || !(focus_distance > 0.) || !std::isfinite(focus_distance))
        return s;
    const double fov_y = 2. * std::atan(1. / p11_frame);
    s.focal_mm         = focal_from_fov_y(fov_y, frame_aspect);
    const double n     = std::clamp(double(dof.f_number), 1., 32.);
    const double aperture = s.focal_mm / n;   // mm
    // Pixels per mm on the plane of focus: p11 * (frame_h / 2) / focus.
    s.focus  = focus_distance;
    s.k_px   = aperture * p11_frame * frame_h_px / (2. * focus_distance);
    s.max_px = DOF_MAX_COC * frame_h_px;
    s.taps   = dof_taps(dof.quality);
    s.active = s.k_px > 0.25 && std::isfinite(s.k_px);
    return s;
}

double coc_px(const DofSetup &setup, double z)
{
    if (!setup.active)
        return 0.;
    if (!(z > 0.) || !std::isfinite(z))
        return std::min(setup.k_px, setup.max_px);
    return std::min(setup.k_px * std::abs(1. - setup.focus / z), setup.max_px);
}

} // namespace Showcase
} // namespace Slic3r
