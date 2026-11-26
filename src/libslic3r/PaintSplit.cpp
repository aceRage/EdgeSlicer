// Paint Split engine, phase 1: the region and its boundary, the Plane cutter and the keep-connected rule.
// Design: tests/design_paint_split.md (sections 4.1 - 4.3b). The Manifold conversion is the colour split's
// (ColorSplitInternal.hpp), so both features ingest and export meshes the same way.
#include "PaintSplit.hpp"
#include "AABBMesh.hpp"
#include "ColorSplit.hpp"
#include "ColorSplitInternal.hpp"
#include "ExPolygon.hpp"
#include "Tesselate.hpp"
#include "TriangleMesh.hpp"
#include "TriangleMeshSlicer.hpp"

#include <Eigen/Eigenvalues>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <unordered_map>
#include <utility>

#ifndef L
#define L(s) (s)
#define PAINT_SPLIT_DEFINED_L
#endif

namespace Slic3r {

const char* paint_split_error_message(PaintSplitError err)
{
    switch (err) {
    case PaintSplitError::None: return "";
    case PaintSplitError::OpenMesh: return L("The part has holes (open edges). Repair it before splitting.");
    case PaintSplitError::RepairFailed: return L("The part has holes or broken edges that could not be repaired automatically.");
    case PaintSplitError::NonManifold: return L("The part is not a clean solid (non-manifold). Repair it before splitting.");
    case PaintSplitError::EmptyMask: return L("Nothing is selected. Select the region to split off first.");
    case PaintSplitError::NoBoundary: return L("The selection covers a whole shell; there is no boundary to cut along.");
    case PaintSplitError::Band: return L("This region is a band with two edges. Split it as two cuts.");
    case PaintSplitError::MultiLoop: return L("This region has several separate edges. Fill its holes or select a simpler region.");
    case PaintSplitError::BadComponent: return L("The selected region no longer exists.");
    case PaintSplitError::BooleanFailed: return L("The split failed in the boolean operation.");
    case PaintSplitError::EmptyPiece: return L("The cut does not reach the selected region.");
    case PaintSplitError::EmptyBody: return L("The piece would take the whole part.");
    case PaintSplitError::CutCrossesRegion: return L("This edge's cut passes through the selected region. Change Offset or Tilt, fill the region's holes, or select a simpler region.");
    }
    return "";
}

const char* paint_split_warning_message(PaintSplitWarning w)
{
    switch (w) {
    case PaintSplitWarning::PlaneFarFromBoundary: return L("The plane is far from the region's edge. Try Follow paint for a twisted edge.");
    case PaintSplitWarning::VolumeMismatch: return L("The pieces' volume differs slightly from the part's.");
    case PaintSplitWarning::ReunionSeam: return L("Returned pieces did not rejoin cleanly; the body was rebuilt by subtraction.");
    case PaintSplitWarning::ComponentsReturned: return L("Cut pieces without selected surface were left on the body.");
    case PaintSplitWarning::PiecesAttached: return L("Material hanging only from the piece was attached to it.");
    case PaintSplitWarning::LoosePieces: return L("Material hanging only from the piece became loose parts.");
    case PaintSplitWarning::Repaired: return L("Holes in the part were repaired while splitting.");
    }
    return "";
}

namespace {

double ps_tri_area(const indexed_triangle_set& its, size_t f)
{
    const Vec3i32& t = its.indices[f];
    const Vec3d    a = its.vertices[t[0]].cast<double>(), b = its.vertices[t[1]].cast<double>(), c = its.vertices[t[2]].cast<double>();
    return 0.5 * (b - a).cross(c - a).norm();
}

Vec3d ps_tri_centroid(const indexed_triangle_set& its, size_t f)
{
    const Vec3i32& t = its.indices[f];
    return (its.vertices[t[0]].cast<double>() + its.vertices[t[1]].cast<double>() + its.vertices[t[2]].cast<double>()) / 3.;
}

struct PsDsu
{
    std::vector<int> p;
    explicit PsDsu(size_t n) : p(n) { std::iota(p.begin(), p.end(), 0); }
    int  find(int x) { while (p[x] != x) { p[x] = p[p[x]]; x = p[x]; } return x; }
    void unite(int a, int b) { a = find(a); b = find(b); if (a != b) p[b] = a; }
};

uint64_t ps_edge_key(int a, int b)
{
    if (a > b) std::swap(a, b);
    return (uint64_t(uint32_t(a)) << 32) | uint64_t(uint32_t(b));
}

// Faces per undirected edge of `its`.
std::unordered_map<uint64_t, std::vector<int>> ps_edge_faces(const indexed_triangle_set& its)
{
    std::unordered_map<uint64_t, std::vector<int>> out;
    out.reserve(its.indices.size() * 2);
    for (size_t f = 0; f < its.indices.size(); ++f)
        for (int i = 0; i < 3; ++i)
            out[ps_edge_key(its.indices[f][i], its.indices[f][(i + 1) % 3])].push_back(int(f));
    return out;
}

// Labels the connected components (across edges) of the faces for which `in_set` holds; -1 elsewhere.
int ps_components(const indexed_triangle_set& its, const std::unordered_map<uint64_t, std::vector<int>>& edges,
                  const std::vector<char>& in_set, std::vector<int>& label)
{
    PsDsu dsu(its.indices.size());
    for (const auto& [key, faces] : edges)
        for (size_t i = 0; i + 1 < faces.size(); ++i)
            for (size_t j = i + 1; j < faces.size(); ++j)
                if (in_set[faces[i]] && in_set[faces[j]])
                    dsu.unite(faces[i], faces[j]);
    label.assign(its.indices.size(), -1);
    std::map<int, int> root_id;
    for (size_t f = 0; f < its.indices.size(); ++f) {
        if (!in_set[f])
            continue;
        const int r  = dsu.find(int(f));
        auto      it = root_id.find(r);
        if (it == root_id.end())
            it = root_id.emplace(r, int(root_id.size())).first;
        label[f] = it->second;
    }
    return int(root_id.size());
}

// The boundary loops of component `comp` of `facet_comp` on `its`, chained from the component's half-edges
// whose other face is not in the component. At a pinch vertex a loop closes as soon as it can.
std::vector<PaintSplitLoop> ps_loops(const indexed_triangle_set& its, const std::unordered_map<uint64_t, std::vector<int>>& edges,
                                     const std::vector<int>& facet_comp, int comp)
{
    struct HalfEdge { int a, b, face; };
    std::vector<HalfEdge>             hes;
    std::multimap<int, size_t>        from;
    for (size_t f = 0; f < its.indices.size(); ++f) {
        if (facet_comp[f] != comp)
            continue;
        for (int i = 0; i < 3; ++i) {
            const int a = its.indices[f][i], b = its.indices[f][(i + 1) % 3];
            const auto it = edges.find(ps_edge_key(a, b));
            bool inner = false;
            if (it != edges.end())
                for (int g : it->second)
                    if (g != int(f) && facet_comp[g] == comp) { inner = true; break; }
            if (!inner) {
                from.emplace(a, hes.size());
                hes.push_back({ a, b, int(f) });
            }
        }
    }
    std::vector<char>           used(hes.size(), 0);
    std::vector<PaintSplitLoop> loops;
    for (size_t s = 0; s < hes.size(); ++s) {
        if (used[s])
            continue;
        PaintSplitLoop loop;
        const int      start = hes[s].a;
        size_t         cur   = s;
        for (size_t guard = 0; guard <= hes.size(); ++guard) {
            used[cur] = 1;
            const HalfEdge& he = hes[cur];
            DrawCutSample   smp;
            smp.pos    = its.vertices[he.a].cast<double>();
            smp.normal = its_face_normal(its, he.face).cast<double>();
            smp.facet  = size_t(he.face);
            loop.pts.push_back(smp);
            loop.length += (its.vertices[he.b] - its.vertices[he.a]).cast<double>().norm();
            if (he.b == start)
                break;
            size_t next = size_t(-1);
            for (auto [it, end] = from.equal_range(he.b); it != end; ++it)
                if (!used[it->second]) { next = it->second; break; }
            if (next == size_t(-1))
                break; // cannot happen on a closed surface; the loop is left open rather than looping forever
            cur = next;
        }
        loops.push_back(std::move(loop));
    }
    // Each loop's local inward direction: a few facet rings on each side of its edges, the selected side
    // flooded within the component, the other side outside it.
    {
        auto neighbours = [&](int f, std::vector<int>& out) {
            out.clear();
            for (int i = 0; i < 3; ++i) {
                const auto it = edges.find(ps_edge_key(its.indices[f][i], its.indices[f][(i + 1) % 3]));
                if (it != edges.end())
                    for (int g : it->second)
                        if (g != f)
                            out.push_back(g);
            }
        };
        std::vector<int> nb;
        for (PaintSplitLoop& loop : loops) {
            // Three rings each side: deeper floods reach round the next bend of a figure and can point the wrong
            // way (owner's figure, 2026-11-25); paint_split_assign_loops tries the other side when this one fails.
            const int RINGS = 3;
            std::unordered_map<int, int> seen_in, seen_out; // face -> ring
            std::vector<int>             front_in, front_out;
            for (const DrawCutSample& s : loop.pts) {
                const int f = int(s.facet);
                if (seen_in.emplace(f, 0).second)
                    front_in.push_back(f);
                neighbours(f, nb);
                for (int g : nb)
                    if (facet_comp[g] != comp && seen_out.emplace(g, 0).second)
                        front_out.push_back(g);
            }
            auto flood = [&](std::vector<int> front, std::unordered_map<int, int>& seen, bool inside) {
                for (int ring = 1; ring < RINGS && !front.empty(); ++ring) {
                    std::vector<int> next;
                    for (int f : front) {
                        neighbours(f, nb);
                        for (int g : nb)
                            if ((facet_comp[g] == comp) == inside && seen.emplace(g, ring).second)
                                next.push_back(g);
                    }
                    front = std::move(next);
                }
                Vec3d  c = Vec3d::Zero();
                double a = 0.;
                for (const auto& [f, ring] : seen) {
                    const double fa = ps_tri_area(its, size_t(f));
                    c += fa * ps_tri_centroid(its, size_t(f));
                    a += fa;
                }
                return a > 0. ? Vec3d(c / a) : Vec3d(Vec3d::Zero());
            };
            const Vec3d cin = flood(front_in, seen_in, true);
            if (!seen_out.empty())
                loop.inward = cin - flood(front_out, seen_out, false);
        }
    }
    std::sort(loops.begin(), loops.end(), [](const PaintSplitLoop& l, const PaintSplitLoop& r) { return l.length > r.length; });
    if (!loops.empty())
        loops.front().is_outer = true;
    return loops;
}

} // namespace

// -------------------------------------------------------------------------------------------- repair

namespace {

// Uses of every undirected edge of `its`.
std::unordered_map<uint64_t, int> ps_edge_use(const indexed_triangle_set& its)
{
    std::unordered_map<uint64_t, int> use;
    use.reserve(its.indices.size() * 2);
    for (const Vec3i32& t : its.indices)
        for (int i = 0; i < 3; ++i)
            ++use[ps_edge_key(t[i], t[(i + 1) % 3])];
    return use;
}

bool ps_degenerate(const Vec3i32& t) { return t[0] == t[1] || t[1] == t[2] || t[2] == t[0]; }

} // namespace

bool paint_split_repair(const indexed_triangle_set& in, indexed_triangle_set& out, std::vector<int>* old_to_new,
                        PaintSplitRepairStats& st)
{
    st  = PaintSplitRepairStats();
    out = in;
    its_merge_vertices(out); // exact duplicates; the face order is kept
    if (old_to_new != nullptr) {
        old_to_new->resize(in.indices.size());
        std::iota(old_to_new->begin(), old_to_new->end(), 0);
    }
    {
        bool closed = std::none_of(out.indices.begin(), out.indices.end(), ps_degenerate);
        if (closed)
            for (const auto& [e, n] : ps_edge_use(out))
                if (n != 2) {
                    closed = false;
                    break;
                }
        if (closed)
            return true;
    }
    st.needed = true;

    // 1. Tolerance weld: vertices a float apart (the slivers a boolean exported in floats leaves) become one.
    BoundingBoxf3 bb;
    for (const Vec3f& v : out.vertices)
        bb.merge(v.cast<double>());
    const double tol = std::max(1e-5, 1e-7 * bb.size().norm());
    {
        struct CellHash
        {
            size_t operator()(const Vec3i64& c) const { return size_t(c.x() * 73856093) ^ size_t(c.y() * 19349663) ^ size_t(c.z() * 83492791); }
        };
        std::unordered_map<Vec3i64, std::vector<int>, CellHash> grid;
        std::vector<int>                                         remap(out.vertices.size());
        for (int v = 0; v < int(out.vertices.size()); ++v) {
            const Vec3d   p = out.vertices[v].cast<double>();
            const Vec3i64 c(int64_t(std::floor(p.x() / tol)), int64_t(std::floor(p.y() / tol)), int64_t(std::floor(p.z() / tol)));
            int           rep = -1;
            for (int dx = -1; dx <= 1 && rep < 0; ++dx)
                for (int dy = -1; dy <= 1 && rep < 0; ++dy)
                    for (int dz = -1; dz <= 1 && rep < 0; ++dz) {
                        const auto it = grid.find(Vec3i64(c.x() + dx, c.y() + dy, c.z() + dz));
                        if (it != grid.end())
                            for (int w : it->second)
                                if ((out.vertices[w].cast<double>() - p).norm() <= tol) {
                                    rep = w;
                                    break;
                                }
                    }
            if (rep < 0) {
                grid[c].push_back(v);
                rep = v;
            } else
                ++st.welded;
            remap[v] = rep;
        }
        for (Vec3i32& t : out.indices)
            for (int k = 0; k < 3; ++k)
                t[k] = remap[t[k]];
    }

    // 2. Drop the faces the weld collapsed; the others keep their order.
    {
        std::vector<Vec3i32> kept;
        kept.reserve(out.indices.size());
        for (size_t f = 0; f < out.indices.size(); ++f) {
            const bool drop = ps_degenerate(out.indices[f]);
            if (old_to_new != nullptr)
                (*old_to_new)[f] = drop ? -1 : int(kept.size());
            if (drop)
                ++st.degenerate_removed;
            else
                kept.push_back(out.indices[f]);
        }
        out.indices = std::move(kept);
    }

    // 3. Close every boundary loop with a fan around its centroid. A boundary half-edge a->b belongs to the one
    //    face using that edge; the fill runs b->a, so the patch faces the same way as the skin around it.
    {
        const auto                 use = ps_edge_use(out);
        std::multimap<int, int>    from; // a -> b of each boundary half-edge
        for (const Vec3i32& t : out.indices)
            for (int i = 0; i < 3; ++i) {
                const int a = t[i], b = t[(i + 1) % 3];
                if (use.at(ps_edge_key(a, b)) == 1)
                    from.emplace(a, b);
            }
        auto fill = [&](const std::vector<int>& loop) {
            const int k = int(loop.size());
            if (k < 3) {
                st.ok = false;
                return;
            }
            if (k == 3) {
                out.indices.emplace_back(loop[1], loop[0], loop[2]);
                ++st.faces_added;
            } else {
                Vec3d c = Vec3d::Zero();
                for (int v : loop)
                    c += out.vertices[v].cast<double>();
                c /= double(k);
                const int ci = int(out.vertices.size());
                out.vertices.push_back(c.cast<float>());
                for (int i = 0; i < k; ++i)
                    out.indices.emplace_back(loop[(i + 1) % k], loop[i], ci);
                st.faces_added += k;
            }
            ++st.holes_filled;
        };
        // Walk the boundary; whenever the walk comes back to a vertex already on it, the part from there is a
        // loop of its own. That splits holes that touch at one vertex (a pinch) into simple loops.
        while (!from.empty()) {
            std::vector<int>                path;
            std::unordered_map<int, size_t> pos;
            int                             cur = from.begin()->first;
            for (size_t guard = 0; guard <= 2 * out.indices.size() + 8; ++guard) {
                const auto it = from.find(cur);
                if (it == from.end()) {
                    st.ok = false; // a dangling boundary: not a loop
                    break;
                }
                pos[cur] = path.size();
                path.push_back(cur);
                const int next = it->second;
                from.erase(it);
                const auto back = pos.find(next);
                if (back == pos.end()) {
                    cur = next;
                    continue;
                }
                const size_t j = back->second;
                fill(std::vector<int>(path.begin() + j, path.end()));
                for (size_t q = j; q < path.size(); ++q)
                    pos.erase(path[q]);
                path.resize(j);
                if (path.empty())
                    break;
                cur = next;
            }
        }
    }
    its_compactify_vertices(out);

    for (const auto& [e, n] : ps_edge_use(out))
        if (n != 2) {
            st.ok = false;
            if (n > 2)
                ++st.nonmanifold_edges;
        }
    return st.ok;
}

TriangleSelector::TriangleSplittingData paint_split_remap_mask(const TriangleSelector::TriangleSplittingData& mask,
                                                               const std::vector<int>& old_to_new)
{
    TriangleSelector::TriangleSplittingData out = mask;
    out.triangles_to_split.clear();
    for (const TriangleSelector::TriangleBitStreamMapping& m : mask.triangles_to_split)
        if (m.triangle_idx >= 0 && size_t(m.triangle_idx) < old_to_new.size() && old_to_new[size_t(m.triangle_idx)] >= 0)
            out.triangles_to_split.emplace_back(old_to_new[size_t(m.triangle_idx)], m.bitstream_start_idx);
    return out;
}

// -------------------------------------------------------------------------------------------- region

PaintSplitRegion paint_split_region(const indexed_triangle_set& mesh, const TriangleSelector::TriangleSplittingData& mask,
                                    const PaintSplitRegionParams& params)
{
    PaintSplitRegion r;
    bool any_state = false;
    for (size_t s = 1; s < mask.used_states.size(); ++s)
        any_state = any_state || mask.used_states[s];
    if (mesh.indices.empty() || (!any_state && mask.triangles_to_split.empty())) {
        r.error = PaintSplitError::EmptyMask;
        return r;
    }

    // An open part is closed for the split (owner 2026-11-25), the mask carried onto the closed mesh.
    indexed_triangle_set                    fixed;
    std::vector<int>                        old_to_new;
    if (!paint_split_repair(mesh, fixed, &old_to_new, r.repair)) {
        r.error = PaintSplitError::RepairFailed;
        return r;
    }
    r.repaired = r.repair.needed;
    const indexed_triangle_set&                   src      = r.repaired ? fixed : mesh;
    const TriangleSelector::TriangleSplittingData src_mask = r.repaired ? paint_split_remap_mask(mask, old_to_new) : mask;

    ColorPatches patches;
    try {
        patches = extract_color_patches(src, src_mask);
    } catch (const ColorSplitError&) {
        r.error = PaintSplitError::NonManifold;
        return r;
    }
    if (patches.states.empty()) {
        r.error = PaintSplitError::EmptyMask;
        return r;
    }
    r.surface = std::move(patches.surface);
    const size_t nf = r.surface.indices.size();

    std::vector<char> selected(nf, 0), unselected(nf, 0);
    for (size_t f = 0; f < nf; ++f) {
        selected[f]   = patches.facet_state[f] != 0;
        unselected[f] = !selected[f];
    }
    const auto edges = ps_edge_faces(r.surface);

    std::vector<int> comp, ucomp;
    const int        ncomp  = ps_components(r.surface, edges, selected, comp);
    const int        nucomp = ps_components(r.surface, edges, unselected, ucomp);
    if (ncomp == 0) {
        r.error = PaintSplitError::EmptyMask;
        return r;
    }

    std::vector<double> area(ncomp, 0.), uarea(nucomp, 0.);
    for (size_t f = 0; f < nf; ++f) {
        const double a = ps_tri_area(r.surface, f);
        if (comp[f] >= 0) area[comp[f]] += a;
        else              uarea[ucomp[f]] += a;
    }
    // Which unselected components border which selected ones.
    std::vector<std::set<int>> adj(ncomp), uadj(nucomp);
    for (const auto& [key, faces] : edges)
        for (int f : faces)
            if (comp[f] >= 0)
                for (int g : faces)
                    if (g != f && ucomp[g] >= 0) {
                        adj[comp[f]].insert(ucomp[g]);
                        uadj[ucomp[g]].insert(comp[f]);
                    }

    // Hole fill (design 4.1 step 4): the biggest neighbouring unselected component is the outside; any other
    // that borders only this component and is small enough joins it.
    std::vector<int> fill_into(nucomp, -1);
    r.holes_filled.assign(ncomp, 0);
    const double frac = std::max(0., params.fill_holes_pct) / 100.;
    for (int c = 0; c < ncomp; ++c) {
        if (adj[c].empty())
            continue;
        const int outside = *std::max_element(adj[c].begin(), adj[c].end(), [&](int a, int b) { return uarea[a] < uarea[b]; });
        for (int u : adj[c])
            if (u != outside && uadj[u].size() == 1 && uarea[u] <= frac * area[c]) {
                fill_into[u] = c;
                ++r.holes_filled[c];
            }
    }
    for (size_t f = 0; f < nf; ++f)
        if (comp[f] < 0 && fill_into[ucomp[f]] >= 0)
            comp[f] = fill_into[ucomp[f]];
    for (int u = 0; u < nucomp; ++u)
        if (fill_into[u] >= 0)
            area[fill_into[u]] += uarea[u];

    r.facet_comp = std::move(comp);
    r.area       = std::move(area);
    r.loops.resize(ncomp);
    r.comp_error.assign(ncomp, PaintSplitError::None);
    for (int c = 0; c < ncomp; ++c) {
        r.loops[c] = ps_loops(r.surface, edges, r.facet_comp, c);
        // Bands and regions with several edges are cut once per edge (owner 2026-11-25, D7 reversed).
        if (r.loops[c].empty())
            r.comp_error[c] = PaintSplitError::NoBoundary;
    }
    return r;
}

void paint_split_transform(PaintSplitRegion& region, const Transform3d& trafo)
{
    const Matrix3d nm = trafo.matrix().block<3, 3>(0, 0).inverse().transpose();
    for (Vec3f& v : region.surface.vertices)
        v = (trafo * v.cast<double>()).cast<float>();
    if (trafo.matrix().block<3, 3>(0, 0).determinant() < 0.)
        for (Vec3i32& t : region.surface.indices)
            std::swap(t[1], t[2]);
    for (std::vector<PaintSplitLoop>& loops : region.loops)
        for (PaintSplitLoop& loop : loops) {
            loop.length = 0.;
            for (size_t i = 0; i < loop.pts.size(); ++i) {
                loop.pts[i].pos    = trafo * loop.pts[i].pos;
                loop.pts[i].normal = (nm * loop.pts[i].normal).normalized();
            }
            loop.inward = trafo.matrix().block<3, 3>(0, 0) * loop.inward;
            for (size_t i = 0; i < loop.pts.size(); ++i)
                loop.length += (loop.pts[(i + 1) % loop.pts.size()].pos - loop.pts[i].pos).norm();
        }
    for (size_t c = 0; c < region.area.size(); ++c) {
        double a = 0.;
        for (size_t f = 0; f < region.surface.indices.size(); ++f)
            if (region.facet_comp[f] == int(c))
                a += ps_tri_area(region.surface, f);
        region.area[c] = a;
    }
}

// -------------------------------------------------------------------------------------------- plane

static void ps_basis(const Vec3d& n, Vec3d& u, Vec3d& v)
{
    const Vec3d a = std::abs(n.x()) < 0.6 ? Vec3d::UnitX() : (std::abs(n.y()) < 0.6 ? Vec3d::UnitY() : Vec3d::UnitZ());
    u = n.cross(a).normalized();
    v = n.cross(u).normalized();
    // u x v = u x (n x u) = n
}

PaintSplitPlane paint_split_fit_plane(const PaintSplitRegion& region, int comp, const PaintSplitPlaneParams& params)
{
    return paint_split_fit_loop_plane(region, comp, 0, params);
}

std::vector<PaintSplitPlane> paint_split_fit_planes(const PaintSplitRegion& region, int comp, const PaintSplitPlaneParams& params)
{
    std::vector<PaintSplitPlane> out;
    if (comp >= 0 && size_t(comp) < region.loops.size())
        for (size_t l = 0; l < region.loops[size_t(comp)].size(); ++l)
            out.push_back(paint_split_fit_loop_plane(region, comp, int(l), params));
    return out;
}

PaintSplitPlane paint_split_fit_loop_plane(const PaintSplitRegion& region, int comp, int loop, const PaintSplitPlaneParams& params)
{
    PaintSplitPlane pl;
    if (region.error != PaintSplitError::None || comp < 0 || size_t(comp) >= region.loops.size() || loop < 0 ||
        size_t(loop) >= region.loops[comp].size())
        return pl;
    const PaintSplitLoop&             the_loop = region.loops[comp][size_t(loop)];
    const std::vector<DrawCutSample>& pts      = the_loop.pts;
    const size_t n = pts.size();
    if (n < 3)
        return pl;

    // Length-weighted centroid and covariance of the outer loop.
    std::vector<double> w(n);
    double              wsum = 0.;
    for (size_t i = 0; i < n; ++i) {
        w[i] = 0.5 * ((pts[i].pos - pts[(i + n - 1) % n].pos).norm() + (pts[(i + 1) % n].pos - pts[i].pos).norm());
        wsum += w[i];
    }
    if (wsum <= 0.)
        return pl;
    Vec3d c = Vec3d::Zero();
    for (size_t i = 0; i < n; ++i)
        c += w[i] * pts[i].pos;
    c /= wsum;
    Matrix3d cov = Matrix3d::Zero();
    for (size_t i = 0; i < n; ++i) {
        const Vec3d d = pts[i].pos - c;
        cov += w[i] * d * d.transpose();
    }
    Eigen::SelfAdjointEigenSolver<Matrix3d> es(cov);
    Vec3d normal = es.eigenvectors().col(0).normalized();

    // Point it at the selection: the side holding most of the selected area.
    // A flat patch has all of its area ON the plane; it then faces out of the part, the way its skin does
    // (the piece is what lies outside the plane, and a negative Offset takes a slab of the part).
    double side = 0., size = 0.;
    Vec3d  skin = Vec3d::Zero();
    for (size_t f = 0; f < region.surface.indices.size(); ++f)
        if (region.facet_comp[f] == comp) {
            const double a = ps_tri_area(region.surface, f);
            const Vec3d  g = ps_tri_centroid(region.surface, f);
            side += a * normal.dot(g - c);
            size = std::max(size, (g - c).norm());
            skin += a * its_face_normal(region.surface, int(f)).cast<double>();
        }
    if (std::abs(side) <= 1e-6 * std::max(1e-9, region.area[comp] * size))
        side = skin.dot(normal);
    // Several loops: the whole region lies on both sides of a band's edge, so each loop looks across itself.
    if (region.loops[comp].size() > 1 && std::abs(normal.dot(the_loop.inward)) > 1e-9)
        side = normal.dot(the_loop.inward);
    if (side < 0.)
        normal = -normal;

    double maxd = 0., radius = 0.;
    for (const DrawCutSample& s : pts) {
        maxd   = std::max(maxd, std::abs(normal.dot(s.pos - c)));
        radius = std::max(radius, (s.pos - c).norm());
    }
    pl.max_loop_distance = maxd;
    pl.far_from_boundary = maxd > std::max(5., 0.25 * 2. * radius);

    if (params.snap_to_bed)
        normal = normal.z() < 0. ? Vec3d(-Vec3d::UnitZ()) : Vec3d(Vec3d::UnitZ());
    Vec3d u, v;
    ps_basis(normal, u, v);
    if (params.tilt_u_deg != 0. || params.tilt_v_deg != 0.) {
        const Eigen::AngleAxisd ru(params.tilt_u_deg * PI / 180., u), rv(params.tilt_v_deg * PI / 180., v);
        normal = (ru * rv * normal).normalized();
        ps_basis(normal, u, v);
    }
    pl.normal = normal;
    pl.u      = u;
    pl.v      = v;
    pl.point  = c + params.offset * normal;
    pl.valid  = true;
    return pl;
}

// A box in the plane's frame: x, y in [-L, L] around the bbox centre's foot on the plane, z in [z0, z1].
static indexed_triangle_set ps_plane_box(const PaintSplitPlane& plane, const BoundingBoxf3& bbox, double z0, double z1)
{
    const Vec3d  centre = bbox.center();
    const double L      = bbox.size().norm() + 2.;
    const Vec3d  o      = centre - plane.normal * plane.normal.dot(centre - plane.point);
    indexed_triangle_set box = its_make_cube(2. * L, 2. * L, z1 - z0);
    for (Vec3f& p : box.vertices) {
        const Vec3d q = o + plane.u * (double(p.x()) - L) + plane.v * (double(p.y()) - L) + plane.normal * (double(p.z()) + z0);
        p             = q.cast<float>();
    }
    return box;
}

// The box on the far side of `plane` (against its normal) round `centre`'s foot on the plane: half-width `half` in
// the plane, reaching past `bbox`.
static indexed_triangle_set ps_loop_far_box(const PaintSplitPlane& plane, const Vec3d& centre, double half, const BoundingBoxf3& bbox)
{
    const double reach = bbox.size().norm() + 2. + std::abs(plane.normal.dot(bbox.center() - plane.point));
    const Vec3d  o     = centre - plane.normal * plane.normal.dot(centre - plane.point);
    indexed_triangle_set box = its_make_cube(2. * half, 2. * half, reach);
    for (Vec3f& p : box.vertices) {
        const Vec3d q = o + plane.u * (double(p.x()) - half) + plane.v * (double(p.y()) - half) + plane.normal * (double(p.z()) - reach);
        p             = q.cast<float>();
    }
    return box;
}

indexed_triangle_set paint_split_half_space(const PaintSplitPlane& plane, const BoundingBoxf3& bbox)
{
    const Vec3d  centre = bbox.center();
    const double reach  = bbox.size().norm() + 2. + std::abs(plane.normal.dot(centre - plane.point));
    return ps_plane_box(plane, bbox, 0., reach);
}

indexed_triangle_set paint_split_slab(const PaintSplitPlane& plane, const BoundingBoxf3& bbox, double kerf)
{
    const double h = 0.5 * std::max(kerf, 1e-3);
    return ps_plane_box(plane, bbox, -h, h);
}

indexed_triangle_set paint_split_cap(const indexed_triangle_set& mesh, const PaintSplitPlane& plane, const PaintSplitLoop& loop)
{
    indexed_triangle_set out;
    if (!plane.valid || mesh.indices.empty() || loop.pts.size() < 3)
        return out;
    // The plane's frame: x = u, y = v, z = normal, origin at plane.point. Only the facets the plane crosses are
    // sliced; they keep the whole vertex array, so their edges still chain into closed contours.
    indexed_triangle_set local;
    local.vertices.reserve(mesh.vertices.size());
    std::vector<double> dist(mesh.vertices.size());
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        const Vec3d d = mesh.vertices[i].cast<double>() - plane.point;
        dist[i]       = plane.normal.dot(d);
        local.vertices.emplace_back(Vec3f(float(plane.u.dot(d)), float(plane.v.dot(d)), float(dist[i])));
    }
    for (const Vec3i32& t : mesh.indices) {
        const double lo = std::min({ dist[t[0]], dist[t[1]], dist[t[2]] }), hi = std::max({ dist[t[0]], dist[t[1]], dist[t[2]] });
        if (lo <= 0. && hi >= 0.)
            local.indices.push_back(t);
    }
    if (local.indices.empty())
        return out;
    const std::vector<ExPolygons> layers = slice_mesh_ex(local, std::vector<float>{ 0.f });
    if (layers.empty() || layers.front().empty())
        return out;
    const ExPolygons& islands = layers.front();

