#include "CurvedCut.hpp"
#include "MeshBoolean.hpp"
#include "MeshSculpt.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

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
    return Vec2d((2.0 * control_u(i) - 1.0) * m_half_size_u,
                 (2.0 * control_u(j) - 1.0) * m_half_size_v);
}

void CurvedCutSheet::set_half_size(double hs_u, double hs_v, bool resample)
{
    hs_u = std::max(hs_u, 1e-6);
    hs_v = std::max(hs_v, 1e-6);
    if (hs_u == m_half_size_u && hs_v == m_half_size_v)
        return;

    if (!resample || is_flat()) {
        m_half_size_u = hs_u;
        m_half_size_v = hs_v;
        return;
    }

    // Keep the SURFACE fixed in the plane, not the control values: each new
    // control point takes the old surface's height at the same local (x,y) in
    // mm. evaluate_local() clamps outside the old domain, so a point that ends
    // up beyond the old rectangle picks up the border value rather than an
    // extrapolated overshoot - the same clamped-boundary rule evaluate() uses.
    const int           n = m_resolution;
    std::vector<double> nz(size_t(n) * size_t(n), 0.0);
    for (int j = 0; j < n; ++ j) {
        const double y = (2.0 * control_u(j) - 1.0) * hs_v;
        for (int i = 0; i < n; ++ i) {
            const double x = (2.0 * control_u(i) - 1.0) * hs_u;
            nz[size_t(j) * n + i] = evaluate_local(x, y);
        }
    }
    m_half_size_u = hs_u;
    m_half_size_v = hs_v;
    m_z           = std::move(nz);
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
    const double u = 0.5 * (x / m_half_size_u + 1.0);
    const double v = 0.5 * (y / m_half_size_v + 1.0);
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
        const double y = (2.0 * v - 1.0) * m_half_size_v;
        for (int i = 0; i < n; ++ i) {
            const double u = double(i) / double(n - 1);
            const double x = (2.0 * u - 1.0) * m_half_size_u;
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

// ---------------------------------------------------------------------------
// Phase 2: fitting to the cross-section, and snapping to the surface
// ---------------------------------------------------------------------------

bool curved_cut_fit_extent(const indexed_triangle_set& mesh,
                           double&                     half_size_u,
                           double&                     half_size_v,
                           double                      margin_rel,
                           double                      margin_abs)
{
    if (mesh.empty())
        return false;

    // The cross-section's bounding box, gathered straight from the edges that
    // cross z == 0. No polygon assembly: the outline's BOUNDING BOX is all the
    // fit needs, and every point of the outline lies on such an edge, so the
    // box the crossings give is the box the outline has. That also sidesteps
    // every degenerate case a contour builder has to handle (an edge lying in
    // the plane, a vertex exactly on it, a non-manifold seam) - a crossing that
    // is counted twice, or a coplanar edge whose endpoints are both taken, only
    // ever contributes points that ARE on the section.
    double min_x =  std::numeric_limits<double>::max();
    double max_x = -std::numeric_limits<double>::max();
    double min_y =  std::numeric_limits<double>::max();
    double max_y = -std::numeric_limits<double>::max();
    bool   any   = false;

    auto take = [&](const Vec3d& p) {
        min_x = std::min(min_x, p.x());
        max_x = std::max(max_x, p.x());
        min_y = std::min(min_y, p.y());
        max_y = std::max(max_y, p.y());
        any   = true;
    };

    for (const Vec3i32& tri : mesh.indices)
        for (int e = 0; e < 3; ++ e) {
            const Vec3d a = mesh.vertices[tri(e)].cast<double>();
            const Vec3d b = mesh.vertices[tri((e + 1) % 3)].cast<double>();
            const double za = a.z(), zb = b.z();
            if (za == 0.0)
                take(a);
            if ((za < 0.0 && zb > 0.0) || (za > 0.0 && zb < 0.0)) {
                const double t = za / (za - zb);
                take(a + t * (b - a));
            }
        }

    if (!any)
        return false;

    // Degenerate in one axis (a plane grazing a flat face along a line) still
    // gives a usable fit once the margin is added, so only the "no crossing at
    // all" case above is a failure.
    const double ext_u = 0.5 * (max_x - min_x);
    const double ext_v = 0.5 * (max_y - min_y);
    half_size_u = std::max(ext_u + std::max(margin_rel * ext_u, margin_abs), 1e-6);
    half_size_v = std::max(ext_v + std::max(margin_rel * ext_v, margin_abs), 1e-6);
    return true;
}

bool curved_cut_snap_distance(const indexed_triangle_set& mesh,
                              const Vec3d&                pos,
                              double&                     distance)
{
    if (mesh.empty())
        return false;

    // Walk the triangles and intersect the vertical line x = pos.x, y = pos.y
    // with each, keeping the hit whose |z - pos.z| is smallest. A line/triangle
    // test rather than a ray one: "nearest hit in EITHER direction" is what the
    // gesture wants (a handle floating above the part snaps down, one buried
    // inside snaps to whichever face is closer), and one pass gets both.
    bool   found = false;
    double best  = 0.0;

    for (const Vec3i32& tri : mesh.indices) {
        const Vec3d a = mesh.vertices[tri(0)].cast<double>();
        const Vec3d b = mesh.vertices[tri(1)].cast<double>();
        const Vec3d c = mesh.vertices[tri(2)].cast<double>();

        // Barycentric solve in the XY projection. A triangle seen edge-on from
        // +Z projects to zero area and is skipped: it can only add a hit that a
        // neighbouring, non-degenerate face already provides.
        const double d = (b.y() - c.y()) * (a.x() - c.x()) + (c.x() - b.x()) * (a.y() - c.y());
        if (std::abs(d) < 1e-12)
            continue;
        const double l0 = ((b.y() - c.y()) * (pos.x() - c.x()) + (c.x() - b.x()) * (pos.y() - c.y())) / d;
        const double l1 = ((c.y() - a.y()) * (pos.x() - c.x()) + (a.x() - c.x()) * (pos.y() - c.y())) / d;
        const double l2 = 1.0 - l0 - l1;
        const double eps = -1e-9;
        if (l0 < eps || l1 < eps || l2 < eps)
            continue;

        const double z    = l0 * a.z() + l1 * b.z() + l2 * c.z();
        const double sign = z - pos.z();
        if (!found || std::abs(sign) < std::abs(best)) {
            best  = sign;
            found = true;
        }
    }

    if (!found)
        return false;
    distance = best;
    return true;
}

indexed_triangle_set curved_cut_lower_slab(const CurvedCutSheet& sheet, const BoundingBoxf3& bbox, int samples, double extent, double extent_v)
{
    const int n = std::max(samples, 2);

    // Floor well below anything the object reaches, and below the sheet itself.
    const double diag  = bbox.defined ? bbox.size().norm() : 100.0;
    const double slack = std::max(1.0, 0.1 * diag);
    const double floor_z = std::min(bbox.defined ? bbox.min.z() : -slack,
                                    -sheet.max_displacement()) - slack;

    indexed_triangle_set its;
    // The slab may be built WIDER than the sheet's own domain. Sampling by local
    // (x,y) through evaluate_local() - not by (u,v) - is what keeps the surface
    // itself fixed: over the sheet's domain the heights are unchanged, and beyond
    // it the clamped edge value is extruded straight outwards.
    const double hs   = extent   > 0.0 ? std::max(extent,   sheet.half_size_u()) : sheet.half_size_u();
    const double hs_v = extent_v > 0.0 ? std::max(extent_v, sheet.half_size_v())
                                       : (extent > 0.0 ? std::max(extent, sheet.half_size_v()) : sheet.half_size_v());

    // Top surface (the sheet) then the floor, both as n x n grids so the rim
    // stitches vertex-for-vertex and the slab comes out watertight.
    its.vertices.reserve(size_t(n) * size_t(n) * 2);
    for (int j = 0; j < n; ++ j) {
        const double v = double(j) / double(n - 1);
        const double y = (2.0 * v - 1.0) * hs_v;
        for (int i = 0; i < n; ++ i) {
            const double u = double(i) / double(n - 1);
            const double x = (2.0 * u - 1.0) * hs;
            its.vertices.emplace_back(Vec3f(float(x), float(y), float(sheet.evaluate_local(x, y))));
        }
    }
    const int base = n * n;
    for (int j = 0; j < n; ++ j) {
        const double y = (2.0 * (double(j) / double(n - 1)) - 1.0) * hs_v;
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

    // The slab must reach past the object on every side, or "below the sheet" is
    // only defined over part of it. WIDEN THE SLAB, never the sheet: set_half_size()
    // on the sheet would drag its control points outwards and stretch the surface,
    // so a small part under a large plane would get a differently shaped cut than
    // the one the gizmo drew (and, at a rotated plane where the object's footprint
    // in the cut frame is much larger than the sheet, a nearly flat one).
    // Phase 2: the sheet's domain is a rectangle, so the slab is widened PER
    // AXIS. Taking one square extent from the larger side would still cover the
    // object, but it would spend the slab's fixed sample budget on empty space
    // along the short axis and coarsen the surface where it actually cuts.
    BoundingBoxf3 bbox     = object.bounding_box();
    double        extent   = sheet.half_size_u();
    double        extent_v = sheet.half_size_v();
    if (bbox.defined) {
        const double need_u = 1.05 * std::max(std::abs(bbox.min.x()), std::abs(bbox.max.x())) + 1.0;
        const double need_v = 1.05 * std::max(std::abs(bbox.min.y()), std::abs(bbox.max.y())) + 1.0;
        extent   = std::max(extent,   need_u);
        extent_v = std::max(extent_v, need_v);
    }

    // THE "ONLY ONE HALF SURVIVES" FIX, part 1: the object's winding.
    //
    // Both booleans decide "inside" from face orientation, so an object whose
    // triangles are wound inwards - a flipped-normal import, or a mesh a previous
    // repair left inside out - reports its COMPLEMENT as its interior. The
    // INTERSECTION then comes back empty (or as the whole part) while the A_NOT_B
    // still succeeds, and the caller sees exactly one half. Detect it from the
    // signed volume, which is negative for precisely this case, and flip the copy
    // handed to the boolean. its_volume() is one pass over the triangles and this
    // runs once per cut, so it costs nothing measurable.
    if (its_volume(object.its) < 0.f) {
        BOOST_LOG_TRIVIAL(warning) << "Curved cut: the input mesh is wound inwards (negative signed volume); "
                                      "flipping it before the boolean";
        for (Vec3i32& t : object.its.indices)
            std::swap(t(1), t(2));
    }

    TriangleMesh slab(curved_cut_lower_slab(sheet, bbox, samples, extent, extent_v));

    // THE "ONLY ONE HALF SURVIVES" FIX, part 2: never let ONE failed boolean
    // cost the caller a half.
    //
    // Each side used to be its own all-or-nothing boolean, and a failure just
    // cleared that side and logged. Downstream, an empty mesh is indistinguishable
    // from "this half does not exist": add_cut_volume() returns early on an empty
    // mesh, the cloned ModelObject ends up with no volumes, and post_process()
    // drops it - so the user gets one part back from a two-part cut, with nothing
    // on screen to say why. That is the reported bug.
    //
    // So: run BOTH sides whenever either was asked for, and when exactly one came
    // back, recover the other from the complement - the missing half is
    // (object - kept), another boolean against a solid the first one already
    // proved workable. Only when the direct boolean AND the complement both fail
    // is a half really unavailable.
    auto complement = [&object](const indexed_triangle_set& kept, indexed_triangle_set& out) -> bool {
        if (kept.empty())
            return false;
        TriangleMesh rest;
        if (!curved_boolean(object, TriangleMesh(kept), "A_NOT_B", rest) || rest.its.empty())
            return false;
        out = rest.its;
        return true;
    };

    indexed_triangle_set lower_its, upper_its;
    bool have_lower = false, have_upper = false;
    {
        TriangleMesh out;
        if (curved_boolean(object, slab, "INTERSECTION", out) && !out.its.empty()) {
            lower_its  = std::move(out.its);
            have_lower = true;
        }
    }
    {
        TriangleMesh out;
        if (curved_boolean(object, slab, "A_NOT_B", out) && !out.its.empty()) {
            upper_its  = std::move(out.its);
            have_upper = true;
        }
    }

    if (!have_upper && have_lower) {
        BOOST_LOG_TRIVIAL(warning) << "Curved cut: the upper boolean gave nothing; recovering it as object - lower";
        have_upper = complement(lower_its, upper_its);
    }
    else if (!have_lower && have_upper) {
        BOOST_LOG_TRIVIAL(warning) << "Curved cut: the lower boolean gave nothing; recovering it as object - upper";
        have_lower = complement(upper_its, lower_its);
    }

    bool ok = true;
    if (lower != nullptr) {
        if (have_lower)
            *lower = std::move(lower_its);
        else {
            lower->clear();
            ok = false;
        }
    }
    if (upper != nullptr) {
        if (have_upper)
            *upper = std::move(upper_its);
        else {
            upper->clear();
            ok = false;
        }
    }
    return ok;
}


// ---------------------------------------------------------------------------
// Phase 2 fixes: side visibility contract (see CurvedCut.hpp).
// ---------------------------------------------------------------------------

float curved_cut_side_alpha(CurvedCutSideVisibility v)
{
    switch (v) {
    case CurvedCutSideVisibility::Ghost:  return 0.25f;
    case CurvedCutSideVisibility::Hidden: return -1.f; // negative == discard, see gouraud.fs
    default:                              return 1.f;
    }
}

bool curved_cut_side_is_ghost(float alpha)
{
    return alpha > 0.f && alpha < 1.f;
}

bool curved_cut_has_ghost_side(float alpha_1, float alpha_2)
{
    return curved_cut_side_is_ghost(alpha_1) || curved_cut_side_is_ghost(alpha_2);
}

} // namespace Slic3r
