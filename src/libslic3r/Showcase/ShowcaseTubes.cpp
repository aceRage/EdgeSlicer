#include "ShowcaseTubes.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

namespace Slic3r {
namespace Showcase {

namespace {

constexpr double TUBE_PI = 3.14159265358979323846;

using tube_v3 = std::array<double, 3>;

tube_v3 tube_sub(const tube_v3 &a, const tube_v3 &b) { return { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; }
tube_v3 tube_add(const tube_v3 &a, const tube_v3 &b) { return { a[0] + b[0], a[1] + b[1], a[2] + b[2] }; }
tube_v3 tube_mul(const tube_v3 &a, double s) { return { a[0] * s, a[1] * s, a[2] * s }; }
double  tube_dot(const tube_v3 &a, const tube_v3 &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double  tube_len(const tube_v3 &a) { return std::sqrt(tube_dot(a, a)); }
tube_v3 tube_cross(const tube_v3 &a, const tube_v3 &b) { return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] }; }
tube_v3 tube_unit(const tube_v3 &a, const tube_v3 &fallback)
{
    const double l = tube_len(a);
    return l > 1e-12 ? tube_mul(a, 1. / l) : fallback;
}

struct TubePoint
{
    tube_v3 p;
    float   width  = 0.f;
    float   height = 0.f;
    float   build  = 0.f;
};

struct TubeRun
{
    int                    extruder = 0;
    std::vector<TubePoint> points;
};

bool tube_kept(TubeRole role, const TubeOptions &o)
{
    switch (role) {
    case TubeRole::SparseInfill:
    case TubeRole::InternalSolid: return o.internals;
    case TubeRole::Support: return o.supports;
    case TubeRole::Skirt: return o.skirt;
    default: return true;
    }
}

double tube_point_line_distance(const tube_v3 &p, const tube_v3 &a, const tube_v3 &b)
{
    const tube_v3 ab = tube_sub(b, a);
    const double  l2 = tube_dot(ab, ab);
    if (l2 < 1e-18)
        return tube_len(tube_sub(p, a));
    const double t = std::clamp(tube_dot(tube_sub(p, a), ab) / l2, 0., 1.);
    return tube_len(tube_sub(p, tube_add(a, tube_mul(ab, t))));
}

// Drops the points that lie on the straight line between their kept neighbours (within `tol` mm, same
// width and height within 2 %). The first and last points stay.
std::vector<TubePoint> tube_thin(const std::vector<TubePoint> &in, double tol)
{
    if (in.size() <= 2 || !(tol > 0.))
        return in;
    std::vector<TubePoint> out;
    out.push_back(in.front());
    size_t kept = 0;
    for (size_t i = 1; i + 1 < in.size(); ++i) {
        bool drop = i - kept <= 64;
        const TubePoint &a = in[kept], &b = in[i + 1];
        for (size_t k = kept + 1; drop && k <= i; ++k) {
            const TubePoint &q = in[k];
            if (tube_point_line_distance(q.p, a.p, b.p) > tol || std::abs(q.width - a.width) > 0.02f * std::max(a.width, 1e-3f) ||
                std::abs(q.height - a.height) > 0.02f * std::max(a.height, 1e-3f))
                drop = false;
        }
        if (!drop) {
            out.push_back(in[i]);
            kept = i;
        }
    }
    out.push_back(in.back());
    return out;
}

float tube_signed_pow(double v, double e) { return float((v < 0. ? -1. : 1.) * std::pow(std::abs(v), e)); }

void tube_emit(const TubeRun &run, int sides, float bulge, TubeMesh &mesh)
{
    const std::vector<TubePoint> &P = run.points;
    const size_t m = P.size();
    if (m < 2)
        return;
    const tube_v3 Z{ 0., 0., 1. };
    std::vector<tube_v3> seg(m - 1);
    for (size_t k = 0; k + 1 < m; ++k)
        seg[k] = tube_unit(tube_sub(P[k + 1].p, P[k].p), k > 0 ? seg[k - 1] : tube_v3{ 1., 0., 0. });

    const double p_exp = std::max(2., double(bulge));
    // The ring's shape, the same for every point: unit superellipse positions and their normals' parts.
    std::vector<std::array<double, 4>> ring(static_cast<size_t>(sides));
    for (int j = 0; j < sides; ++j) {
        const double th = TUBE_PI / double(sides) + 2. * TUBE_PI * double(j) / double(sides);
        const double c = std::cos(th), s = std::sin(th);
        const double x = tube_signed_pow(c, 2. / p_exp), y = tube_signed_pow(s, 2. / p_exp);
        ring[size_t(j)] = { x, y, tube_signed_pow(x, p_exp - 1.), tube_signed_pow(y, p_exp - 1.) };
    }

    auto push3 = [](std::vector<float> &v, const tube_v3 &a) {
        v.push_back(float(a[0]));
        v.push_back(float(a[1]));
        v.push_back(float(a[2]));
    };
    auto frame = [&](size_t k, tube_v3 &t, tube_v3 &side, tube_v3 &up, double &miter) {
        if (k == 0)
            t = seg[0];
        else if (k + 1 == m)
            t = seg[m - 2];
        else
            t = tube_unit(tube_add(seg[k - 1], seg[k]), seg[k - 1]);
        miter = 1.;
        if (k > 0 && k + 1 < m)
            miter = 1. / std::max(0.5, tube_dot(t, seg[k]));
        side = tube_unit(tube_cross(t, Z), tube_v3{ 0., -1., 0. });
        up   = tube_cross(side, t);
    };

    const uint32_t base = uint32_t(mesh.vertex_count());
    for (size_t k = 0; k < m; ++k) {
        tube_v3 t, side, up;
        double  miter;
        frame(k, t, side, up, miter);
        const double  hw = 0.5 * double(P[k].width), hh = 0.5 * double(P[k].height);
        const tube_v3 centre = tube_sub(P[k].p, tube_mul(Z, hh));
        for (int j = 0; j < sides; ++j) {
            const auto &r = ring[size_t(j)];
            push3(mesh.positions, tube_add(centre, tube_add(tube_mul(side, hw * miter * r[0]), tube_mul(up, hh * r[1]))));
            push3(mesh.normals, tube_unit(tube_add(tube_mul(side, r[2] / std::max(hw, 1e-6)), tube_mul(up, r[3] / std::max(hh, 1e-6))), up));
            push3(mesh.direction, t);
            mesh.build.push_back(P[k].build);
        }
    }
    const uint32_t S = uint32_t(sides);
    for (uint32_t k = 0; k + 1 < uint32_t(m); ++k)
        for (uint32_t j = 0; j < S; ++j) {
            const uint32_t a = base + k * S + j, b = base + k * S + (j + 1) % S;
            const uint32_t c = base + (k + 1) * S + j, d = base + (k + 1) * S + (j + 1) % S;
            mesh.indices.insert(mesh.indices.end(), { a, c, b, b, c, d });
        }
    // Caps: flat, with their own vertices so the ends are not shaded round.
    for (int end = 0; end < 2; ++end) {
        const size_t k = end == 0 ? 0 : m - 1;
        tube_v3 t, side, up;
        double  miter;
        frame(k, t, side, up, miter);
        const tube_v3  n  = end == 0 ? tube_mul(t, -1.) : t;
        const double   hw = 0.5 * double(P[k].width), hh = 0.5 * double(P[k].height);
        const tube_v3  centre = tube_sub(P[k].p, tube_mul(Z, hh));
        const uint32_t c0 = uint32_t(mesh.vertex_count());
        push3(mesh.positions, centre);
        push3(mesh.normals, n);
        push3(mesh.direction, t);
        mesh.build.push_back(P[k].build);
        for (int j = 0; j < sides; ++j) {
            const auto &r = ring[size_t(j)];
            push3(mesh.positions, tube_add(centre, tube_add(tube_mul(side, hw * r[0]), tube_mul(up, hh * r[1]))));
            push3(mesh.normals, n);
            push3(mesh.direction, t);
            mesh.build.push_back(P[k].build);
        }
        for (uint32_t j = 0; j < S; ++j) {
            const uint32_t a = c0 + 1 + j, b = c0 + 1 + (j + 1) % S;
            if (end == 0)
                mesh.indices.insert(mesh.indices.end(), { c0, a, b });
            else
                mesh.indices.insert(mesh.indices.end(), { c0, b, a });
        }
    }
}

} // namespace

size_t tube_triangles(size_t points, int sides)
{
    if (points < 2 || sides < 3)
        return 0;
    return (points - 1) * size_t(sides) * 2 + 2 * size_t(sides);
}

TubeResult build_tubes(const std::vector<TubeMove> &moves, const TubeOptions &options)
{
    TubeResult result;
    const double denom = moves.size() > 1 ? double(moves.size() - 1) : 1.;
    std::vector<TubeRun> runs;
    TubeRun current;
    auto close_run = [&]() {
        if (current.points.size() >= 2) {
            current.points = tube_thin(current.points, double(options.simplify_mm));
            runs.push_back(std::move(current));
        }
        current = TubeRun();
    };
    for (size_t i = 0; i < moves.size(); ++i) {
        const TubeMove &mv = moves[i];
        const bool extrude = mv.kind == TubeKind::Extrude && mv.width > 0.f && mv.height > 0.f && i > 0;
        if (!extrude) {
            close_run();
            continue;
        }
        if (!tube_kept(mv.role, options)) {
            ++result.culled;
            close_run();
            continue;
        }
        const TubeMove &prev = moves[i - 1];
        const tube_v3   a{ prev.x, prev.y, prev.z }, b{ mv.x, mv.y, mv.z };
        if (tube_len(tube_sub(b, a)) < 1e-4)
            continue;
        if (!current.points.empty() && current.extruder != mv.extruder)
            close_run();
        if (current.points.empty()) {
            current.extruder = mv.extruder;
            // The run starts where the previous move ended, at this extrusion's size.
            current.points.push_back({ a, mv.width, mv.height, float(double(i - 1) / denom) });
        }
        current.points.push_back({ b, mv.width, mv.height, float(double(i) / denom) });
    }
    close_run();

    int sides = options.sides > 0 ? std::clamp(options.sides, 3, 16) : 0;
    if (sides == 0) {
        sides = 4;
        for (int s : { 8, 6, 4 }) {
            size_t total = 0;
            for (const TubeRun &r : runs)
                total += tube_triangles(r.points.size(), s);
            if (total <= options.max_triangles) {
                sides = s;
                break;
            }
        }
    }
    result.sides = sides;

    std::map<int, TubeMesh> by_extruder;
    size_t total = 0;
    bool   full  = false;
    for (const TubeRun &r : runs) {
        const size_t tris = tube_triangles(r.points.size(), sides);
        if (full || total + tris > options.max_triangles) {
            full = true;   // the rest of the print, in order: a build-up then stops at one height
            ++result.dropped;
            continue;
        }
        total += tris;
        TubeMesh &mesh = by_extruder[r.extruder];
        mesh.extruder  = r.extruder;
        tube_emit(r, sides, options.bulge, mesh);
        ++result.runs;
        result.points += r.points.size();
    }
    result.triangles = total;
    for (auto &kv : by_extruder)
        result.meshes.push_back(std::move(kv.second));
    return result;
}

} // namespace Showcase
} // namespace Slic3r
