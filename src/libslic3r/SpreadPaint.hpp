#ifndef libslic3r_SpreadPaint_hpp_
#define libslic3r_SpreadPaint_hpp_

// Edge: the "Spread" paint tool (design: tests/design_spread_paint_tool.md).
//
// A press drops a seed on the surface. While the button is held the painted patch grows round the
// seed, over the surface, and stops at facets that turn away by more than an angle limit (the same
// test as Smart fill). This file holds the GUI-free part: a SpreadSession grows the patch to a given
// radius (world mm, measured over the surface) and paints it into a TriangleSelector through the
// ordinary splitting machinery, so the saved paint data keeps its format.
//
// Distance model (DistanceMode::Geodesic, the default): a Dijkstra-ordered front over the original
// mesh vertices with a fast-marching "unfolding" update (Kimmel-Sethian), plus one unfolded source
// per facet carried across edges with its visibility window. It is exact on planar regions, also
// when the facets are much bigger than the patch, and it follows folds and curves instead of
// jumping across slots. DistanceMode::Euclidean (a ball round the seed, clipped to the connected
// angle-limited region) is kept as the test reference.

#include "TriangleSelector.hpp"

#include <boost/container/small_vector.hpp>

#include <limits>
#include <memory>
#include <queue>
#include <unordered_map>
#include <vector>

namespace Slic3r {

struct SpreadParams
{
    enum class AngleMode {
        Neighbour, // facet to facet, like Smart fill: stops at creases, wraps smooth curves
        Seed,      // against the seed facet's normal: caps the total turning
    };
    enum class DistanceMode {
        Geodesic,  // over the surface (default)
        Euclidean, // straight-line ball, clipped to the connected allowed region (test reference)
    };

    Vec3f                           hit { Vec3f::Zero() };      // seed point, mesh coordinates
    int                             facet { -1 };               // original facet the seed lies on
    Transform3d                     trafo_no_translate { Transform3d::Identity() }; // mesh to world, no translation
    TriangleSelector::ClippingPlane clp;                        // facets cut by an active plane are not entered
    EnforcerBlockerType             state { EnforcerBlockerType::Extruder1 };      // NONE erases
    float                           angle_deg { 30.f };         // angle limit; negative: no limit
    AngleMode                       angle_mode { AngleMode::Neighbour };
    float                           overhang_deg { 0.f };       // non-zero: only overhangs of this angle (as Smart fill)
    bool                            keep_existing { false };    // keep paint of other states; fully painted facets stop the spread
    DistanceMode                    distance_mode { DistanceMode::Geodesic };
    float                           edge_limit { 0.05f };       // smallest edge split along the boundary, world mm
};

class SpreadSession;

// Cursor whose inside test is "surface distance from the seed <= radius". It only works through
// TriangleSelector::select_triangles_with_cursor(), which hands it the triangle (and so the
// original facet) for every test.
class GeodesicCursor : public TriangleSelector::Cursor
{
public:
    GeodesicCursor(const SpreadSession &session, float radius);
    ~GeodesicCursor() override = default;

    bool is_mesh_point_inside(const Vec3f &point) const override;
    bool is_pointer_in_triangle(const Vec3f &p1, const Vec3f &p2, const Vec3f &p3) const override;
    int  vertices_inside(const TriangleSelector::Triangle &tr, const std::vector<TriangleSelector::Vertex> &vertices) const override;
    bool is_edge_inside_cursor(const TriangleSelector::Triangle &tr, const std::vector<TriangleSelector::Vertex> &vertices) const override;
    bool is_facet_visible(int, const std::vector<Vec3f> &) const override { return true; }

private:
    const SpreadSession &m_session;
    float                m_r;
    // Original facet of the triangle being tested. select_triangle_recursive() always calls
    // vertices_inside() first, so the point-only overloads can use it.
    mutable int          m_facet { -1 };
};

class SpreadSession
{
public:
    // The selector must outlive the session and must not be reset or deserialized while it lives.
    SpreadSession(TriangleSelector &selector, const SpreadParams &params);

    // Grow the painted patch to radius r_mm (world mm, over the surface) and paint it. Calls with a
    // radius not above the last one do nothing; r_mm may be +infinity. Growing in several steps
    // gives exactly the same paint as one step to the final radius.
    void grow_to(float r_mm);