    // Keep the islands the loop runs round: most of its samples, projected on the plane, lie in or near them.
    double r  = 0.;
    Vec3d  lc = Vec3d::Zero();
    for (const DrawCutSample& s : loop.pts)
        lc += s.pos;
    lc /= double(loop.pts.size());
    for (const DrawCutSample& s : loop.pts)
        r = std::max(r, (s.pos - lc).norm());
    const double tol = 0.25 * r + std::abs(plane.normal.dot(lc - plane.point)) + 1.;
    std::vector<double> score(islands.size(), 0.);
    const size_t        step = std::max<size_t>(1, loop.pts.size() / 64);
    size_t              used = 0;
    for (size_t si = 0; si < loop.pts.size(); si += step) {
        ++used;
        const Vec3d d = loop.pts[si].pos - plane.point;
        const Point p = Point::new_scale(plane.u.dot(d), plane.v.dot(d));
        for (size_t k = 0; k < islands.size(); ++k) {
            double dd = 0.;
            if (!islands[k].contains(p)) {
                dd = std::numeric_limits<double>::max();
                for (const Line& ln : islands[k].lines())
                    dd = std::min(dd, ln.distance_to(p));
                dd = unscale<double>(dd);
            }
            if (dd <= tol)
                score[k] += 1.;
        }
    }
    const double best = *std::max_element(score.begin(), score.end());
    if (best <= 0.)
        return out;
    ExPolygons keep;
    for (size_t k = 0; k < islands.size(); ++k)
        if (score[k] >= 0.5 * double(used) || score[k] == best)
            keep.push_back(islands[k]);
    const std::vector<Vec3d> tris = triangulate_expolygons_3d(keep, 0., true);
    for (size_t i = 0; i + 2 < tris.size(); i += 3) {
        const int b = int(out.vertices.size());
        for (int k = 0; k < 3; ++k) {
            const Vec3d& q = tris[i + size_t(k)];
            out.vertices.emplace_back((plane.point + q.x() * plane.u + q.y() * plane.v + q.z() * plane.normal).cast<float>());
        }
        out.indices.emplace_back(b, b + 1, b + 2);
    }
    // Facing along the normal, whatever the tessellator's winding.
    for (size_t f = 0; f < out.indices.size(); ++f)
        if (its_face_normal(out, int(f)).cast<double>().dot(plane.normal) < 0.)
            std::swap(out.indices[f][1], out.indices[f][2]);
    return out;
}

