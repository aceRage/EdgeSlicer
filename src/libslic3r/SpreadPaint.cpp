#include "SpreadPaint.hpp"

#include "Geometry.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r {

namespace {

// Closest distance from point p to segment ab.
float point_segment_distance(const Vec3f &p, const Vec3f &a, const Vec3f &b)
{
    const Vec3f ab  = b - a;
    const float len = ab.squaredNorm();
    float       t   = len > 0.f ? (p - a).dot(ab) / len : 0.f;
    t               = std::clamp(t, 0.f, 1.f);
    return (a + t * ab - p).norm();
}

// Closest distance from point p to triangle abc (Ericson, Real-Time Collision Detection 5.1.5).
float point_triangle_distance(const Vec3f &p, const Vec3f &a, const Vec3f &b, const Vec3f &c)
{
    const Vec3f ab = b - a, ac = c - a, ap = p - a;
    const float d1 = ab.dot(ap), d2 = ac.dot(ap);
    if (d1 <= 0.f && d2 <= 0.f)
        return ap.norm();
    const Vec3f bp = p - b;
    const float d3 = ab.dot(bp), d4 = ac.dot(bp);
    if (d3 >= 0.f && d4 <= d3)
        return bp.norm();
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.f && d1 >= 0.f && d3 <= 0.f)
        return (a + (d1 / (d1 - d3)) * ab - p).norm();
    const Vec3f cp = p - c;
    const float d5 = ab.dot(cp), d6 = ac.dot(cp);
    if (d6 >= 0.f && d5 <= d6)
        return cp.norm();
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.f && d2 >= 0.f && d6 <= 0.f)
        return (a + (d2 / (d2 - d6)) * ac - p).norm();
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.f && (d4 - d3) >= 0.f && (d5 - d6) >= 0.f)
        return (b + ((d4 - d3) / ((d4 - d3) + (d5 - d6))) * (c - b) - p).norm();
    const float denom = 1.f / (va + vb + vc);
    return (a + ab * (vb * denom) + ac * (vc * denom) - p).norm();
}

// Unit vector in the plane of triangle (a, b, other), perpendicular to ab, pointing towards `other`.
// Returns false for a degenerate triangle.
bool in_plane_perpendicular(const Vec3f &a, const Vec3f &b, const Vec3f &other, Vec3f &u_out, Vec3f &v_out, float &len_out)
{
    const Vec3f ab = b - a;
    len_out        = ab.norm();
    if (len_out <= 0.f)
        return false;
    u_out          = ab / len_out;
    Vec3f v        = (other - a) - (other - a).dot(u_out) * u_out;
    const float vn = v.norm();
    if (vn <= 1e-12f * std::max(1.f, len_out))
        return false;
    v_out = v / vn;
    return true;
}

// The fast-marching triangle update. A and B are vertices with distances dA and dB; C is the third
// vertex of their facet. The virtual source Q lies in the facet's plane on the far side of AB from
// C, at distances dA and dB from A and B. Returns false when there is no consistent planar source.
bool unfolded_source(const Vec3f &A, float dA, const Vec3f &B, float dB, const Vec3f &C, Vec3f &Q, Vec3f &u, Vec3f &v, float &c)
{
    if (! in_plane_perpendicular(A, B, C, u, v, c))
        return false;
    const float qx  = (dA * dA - dB * dB + c * c) / (2.f * c);
    const float qy2 = dA * dA - qx * qx;
    if (qy2 < 0.f)
        return false; // |dA - dB| > |AB|
    Q = A + qx * u - std::sqrt(qy2) * v;
    return true;
}

// Distance from that source to p (a point of the facet) when the straight segment from the source
// to p crosses AB, +inf otherwise.
float unfolded_edge_distance(const Vec3f &A, float dA, const Vec3f &B, float dB, const Vec3f &C, const Vec3f &p)
{
    Vec3f Q, u, v;
    float c;
    if (! unfolded_source(A, dA, B, dB, C, Q, u, v, c))
        return std::numeric_limits<float>::infinity();
    const float qx = (Q - A).dot(u), qy = std::min(0.f, (Q - A).dot(v));
    const float px = (p - A).dot(u), py = std::max(0.f, (p - A).dot(v));
    const float x  = py - qy <= 0.f ? qx : qx + (px - qx) * (-qy / (py - qy));
    const float tol = 1e-4f * c;
    if (x < -tol || x > c + tol)
        return std::numeric_limits<float>::infinity();
    return (p - Q).norm();
}

} // namespace