    // Nothing is left to grow: every facet the spread can reach is painted whole.
    bool  exhausted() const;
    // Last radius passed to grow_to(); 0 before the first call.
    float radius() const { return m_radius; }
    // Largest distance from the seed reached so far over the mesh vertices; with exhausted() it is
    // the farthest the spread can go.
    float max_reached() const { return m_max_reached; }
    // Original facets the spread has entered so far, in the order it entered them.
    const std::vector<int> &touched_facets() const { return m_touched; }
    const SpreadParams     &params() const { return m_params; }

    // Distance (world mm) from the seed to a point (mesh coordinates) of an entered original facet,
    // as the painting sees it; +infinity when the facet has not been entered.
    float distance_at(int facet, const Vec3f &pt_mesh) const;
    // Lower bound of distance_at() over a segment (mesh coordinates) of an entered facet.
    float distance_lower_bound(int facet, const Vec3f &a_mesh, const Vec3f &b_mesh) const;

private:
    friend class GeodesicCursor;

    static constexpr float INF = std::numeric_limits<float>::infinity();

    // A source is a point q in the facet's plane (the seed unfolded into it) at offset sigma: the
    // distance to p is sigma + |p - q| for every p whose segment from q crosses the window [t0, t1]
    // of the facet's entry edge (va, vb). "full" means every point of the facet sees q (the seed
    // facet, or a source lying on the entry edge itself).
    struct FacetInfo
    {
        int   facet { -1 };
        bool  has_src { false };
        bool  full { false };
        int   va { -1 }, vb { -1 };
        float t0 { 0.f }, t1 { 1.f };
        Vec3f q { Vec3f::Zero() };
        float sigma { 0.f };
        bool  done { false };
    };

    struct Event
    {
        float key;
        int   kind; // 0 vertex settles, 1 facet entered through a window
        int   idx;  // vertex index, or index into m_window_events
        bool  operator>(const Event &o) const
        {
            if (key != o.key) return key > o.key;
            if (kind != o.kind) return kind > o.kind;
            return idx > o.idx;
        }
    };
    struct WindowEvent
    {
        int to_facet;
        int from_facet;
    };

    // Geometry, world mm without translation.
    Vec3f pos(int vertex) const;                         // original vertex
    Vec3f to_world(const Vec3f &pt_mesh) const { return m_L * pt_mesh; }
    float field(int facet, const Vec3f &p_world) const;  // see distance_at()
    float field_lower_bound(int facet, const Vec3f &a_world, const Vec3f &b_world) const;

    bool  allowed(int facet, int from_facet) const;
    bool  fully_other_state(int tri) const;
    bool  propagate(const FacetInfo &from, int to_facet, FacetInfo &out) const;
    bool  window_entry_key(const FacetInfo &from, int to_facet, float &key) const;
    bool  source_visible(const FacetInfo &fi, const Vec3f &p_world) const;

    void  add_facet(const FacetInfo &fi);
    void  try_enter(int facet, int from_facet);
    void  relax_facet(int facet);
    void  settle(int vertex);
    void  drain();
    void  push_window_events(int facet);
    void  paint();

    TriangleSelector &m_sel;
    SpreadParams      m_params;
    Matrix3f          m_L;              // mesh to world linear part
    Matrix3f          m_normal_matrix;  // for the overhang filter
    float             m_uniform_scale { 0.f }; // > 0 when the scaling is uniform
    Vec3f             m_seed_world;
    Vec3f             m_seed_normal;    // mesh normal of the seed facet
    double            m_cos_limit { -2. };
    bool              m_has_angle_limit { false };
    float             m_overhang_limit { 0.f };

    std::vector<float>   m_dist;        // per original vertex
    std::vector<uint8_t> m_settled;
    std::vector<int>     m_slot;        // per original facet, index into m_facets or -1
    std::vector<FacetInfo> m_facets;
    std::unordered_map<int, boost::container::small_vector<int, 8>> m_vertex_facets; // entered facets round a vertex
    std::priority_queue<Event, std::vector<Event>, std::greater<Event>> m_queue;
    std::vector<WindowEvent> m_window_events;
    std::vector<int>     m_work;        // facets whose settled vertices still have to open their edges
    size_t               m_work_head { 0 };

    std::vector<int>     m_active;      // entered facets not yet painted whole
    std::vector<int>     m_touched;
    float                m_radius { 0.f };
    bool                 m_started { false };
    float                m_max_reached { 0.f };
    bool                 m_dead { false }; // the seed facet itself is not allowed

    // Euclidean reference: region facets sorted by distance to the seed.
    std::vector<std::pair<float, int>> m_euclid_order;
    size_t                             m_euclid_next { 0 };
};

} // namespace Slic3r

#endif // libslic3r_SpreadPaint_hpp_
