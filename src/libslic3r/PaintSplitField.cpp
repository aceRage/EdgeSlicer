// Paint Split R1 (tests/design_paint_split.md section 8): the nearest-skin claim, evaluated sparsely.
//
// The cut is unchanged: the piece is the material nearer to the region's selected skin S than to the rest of
// the part's skin U, limited by Depth and by the reach. What changes is how its surface is found:
//   E1  a coarse-to-fine sparse field. The field g is 2-Lipschitz, so a cell whose centre value exceeds
//       2 x its half-diagonal holds no zero and is never refined. Only cells near the surface are sampled.
//   S1  exact edge roots: a consistent tetrahedral split of every cell (Kuhn), each crossing found on the exact
//       field by regula falsi. The result is a closed 2-manifold by construction.
//   D23 near the skin (within a voxel, then half a voxel) the claim's edges are split and the new vertices put on the exact
//       zero set, three times by default: a finer voxel where the cut meets the paint line.
//   S2  pinned Taubin smoothing of the claim (the cutter, so both halves still mate). Triangles within 1.5
//       voxels of the part's skin do not move, so the seam stays on the paint line.
// The claim is also clipped a little outside the part (it is intersected with the part anyway), and when the
// selected skin is the larger side the complement is meshed instead: the claim's outside shell then wraps the
// smaller skin.
#include "PaintSplit.hpp"
#include "AABBMesh.hpp"
#include "MeshRepair.hpp"
#include "TriangleMesh.hpp"

#include <tbb/blocked_range.h>
#include <tbb/enumerable_thread_specific.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_sort.h>

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <vector>

namespace Slic3r {

namespace {

using PsClock = std::chrono::steady_clock;
double ps_secs_since(PsClock::time_point t0) { return std::chrono::duration<double>(PsClock::now() - t0).count(); }

// ----------------------------------------------------------------------------------------- inside / outside

// Definitely outside the part: a coarse grid over the part's box, flooded from its border through the cells the
// skin does not reach (6-connected). A closed skin cannot be crossed that way, whatever its shells do inside
// (overlapping shells, fan-filled holes), so a flooded cell is outside for sure. A cell the flood misses may still
// be outside: the claim is just not clipped there. (Pseudo-normal signs were tried first and flip on overlapping
// shells, which carved holes in the claim.)
class PsOutside
{
public:
    PsOutside(const indexed_triangle_set& mesh, double margin)
    {
        BoundingBoxf3 mb;
        for (const Vec3f& v : mesh.vertices)
            mb.merge(v.cast<double>());
        const Vec3d size = mb.size() + Vec3d::Constant(2. * margin);
        m_c              = std::max(1.5, std::cbrt(std::max(1., size.x() * size.y() * size.z()) / 600000.));
        m_o              = mb.min - Vec3d::Constant(margin + m_c);
        for (int a = 0; a < 3; ++a)
            m_n[a] = int(std::ceil((size(a) + 2. * m_c) / m_c)) + 1;
        const size_t   total = size_t(m_n[0]) * size_t(m_n[1]) * size_t(m_n[2]);
        const AABBMesh tree(mesh);
        const double   lim = 0.5 * std::sqrt(3.) * m_c * 1.01 + 1e-6;
        std::vector<uint8_t> barrier(total, 0);
        tbb::parallel_for(tbb::blocked_range<int>(0, m_n[2]), [&](const tbb::blocked_range<int>& r) {
            for (int k = r.begin(); k < r.end(); ++k)
                for (int j = 0; j < m_n[1]; ++j)
                    for (int i = 0; i < m_n[0]; ++i)
                        barrier[idx(i, j, k)] = tree.squared_distance(m_o + m_c * Vec3d(i + 0.5, j + 0.5, k + 0.5)) <= lim * lim;
        });
        m_flood.assign(total, 0);
        std::vector<size_t> queue;
        auto push = [&](int i, int j, int k) {
            if (i < 0 || j < 0 || k < 0 || i >= m_n[0] || j >= m_n[1] || k >= m_n[2])
                return;
            const size_t c = idx(i, j, k);
            if (barrier[c] || m_flood[c])
                return;
            m_flood[c] = 1;
            queue.push_back(c);
        };
        for (int k = 0; k < m_n[2]; ++k)
            for (int j = 0; j < m_n[1]; ++j)
                for (int i = 0; i < m_n[0]; ++i)
                    if (i == 0 || j == 0 || k == 0 || i == m_n[0] - 1 || j == m_n[1] - 1 || k == m_n[2] - 1)
                        push(i, j, k);
        for (size_t q = 0; q < queue.size(); ++q) {
            const size_t c = queue[q];
            const int    i = int(c % size_t(m_n[0])), j = int((c / size_t(m_n[0])) % size_t(m_n[1])),
                      k = int(c / (size_t(m_n[0]) * size_t(m_n[1])));
            push(i - 1, j, k), push(i + 1, j, k), push(i, j - 1, k), push(i, j + 1, k), push(i, j, k - 1), push(i, j, k + 1);
        }
    }

    bool flooded(const Vec3d& p) const
    {
        const Vec3d q = (p - m_o) / m_c;
        const int   i = int(std::floor(q.x())), j = int(std::floor(q.y())), k = int(std::floor(q.z()));
        if (i < 0 || j < 0 || k < 0 || i >= m_n[0] || j >= m_n[1] || k >= m_n[2])
            return true;
        return m_flood[idx(i, j, k)] != 0;
    }

private:
    size_t idx(int i, int j, int k) const { return (size_t(k) * size_t(m_n[1]) + size_t(j)) * size_t(m_n[0]) + size_t(i); }

