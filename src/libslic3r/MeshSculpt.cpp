#include "MeshSculpt.hpp"

#include "Model.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace Slic3r {
namespace Sculpt {

float falloff_weight(float d, float r)
{
    if (r <= 0.f || d >= r)
        return 0.f;
    if (d <= 0.f)
        return 1.f;
    const float t = 1.f - (d * d) / (r * r);
    return t * t;
}

std::vector<Vec3f> its_vertex_normals(const indexed_triangle_set &its)
{
    std::vector<Vec3f> normals(its.vertices.size(), Vec3f::Zero());
    for (const Vec3i32 &f : its.indices) {
        const Vec3f &a = its.vertices[f(0)];
        const Vec3f &b = its.vertices[f(1)];
        const Vec3f &c = its.vertices[f(2)];
        // Not normalised on purpose: the magnitude is twice the triangle area,
        // which is exactly the weighting we want.
        const Vec3f n = (b - a).cross(c - a);
        normals[f(0)] += n;
        normals[f(1)] += n;
        normals[f(2)] += n;
    }
    for (Vec3f &n : normals) {
        const float len = n.norm();
        n = (len > 1e-12f) ? Vec3f(n / len) : Vec3f(0.f, 0.f, 1.f);
    }
    return normals;
}

static void build_vertex_face_csr(const indexed_triangle_set &its,
                                  std::vector<uint32_t>      &vertex_faces,
                                  std::vector<uint32_t>      &vertex_faces_start)
{
    const size_t nv = its.vertices.size();
    vertex_faces_start.assign(nv + 1, 0);
    for (const Vec3i32 &f : its.indices)
        for (int j = 0; j < 3; ++j)
            ++vertex_faces_start[size_t(f(j)) + 1];
    for (size_t i = 0; i < nv; ++i)
        vertex_faces_start[i + 1] += vertex_faces_start[i];

    vertex_faces.assign(vertex_faces_start.back(), 0);
    std::vector<uint32_t> cursor(vertex_faces_start.begin(), vertex_faces_start.end() - 1);
    for (uint32_t t = 0; t < uint32_t(its.indices.size()); ++t) {
        const Vec3i32 &f = its.indices[t];
        for (int j = 0; j < 3; ++j)
            vertex_faces[cursor[size_t(f(j))]++] = t;
    }
}

float its_laplacian_energy(const indexed_triangle_set &its)
{
    std::vector<uint32_t> vf, vfs;
    build_vertex_face_csr(its, vf, vfs);

    double energy = 0.;
    std::vector<uint32_t> ring;
    for (uint32_t v = 0; v < uint32_t(its.vertices.size()); ++v) {
        ring.clear();
        for (uint32_t k = vfs[v]; k < vfs[v + 1]; ++k) {
            const Vec3i32 &f = its.indices[vf[k]];
            for (int j = 0; j < 3; ++j) {
                const uint32_t o = uint32_t(f(j));
                if (o == v)
                    continue;
                if (std::find(ring.begin(), ring.end(), o) == ring.end())
                    ring.emplace_back(o);
            }
        }
        if (ring.empty())
            continue;
        Vec3f mean = Vec3f::Zero();
        for (uint32_t o : ring)
            mean += its.vertices[o];
        mean /= float(ring.size());
        energy += double((mean - its.vertices[v]).squaredNorm());
    }
    return float(energy);
}

indexed_triangle_set its_subdivide_midpoint(const indexed_triangle_set &its)
{
    indexed_triangle_set out;
    out.vertices = its.vertices;
    out.indices.reserve(its.indices.size() * 4);

    // Edge key -> index of the midpoint vertex, so the two triangles sharing an
    // edge get the same midpoint and the result stays watertight.
    std::unordered_map<uint64_t, uint32_t> midpoints;
    midpoints.reserve(its.indices.size() * 2);

    auto midpoint = [&](uint32_t a, uint32_t b) -> uint32_t {
        const uint64_t key = (uint64_t(std::min(a, b)) << 32) | uint64_t(std::max(a, b));
        auto it = midpoints.find(key);
        if (it != midpoints.end())
            return it->second;
        const uint32_t idx = uint32_t(out.vertices.size());
        out.vertices.emplace_back(0.5f * (its.vertices[a] + its.vertices[b]));
        midpoints.emplace(key, idx);
        return idx;
    };

    for (const Vec3i32 &f : its.indices) {
        const uint32_t a = uint32_t(f(0)), b = uint32_t(f(1)), c = uint32_t(f(2));
        const uint32_t ab = midpoint(a, b);
        const uint32_t bc = midpoint(b, c);
        const uint32_t ca = midpoint(c, a);
        out.indices.emplace_back(Vec3i32(int(a), int(ab), int(ca)));
        out.indices.emplace_back(Vec3i32(int(ab), int(b), int(bc)));
        out.indices.emplace_back(Vec3i32(int(ca), int(bc), int(c)));
        out.indices.emplace_back(Vec3i32(int(ab), int(bc), int(ca)));
    }
    return out;
}

bool indices_match(const ModelVolume &mv, const indexed_triangle_set &its)
{
    const indexed_triangle_set &cur = mv.mesh().its;
    if (cur.indices.size() != its.indices.size())
        return false;
    for (size_t i = 0; i < cur.indices.size(); ++ i)
        if (cur.indices[i] != its.indices[i])
            return false;
    return true;
}

bool commit_sculpted_mesh(ModelVolume &mv, indexed_triangle_set &&its, bool ensure_on_bed)
{
    if (! indices_match(mv, its))
        return false;

    // Deliberately NOT going through Plater::clear_before_change_mesh(): the
    // facet indexing is unchanged, so supported_facets / seam_facets /
    // mmu_segmentation_facets / fuzzy_skin_facets stay meaningful. set_mesh()
    // itself only swaps the mesh pointer and never touches them.
    mv.set_mesh(std::move(its));
    mv.calculate_convex_hull();
    mv.invalidate_convex_hull_2d();
    mv.set_new_unique_id();
    if (ModelObject *obj = mv.get_object(); obj != nullptr) {
        obj->invalidate_bounding_box();
        if (ensure_on_bed)
            obj->ensure_on_bed();
    }
    return true;
}

// ----------------------------------------------------------------------------
// SculptSession
// ----------------------------------------------------------------------------

SculptSession::SculptSession(const indexed_triangle_set &its) : m_its(its)
{
    build_adjacency();
    m_vertex_normals = its_vertex_normals(m_its);
    m_stamp.assign(m_its.vertices.size(), 0);
    m_tri_stamp.assign(m_its.indices.size(), 0);
    rebuild_tree();
}

void SculptSession::build_adjacency()
{
    build_vertex_face_csr(m_its, m_vertex_faces, m_vertex_faces_start);
}

void SculptSession::rebuild_tree()
{
    m_tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(m_its.vertices, m_its.indices);
}

void SculptSession::one_ring(uint32_t vertex, std::vector<uint32_t> &out) const
{
    out.clear();
    for (uint32_t k = m_vertex_faces_start[vertex]; k < m_vertex_faces_start[vertex + 1]; ++k) {
        const Vec3i32 &f = m_its.indices[m_vertex_faces[k]];
        for (int j = 0; j < 3; ++j) {
            const uint32_t o = uint32_t(f(j));
            if (o == vertex)
                continue;
            if (std::find(out.begin(), out.end(), o) == out.end())
                out.emplace_back(o);
        }
    }
}

void SculptSession::collect_triangles_in_radius(const Vec3f &center, float radius, std::vector<uint32_t> &out)
{
    out.clear();
    if (radius <= 0.f || m_its.indices.empty())
        return;

    using BBox = AABBTreeIndirect::Tree<3, float>::BoundingBox;
    const Vec3f r(radius, radius, radius);
    const BBox  query(Vec3f(center - r), Vec3f(center + r));

    AABBTreeIndirect::traverse(
        m_tree, AABBTreeIndirect::intersecting(query),
        [&out](const AABBTreeIndirect::Tree<3, float>::Node &node) {
            out.emplace_back(uint32_t(node.idx));
            return true;
        });
}

std::vector<uint32_t> SculptSession::triangles_in_radius(const Vec3f &center, float radius)
{
    std::vector<uint32_t> out;
    collect_triangles_in_radius(center, radius, out);
    return out;
}

void SculptSession::collect_vertices_in_radius(const Vec3f &center, float radius, std::vector<uint32_t> &out)
{
    out.clear();
    if (radius <= 0.f || m_its.indices.empty())
        return;

    // One pass: walk the tree and stamp the vertices of every candidate triangle
    // as we go, so no intermediate triangle list has to be built.
    const float    r2    = radius * radius;
    const uint32_t epoch = ++ m_epoch;
    uint32_t      *stamp = m_stamp.data();

    using BBox = AABBTreeIndirect::Tree<3, float>::BoundingBox;
    const Vec3f r(radius, radius, radius);
    const BBox  query(Vec3f(center - r), Vec3f(center + r));

    AABBTreeIndirect::traverse(
        m_tree, AABBTreeIndirect::intersecting(query),
        [this, &out, center, r2, epoch, stamp](const AABBTreeIndirect::Tree<3, float>::Node &node) {
            const Vec3i32 &f = m_its.indices[node.idx];
            for (int j = 0; j < 3; ++ j) {
                const uint32_t v = uint32_t(f(j));
                if (stamp[v] == epoch)
                    continue;
                stamp[v] = epoch;
                if ((m_its.vertices[v] - center).squaredNorm() < r2)
                    out.emplace_back(v);
            }
            return true;
        });
    std::sort(out.begin(), out.end());
}

std::vector<uint32_t> SculptSession::vertices_in_radius(const Vec3f &center, float radius)
{
    std::vector<uint32_t> out;
    collect_vertices_in_radius(center, radius, out);
    return out;
}

float SculptSession::local_edge_length(const Vec3f &center, float radius)
{
    std::vector<uint32_t> tris;
    collect_triangles_in_radius(center, radius, tris);
    if (tris.empty())
        return -1.f;
    double sum   = 0.;
    size_t count = 0;
    for (uint32_t t : tris) {
        const Vec3i32 &f = m_its.indices[t];
        for (int j = 0; j < 3; ++j) {
            sum += double((m_its.vertices[f((j + 1) % 3)] - m_its.vertices[f(j)]).norm());
            ++count;
        }
    }
    return count == 0 ? -1.f : float(sum / double(count));
}

void SculptSession::recompute_normals(const std::vector<uint32_t> &triangles)
{
    // A vertex normal depends on every incident face, so recompute it from
    // scratch for each vertex of a dirty triangle. That set already covers the
    // one-ring of every moved vertex.
    const uint32_t epoch = ++ m_epoch;
    std::vector<uint32_t> &verts = m_scratch_normal_verts;
    verts.clear();
    verts.reserve(triangles.size() * 3);
    for (uint32_t t : triangles) {
        const Vec3i32 &f = m_its.indices[t];
        for (int j = 0; j < 3; ++j) {
            const uint32_t v = uint32_t(f(j));
            if (m_stamp[v] == epoch)
                continue;
            m_stamp[v] = epoch;
            verts.emplace_back(v);
        }
    }
    for (uint32_t v : verts) {
        Vec3f n = Vec3f::Zero();
        for (uint32_t k = m_vertex_faces_start[v]; k < m_vertex_faces_start[v + 1]; ++k) {
            const Vec3i32 &f = m_its.indices[m_vertex_faces[k]];
            const Vec3f   &a = m_its.vertices[f(0)];
            const Vec3f   &b = m_its.vertices[f(1)];
            const Vec3f   &c = m_its.vertices[f(2)];
            n += (b - a).cross(c - a);
        }
        const float len = n.norm();
        m_vertex_normals[v] = (len > 1e-12f) ? Vec3f(n / len) : Vec3f(0.f, 0.f, 1.f);
    }
}

StrokeStep SculptSession::apply(const BrushParams &params)
{
    StrokeStep step;
    apply(params, step);
    return step;
}

void SculptSession::apply(const BrushParams &params, StrokeStep &step)
{
    std::vector<uint32_t> &verts = step.moved_vertices;
    std::vector<uint32_t> &dirty = step.dirty_triangles;
    verts.clear();
    dirty.clear();
    if (params.radius <= 0.f)
        return;

    collect_vertices_in_radius(params.center, params.radius, verts);
    if (verts.empty())
        return;

    std::vector<float> &weights = m_scratch_weights;
    weights.resize(verts.size());
    for (size_t i = 0; i < verts.size(); ++i) {
        const float d = (m_its.vertices[verts[i]] - params.center).norm();
        const float w = params.falloff ? falloff_weight(d, params.radius)
                                       : (d < params.radius ? 1.f : 0.f);
        weights[i] = params.strength * w;
    }

    switch (params.type) {
    case BrushType::Grab: {
        for (size_t i = 0; i < verts.size(); ++i)
            m_its.vertices[verts[i]] += weights[i] * params.displacement;
        break;
    }
    case BrushType::Inflate: {
        const float sign = params.deflate ? -1.f : 1.f;
        for (size_t i = 0; i < verts.size(); ++i)
            m_its.vertices[verts[i]] += (weights[i] * params.amount * sign) * m_vertex_normals[verts[i]];
        break;
    }
    case BrushType::Smooth: {
        std::vector<Vec3f>    &updated = m_scratch_updated;
        std::vector<uint32_t> &ring    = m_scratch_ring;
        updated.resize(verts.size());
        const int iterations = std::max(1, params.iterations);
        // A Taubin step is a +lambda shrinking pass followed by a -mu
        // unshrinking pass; plain Laplacian is the lambda pass alone.
        const int passes = params.taubin ? 2 : 1;
        for (int it = 0; it < iterations; ++it) {
            for (int pass = 0; pass < passes; ++pass) {
                const float factor = (pass == 0) ? params.lambda : params.mu;
                for (size_t i = 0; i < verts.size(); ++i) {
                    const uint32_t v = verts[i];
                    one_ring(v, ring);
                    if (ring.empty()) {
                        updated[i] = m_its.vertices[v];
                        continue;
                    }
                    Vec3f mean = Vec3f::Zero();
                    for (uint32_t o : ring)
                        mean += m_its.vertices[o];
                    mean /= float(ring.size());
                    // Jacobi update: the whole pass reads the old positions.
                    updated[i] = m_its.vertices[v] + (weights[i] * factor) * (mean - m_its.vertices[v]);
                }
                for (size_t i = 0; i < verts.size(); ++i)
                    m_its.vertices[verts[i]] = updated[i];
            }
        }
        break;
    }
    }

    // Every triangle incident to a moved vertex is now stale.
    const uint32_t epoch = ++ m_epoch;
    for (uint32_t v : verts)
        for (uint32_t k = m_vertex_faces_start[v]; k < m_vertex_faces_start[v + 1]; ++k) {
            const uint32_t t = m_vertex_faces[k];
            if (m_tri_stamp[t] == epoch)
                continue;
            m_tri_stamp[t] = epoch;
            dirty.emplace_back(t);
        }
    std::sort(dirty.begin(), dirty.end());

    recompute_normals(dirty);
}

} // namespace Sculpt
} // namespace Slic3r
