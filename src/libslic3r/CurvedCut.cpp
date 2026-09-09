#include "CurvedCut.hpp"
#include "MeshBoolean.hpp"
#include "MeshSculpt.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>

namespace Slic3r {

const int CurvedCutSheet::MinResolution;
const int CurvedCutSheet::MaxResolution;
const int CurvedCutSheet::DefaultResolution;
const int CurvedCutSheet::DefaultSamples;
const int CurvedCutSheet::CutSamples;

// ---------------------------------------------------------------------------
// Control grid
// ---------------------------------------------------------------------------

void CurvedCutSheet::reset(int resolution)
{
    if (resolution > 0)
        m_resolution = std::clamp(resolution, MinResolution, MaxResolution);
    m_z.assign(size_t(m_resolution) * size_t(m_resolution), 0.0);
}

void CurvedCutSheet::set_values(const std::vector<double>& z)
{
    if (z.size() == m_z.size())
        m_z = z;
}

Vec2d CurvedCutSheet::control_xy(int i, int j) const
{
    return Vec2d((2.0 * control_u(i) - 1.0) * m_half_size,
                 (2.0 * control_u(j) - 1.0) * m_half_size);
}

Vec3d CurvedCutSheet::control_pos(int i, int j) const
{
    const Vec2d xy = control_xy(i, j);
    return Vec3d(xy.x(), xy.y(), at(i, j));
}

bool CurvedCutSheet::is_flat() const
{
    for (double z : m_z)
        if (z != 0.0)
            return false;
    return true;
}

double CurvedCutSheet::max_displacement() const
{
    double m = 0.0;
    for (double z : m_z)
        m = std::max(m, std::abs(z));
    return m;
}

// ---------------------------------------------------------------------------
// Catmull-Rom evaluation
// ---------------------------------------------------------------------------

// One-dimensional uniform Catmull-Rom through p1,p2 with p0/p3 as the tangent
// neighbours; t in [0,1] runs from p1 to p2.
static inline double catmull_rom(double p0, double p1, double p2, double p3, double t)
{
    const double t2 = t * t;
    const double t3 = t2 * t;
    return 0.5 * ((2.0 * p1) +
                  (-p0 + p2) * t +
                  (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 +
                  (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3);
}

double CurvedCutSheet::evaluate(double u, double v) const
{
    const int n = m_resolution;
    if (n < 2 || m_z.empty())
        return 0.0;
    // A flat grid is flat everywhere - short-circuit so the invariant that
    // zero displacement gives exactly z == 0 does not depend on float maths.
    if (is_flat())
        return 0.0;

    u = std::clamp(u, 0.0, 1.0);
    v = std::clamp(v, 0.0, 1.0);

    const double fu = u * double(n - 1);
    const double fv = v * double(n - 1);
    int          iu = int(std::floor(fu));
    int          iv = int(std::floor(fv));
    iu = std::clamp(iu, 0, n - 2);
    iv = std::clamp(iv, 0, n - 2);
    const double tu = fu - double(iu);
    const double tv = fv - double(iv);

    // Clamped boundary: the neighbour outside the grid repeats the edge row,
    // which gives a zero second derivative at the border rather than an
    // extrapolated overshoot.
    auto z_at = [this, n](int i, int j) -> double {
        return at(std::clamp(i, 0, n - 1), std::clamp(j, 0, n - 1));
    };

    double col[4];
    for (int k = 0; k < 4; ++ k) {
        const int j = iv - 1 + k;
        col[k] = catmull_rom(z_at(iu - 1, j), z_at(iu, j), z_at(iu + 1, j), z_at(iu + 2, j), tu);
    }
    return catmull_rom(col[0], col[1], col[2], col[3], tv);
}

double CurvedCutSheet::evaluate_local(double x, double y) const
{
    const double u = 0.5 * (x / m_half_size + 1.0);
    const double v = 0.5 * (y / m_half_size + 1.0);
    return evaluate(u, v);
}

void CurvedCutSheet::set_resolution(int resolution)
{
    const int n = std::clamp(resolution, MinResolution, MaxResolution);
    if (n == m_resolution)
        return;
    // Re-sample the current surface onto the new grid. Catmull-Rom interpolates
    // its control points, so going to a finer grid whose nodes include the old
    // ones (e.g. 3 -> 5 -> 9) reproduces the old values exactly at those nodes,
    // and coming back lands on them again.
    std::vector<double> nz(size_t(n) * size_t(n), 0.0);
    if (!is_flat())
        for (int j = 0; j < n; ++ j) {
            const double v = n < 2 ? 0.5 : double(j) / double(n - 1);
            for (int i = 0; i < n; ++ i) {
                const double u = n < 2 ? 0.5 : double(i) / double(n - 1);
                nz[size_t(j) * n + i] = evaluate(u, v);
            }
        }
    m_resolution = n;
    m_z          = std::move(nz);
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

void CurvedCutSheet::grab(const Vec2d& center_xy, double radius, double delta, bool falloff)
{
    if (radius <= 0.0 || delta == 0.0)
        return;
    for (int j = 0; j < m_resolution; ++ j)
        for (int i = 0; i < m_resolution; ++ i) {
            const double d = (control_xy(i, j) - center_xy).norm();
            if (d >= radius)
                continue;
            // The same quartic bump the Sculpt gizmo's brush uses.
            const double w = falloff ? double(Sculpt::falloff_weight(float(d), float(radius))) : 1.0;
            at(i, j) += delta * w;
        }
}

void CurvedCutSheet::smooth(double strength, const Vec2d* center_xy, double radius, bool falloff)
{
    if (m_resolution < 3 || strength <= 0.0)
        return;
    strength = std::clamp(strength, 0.0, 1.0);

    const std::vector<double> src = m_z;
    auto z_at = [&src, this](int i, int j) {
        i = std::clamp(i, 0, m_resolution - 1);
        j = std::clamp(j, 0, m_resolution - 1);
        return src[size_t(j) * m_resolution + i];
    };

    for (int j = 0; j < m_resolution; ++ j)
        for (int i = 0; i < m_resolution; ++ i) {
            double w = 1.0;
            if (center_xy != nullptr && radius > 0.0) {
                const double d = (control_xy(i, j) - *center_xy).norm();
                if (d >= radius)
                    continue;
                w = falloff ? double(Sculpt::falloff_weight(float(d), float(radius))) : 1.0;
            }
            const double avg = 0.25 * (z_at(i - 1, j) + z_at(i + 1, j) + z_at(i, j - 1) + z_at(i, j + 1));
            at(i, j) = src[size_t(j) * m_resolution + i] * (1.0 - strength * w) + avg * (strength * w);
        }
}

// ---------------------------------------------------------------------------
// Sampling
// ---------------------------------------------------------------------------

indexed_triangle_set CurvedCutSheet::sample_sheet(int samples) const
{
    const int  n = std::max(samples, 2);
    indexed_triangle_set its;
    its.vertices.reserve(size_t(n) * size_t(n));
    for (int j = 0; j < n; ++ j) {
        const double v = double(j) / double(n - 1);
        const double y = (2.0 * v - 1.0) * m_half_size;
        for (int i = 0; i < n; ++ i) {
            const double u = double(i) / double(n - 1);
            const double x = (2.0 * u - 1.0) * m_half_size;
            its.vertices.emplace_back(Vec3f(float(x), float(y), float(evaluate(u, v))));
        }
    }
    its.indices.reserve(size_t(n - 1) * size_t(n - 1) * 2);
    for (int j = 0; j + 1 < n; ++ j)
        for (int i = 0; i + 1 < n; ++ i) {
            const int a = j * n + i;
            const int b = j * n + i + 1;
            const int c = (j + 1) * n + i + 1;
            const int d = (j + 1) * n + i;
            // CCW seen from +Z: the sheet's own outward normal points up.
            its.indices.emplace_back(Vec3i32(a, b, c));
            its.indices.emplace_back(Vec3i32(a, c, d));
        }
    return its;
}

// ---------------------------------------------------------------------------
// The cutter solid
// ---------------------------------------------------------------------------

indexed_triangle_set curved_cut_lower_slab(const CurvedCutSheet& sheet, const BoundingBoxf3& bbox, int samples)
{
    const int n = std::max(samples, 2);

    // Floor well below anything the object reaches, and below the sheet itself.
    const double diag  = bbox.defined ? bbox.size().norm() : 100.0;
    const double slack = std::max(1.0, 0.1 * diag);
    const double floor_z = std::min(bbox.defined ? bbox.min.z() : -slack,
                                    -sheet.max_displacement()) - slack;

    indexed_triangle_set its;
    const double hs = sheet.half_size();

    // Top surface (the sheet) then the floor, both as n x n grids so the rim
    // stitches vertex-for-vertex and the slab comes out watertight.
    its.vertices.reserve(size_t(n) * size_t(n) * 2);
    for (int j = 0; j < n; ++ j) {
        const double v = double(j) / double(n - 1);
        const double y = (2.0 * v - 1.0) * hs;
        for (int i = 0; i < n; ++ i) {
            const double u = double(i) / double(n - 1);
            const double x = (2.0 * u - 1.0) * hs;
            its.vertices.emplace_back(Vec3f(float(x), float(y), float(sheet.evaluate(u, v))));
        }
    }
    const int base = n * n;
    for (int j = 0; j < n; ++ j) {
        const double y = (2.0 * (double(j) / double(n - 1)) - 1.0) * hs;
        for (int i = 0; i < n; ++ i) {
            const double x = (2.0 * (double(i) / double(n - 1)) - 1.0) * hs;
            its.vertices.emplace_back(Vec3f(float(x), float(y), float(floor_z)));
        }
    }

    auto top = [n](int i, int j) { return j * n + i; };
    auto bot = [n, base](int i, int j) { return base + j * n + i; };

    // Top: outward normal +Z. Bottom: outward normal -Z (reversed winding).
    for (int j = 0; j + 1 < n; ++ j)
        for (int i = 0; i + 1 < n; ++ i) {
            its.indices.emplace_back(Vec3i32(top(i, j), top(i + 1, j), top(i + 1, j + 1)));
            its.indices.emplace_back(Vec3i32(top(i, j), top(i + 1, j + 1), top(i, j + 1)));
            its.indices.emplace_back(Vec3i32(bot(i, j), bot(i + 1, j + 1), bot(i + 1, j)));
            its.indices.emplace_back(Vec3i32(bot(i, j), bot(i, j + 1), bot(i + 1, j + 1)));
        }

    // The four side walls, vertical in local Z, stitched between the sheet's
    // boundary row and the matching floor row. This is the rim the research
    // spec calls out: without it the two sheets are two open surfaces, not a
    // solid, and the boolean has nothing to intersect.
    for (int i = 0; i + 1 < n; ++ i) {
        // y = -hs edge (outward -Y)
        its.indices.emplace_back(Vec3i32(top(i, 0), bot(i, 0), bot(i + 1, 0)));
        its.indices.emplace_back(Vec3i32(top(i, 0), bot(i + 1, 0), top(i + 1, 0)));
        // y = +hs edge (outward +Y)
        its.indices.emplace_back(Vec3i32(top(i, n - 1), top(i + 1, n - 1), bot(i + 1, n - 1)));
        its.indices.emplace_back(Vec3i32(top(i, n - 1), bot(i + 1, n - 1), bot(i, n - 1)));
    }
    for (int j = 0; j + 1 < n; ++ j) {
        // x = -hs edge (outward -X)
        its.indices.emplace_back(Vec3i32(top(0, j), top(0, j + 1), bot(0, j + 1)));
        its.indices.emplace_back(Vec3i32(top(0, j), bot(0, j + 1), bot(0, j)));
        // x = +hs edge (outward +X)
        its.indices.emplace_back(Vec3i32(top(n - 1, j), bot(n - 1, j), bot(n - 1, j + 1)));
        its.indices.emplace_back(Vec3i32(top(n - 1, j), bot(n - 1, j + 1), top(n - 1, j + 1)));
    }

    return its;
}

// Manifold first, mcut as the fallback - the same chain CutUtils' flexi_boolean
// and the Mesh Boolean gizmo use.
static bool curved_boolean(const TriangleMesh& a, const TriangleMesh& b, const std::string& op, TriangleMesh& out)
{
    std::vector<TriangleMesh> dst;
    bool ok = MeshBoolean::mfd::make_boolean(a, b, dst, op);
    if (!ok) {
        BOOST_LOG_TRIVIAL(warning) << "Curved cut: Manifold boolean " << op << " failed, falling back to mcut";
        dst.clear();
        try {
            MeshBoolean::mcut::make_boolean(a, b, dst, op);
        } catch (const std::exception& ex) {
            BOOST_LOG_TRIVIAL(error) << "Curved cut: mcut boolean " << op << " failed: " << ex.what();
            return false;
        }
    }
    if (dst.empty())
        return false;
    TriangleMesh merged = dst.front();
    for (size_t i = 1; i < dst.size(); ++ i)
        merged.merge(dst[i]);
    if (merged.empty())
        return false;
    out = std::move(merged);
    return true;
}

bool curved_cut_split(const indexed_triangle_set& mesh,
                      const CurvedCutSheet&       sheet,
                      indexed_triangle_set*       upper,
                      indexed_triangle_set*       lower,
                      int                         samples)
{
    if (mesh.empty())
        return false;

    TriangleMesh object(mesh);

    // The slab must reach past the object on every side, or "below the sheet"
    // is only defined over part of it.
    BoundingBoxf3  bbox = object.bounding_box();
    CurvedCutSheet s    = sheet;
    if (bbox.defined) {
        const double need = 1.05 * std::max(std::max(std::abs(bbox.min.x()), std::abs(bbox.max.x())),
                                            std::max(std::abs(bbox.min.y()), std::abs(bbox.max.y()))) + 1.0;
        if (s.half_size() < need)
            s.set_half_size(need);
    }

    TriangleMesh slab(curved_cut_lower_slab(s, bbox, samples));

    bool ok = true;
    if (lower != nullptr) {
        TriangleMesh out;
        if (curved_boolean(object, slab, "INTERSECTION", out))
            *lower = out.its;
        else {
            lower->clear();
            ok = false;
        }
    }
    if (upper != nullptr) {
        TriangleMesh out;
        if (curved_boolean(object, slab, "A_NOT_B", out))
            *upper = out.its;
        else {
            upper->clear();
            ok = false;
        }
    }
    return ok;
}

} // namespace Slic3r