    double               m_c{ 1. };
    Vec3d                m_o{ Vec3d::Zero() };
    int                  m_n[3]{ 0, 0, 0 };
    std::vector<uint8_t> m_flood;
};

// ----------------------------------------------------------------------------------------- the field

struct PsValue
{
    double g{ 0. };        // the field (exact where its sign can change; otherwise a valid lower bound of |g|)
    float  d{ 0.f };       // distance to the part's skin
    bool   clip{ false };  // decided by the clip or the box, not by the skins
};

// g < 0 is the claim (or, with `complement`, its complement). g = max(+-f, clip, box) with
// f = max(ds - du, ds - reach[, ds - depth]): every term is 1- or 2-Lipschitz, so g is 2-Lipschitz.
struct PsField
{
    const AABBMesh*  ts{ nullptr };
    const AABBMesh*  tu{ nullptr };
    const PsOutside* outside{ nullptr };
    double           reach{ 0. }, depth{ 0. }, clip{ 0. };
    bool             complement{ false };
    BoundingBoxf3    box;
    std::atomic<size_t>* queries{ nullptr };

    double box_sdf(const Vec3d& p) const
    {
        const Vec3d  q = (p - 0.5 * (box.min + box.max)).cwiseAbs() - 0.5 * (box.max - box.min);
        const double out = q.cwiseMax(Vec3d::Zero()).norm();
        return out > 0. ? out : std::min(0., q.maxCoeff());
    }

    PsValue eval(const Vec3d& p) const
    {
        PsValue      out;
        const double bx = box_sdf(p);
        const double ds = std::sqrt(ts->squared_distance(p));
        size_t       nq = 1;
        if (!complement && ds - reach > 0.) {
            // Beyond the reach: outside the claim whatever U says.
            out.g    = std::max(ds - reach, bx);
            out.d    = float(ds);
            out.clip = bx >= ds - reach;
            if (queries != nullptr)
                queries->fetch_add(nq, std::memory_order_relaxed);
            return out;
        }
        const double du = std::sqrt(tu->squared_distance(p));
        ++nq;
        double f = std::max(ds - du, ds - reach);
        if (depth > 0.)
            f = std::max(f, ds - depth);
        double       v = complement ? -f : f;
        const double d = std::min(ds, du);
        out.d          = float(d);
        // The clip (the part dilated by `clip`) only where the point is surely outside the part. It can only win
        // where the distance to the skin exceeds it. Across a flood boundary g jumps, which the closure pass below
        // makes up for.
        if (d - clip > v && outside->flooded(p))
            v = d - clip, out.clip = true;
        if (bx > v)
            v = bx, out.clip = true;
        out.g = v;
        if (queries != nullptr)
            queries->fetch_add(nq, std::memory_order_relaxed);
        return out;
    }