// ---------------------------------------------------------------------------------------------
// GeodesicCursor

GeodesicCursor::GeodesicCursor(const SpreadSession &session, float radius)
    : TriangleSelector::Cursor(Vec3f::Zero(), 1.f, session.params().trafo_no_translate, session.params().clp)
    , m_session(session)
    , m_r(radius)
{}

bool GeodesicCursor::is_mesh_point_inside(const Vec3f &point) const
{
    return m_facet >= 0 && m_session.field(m_facet, m_session.to_world(point)) <= m_r;
}

bool GeodesicCursor::is_pointer_in_triangle(const Vec3f &p1, const Vec3f &p2, const Vec3f &p3) const
{
    // The seed itself is always painted, even before the radius reaches a vertex.
    if (m_facet != m_session.params().facet)
        return false;
    const Vec3f &p   = m_session.params().hit;
    const Vec3f  v0  = p2 - p1, v1 = p3 - p1, v2 = p - p1;
    const float  d00 = v0.dot(v0), d01 = v0.dot(v1), d11 = v1.dot(v1), d20 = v2.dot(v0), d21 = v2.dot(v1);
    const float  den = d00 * d11 - d01 * d01;
    if (std::abs(den) <= 0.f)
        return false;
    const float  b   = (d11 * d20 - d01 * d21) / den;
    const float  c   = (d00 * d21 - d01 * d20) / den;
    const float  eps = -1e-5f;
    return b >= eps && c >= eps && 1.f - b - c >= eps;
}

int GeodesicCursor::vertices_inside(const TriangleSelector::Triangle &tr, const std::vector<TriangleSelector::Vertex> &vertices) const
{
    m_facet    = tr.source_triangle;
    int inside = 0;
    for (int i = 0; i < 3; ++i)
        if (m_session.field(m_facet, m_session.to_world(vertices[tr.verts_idxs[i]].v)) <= m_r)
            ++inside;
    return inside;
}