// -------------------------------------------------------------------------------------------- assign

namespace {

using manifold::Manifold;

double ps_area_of_original(const manifold::MeshGL64& gl, uint32_t id)
{
    double a = 0.;
    if (gl.runIndex.size() != gl.runOriginalID.size() + 1)
        return 0.;
    const size_t stride = size_t(gl.numProp);
    auto         P      = [&](uint64_t v) { return Vec3d(gl.vertProperties[v * stride], gl.vertProperties[v * stride + 1], gl.vertProperties[v * stride + 2]); };
    for (size_t run = 0; run < gl.runOriginalID.size(); ++run) {
        if (gl.runOriginalID[run] != id)
            continue;
        const size_t end = std::min(size_t(gl.runIndex[run + 1]), gl.triVerts.size());
        for (size_t i = size_t(gl.runIndex[run]); i + 3 <= end; i += 3) {
            const Vec3d p0 = P(gl.triVerts[i]), p1 = P(gl.triVerts[i + 1]), p2 = P(gl.triVerts[i + 2]);
            a += 0.5 * (p1 - p0).cross(p2 - p0).norm();
        }
    }
    return a;
}

Manifold ps_union(const std::vector<Manifold>& parts)
{
    if (parts.empty())
        return Manifold();
    if (parts.size() == 1)
        return parts.front();
    return Manifold::BatchBoolean(parts, manifold::OpType::Add);
}

double ps_median(std::vector<double> v)
{
    if (v.empty())
        return std::numeric_limits<double>::max();
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

} // namespace

PaintSplitPieces paint_split_assign(const indexed_triangle_set& mesh, const PaintSplitRegion& region, int comp,
                                    const indexed_triangle_set& cutter, bool piece_inside, const PaintSplitAssignParams& params)
{
    PaintSplitPieces out;
    if (region.error != PaintSplitError::None) {
        out.error = region.error;
        return out;
    }
    if (comp < 0 || size_t(comp) >= region.num_components()) {
        out.error = PaintSplitError::BadComponent;
        return out;
    }

    // The selected facets of this component, as (centroid, area).
    std::vector<std::pair<Vec3d, double>> sel;
    for (size_t f = 0; f < region.surface.indices.size(); ++f)
        if (region.facet_comp[f] == comp)
            sel.emplace_back(ps_tri_centroid(region.surface, f), ps_tri_area(region.surface, f));
    const double sel_area = region.area[comp];

    // An open part is closed for the split (owner 2026-11-25); its painting is carried by the caller.
    indexed_triangle_set  fixed;
    PaintSplitRepairStats repair;
    if (!paint_split_repair(mesh, fixed, nullptr, repair)) {
        out.error = PaintSplitError::RepairFailed;
        return out;
    }
    if (repair.needed)
        out.warnings.push_back(PaintSplitWarning::Repaired);
    const indexed_triangle_set& src = repair.needed ? fixed : mesh;

    manifold::ExecutionContext ctx;
    Manifold                   M;
    try {
        M = ColorSplitDetail::to_manifold64(src, ctx);
    } catch (...) {
        out.error = PaintSplitError::NonManifold;
        return out;
    }
    if (M.Status() != Manifold::Error::NoError || M.IsEmpty()) {
        out.error = PaintSplitError::NonManifold;
        return out;
    }

    try {
        const Manifold C = ColorSplitDetail::to_manifold64(cutter, ctx);
        ColorSplitDetail::require_ok(C, "cutter");
        const uint32_t cutter_id = uint32_t(C.OriginalID());
        out.source_volume        = M.Volume();

        // A. One Split, so the two sides share bit-identical faces.
        auto [inside, outside] = M.Split(C);
        ColorSplitDetail::require_ok(inside, "split inside");
        ColorSplitDetail::require_ok(outside, "split outside");
        const Manifold piece_all = piece_inside ? inside : outside;
        const Manifold body_all  = piece_inside ? outside : inside;

        std::vector<Manifold> carrying, returned;
        for (Manifold& pc : piece_all.Decompose()) {
            const indexed_triangle_set its = ColorSplitDetail::from_manifold(pc);
            const AABBMesh             aabb(its);
            double                     carried = 0.;
            for (const auto& [g, a] : sel)
                if (aabb.squared_distance(g) <= 1e-6)
                    carried += a;
            (carried >= params.carry_frac * sel_area ? carrying : returned).push_back(pc);
        }
        if (carrying.empty()) {
            out.error = PaintSplitError::EmptyPiece;
            return out;
        }
        Manifold carry = ps_union(carrying);

        // The re-union, measured (design 4.2 A3).
        PaintSplitRejoin& rj      = out.rejoin;
        rj.returned               = int(returned.size());
        rj.body_components_before = int(body_all.Decompose().size());
        rj.piece_cut_area         = ps_area_of_original(carry.GetMeshGL64(), cutter_id);
        Manifold body             = body_all;
        if (!returned.empty()) {
            std::vector<Manifold> parts{ body_all };
            double                vol_returned = 0.;
            for (const Manifold& m : returned) {
                parts.push_back(m);
                vol_returned += m.Volume();
            }
            Manifold reunion = ps_union(parts);
            ColorSplitDetail::require_ok(reunion, "re-union");
            rj.body_cut_area_reunion = ps_area_of_original(reunion.GetMeshGL64(), cutter_id);
            rj.membrane_area         = rj.body_cut_area_reunion - rj.piece_cut_area;
            rj.reunion_volume_delta  = reunion.Volume() - body_all.Volume() - vol_returned;
            rj.reunion_genus         = reunion.Genus();
            rj.body_components_after = int(reunion.Decompose().size());
            rj.clean                 = std::abs(rj.membrane_area) <= 1e-5 * std::max(1., rj.piece_cut_area);
            if (rj.clean)
                body = reunion;
            else {
                // Fallback: the body is the source less the piece, so the returned components never had a cut.
                body             = M - carry;
                ColorSplitDetail::require_ok(body, "subtract fallback");
                rj.fallback_used = true;
                out.warnings.push_back(PaintSplitWarning::ReunionSeam);
            }
            out.returned_components = rj.returned;
            out.warnings.push_back(PaintSplitWarning::ComponentsReturned);
        } else {
            rj.body_cut_area_reunion = ps_area_of_original(body_all.GetMeshGL64(), cutter_id);
            rj.body_components_after = rj.body_components_before;
        }

        // B. Body-side material that hangs only from the piece (a ponytail below the neck plane).
        std::vector<Manifold> bcomps = body.Decompose();
        std::vector<Manifold> attached;
        if (bcomps.size() > 1 && !region.loops[comp].empty()) {
            const indexed_triangle_set carry_its = ColorSplitDetail::from_manifold(carry);
            const AABBMesh             carry_aabb(carry_its);
            const std::vector<DrawCutSample>& loop = region.loops[comp].front().pts;
            double                     radius = 0.;
            Vec3d                      lc     = Vec3d::Zero();
            for (const DrawCutSample& s : loop) lc += s.pos;
            lc /= double(loop.size());
            for (const DrawCutSample& s : loop) radius = std::max(radius, (s.pos - lc).norm());

            std::vector<double> dist(bcomps.size());
            std::vector<char>   adjacent(bcomps.size(), 0);
            for (size_t j = 0; j < bcomps.size(); ++j) {
                const indexed_triangle_set its = ColorSplitDetail::from_manifold(bcomps[j]);
                const AABBMesh             aabb(its);
                std::vector<double>        d;
                d.reserve(loop.size());
                for (const DrawCutSample& s : loop)
                    d.push_back(std::sqrt(aabb.squared_distance(s.pos)));
                dist[j] = ps_median(std::move(d));
                int touching = 0;
                for (const Vec3f& p : its.vertices)
                    if (carry_aabb.squared_distance(p.cast<double>()) <= 1e-6 && ++touching >= 3)
                        break;
                adjacent[j] = touching >= 3;
            }
            const double dmin = *std::min_element(dist.begin(), dist.end());
            const double tol  = std::max(0.5, 0.05 * 2. * radius);
            std::vector<Manifold> stay;
            for (size_t j = 0; j < bcomps.size(); ++j) {
                const bool main = dist[j] <= dmin + tol;
                if (!main && adjacent[j])
                    attached.push_back(bcomps[j]);
                else
                    stay.push_back(bcomps[j]);
            }
            if (!attached.empty()) {
                body = ps_union(stay);
                if (params.loose_mode == PaintSplitLooseMode::AttachToPiece) {
                    std::vector<Manifold> parts{ carry };
                    parts.insert(parts.end(), attached.begin(), attached.end());
                    carry = ps_union(parts);
                    ColorSplitDetail::require_ok(carry, "attach");
                    out.attached_components = int(attached.size());
                    out.warnings.push_back(PaintSplitWarning::PiecesAttached);
                } else
                    out.warnings.push_back(PaintSplitWarning::LoosePieces);
            }
        }
        std::vector<Manifold> loose;
        if (params.loose_mode == PaintSplitLooseMode::Loose)
            loose = std::move(attached);

        // C. Kerf, clipped to the carrying piece's dilated hull so returned components keep their volume.
        if (params.kerf_solid != nullptr && params.kerf > 0.) {
            const Manifold K = ColorSplitDetail::to_manifold64(*params.kerf_solid, ctx);
            ColorSplitDetail::require_ok(K, "kerf");
            const manifold::MeshGL64      gl = carry.GetMeshGL64();
            std::vector<manifold::vec3>   pts;
            const size_t                  stride = size_t(gl.numProp);
            const double                  k      = params.kerf;
            for (size_t i = 0; i + 2 < gl.vertProperties.size(); i += stride) {
                const manifold::vec3 p(gl.vertProperties[i], gl.vertProperties[i + 1], gl.vertProperties[i + 2]);
                pts.push_back(p);
                for (int a = 0; a < 3; ++a)
                    for (double s : { -k, k }) {
                        manifold::vec3 q = p;
                        q[a] += s;
                        pts.push_back(q);
                    }
            }
            const Manifold H  = Manifold::Hull(pts);
            const Manifold Kc = K ^ H;
            carry             = carry - Kc;
            body              = body - Kc;
            ColorSplitDetail::require_ok(carry, "kerf piece");
            ColorSplitDetail::require_ok(body, "kerf body");
        }

        out.piece_volume = carry.Volume();
        out.body_volume  = body.IsEmpty() ? 0. : body.Volume();
        for (const Manifold& m : loose)
            out.loose_volume += m.Volume();
        if (body.IsEmpty() && loose.empty()) {
            out.error = PaintSplitError::EmptyBody;
            return out;
        }
        if (!(params.kerf_solid != nullptr && params.kerf > 0.)) {
            const double total = out.piece_volume + out.body_volume + out.loose_volume;
            if (std::abs(total - out.source_volume) > 1e-6 * std::abs(out.source_volume) + 1e-9)
                out.warnings.push_back(PaintSplitWarning::VolumeMismatch);
        }
        out.piece = ColorSplitDetail::from_manifold(carry);
        if (!body.IsEmpty())
            out.body = ColorSplitDetail::from_manifold(body);
        for (const Manifold& m : loose)
            out.loose.push_back(ColorSplitDetail::from_manifold(m));
    } catch (const ColorSplitError& e) {
        BOOST_LOG_TRIVIAL(error) << "Paint split: " << e.what();
        out.error = PaintSplitError::BooleanFailed;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Paint split: " << e.what();
        out.error = PaintSplitError::BooleanFailed;
    }
    return out;
}

PaintSplitPieces paint_split_assign_loops(const indexed_triangle_set& mesh, const PaintSplitRegion& region, int comp,
                                          const std::vector<PaintSplitPlane>& planes, const PaintSplitAssignParams& params)
{
    PaintSplitPieces out;
    if (region.error != PaintSplitError::None) {
        out.error = region.error;
        return out;
    }
    if (comp < 0 || size_t(comp) >= region.num_components() || planes.size() != region.loops[size_t(comp)].size() || planes.empty()) {
        out.error = PaintSplitError::BadComponent;
        return out;
    }
    for (size_t i = 0; i < planes.size(); ++i)
        if (!planes[i].valid) {
            out.error    = PaintSplitError::BadComponent;
            out.bad_loop = int(i);
            return out;
        }

    std::vector<std::pair<Vec3d, double>> sel;
    for (size_t f = 0; f < region.surface.indices.size(); ++f)
        if (region.facet_comp[f] == comp)
            sel.emplace_back(ps_tri_centroid(region.surface, f), ps_tri_area(region.surface, f));
    const double sel_area = region.area[size_t(comp)];

    indexed_triangle_set  fixed;
    PaintSplitRepairStats repair;
    if (!paint_split_repair(mesh, fixed, nullptr, repair)) {
        out.error = PaintSplitError::RepairFailed;
        return out;
    }
    if (repair.needed)
        out.warnings.push_back(PaintSplitWarning::Repaired);
    const indexed_triangle_set& src = repair.needed ? fixed : mesh;
    BoundingBoxf3               bbox;
    for (const Vec3f& v : src.vertices)
        bbox.merge(v.cast<double>());

    manifold::ExecutionContext ctx;
    Manifold                   M;
    try {
        M = ColorSplitDetail::to_manifold64(src, ctx);
    } catch (...) {
        out.error = PaintSplitError::NonManifold;
        return out;
    }
    if (M.Status() != Manifold::Error::NoError || M.IsEmpty()) {
        out.error = PaintSplitError::NonManifold;
        return out;
    }

    try {
        out.source_volume = M.Volume();
        auto carried_by   = [&](const AABBMesh& aabb) {
            double a = 0.;
            for (const auto& [g, ga] : sel)
                if (aabb.squared_distance(g) <= 1e-6)
                    a += ga;
            return a;
        };

        Manifold cand = M;
        for (size_t i = 0; i < planes.size(); ++i) {
            const PaintSplitLoop& loop = region.loops[size_t(comp)][i];
            // How far a piece across the cut may lie from this loop and still be "across this edge".
            Vec3d lc = Vec3d::Zero();
            for (const DrawCutSample& s : loop.pts)
                lc += s.pos;
            lc /= double(std::max<size_t>(1, loop.pts.size()));
            double r = 0., maxd = 0., rin = 0.;
            for (const DrawCutSample& s : loop.pts) {
                const Vec3d d = s.pos - lc;
                r    = std::max(r, d.norm());
                rin  = std::max(rin, (d - planes[i].normal * planes[i].normal.dot(d)).norm());
                maxd = std::max(maxd, std::abs(planes[i].normal.dot(s.pos - planes[i].point)));
            }
            const double tol = std::max(1., 0.1 * 2. * r) + maxd;

            // What this loop's cut takes away: the far side of its plane, but only within a box round the loop
            // (1.5 times its radius, plus 2 mm). An unbounded half-space through a small hole of a curved
            // region would slice the region itself far from the hole (owner's figure, 2026-11-25).
            auto cut_once = [&](const PaintSplitPlane& plane, bool bounded, std::vector<Manifold>& kept, int& dropped) {
                kept.clear();
                dropped        = 0;
                double carried = 0.;
                PaintSplitPlane far_side = plane; // the unbounded far side: the half-space of the flipped plane
                far_side.normal          = -plane.normal;
                std::swap(far_side.u, far_side.v);
                const Manifold C = ColorSplitDetail::to_manifold64(
                    bounded ? ps_loop_far_box(plane, lc, 1.5 * rin + 2., bbox) : paint_split_half_space(far_side, bbox), ctx);
                ColorSplitDetail::require_ok(C, "cutter");
                auto [outside, inside] = cand.Split(C);
                ColorSplitDetail::require_ok(inside, "split inside");
                ColorSplitDetail::require_ok(outside, "split outside");
                for (Manifold& pc : inside.Decompose()) {
                    const indexed_triangle_set its = ColorSplitDetail::from_manifold(pc);
                    const AABBMesh             aabb(its);
                    const double               a = carried_by(aabb);
                    if (a >= params.carry_frac * sel_area) {
                        kept.push_back(pc);
                        carried += a;
                    } else
                        ++dropped; // material on the region's side without selection (an arm crossing the cut)
                }
                for (Manifold& pc : outside.Decompose()) {
                    const indexed_triangle_set its = ColorSplitDetail::from_manifold(pc);
                    const AABBMesh             aabb(its);
                    std::vector<double>        d;
                    d.reserve(loop.pts.size());
                    for (const DrawCutSample& s : loop.pts)
                        d.push_back(std::sqrt(aabb.squared_distance(s.pos)));
                    if (ps_median(std::move(d)) <= tol)
                        continue; // across this loop's own edge: the rest
                    // Cut off by this plane, but far from this edge (beyond another loop's cut): still the piece's.
                    carried += carried_by(aabb);
                    kept.push_back(pc);
                }
                return carried;
            };
            // A flat cut through a wavy edge leaves some of the region's skin across it (that skin keeps its colour,
            // on the rest); a cut that keeps less than a third of the selection goes through the region.
            const double keep_min = 1. / 3.;
            // Up to four ways to cut this edge. First the plane facing the way the skin says, with its far side
            // taken whole (the clean cut: nothing across the edge stays on the piece) or, when that takes away
            // noticeably more of the selection (a plane through a small hole of a curved region), only near
            // the edge. When neither keeps a third of the selection, the same with the plane facing the other way.
            PaintSplitPlane flipped = planes[i];
            // Offset is towards the piece, so it flips with the side: mirror the plane about the loop's centre.
            flipped.point  = flipped.point - 2. * flipped.normal * flipped.normal.dot(flipped.point - lc);
            flipped.normal = -flipped.normal;
            std::swap(flipped.u, flipped.v);
            std::vector<Manifold> kept;
            int                   dropped = 0;
            double                carried = -1.;
            bool                  was_flipped = false;
            for (int side = 0; side < 2 && carried < keep_min * sel_area; ++side) {
                const PaintSplitPlane& pl = side == 0 ? planes[i] : flipped;
                std::vector<Manifold>  whole_kept, near_kept;
                int                    whole_dropped = 0, near_dropped = 0;
                const double           whole = cut_once(pl, false, whole_kept, whole_dropped);
                const double           near  = cut_once(pl, true, near_kept, near_dropped);
                double vw = 0., vn = 0.;
                for (const Manifold& m : whole_kept)
                    vw += m.Volume();
                for (const Manifold& m : near_kept)
                    vn += m.Volume();
                BOOST_LOG_TRIVIAL(debug) << "Paint split loop " << i << " side " << side << ": whole keeps " << whole / sel_area
                                         << " of the selection (" << vw << " mm3), near " << near / sel_area << " (" << vn << " mm3)";
                // The near-only cut leaves whatever lies across the edge beyond its box on the piece: on a figure that
                // can be half the body (owner's figure: 74 cm3 instead of 6). It is taken only when the whole cut
                // fails, or keeps clearly less of the selection for about the same piece.
                const bool whole_ok  = !whole_kept.empty() && whole >= keep_min * sel_area;
                const bool use_whole = whole_ok ? (near_kept.empty() || whole >= near - 0.1 * sel_area || vn > 1.5 * vw) :
                                                  (near_kept.empty() || whole >= near);
                const double best = use_whole ? whole : (near_kept.empty() ? -1. : near);
                if (best > carried) {
                    kept        = use_whole ? std::move(whole_kept) : std::move(near_kept);
                    dropped     = use_whole ? whole_dropped : near_dropped;
                    carried     = best;
                    was_flipped = side == 1;
                }
            }
            if (was_flipped)
                ++out.flipped_loops;
            out.returned_components += dropped;
            if (kept.empty()) {
                out.error    = PaintSplitError::EmptyPiece;
                out.bad_loop = int(i);
                return out;
            }
            if (carried < keep_min * sel_area) {
                out.error    = PaintSplitError::CutCrossesRegion;
                out.bad_loop = int(i);
                return out;
            }
            cand = ps_union(kept);
            ColorSplitDetail::require_ok(cand, "piece");
        }

        // The rest: the part less the piece, seam-free, in connected pieces.
        const Manifold rest = M - cand;
        ColorSplitDetail::require_ok(rest, "rest");
        std::vector<Manifold> parts;
        for (Manifold& m : rest.Decompose())
            if (m.Volume() > 1e-9 * std::max(1., out.source_volume))
                parts.push_back(m);
        if (parts.empty()) {
            out.error = PaintSplitError::EmptyBody;
            return out;
        }
        size_t body_idx = 0;
        if (parts.size() > 1) {
            std::vector<int>    hits(parts.size(), 0);
            std::vector<double> vol(parts.size());
            for (size_t j = 0; j < parts.size(); ++j) {
                vol[j] = parts[j].Volume();
                if (!params.body_hints.empty()) {
                    const indexed_triangle_set its = ColorSplitDetail::from_manifold(parts[j]);
                    const AABBMesh             aabb(its);
                    for (const Vec3d& h : params.body_hints)
                        if (aabb.squared_distance(h) <= 1e-6)
                            ++hits[j];
                }
            }
            for (size_t j = 1; j < parts.size(); ++j)
                if (hits[j] > hits[body_idx] || (hits[j] == hits[body_idx] && vol[j] > vol[body_idx]))
                    body_idx = j;
        }

        out.piece_volume = cand.Volume();
        out.piece        = ColorSplitDetail::from_manifold(cand);
        for (size_t j = 0; j < parts.size(); ++j) {
            if (j == body_idx) {
                out.body_volume = parts[j].Volume();
                out.body        = ColorSplitDetail::from_manifold(parts[j]);
            } else {
                out.loose_volume += parts[j].Volume();
                out.loose.push_back(ColorSplitDetail::from_manifold(parts[j]));
            }
        }
        if (!out.loose.empty())
            out.warnings.push_back(PaintSplitWarning::LoosePieces);
        const double total = out.piece_volume + out.body_volume + out.loose_volume;
        if (std::abs(total - out.source_volume) > 1e-6 * std::abs(out.source_volume) + 1e-9)
            out.warnings.push_back(PaintSplitWarning::VolumeMismatch);
    } catch (const ColorSplitError& e) {
        BOOST_LOG_TRIVIAL(error) << "Paint split: " << e.what();
        out.error = PaintSplitError::BooleanFailed;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Paint split: " << e.what();
        out.error = PaintSplitError::BooleanFailed;
    }
    return out;
}

double paint_split_inside_fraction(const indexed_triangle_set& mesh, const std::vector<indexed_triangle_set>& cutters)
{
    if (cutters.size() == 1)
        return paint_split_inside_fraction(mesh, cutters.front());
    try {
        indexed_triangle_set  fixed;
        PaintSplitRepairStats repair;
        if (cutters.empty() || !paint_split_repair(mesh, fixed, nullptr, repair))
            return -1.;
        manifold::ExecutionContext ctx;
        const Manifold             M = ColorSplitDetail::to_manifold64(repair.needed ? fixed : mesh, ctx);
        if (M.Status() != Manifold::Error::NoError)
            return -1.;
        const double v = M.Volume();
        if (v <= 0.)
            return -1.;
        Manifold in = M;
        for (const indexed_triangle_set& c : cutters) {
            const Manifold C = ColorSplitDetail::to_manifold64(c, ctx);
            if (C.Status() != Manifold::Error::NoError)
                return -1.;
            in = in ^ C;
        }
        return std::clamp(in.Volume() / v, 0., 1.);
    } catch (...) {
        return -1.;
    }
}

double paint_split_inside_fraction(const indexed_triangle_set& mesh, const indexed_triangle_set& cutter)
{
    try {
        indexed_triangle_set  fixed;
        PaintSplitRepairStats repair;
        if (!paint_split_repair(mesh, fixed, nullptr, repair))
            return -1.;
        manifold::ExecutionContext ctx;
        const Manifold             M = ColorSplitDetail::to_manifold64(repair.needed ? fixed : mesh, ctx);
        const Manifold             C = ColorSplitDetail::to_manifold64(cutter, ctx);
        if (M.Status() != Manifold::Error::NoError || C.Status() != Manifold::Error::NoError)
            return -1.;
        const double v = M.Volume();
        if (v <= 0.)
            return -1.;
        const Manifold in = M ^ C;
        return std::clamp(in.Volume() / v, 0., 1.);
    } catch (...) {
        return -1.;
    }
}

// -------------------------------------------------------------------------------------------- picking

TriangleSelector::TriangleSplittingData paint_split_mask_from_paint(const TriangleMesh& mesh,
                                                                    const TriangleSelector::TriangleSplittingData& paint,
                                                                    EnforcerBlockerType state, int seed_facet, const Vec3f* hit_in)
{
    TriangleSelector sel(mesh);
    sel.deserialize(paint, /*needs_reset=*/true);
    EnforcerBlockerStateMap map;
    if (seed_facet >= 0 && size_t(seed_facet) < mesh.its.indices.size()) {
        // Pick segment: a hit point inside a leaf of `state` on the seed facet, then an unlimited bucket fill
        // over that state (no crease stops), recoloured to a scratch state.
        std::vector<int>           src;
        const indexed_triangle_set leaves = sel.get_facets_strict(state, &src);
        Vec3f                      hit    = hit_in != nullptr ? *hit_in : Vec3f::Zero();
        bool                       found  = hit_in != nullptr;
        for (size_t i = 0; i < leaves.indices.size() && !found; ++i)
            if (src.size() > i && src[i] == seed_facet) {
                const Vec3i32& t = leaves.indices[i];
                hit   = (leaves.vertices[t[0]] + leaves.vertices[t[1]] + leaves.vertices[t[2]]) / 3.f;
                found = true;
            }
        if (!found)
            return TriangleSelector::TriangleSplittingData();
        const EnforcerBlockerType scratch = EnforcerBlockerType::ExtruderMax;
        sel.bucket_fill_select_triangles(hit, seed_facet, TriangleSelector::ClippingPlane(), -1.f, true, true);
        sel.seed_fill_apply_on_triangles(scratch);
        sel.seed_fill_unselect_all_triangles();
        map.fill(EnforcerBlockerType::NONE);
        map[size_t(scratch)] = EnforcerBlockerType::ENFORCER;
    } else {
        map.fill(EnforcerBlockerType::NONE);
        map[size_t(state)] = EnforcerBlockerType::ENFORCER;
    }
    sel.remap_triangle_state(map);
    sel.garbage_collect();
    return sel.serialize();
}

EnforcerBlockerType paint_split_state_at(const TriangleMesh& mesh, const TriangleSelector::TriangleSplittingData& paint,
                                         int facet, const Vec3f& hit)
{
    if (facet < 0 || size_t(facet) >= mesh.its.indices.size())
        return EnforcerBlockerType::NONE;
    TriangleSelector sel(mesh);
    sel.deserialize(paint, true);
    // Every state's leaves on this facet; the one whose triangle holds the hit (closest when none does exactly).
    EnforcerBlockerType best_state = EnforcerBlockerType::NONE;
    double              best       = std::numeric_limits<double>::max();
    const Vec3d         p          = hit.cast<double>();
    for (size_t s = 0; s < paint.used_states.size() || s == 0; ++s) {
        if (s > 0 && !paint.used_states[s])
            continue;
        std::vector<int>           src;
        const indexed_triangle_set leaves = sel.get_facets_strict(EnforcerBlockerType(s), &src);
        for (size_t i = 0; i < leaves.indices.size(); ++i) {
            if (i >= src.size() || src[i] != facet)
                continue;
            const Vec3i32& t = leaves.indices[i];
            const Vec3d    a = leaves.vertices[t[0]].cast<double>(), b = leaves.vertices[t[1]].cast<double>(),
                        c = leaves.vertices[t[2]].cast<double>();
            // Distance from p to the triangle, measured in its plane (p lies on the facet).
            const Vec3d n  = (b - a).cross(c - a);
            const double nn = n.squaredNorm();
            if (nn <= 0.)
                continue;
            const double w0 = (b - p).cross(c - p).dot(n) / nn, w1 = (c - p).cross(a - p).dot(n) / nn, w2 = 1. - w0 - w1;
            const double out = std::max({ 0., -w0, -w1, -w2 });
            if (out < best) {
                best       = out;
                best_state = EnforcerBlockerType(s);
            }
        }
    }
    return best_state;
}

double paint_split_mask_area(const TriangleMesh& mesh, const TriangleSelector::TriangleSplittingData& mask)
{
    TriangleSelector sel(mesh);
    sel.deserialize(mask, true);
    const indexed_triangle_set its = sel.get_facets_strict(EnforcerBlockerType::ENFORCER);
    double                     a   = 0.;
    for (size_t f = 0; f < its.indices.size(); ++f)
        a += ps_tri_area(its, f);
    return a;
}

TriangleSelector::TriangleSplittingData paint_split_mask_combine(const TriangleMesh& mesh, const TriangleSelector::TriangleSplittingData& a,
                                                                 const TriangleSelector::TriangleSplittingData& b, PaintSplitMaskOp op)
{
    TriangleSelector sa(mesh), sb(mesh);
    sa.deserialize(a, true);
    sb.deserialize(b, true);
    const size_t      n = mesh.its.indices.size();
    std::vector<bool> skip(n, true); // facets neither mask touches stay unselected
    for (const TriangleSelector::TriangleBitStreamMapping& m : a.triangles_to_split)
        if (m.triangle_idx >= 0 && size_t(m.triangle_idx) < n)
            skip[size_t(m.triangle_idx)] = false;
    for (const TriangleSelector::TriangleBitStreamMapping& m : b.triangles_to_split)
        if (m.triangle_idx >= 0 && size_t(m.triangle_idx) < n)
            skip[size_t(m.triangle_idx)] = false;
    TriangleSelector out(mesh);
    out.resample_facets(
        [&](const Vec3f& p, int facet) {
            TriangleSelector::SampledState s;
            float      la = 0.f, lb = 0.f;
            int        ia = -1, ib = -1;
            const bool ina = sa.leaf_state_at(facet, p, &la, &ia) == EnforcerBlockerType::ENFORCER;
            const bool inb = sb.leaf_state_at(facet, p, &lb, &ib) == EnforcerBlockerType::ENFORCER;
            const bool r   = op == PaintSplitMaskOp::Union ? (ina || inb) : (ina && !inb);
            s.state        = r ? EnforcerBlockerType::ENFORCER : EnforcerBlockerType::NONE;
            s.leaf_area    = (la > 0.f && lb > 0.f) ? std::min(la, lb) : std::max(la, lb);
            s.leaf         = (int64_t(ia + 1) << 32) | int64_t(uint32_t(ib + 1));
            return s;
        },
        0.005f, &skip);
    out.garbage_collect();
    return out.serialize();
}

TriangleSelector::TriangleSplittingData paint_split_pick(const TriangleMesh& mesh, const TriangleSelector::TriangleSplittingData& paint,
                                                         const TriangleSelector::TriangleSplittingData& mask, int facet,
                                                         const Vec3f& hit, bool add, EnforcerBlockerType* picked)
{
    const EnforcerBlockerType state = paint_split_state_at(mesh, paint, facet, hit);
    if (picked != nullptr)
        *picked = state;
    // The connected patch of that colour under the click (a flood over the facets of the same state).
    const TriangleSelector::TriangleSplittingData patch = paint_split_mask_from_paint(mesh, paint, state, facet, &hit);
    if (!add)
        return patch;
    const bool on_mask = paint_split_state_at(mesh, mask, facet, hit) == EnforcerBlockerType::ENFORCER;
    return paint_split_mask_combine(mesh, mask, patch, on_mask ? PaintSplitMaskOp::Subtract : PaintSplitMaskOp::Union);
}

} // namespace Slic3r

#ifdef PAINT_SPLIT_DEFINED_L
#undef L
#undef PAINT_SPLIT_DEFINED_L
#endif