    // The zero crossing on a -> b (signs of ga and gb differ; zero counts as positive), by Illinois regula falsi.
    double root(const Vec3d& a, const Vec3d& b, double ga, double gb, int iters, float* d_out) const
    {
        double t0 = 0., t1 = 1., f0 = ga, f1 = gb;
        int    side = 0;
        double t    = f0 / (f0 - f1);
        float  d    = 0.f;
        for (int it = 0; it < iters; ++it) {
            const PsValue v = eval(a + t * (b - a));
            d               = v.d;
            if (v.g == 0.)
                break;
            if ((v.g < 0.) == (f0 < 0.)) {
                t0 = t, f0 = v.g;
                if (side == -1)
                    f1 *= 0.5;
                side = -1;
            } else {
                t1 = t, f1 = v.g;
                if (side == 1)
                    f0 *= 0.5;
                side = 1;
            }
            t = (f0 * t1 - f1 * t0) / (f0 - f1);
        }
        if (d_out != nullptr)
            *d_out = d;
        return t;
    }
};

// Lattice keys: 21 bits per axis.
inline uint64_t ps_key(uint32_t i, uint32_t j, uint32_t k) { return (uint64_t(k) << 42) | (uint64_t(j) << 21) | uint64_t(i); }
inline uint32_t ps_ki(uint64_t key) { return uint32_t(key & 0x1FFFFF); }
inline uint32_t ps_kj(uint64_t key) { return uint32_t((key >> 21) & 0x1FFFFF); }
inline uint32_t ps_kk(uint64_t key) { return uint32_t(key >> 42); }

// ----------------------------------------------------------------------------------------- smoothing

// Taubin (lambda 0.5, mu -0.53) on the claim with a weight per vertex (0 = pinned).
void ps_taubin(indexed_triangle_set& its, const std::vector<float>& weight, int passes)
{
    const size_t nv = its.vertices.size();
    if (passes <= 0 || nv == 0 || weight.size() != nv)
        return;
    std::vector<std::pair<uint32_t, uint32_t>> pairs;
    pairs.reserve(its.indices.size() * 6);
    for (const Vec3i32& t : its.indices)
        for (int k = 0; k < 3; ++k) {
            pairs.emplace_back(uint32_t(t[k]), uint32_t(t[(k + 1) % 3]));
            pairs.emplace_back(uint32_t(t[(k + 1) % 3]), uint32_t(t[k]));
        }
    tbb::parallel_sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    std::vector<uint32_t> start(nv + 1, 0), nbr(pairs.size());
    for (const auto& p : pairs)
        ++start[p.first + 1];
    for (size_t v = 0; v < nv; ++v)
        start[v + 1] += start[v];
    for (size_t i = 0; i < pairs.size(); ++i)
        nbr[i] = pairs[i].second;
    std::vector<Vec3f> next(nv);
    auto step = [&](float factor) {
        tbb::parallel_for(tbb::blocked_range<size_t>(0, nv), [&](const tbb::blocked_range<size_t>& r) {
            for (size_t v = r.begin(); v < r.end(); ++v) {
                const Vec3f p = its.vertices[v];
                if (weight[v] <= 0.f || start[v + 1] == start[v]) {
                    next[v] = p;
                    continue;
                }
                Vec3f c = Vec3f::Zero();
                for (uint32_t i = start[v]; i < start[v + 1]; ++i)
                    c += its.vertices[nbr[i]];
                c /= float(start[v + 1] - start[v]);
                next[v] = p + factor * weight[v] * (c - p);
            }
        });
        its.vertices.swap(next);
    };
    for (int i = 0; i < passes; ++i) {
        step(0.5f);
        step(-0.53f);
    }
}

// ----------------------------------------------------------------------------------------- near-skin refinement

// Splits every edge whose ends both lie within `band` of the skin and puts the new vertex on the zero set
// (searched along the edge's normal). Conforming: each triangle is split by the pattern of its split edges.
void ps_refine_near_skin(indexed_triangle_set& its, std::vector<float>& dist, const PsField& field, double band)
{
    const size_t nt = its.indices.size();
    std::vector<Vec3f> tn(nt);
    for (size_t f = 0; f < nt; ++f) {
        const Vec3i32& t = its.indices[f];
        tn[f]            = (its.vertices[t[1]] - its.vertices[t[0]]).cross(its.vertices[t[2]] - its.vertices[t[0]]);
    }
    auto ekey = [](int a, int b) {
        if (a > b)
            std::swap(a, b);
        return (uint64_t(uint32_t(a)) << 32) | uint32_t(b);
    };
    // The edges to split, with the summed normals of their triangles.
    std::vector<std::pair<uint64_t, uint32_t>> edges;
    for (size_t f = 0; f < nt; ++f) {
        const Vec3i32& t = its.indices[f];
        for (int k = 0; k < 3; ++k) {
            const int a = t[k], b = t[(k + 1) % 3];
            if (dist[size_t(a)] < band && dist[size_t(b)] < band)
                edges.emplace_back(ekey(a, b), uint32_t(f));
        }
    }
    if (edges.empty())
        return;
    tbb::parallel_sort(edges.begin(), edges.end());
    std::vector<uint64_t> keys;
    std::vector<Vec3f>    normal;
    for (size_t i = 0; i < edges.size();) {
        size_t e = i;
        Vec3f  n = Vec3f::Zero();
        for (; e < edges.size() && edges[e].first == edges[i].first; ++e)
            n += tn[edges[e].second];
        keys.push_back(edges[i].first);
        normal.push_back(n);
        i = e;
    }
    const size_t       base = its.vertices.size();
    std::vector<Vec3f> mid(keys.size());
    std::vector<float> mid_d(keys.size());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, keys.size()), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t i = r.begin(); i < r.end(); ++i) {
            const Vec3d  a = its.vertices[size_t(keys[i] >> 32)].cast<double>(), b = its.vertices[size_t(keys[i] & 0xFFFFFFFFu)].cast<double>();
            const Vec3d  m = 0.5 * (a + b);
            const double half = 0.5 * (b - a).norm();
            Vec3d        n    = normal[i].cast<double>();
            const double nl   = n.norm();
            mid[i]            = m.cast<float>();
            mid_d[i]          = 0.5f * (dist[size_t(keys[i] >> 32)] + dist[size_t(keys[i] & 0xFFFFFFFFu)]);
            if (nl <= 0. || half <= 0.)
                continue;
            n /= nl;
            const PsValue v0 = field.eval(m);
            mid_d[i]         = v0.d;
            if (v0.g == 0.)
                continue;
            // Outward (towards positive g) along the normal when inside, inward when outside.
            const Vec3d   q  = v0.g < 0. ? Vec3d(m + half * n) : Vec3d(m - half * n);
            const PsValue v1 = field.eval(q);
            if ((v1.g < 0.) == (v0.g < 0.))
                continue; // no crossing within half an edge: keep the midpoint
            float        d = v0.d;
            const double t = std::clamp(field.root(m, q, v0.g, v1.g, 4, &d), 0., 1.);
            mid[i]         = (m + t * (q - m)).cast<float>();
            mid_d[i]       = d;
        }
    });
    its.vertices.insert(its.vertices.end(), mid.begin(), mid.end());
    dist.insert(dist.end(), mid_d.begin(), mid_d.end());
    auto split_of = [&](int a, int b) -> int {
        const uint64_t k  = ekey(a, b);
        auto           it = std::lower_bound(keys.begin(), keys.end(), k);
        return it != keys.end() && *it == k ? int(base + size_t(it - keys.begin())) : -1;
    };
    std::vector<Vec3i32> out;
    out.reserve(nt + 3 * keys.size());
    for (size_t f = 0; f < nt; ++f) {
        const Vec3i32 t = its.indices[f];
        const int     m[3] = { split_of(t[0], t[1]), split_of(t[1], t[2]), split_of(t[2], t[0]) };
        const int     n    = (m[0] >= 0) + (m[1] >= 0) + (m[2] >= 0);
        if (n == 0)
            out.push_back(t);
        else if (n == 3) {
            out.emplace_back(t[0], m[0], m[2]);
            out.emplace_back(m[0], t[1], m[1]);
            out.emplace_back(m[2], m[1], t[2]);
            out.emplace_back(m[0], m[1], m[2]);
        } else if (n == 1) {
            const int k = m[0] >= 0 ? 0 : m[1] >= 0 ? 1 : 2; // edge k runs t[k] -> t[k+1]
            const int a = t[k], b = t[(k + 1) % 3], c = t[(k + 2) % 3];
            out.emplace_back(a, m[k], c);
            out.emplace_back(m[k], b, c);
        } else {
            const int k = m[0] < 0 ? 0 : m[1] < 0 ? 1 : 2; // the edge NOT split runs t[k] -> t[k+1]
            const int a = t[k], b = t[(k + 1) % 3], c = t[(k + 2) % 3];
            const int mb = m[(k + 1) % 3], mc = m[(k + 2) % 3]; // on b -> c and c -> a
            out.emplace_back(mb, c, mc);
            // The quad a, b, mb, mc: the shorter diagonal.
            if ((its.vertices[size_t(a)] - its.vertices[size_t(mb)]).squaredNorm() <
                (its.vertices[size_t(b)] - its.vertices[size_t(mc)]).squaredNorm()) {
                out.emplace_back(a, b, mb);
                out.emplace_back(a, mb, mc);
            } else {
                out.emplace_back(a, b, mc);
                out.emplace_back(b, mb, mc);
            }
        }
    }
    its.indices.swap(out);
}