bool GeodesicCursor::is_edge_inside_cursor(const TriangleSelector::Triangle &tr, const std::vector<TriangleSelector::Vertex> &vertices) const
{
    m_facet = tr.source_triangle;
    for (int i = 0; i < 3; ++i) {
        const Vec3f a = m_session.to_world(vertices[tr.verts_idxs[i]].v);
        const Vec3f b = m_session.to_world(vertices[tr.verts_idxs[(i + 1) % 3]].v);
        if (m_session.field_lower_bound(m_facet, a, b) <= m_r)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// SpreadSession

SpreadSession::SpreadSession(TriangleSelector &selector, const SpreadParams &params) : m_sel(selector), m_params(params)
{
    m_L             = params.trafo_no_translate.matrix().block<3, 3>(0, 0).cast<float>();
    m_normal_matrix = params.trafo_no_translate.matrix().block<3, 3>(0, 0).inverse().transpose().cast<float>();
    {
        const Vec3d sf = Geometry::Transformation(params.trafo_no_translate).get_scaling_factor();
        if (is_approx(sf(0), sf(1)) && is_approx(sf(1), sf(2)))
            m_uniform_scale = float(sf(0));
    }
    // A state cannot protect itself from being erased: "keep existing" only applies to painting.
    if (m_params.state == EnforcerBlockerType::NONE)
        m_params.keep_existing = false;

    m_has_angle_limit = m_params.angle_deg >= 0.f;
    if (m_has_angle_limit)
        m_cos_limit = std::cos(Geometry::deg2rad(double(m_params.angle_deg))) - EPSILON; // as Smart fill
    m_overhang_limit = -std::cos(Geometry::deg2rad(m_params.overhang_deg));

    const int n_facets   = m_sel.m_orig_size_indices;
    const int n_vertices = m_sel.m_orig_size_vertices;
    if (m_params.facet < 0 || m_params.facet >= n_facets) {
        m_dead = true;
        return;
    }
    m_seed_world  = to_world(m_params.hit);
    m_seed_normal = m_sel.m_face_normals[m_params.facet];
    m_slot.assign(size_t(n_facets), -1);

    // The seed facet: Smart fill does not test the clipping plane on it either.
    bool seed_ok = true;
    if (m_params.overhang_deg != 0.f) {
        const float z = (m_normal_matrix * m_seed_normal).normalized().z();
        seed_ok       = z < m_overhang_limit;
    }
    if (seed_ok && m_params.keep_existing && fully_other_state(m_params.facet))
        seed_ok = false;
    if (! seed_ok) {
        m_dead = true;
        return;
    }

    if (m_params.distance_mode == SpreadParams::DistanceMode::Euclidean) {
        // The whole connected allowed region, sorted by distance to the seed.
        std::vector<int> region { m_params.facet };
        m_slot[m_params.facet] = 0;
        for (size_t head = 0; head < region.size(); ++head) {
            const int g = region[head];
            for (int n : m_sel.m_neighbors[g])
                if (n >= 0 && m_slot[n] < 0 && allowed(n, g)) {
                    m_slot[n] = 0;
                    region.push_back(n);
                }
        }
        m_euclid_order.reserve(region.size());
        for (int f : region) {
            const auto &vi = m_sel.m_triangles[f].verts_idxs;
            m_euclid_order.emplace_back(f == m_params.facet ? 0.f : point_triangle_distance(m_seed_world, pos(vi[0]), pos(vi[1]), pos(vi[2])), f);
        }
        std::sort(m_euclid_order.begin(), m_euclid_order.end());
        return;
    }

    m_dist.assign(size_t(n_vertices), INF);
    m_settled.assign(size_t(n_vertices), 0);
    FacetInfo seed;
    seed.facet   = m_params.facet;
    seed.has_src = true;
    seed.full    = true;
    seed.q       = m_seed_world;
    seed.sigma   = 0.f;
    add_facet(seed);
    drain();
}

Vec3f SpreadSession::pos(int vertex) const { return m_L * m_sel.m_vertices[vertex].v; }

bool SpreadSession::fully_other_state(int tri) const
{
    const TriangleSelector::Triangle &t = m_sel.m_triangles[tri];
    if (! t.is_split()) {
        const EnforcerBlockerType s = t.get_state();
        return s != EnforcerBlockerType::NONE && s != m_params.state;
    }
    for (int i = 0; i <= t.number_of_split_sides(); ++i)
        if (! fully_other_state(t.children[i]))
            return false;
    return true;
}

bool SpreadSession::allowed(int facet, int from_facet) const
{
    if (m_sel.is_facet_clipped(facet, m_params.clp))
        return false;
    const Vec3f &n = m_sel.m_face_normals[facet];
    if (m_params.overhang_deg != 0.f && !((m_normal_matrix * n).normalized().z() < m_overhang_limit))
        return false;
    if (m_has_angle_limit) {
        const Vec3f &ref = m_params.angle_mode == SpreadParams::AngleMode::Seed ? m_seed_normal : m_sel.m_face_normals[from_facet];
        if (double(std::clamp(n.dot(ref), 0.f, 1.f)) < m_cos_limit)
            return false;
    }
    if (m_params.keep_existing && fully_other_state(facet))
        return false;
    return true;
}

bool SpreadSession::source_visible(const FacetInfo &fi, const Vec3f &p) const
{
    if (fi.full)
        return true;
    const auto &vi = m_sel.m_triangles[fi.facet].verts_idxs;
    int         third = -1;
    for (int v : vi)
        if (v != fi.va && v != fi.vb)
            third = v;
    if (third < 0)
        return false;
    const Vec3f A = pos(fi.va), B = pos(fi.vb);
    Vec3f       u, v;
    float       c;
    if (! in_plane_perpendicular(A, B, pos(third), u, v, c))
        return false;
    const float qx = (fi.q - A).dot(u), qy = std::min(0.f, (fi.q - A).dot(v));
    const float px = (p - A).dot(u), py = std::max(0.f, (p - A).dot(v));
    const float x  = py - qy <= 0.f ? qx : qx + (px - qx) * (-qy / (py - qy));
    const float t  = x / c;
    const float tol = 1e-4f;
    return t >= fi.t0 - tol && t <= fi.t1 + tol;
}

float SpreadSession::field(int facet, const Vec3f &p) const
{
    if (m_dead)
        return INF;
    if (m_params.distance_mode == SpreadParams::DistanceMode::Euclidean)
        return (p - m_seed_world).norm();
    if (facet < 0 || facet >= int(m_slot.size()) || m_slot[facet] < 0)
        return INF;
    const FacetInfo &fi = m_facets[m_slot[facet]];
    float            d  = INF;
    if (fi.has_src) {
        if (source_visible(fi, p))
            d = fi.sigma + (p - fi.q).norm();
        if (! fi.full) {
            // Straight to a window end, then straight across the facet: a real path, so never too short.
            const Vec3f A = pos(fi.va), B = pos(fi.vb);
            for (float t : { fi.t0, fi.t1 }) {
                const Vec3f X = A + t * (B - A);
                d             = std::min(d, fi.sigma + (X - fi.q).norm() + (p - X).norm());
            }
        }
    }
    const auto &vi = m_sel.m_triangles[facet].verts_idxs;
    for (int i = 0; i < 3; ++i) {
        const int a = vi[i];
        if (! m_settled[a])
            continue;
        const Vec3f Pa = pos(a);
        d              = std::min(d, m_dist[a] + (p - Pa).norm());
        const int b    = vi[(i + 1) % 3];
        if (m_settled[b])
            d = std::min(d, unfolded_edge_distance(Pa, m_dist[a], pos(b), m_dist[b], pos(vi[(i + 2) % 3]), p));
    }
    return d;
}

float SpreadSession::field_lower_bound(int facet, const Vec3f &a, const Vec3f &b) const
{
    if (m_dead)
        return INF;
    if (m_params.distance_mode == SpreadParams::DistanceMode::Euclidean)
        return point_segment_distance(m_seed_world, a, b);
    if (facet < 0 || facet >= int(m_slot.size()) || m_slot[facet] < 0)
        return INF;
    const FacetInfo &fi = m_facets[m_slot[facet]];
    float            d  = INF;
    if (fi.has_src)
        d = fi.sigma + point_segment_distance(fi.q, a, b);
    const auto &vi = m_sel.m_triangles[facet].verts_idxs;
    for (int i = 0; i < 3; ++i) {
        const int ia = vi[i];
        if (! m_settled[ia])
            continue;
        const Vec3f Pa = pos(ia);
        d              = std::min(d, m_dist[ia] + point_segment_distance(Pa, a, b));
        const int ib   = vi[(i + 1) % 3];
        if (m_settled[ib]) {
            Vec3f Q, u, v;
            float c;
            if (unfolded_source(Pa, m_dist[ia], pos(ib), m_dist[ib], pos(vi[(i + 2) % 3]), Q, u, v, c))
                d = std::min(d, point_segment_distance(Q, a, b));
        }
    }
    return d;
}

float SpreadSession::distance_at(int facet, const Vec3f &pt_mesh) const { return field(facet, to_world(pt_mesh)); }

float SpreadSession::distance_lower_bound(int facet, const Vec3f &a_mesh, const Vec3f &b_mesh) const
{
    return field_lower_bound(facet, to_world(a_mesh), to_world(b_mesh));
}

// Carry the source of `from` across the edge it shares with `to_facet`: the part of the shared
// edge it sees through its own window, and the source unfolded into the plane of `to_facet`.
bool SpreadSession::propagate(const FacetInfo &from, int to_facet, FacetInfo &out) const
{
    if (! from.has_src)
        return false;
    const auto &gv = m_sel.m_triangles[from.facet].verts_idxs;
    const auto &hv = m_sel.m_triangles[to_facet].verts_idxs;
    // Shared edge A B, G's third vertex g, H's third vertex h.
    int A = -1, B = -1, g = -1, h = -1;
    for (int i = 0; i < 3; ++i) {
        const int a = gv[i], b = gv[(i + 1) % 3];
        const bool a_in = std::find(hv.begin(), hv.end(), a) != hv.end();
        const bool b_in = std::find(hv.begin(), hv.end(), b) != hv.end();
        if (a_in && b_in) {
            A = a;
            B = b;
            g = gv[(i + 2) % 3];
        }
    }
    if (A < 0)
        return false;
    for (int v : hv)
        if (v != A && v != B)
            h = v;
    if (h < 0)
        return false;

    const Vec3f PA = pos(A), PB = pos(B), Pg = pos(g), Ph = pos(h);
    float       s0 = 0.f, s1 = 1.f;
    if (! from.full) {
        // Visible interval of AB: inside the wedge from q through the window W0 W1.
        const Vec3f FA = pos(from.va), FB = pos(from.vb);
        const Vec3f W0 = FA + from.t0 * (FB - FA), W1 = FA + from.t1 * (FB - FA);
        const Vec3f n  = (PB - PA).cross(Pg - PA);
        const Vec3f d0 = W0 - from.q, d1 = W1 - from.q;
        const float s  = n.dot(d0.cross(d1));
        if (std::abs(s) <= 1e-12f * n.norm() * d0.norm() * d1.norm())
            return false;
        const float sg = s > 0.f ? 1.f : -1.f;
        // sg * n.(d0 x (X - q)) >= 0 and sg * n.((X - q) x d1) >= 0 with X = PA + t (PB - PA).
        const Vec3f e   = PB - PA, m = PA - from.q;
        const float a0  = sg * n.dot(d0.cross(m)), b0 = sg * n.dot(d0.cross(e));
        const float a1  = sg * n.dot(m.cross(d1)), b1 = sg * n.dot(e.cross(d1));
        auto        clip = [&s0, &s1](float a, float b) {
            // a + b t >= 0
            if (std::abs(b) <= 0.f) {
                if (a < 0.f)
                    s1 = -1.f;
                return;
            }
            const float t = -a / b;
            if (b > 0.f)
                s0 = std::max(s0, t);
            else
                s1 = std::min(s1, t);
        };
        clip(a0, b0);
        clip(a1, b1);
        if (s1 - s0 <= 1e-6f)
            return false;
    }

    Vec3f u, vg, vh;
    float c;
    if (! in_plane_perpendicular(PA, PB, Pg, u, vg, c))
        return false;
    {
        Vec3f u2;
        float c2;
        if (! in_plane_perpendicular(PA, PB, Ph, u2, vh, c2))
            return false;
    }
    const float along = (from.q - PA).dot(u);
    const float side  = (from.q - PA).dot(vg); // the source is on g's side of AB
    if (side < -1e-5f * c)
        return false;

    out          = FacetInfo();
    out.facet    = to_facet;
    out.has_src  = true;
    out.sigma    = from.sigma;
    out.va       = A;
    out.vb       = B;
    out.t0       = s0;
    out.t1       = s1;
    out.q        = PA + along * u - std::max(side, 0.f) * vh;
    // A source on the shared edge, inside the window, sees all of the (convex) facet.
    const float tq = along / c;
    if (side <= 1e-5f * c && tq >= s0 && tq <= s1)
        out.full = true;
    return true;
}

bool SpreadSession::window_entry_key(const FacetInfo &from, int to_facet, float &key) const
{
    FacetInfo fi;
    if (! propagate(from, to_facet, fi))
        return false;
    const Vec3f A = pos(fi.va), B = pos(fi.vb);
    key = fi.sigma + point_segment_distance(fi.q, A + fi.t0 * (B - A), A + fi.t1 * (B - A));
    return true;
}

void SpreadSession::add_facet(const FacetInfo &fi)
{
    const int facet  = fi.facet;
    m_slot[facet]    = int(m_facets.size());
    m_facets.push_back(fi);
    m_active.push_back(facet);
    m_touched.push_back(facet);
    for (int v : m_sel.m_triangles[facet].verts_idxs)
        m_vertex_facets[v].push_back(facet);
    relax_facet(facet);
    push_window_events(facet);
    m_work.push_back(facet);
}

void SpreadSession::try_enter(int facet, int from_facet)
{
    if (facet < 0 || m_slot[facet] >= 0 || ! allowed(facet, from_facet))
        return;
    FacetInfo fi;
    if (! propagate(m_facets[m_slot[from_facet]], facet, fi)) {
        fi       = FacetInfo();
        fi.facet = facet;
    }
    add_facet(fi);
}

void SpreadSession::relax_facet(int facet)
{
    const auto &vi = m_sel.m_triangles[facet].verts_idxs;
    for (int w : vi) {
        if (m_settled[w])
            continue;
        const float d = field(facet, pos(w));
        if (d < m_dist[w]) {
            m_dist[w] = d;
            m_queue.push(Event { d, 0, w });
        }
    }
}

void SpreadSession::push_window_events(int facet)
{
    const FacetInfo &fi = m_facets[m_slot[facet]];
    if (! fi.has_src)
        return;
    for (int n : m_sel.m_neighbors[facet]) {
        if (n < 0 || m_slot[n] >= 0)
            continue;
        float key;
        if (window_entry_key(fi, n, key)) {
            m_queue.push(Event { key, 1, int(m_window_events.size()) });
            m_window_events.push_back({ n, facet });
        }
    }
}

void SpreadSession::settle(int vertex)
{
    m_settled[vertex] = 1;
    m_max_reached     = std::max(m_max_reached, m_dist[vertex]);
    auto it           = m_vertex_facets.find(vertex);
    if (it == m_vertex_facets.end())
        return;
    // Copy: entering facets appends to the list.
    const boost::container::small_vector<int, 8> facets = it->second;
    for (int f : facets) {
        relax_facet(f);
        m_work.push_back(f);
    }
}

// Open the edges at settled vertices of the facets queued in m_work: the front has reached those
// edges, so the facets across them are entered (when allowed).
void SpreadSession::drain()
{
    while (m_work_head < m_work.size()) {
        const int   facet = m_work[m_work_head++];
        const auto &vi    = m_sel.m_triangles[facet].verts_idxs;
        for (int n : m_sel.m_neighbors[facet]) {
            if (n < 0 || m_slot[n] >= 0)
                continue;
            // Does the edge to n have a settled end?
            const auto &nv = m_sel.m_triangles[n].verts_idxs;
            bool        reached = false;
            for (int v : vi)
                if (m_settled[v] && std::find(nv.begin(), nv.end(), v) != nv.end())
                    reached = true;
            if (reached)
                try_enter(n, facet);
        }
    }
    m_work.clear();
    m_work_head = 0;
}

void SpreadSession::grow_to(float r_mm)
{
    if (m_dead || (m_started && !(r_mm > m_radius)))
        return;
    m_started = true;
    m_radius  = std::max(r_mm, 0.f);

    if (m_params.distance_mode == SpreadParams::DistanceMode::Euclidean) {
        while (m_euclid_next < m_euclid_order.size() && m_euclid_order[m_euclid_next].first <= m_radius) {
            const int f = m_euclid_order[m_euclid_next++].second;
            m_active.push_back(f);
            m_touched.push_back(f);
        }
        if (! m_euclid_order.empty())
            m_max_reached = std::min(m_radius, m_euclid_order.back().first);
    } else {
        while (! m_queue.empty() && m_queue.top().key <= m_radius) {
            const Event ev = m_queue.top();
            m_queue.pop();
            if (ev.kind == 0) {
                if (m_settled[ev.idx] || ev.key != m_dist[ev.idx])
                    continue; // stale
                settle(ev.idx);
            } else {
                const WindowEvent &we = m_window_events[ev.idx];
                try_enter(we.to_facet, we.from_facet);
            }
            drain();
        }
    }
    paint();
}

void SpreadSession::paint()
{
    if (m_active.empty())
        return;
    // Split edge limit, in the units split_triangle() measures in.
    const float edge_limit = m_uniform_scale > 0.f ? m_params.edge_limit / m_uniform_scale : m_params.edge_limit;
    std::vector<uint8_t> whole;
    m_sel.select_triangles_with_cursor(m_active, std::make_unique<GeodesicCursor>(*this, m_radius), m_params.state, edge_limit,
                                       m_params.keep_existing, &whole);
    // Facets painted whole are finished: the field only shrinks and the radius only grows.
    size_t j = 0;
    for (size_t i = 0; i < m_active.size(); ++i)
        if (! whole[i])
            m_active[j++] = m_active[i];
    m_active.resize(j);
}

bool SpreadSession::exhausted() const
{
    if (m_dead)
        return true;
    if (! m_active.empty())
        return false;
    if (m_params.distance_mode == SpreadParams::DistanceMode::Euclidean)
        return m_euclid_next >= m_euclid_order.size();
    return m_queue.empty();
}

} // namespace Slic3r