// ----------------------------------------------------------------------------------------- the dense field (P2)

indexed_triangle_set ps_dense_claim(const indexed_triangle_set& mesh, const indexed_triangle_set& S, const indexed_triangle_set& U,
                                    const BoundingBoxf3& sb, const BoundingBoxf3& mb, const PaintSplitSkinParams& params,
                                    PaintSplitClaimStats& stats)
{
    const auto   t0    = PsClock::now();
    const double depth = std::max(0., params.depth);
    const double reach = depth > 0. ? depth : std::min(0.5 * sb.size().norm() + 5., 0.5 * mb.size().norm());
    BoundingBoxf3 box(sb.min - Vec3d::Constant(reach + 1.), sb.max + Vec3d::Constant(reach + 1.));
    box.min = box.min.cwiseMax(mb.min - Vec3d::Constant(1.));
    box.max = box.max.cwiseMin(mb.max + Vec3d::Constant(1.));
    const Vec3d size = box.size();
    double      h    = params.voxel;
    if (h <= 0.)
        h = std::max(0.15, std::cbrt(size.x() * size.y() * size.z() / double(std::max<size_t>(1000, params.max_samples))));
    stats.voxel = stats.near_skin_voxel = h;
    const int      nx = int(std::ceil(size.x() / h)) + 3, ny = int(std::ceil(size.y() / h)) + 3, nz = int(std::ceil(size.z() / h)) + 3;
    const Vec3d    origin = box.min - Vec3d::Constant(h);
    const AABBMesh tree_s(S), tree_u(U);
    const float    out_v = float(2. * h);
    std::vector<float> phi(size_t(nx) * size_t(ny) * size_t(nz), out_v);
    stats.setup_s = ps_secs_since(t0);
    const auto t1 = PsClock::now();
    tbb::parallel_for(tbb::blocked_range<int>(1, nz - 1), [&](const tbb::blocked_range<int>& range) {
        for (int k = range.begin(); k < range.end(); ++k)
            for (int j = 1; j < ny - 1; ++j)
                for (int i = 1; i < nx - 1; ++i) {
                    const Vec3d  p  = origin + h * Vec3d(i, j, k);
                    const double ds = std::sqrt(tree_s.squared_distance(p));
                    float        v;
                    if (ds > reach + h)
                        v = out_v;
                    else if (depth > 0. && ds > depth)
                        v = float(std::min(ds - depth, 2. * h));
                    else {
                        const double du = std::sqrt(tree_u.squared_distance(p));
                        double       f  = ds - du;
                        if (depth > 0.)
                            f = std::max(f, ds - depth);
                        v = float(std::clamp(f, -2. * h, 2. * h));
                    }
                    phi[(size_t(k) * size_t(ny) + size_t(j)) * size_t(nx) + size_t(i)] = v;
                }
    });
    stats.samples = phi.size();
    stats.field_s = ps_secs_since(t1);
    const auto           t2    = PsClock::now();
    indexed_triangle_set claim = mesh_dense_field(phi, nx, ny, nz, origin, h);
    // Outward, whichever way the mesher winds.
    if (its_volume(claim) < 0.)
        its_flip_triangles(claim);
    stats.mesh_s    = ps_secs_since(t2);
    stats.triangles = claim.indices.size();
    return claim;
}

} // namespace

// ----------------------------------------------------------------------------------------- the sparse claim

PaintSplitClaim paint_split_nearest_cutter(const indexed_triangle_set& mesh, const PaintSplitRegion& region, int comp,
                                           const PaintSplitSkinParams& params)
{
    PaintSplitClaim out;
    if (region.error != PaintSplitError::None || comp < 0 || size_t(comp) >= region.num_components() || mesh.indices.empty())
        return out;
    const auto t0 = PsClock::now();
    // The skin split in two: this component's selected facets, and every other facet of the part.
    indexed_triangle_set S, U;
    S.vertices = region.surface.vertices;
    U.vertices = region.surface.vertices;
    double area_s = 0., area_u = 0.;
    for (size_t f = 0; f < region.surface.indices.size(); ++f) {
        const Vec3i32& t = region.surface.indices[f];
        const double   a = 0.5 * double((region.surface.vertices[t[1]] - region.surface.vertices[t[0]])
                                            .cross(region.surface.vertices[t[2]] - region.surface.vertices[t[0]])
                                            .norm());
        if (region.facet_comp[f] == comp)
            S.indices.push_back(t), area_s += a;
        else
            U.indices.push_back(t), area_u += a;
    }
    if (S.indices.empty() || U.indices.empty())
        return out;
    BoundingBoxf3 sb, mb;
    for (const Vec3i32& t : S.indices)
        for (int k = 0; k < 3; ++k)
            sb.merge(S.vertices[t[k]].cast<double>());
    for (const Vec3f& v : mesh.vertices)
        mb.merge(v.cast<double>());
    PaintSplitClaimStats& st = out.stats;
    if (params.dense) {
        out.mesh = ps_dense_claim(mesh, S, U, sb, mb, params, st);
        return out;
    }

    const double depth = std::max(0., params.depth);
    const double reach = depth > 0. ? depth : std::min(0.5 * sb.size().norm() + 5., 0.5 * mb.size().norm());
    BoundingBoxf3 box(sb.min - Vec3d::Constant(reach + 1.), sb.max + Vec3d::Constant(reach + 1.));
    box.min = box.min.cwiseMax(mb.min - Vec3d::Constant(1.));
    box.max = box.max.cwiseMin(mb.max + Vec3d::Constant(1.));
    // 0.6 mm by default: on the owner's figure (455k facets) 0.4 mm halved the face's roughness again but doubled
    // the booleans' time; the seam gets its fine voxel from the near-skin refinement instead (0.075 mm).
    const double h0 = params.voxel > 0. ? params.voxel : 0.6;

    const AABBMesh      tree_s(S), tree_u(U);
    const PsOutside     outside(mesh, std::max(8. * h0, 1.) + 1.);
    std::atomic<size_t> queries{ 0 };
    PsField             field;
    field.ts         = &tree_s;
    field.tu         = &tree_u;
    field.outside    = &outside;
    field.reach      = reach + h0;
    field.depth      = depth;
    field.clip       = std::max(8. * h0, 1.);
    field.complement = params.allow_complement && area_s > area_u;
    // Outside the box counts as outside the cutter: for the complement that would claim it, so its box holds
    // the whole part and the clip shell round it.
    if (field.complement)
        box = BoundingBoxf3(mb.min - Vec3d::Constant(field.clip + 2. * h0), mb.max + Vec3d::Constant(field.clip + 2. * h0));
    field.box = box;
    field.queries    = &queries;
    out.complement   = field.complement;
    st.complement    = field.complement;
    st.setup_s       = ps_secs_since(t0);

    // ---- E1: coarse to fine. The top cells are 2^L fine voxels, about 4 mm.
    const auto t1 = PsClock::now();
    int        L  = 0;
    while (L < 8 && h0 * double(1 << (L + 1)) <= 4.5)
        ++L;
    const Vec3d    origin = box.min - Vec3d::Constant(h0);
    const Vec3d    span   = box.size() + Vec3d::Constant(2. * h0);
    const uint32_t top    = 1u << L;
    uint32_t       ntop[3];
    for (int a = 0; a < 3; ++a)
        ntop[a] = uint32_t(std::ceil(span(a) / (h0 * double(top)))) + 1;
    if (uint64_t(ntop[0]) * top >= (1u << 21) || uint64_t(ntop[1]) * top >= (1u << 21) || uint64_t(ntop[2]) * top >= (1u << 21)) {
        BOOST_LOG_TRIVIAL(error) << "Paint split: the claim's lattice is too large";
        return out;
    }
    auto node_pos = [&](double i, double j, double k) { return Vec3d(origin + h0 * Vec3d(i, j, k)); };

    std::vector<uint64_t> cells; // cell keys in units of the current level
    cells.reserve(size_t(ntop[0]) * ntop[1] * ntop[2]);
    for (uint32_t k = 0; k < ntop[2]; ++k)
        for (uint32_t j = 0; j < ntop[1]; ++j)
            for (uint32_t i = 0; i < ntop[0]; ++i)
                cells.push_back(ps_key(i, j, k));
    int    level = L;
    size_t evaluated = 0, prev_kept = 0;
    for (;; --level) {
        const double          size = double(1u << level);
        const double          lim  = std::sqrt(3.) * size * h0 * 1.001 + 1e-9; // 2 x the half-diagonal
        std::vector<uint8_t>  keep(cells.size(), 0);
        tbb::parallel_for(tbb::blocked_range<size_t>(0, cells.size()), [&](const tbb::blocked_range<size_t>& r) {
            for (size_t c = r.begin(); c < r.end(); ++c) {
                const uint64_t key = cells[c];
                const Vec3d    p   = node_pos((ps_ki(key) + 0.5) * size, (ps_kj(key) + 0.5) * size, (ps_kk(key) + 0.5) * size);
                keep[c]            = std::abs(field.eval(p).g) <= lim;
            }
        });
        evaluated += cells.size();
        size_t w = 0;
        for (size_t c = 0; c < cells.size(); ++c)
            if (keep[c])
                cells[w++] = cells[c];
        cells.resize(w);
        if (level == 0)
            break;
        // The cell budget: a surface's cells grow about 4 times per level, so stop where the next level would not
        // fit. The voxel then doubles once per level short of the finest.
        // The growth from the last level predicts the next one better once the cells are small.
        const double growth = prev_kept > 0 ? std::max(4., double(cells.size()) / double(prev_kept)) : 4.;
        if (growth * double(cells.size()) > double(std::max<size_t>(1000, params.max_cells)))
            break;
        prev_kept = cells.size();
        std::vector<uint64_t> next(cells.size() * 8);
        tbb::parallel_for(tbb::blocked_range<size_t>(0, cells.size()), [&](const tbb::blocked_range<size_t>& r) {
            for (size_t c = r.begin(); c < r.end(); ++c) {
                const uint32_t i = 2 * ps_ki(cells[c]), j = 2 * ps_kj(cells[c]), k = 2 * ps_kk(cells[c]);
                for (int q = 0; q < 8; ++q)
                    next[8 * c + size_t(q)] = ps_key(i + (q & 1), j + ((q >> 1) & 1), k + ((q >> 2) & 1));
            }
        });
        cells.swap(next);
    }
    const uint32_t step = 1u << level; // fine voxels per cell
    const double   h    = h0 * double(step);
    st.voxel            = h;

    // The cells' corners, evaluated once each; then the closure pass: a cell whose face to a cell outside the set
    // changes sign takes that neighbour in too, until no face of the set does. The claim is then closed even where
    // g jumps (at the clip's flood boundary) and the Lipschitz skip was wrong there.
    tbb::parallel_sort(cells.begin(), cells.end());
    std::vector<uint64_t> nodes;
    std::vector<PsValue>  nval;
    auto corner_key = [step](uint64_t cell, int q) {
        return ps_key((ps_ki(cell) + uint32_t(q & 1)) * step, (ps_kj(cell) + uint32_t((q >> 1) & 1)) * step,
                      (ps_kk(cell) + uint32_t((q >> 2) & 1)) * step);
    };
    auto add_nodes = [&](const std::vector<uint64_t>& new_cells) {
        std::vector<uint64_t> add(new_cells.size() * 8);
        tbb::parallel_for(tbb::blocked_range<size_t>(0, new_cells.size()), [&](const tbb::blocked_range<size_t>& r) {
            for (size_t c = r.begin(); c < r.end(); ++c)
                for (int q = 0; q < 8; ++q)
                    add[8 * c + size_t(q)] = corner_key(new_cells[c], q);
        });
        tbb::parallel_sort(add.begin(), add.end());
        add.erase(std::unique(add.begin(), add.end()), add.end());
        std::vector<uint64_t> fresh;
        fresh.reserve(add.size());
        for (uint64_t k : add)
            if (!std::binary_search(nodes.begin(), nodes.end(), k))
                fresh.push_back(k);
        std::vector<PsValue> fv(fresh.size());
        tbb::parallel_for(tbb::blocked_range<size_t>(0, fresh.size()), [&](const tbb::blocked_range<size_t>& r) {
            for (size_t n = r.begin(); n < r.end(); ++n)
                fv[n] = field.eval(node_pos(ps_ki(fresh[n]), ps_kj(fresh[n]), ps_kk(fresh[n])));
        });
        std::vector<uint64_t> mk;
        std::vector<PsValue>  mv;
        mk.reserve(nodes.size() + fresh.size());
        mv.reserve(nodes.size() + fresh.size());
        size_t x = 0, y = 0;
        while (x < nodes.size() || y < fresh.size())
            if (y == fresh.size() || (x < nodes.size() && nodes[x] < fresh[y]))
                mk.push_back(nodes[x]), mv.push_back(nval[x++]);
            else
                mk.push_back(fresh[y]), mv.push_back(fv[y++]);
        nodes.swap(mk);
        nval.swap(mv);
    };
    add_nodes(cells);
    size_t                closure_added = 0;
    std::vector<uint64_t> frontier      = cells;
    for (int pass = 0; pass < 100000 && !frontier.empty(); ++pass) {
        tbb::enumerable_thread_specific<std::vector<uint64_t>> found;
        tbb::parallel_for(tbb::blocked_range<size_t>(0, frontier.size()), [&](const tbb::blocked_range<size_t>& r) {
            std::vector<uint64_t>& out = found.local();
            for (size_t c = r.begin(); c < r.end(); ++c) {
                const uint64_t cell = frontier[c];
                bool           in[8];
                for (int q = 0; q < 8; ++q)
                    in[q] = nval[size_t(std::lower_bound(nodes.begin(), nodes.end(), corner_key(cell, q)) - nodes.begin())].g < 0.;
                const uint32_t ijk[3] = { ps_ki(cell), ps_kj(cell), ps_kk(cell) };
                for (int axis = 0; axis < 3; ++axis)
                    for (int side = 0; side < 2; ++side) {
                        int n_in = 0;
                        for (int q = 0; q < 8; ++q)
                            if (((q >> axis) & 1) == side)
                                n_in += in[q];
                        if (n_in == 0 || n_in == 4)
                            continue;
                        if (side == 0 && ijk[axis] == 0)
                            continue;
                        uint32_t nb[3] = { ijk[0], ijk[1], ijk[2] };
                        nb[axis]       = side == 0 ? nb[axis] - 1 : nb[axis] + 1;
                        out.push_back(ps_key(nb[0], nb[1], nb[2]));
                    }
            }
        });
        std::vector<uint64_t> cand;
        for (const std::vector<uint64_t>& f : found)
            cand.insert(cand.end(), f.begin(), f.end());
        std::sort(cand.begin(), cand.end());
        cand.erase(std::unique(cand.begin(), cand.end()), cand.end());
        cand.erase(std::remove_if(cand.begin(), cand.end(), [&](uint64_t k) { return std::binary_search(cells.begin(), cells.end(), k); }),
                   cand.end());
        if (cand.empty())
            break;
        add_nodes(cand);
        std::vector<uint64_t> merged(cells.size() + cand.size());
        std::merge(cells.begin(), cells.end(), cand.begin(), cand.end(), merged.begin());
        cells.swap(merged);
        closure_added += cand.size();
        frontier.swap(cand);
    }
    st.cells = cells.size();
    st.samples  = evaluated + nodes.size();
    st.field_s  = ps_secs_since(t1);

    // ---- S1: marching tetrahedra (Kuhn split, consistent across cells) with exact roots.
    const auto t2      = PsClock::now();
    auto       node_of = [&](uint64_t key) { return uint32_t(std::lower_bound(nodes.begin(), nodes.end(), key) - nodes.begin()); };
    using Tri          = std::array<uint64_t, 3>; // edge keys (node pairs)
    tbb::enumerable_thread_specific<std::vector<Tri>> tls;
    static const int kuhn[6][4] = { { 0, 1, 3, 7 }, { 0, 1, 5, 7 }, { 0, 2, 3, 7 }, { 0, 2, 6, 7 }, { 0, 4, 5, 7 }, { 0, 4, 6, 7 } };
    auto ekey = [](uint32_t a, uint32_t b) {
        if (a > b)
            std::swap(a, b);
        return (uint64_t(a) << 32) | uint64_t(b);
    };
    tbb::parallel_for(tbb::blocked_range<size_t>(0, cells.size()), [&](const tbb::blocked_range<size_t>& r) {
        std::vector<Tri>& tris = tls.local();
        for (size_t c = r.begin(); c < r.end(); ++c) {
            const uint32_t i = ps_ki(cells[c]) * step, j = ps_kj(cells[c]) * step, k = ps_kk(cells[c]) * step;
            uint32_t       cn[8];
            bool           in[8];
            int            nin = 0;
            for (int q = 0; q < 8; ++q) {
                cn[q] = node_of(ps_key(i + (q & 1) * step, j + ((q >> 1) & 1) * step, k + ((q >> 2) & 1) * step));
                in[q] = nval[cn[q]].g < 0.;
                nin += in[q];
            }
            if (nin == 0 || nin == 8)
                continue;
            for (const auto& tet : kuhn) {
                int ins[4], outs[4], ni = 0, no = 0;
                for (int q = 0; q < 4; ++q)
                    (in[tet[q]] ? ins[ni++] : outs[no++]) = tet[q];
                if (ni == 0 || no == 0)
                    continue;
                // Corner positions in cell units, for the orientation test.
                auto cp = [](int q) { return Vec3d(q & 1, (q >> 1) & 1, (q >> 2) & 1); };
                Vec3d in_c = Vec3d::Zero(), out_c = Vec3d::Zero();
                for (int q = 0; q < ni; ++q)
                    in_c += cp(ins[q]) / double(ni);
                for (int q = 0; q < no; ++q)
                    out_c += cp(outs[q]) / double(no);
                const Vec3d dir = out_c - in_c; // the claim's outward normal points from inside to outside
                auto emit = [&](std::array<std::pair<int, int>, 3> e) {
                    Vec3d m[3];
                    for (int q = 0; q < 3; ++q)
                        m[q] = 0.5 * (cp(e[q].first) + cp(e[q].second));
                    if ((m[1] - m[0]).cross(m[2] - m[0]).dot(dir) < 0.)
                        std::swap(e[1], e[2]);
                    tris.push_back({ ekey(cn[e[0].first], cn[e[0].second]), ekey(cn[e[1].first], cn[e[1].second]),
                                     ekey(cn[e[2].first], cn[e[2].second]) });
                };
                if (ni == 1)
                    emit({ { { ins[0], outs[0] }, { ins[0], outs[1] }, { ins[0], outs[2] } } });
                else if (no == 1)
                    emit({ { { ins[0], outs[0] }, { ins[1], outs[0] }, { ins[2], outs[0] } } });
                else {
                    // A quad: (i0,o0) (i0,o1) (i1,o1) (i1,o0) in order round it.
                    emit({ { { ins[0], outs[0] }, { ins[0], outs[1] }, { ins[1], outs[1] } } });
                    emit({ { { ins[0], outs[0] }, { ins[1], outs[1] }, { ins[1], outs[0] } } });
                }
            }
        }
    });
    std::vector<Tri> tris;
    for (std::vector<Tri>& t : tls)
        tris.insert(tris.end(), t.begin(), t.end());
    std::vector<uint64_t> edges;
    edges.reserve(tris.size() * 3 / 2 + 16);
    for (const Tri& t : tris)
        edges.insert(edges.end(), t.begin(), t.end());
    tbb::parallel_sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());

    indexed_triangle_set claim;
    claim.vertices.resize(edges.size());
    std::vector<float> dist(edges.size());
    const int          iters = std::max(0, params.root_iterations);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, edges.size()), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t e = r.begin(); e < r.end(); ++e) {
            const uint32_t a = uint32_t(edges[e] >> 32), b = uint32_t(edges[e] & 0xFFFFFFFFu);
            const Vec3d    pa = node_pos(ps_ki(nodes[a]), ps_kj(nodes[a]), ps_kk(nodes[a]));
            const Vec3d    pb = node_pos(ps_ki(nodes[b]), ps_kj(nodes[b]), ps_kk(nodes[b]));
            const double   ga = nval[a].g, gb = nval[b].g;
            double         t;
            float          d;
            if ((nval[a].clip && nval[b].clip) || iters == 0) {
                // The clip shell outside the part (or linear roots asked for): linear is enough.
                t = ga / (ga - gb);
                d = float((1. - t) * nval[a].d + t * nval[b].d);
            } else
                t = field.root(pa, pb, ga, gb, iters, &d);
            t                 = std::clamp(t, 0.01, 0.99); // keep the vertices of one corner apart
            claim.vertices[e] = (pa + t * (pb - pa)).cast<float>();
            dist[e]           = d;
        }
    });
    claim.indices.resize(tris.size());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, tris.size()), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t f = r.begin(); f < r.end(); ++f)
            for (int q = 0; q < 3; ++q)
                claim.indices[f][q] = int(std::lower_bound(edges.begin(), edges.end(), tris[f][q]) - edges.begin());
    });
    tris  = std::vector<Tri>();
    edges = std::vector<uint64_t>();

    // ---- D23 (a): a finer voxel near the skin.
    double fine = h;
    // (Not below 0.05 mm: a tenth of a nozzle width is the target, D23.)
    for (int round = 0; round < params.skin_refine && fine * 0.5 >= 0.05 - 1e-9; ++round) {
        // The band narrows each round: the seam needs the fine voxel, the face beside it does not.
        ps_refine_near_skin(claim, dist, field, (round == 0 ? 1. : 0.5) * h);
        fine *= 0.5;
    }
    st.near_skin_voxel = fine;
    st.mesh_s          = ps_secs_since(t2);

    // ---- S2: pinned Taubin smoothing.
    const auto t3     = PsClock::now();
    const int  passes = int(std::lround(std::clamp(params.smoothing, 0., 1.) * 20.));
    if (passes > 0) {
        const double         pin = 1.5 * h;
        std::vector<float>   w(claim.vertices.size());
        for (size_t v = 0; v < w.size(); ++v)
            w[v] = float(std::clamp((double(dist[v]) - pin) / pin, 0., 1.));
        // A triangle touching the pin band pins all three corners: one moving corner would move the seam.
        std::vector<uint8_t> pinned(claim.vertices.size(), 0);
        for (const Vec3i32& t : claim.indices)
            if (dist[size_t(t[0])] < pin || dist[size_t(t[1])] < pin || dist[size_t(t[2])] < pin)
                pinned[size_t(t[0])] = pinned[size_t(t[1])] = pinned[size_t(t[2])] = 1;
        for (size_t v = 0; v < w.size(); ++v)
            if (pinned[v])
                w[v] = 0.f;
        ps_taubin(claim, w, passes);
    }
    st.smooth_s = ps_secs_since(t3);

    if (its_volume(claim) < 0.)
        its_flip_triangles(claim);
    st.triangles = claim.indices.size();
    st.queries   = queries.load();
    out.mesh     = std::move(claim);
    BOOST_LOG_TRIVIAL(info) << "Paint split claim: voxel " << st.voxel << " mm (near the skin " << st.near_skin_voxel << " mm), "
                            << st.cells << " cells (" << closure_added << " from the closure pass), " << st.samples << " samples, " << st.queries << " distance queries, "
                            << st.triangles << " triangles" << (st.complement ? " (complement)" : "") << "; setup " << st.setup_s
                            << " s, field " << st.field_s << " s, mesh " << st.mesh_s << " s, smooth " << st.smooth_s << " s";
    return out;
}

indexed_triangle_set paint_split_nearest_claim(const indexed_triangle_set& mesh, const PaintSplitRegion& region, int comp,
                                               const PaintSplitSkinParams& params, double* voxel_used)
{
    PaintSplitSkinParams p = params;
    p.allow_complement     = false;
    PaintSplitClaim c      = paint_split_nearest_cutter(mesh, region, comp, p);
    if (voxel_used != nullptr)
        *voxel_used = c.stats.voxel;
    return std::move(c.mesh);
}

} // namespace Slic3r
