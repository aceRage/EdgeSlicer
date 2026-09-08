#ifndef slic3r_MeshSculpt_hpp_
#define slic3r_MeshSculpt_hpp_

// Ultra: brush sculpting on an indexed_triangle_set.
//
// The whole point of this module is that it never changes its.indices - a stroke
// only rewrites vertex positions. That keeps every per-facet annotation on the
// ModelVolume (supported_facets / seam_facets / mmu_segmentation_facets /
// fuzzy_skin_facets) valid across a sculpt commit, so the commit path may call
// ModelVolume::set_mesh() WITHOUT Plater::clear_before_change_mesh().
//
// The one operation in here that does change the topology is
// its_subdivide_midpoint(); it is a separate, opt-in pre-step and its caller
// must go through clear_before_change_mesh() like the Simplify gizmo does.

#include <cstdint>
#include <vector>

#include "Point.hpp"
#include "TriangleMesh.hpp"
#include "AABBTreeIndirect.hpp"

namespace Slic3r {

class ModelVolume;

namespace Sculpt {

enum class BrushType : unsigned char {
    Grab,    // drag the surface with the mouse, in the view plane
    Inflate, // push the surface along its vertex normals (or pull it in)
    Smooth,  // Laplacian relaxation, optionally Taubin lambda/mu
    Flatten, // move vertices onto a plane fitted to the patch under the brush
    Crease   // pinch toward the stroke axis and push along -normal (or +normal)
};

// Brush weight at distance d from the brush centre for a brush of radius r.
// Smooth quartic bump, w(0) = 1, w(r) = 0, w'(0) = w'(r) = 0:
//     w = (1 - (d/r)^2)^2
// Returns 0 outside the brush and for a non-positive radius.
float falloff_weight(float d, float r);

struct BrushParams
{
    BrushType type   = BrushType::Grab;
    // Brush centre, in the mesh's own (volume) coordinates.
    Vec3f     center = Vec3f::Zero();
    // Brush radius in mesh units (mm for a part at scale 1).
    float     radius = 1.f;
    // Dimensionless 0..1 multiplier applied on top of the falloff weight.
    float     strength = 1.f;
    // false: every vertex inside the brush gets weight 1 (hard-edged brush).
    bool      falloff  = true;

    // Grab: the move applied at full weight, in mesh coordinates.
    Vec3f     displacement = Vec3f::Zero();

    // Inflate: displacement along the vertex normal at full weight, in mesh units.
    float     amount  = 0.1f;
    bool      deflate = false;

    // Flatten / Crease: the plane (Flatten) or the axis (Crease) the brush works
    // against. A zero normal means "fit it from the patch under the brush", which
    // is what the gizmo does every tick; a caller may pin it to hold one plane
    // for a whole stroke.
    Vec3f     plane_normal = Vec3f::Zero();
    // Flatten: false moves every vertex toward the plane (symmetric - a bump is
    // pushed down and a dent is pulled up). true is Blender's "Fill": only the
    // vertices BELOW the plane (on the -normal side) move, so dents are filled
    // and bumps are left alone.
    bool      fill_only = false;
    // Crease: false is a valley (pinch + push along -normal), true is a ridge
    // (pinch + push along +normal).
    bool      ridge = false;
    // Crease: the size of the normal push relative to the tangential pinch.
    float     crease_normal_ratio = 1.f;

    // Smooth
    int       iterations = 1;
    // Plain Laplacian uses lambda only. Taubin alternates a +lambda shrinking
    // pass with a -mu unshrinking pass, which is what preserves the volume.
    bool      taubin = false;
    float     lambda = 0.6307f;
    float     mu     = -0.6732f;
};

// What one brush tick touched. Both lists are sorted and free of duplicates.
struct StrokeStep
{
    std::vector<uint32_t> moved_vertices;
    // Every triangle incident to a moved vertex: its position and its flat
    // normal are now stale on the GPU and in the cached vertex normals.
    std::vector<uint32_t> dirty_triangles;

    bool empty() const { return moved_vertices.empty(); }
};

// Per-part state cached for the duration of a sculpt session: the working copy
// of the mesh, an AABB tree over its triangles, the vertex->face adjacency and
// the vertex normals. Everything a brush tick needs is O(touched) after this.
class SculptSession
{
public:
    explicit SculptSession(const indexed_triangle_set &its);

    const indexed_triangle_set &mesh() const { return m_its; }

    const std::vector<Vec3f> &vertex_normals() const { return m_vertex_normals; }

    size_t vertices_count() const { return m_its.vertices.size(); }
    size_t triangles_count() const { return m_its.indices.size(); }

    // Triangles whose (pre-stroke) bounding box overlaps the brush sphere.
    std::vector<uint32_t> triangles_in_radius(const Vec3f &center, float radius);
    // Vertices strictly inside the brush sphere. A vertex exactly on the rim has
    // weight 0 either way, so it is left out.
    std::vector<uint32_t> vertices_in_radius(const Vec3f &center, float radius);
    // Allocation-free variants: a drag reuses the caller's buffers.
    void collect_triangles_in_radius(const Vec3f &center, float radius, std::vector<uint32_t> &out);
    void collect_vertices_in_radius(const Vec3f &center, float radius, std::vector<uint32_t> &out);

    // Mean edge length over the triangles under the brush; -1 if the brush is
    // over nothing. Used by the subdivide-on-demand check.
    float local_edge_length(const Vec3f &center, float radius);

    // Apply one brush tick. Returns what moved.
    StrokeStep apply(const BrushParams &params);
    // Same, writing into a caller-owned StrokeStep so a drag allocates nothing.
    void apply(const BrushParams &params, StrokeStep &out);

    // The AABB tree goes stale as soon as vertices move. Cheap enough to leave
    // stale during a stroke (the brush centre comes from the GUI raycaster);
    // rebuild it once when the stroke ends.
    void rebuild_tree();

    // The one-ring of a vertex, appended to `out` (which is cleared first).
    void one_ring(uint32_t vertex, std::vector<uint32_t> &out) const;

private:
    void build_adjacency();
    void recompute_normals(const std::vector<uint32_t> &triangles);

    indexed_triangle_set                m_its;
    AABBTreeIndirect::Tree<3, float>    m_tree;
    // vertex -> incident faces, CSR
    std::vector<uint32_t>               m_vertex_faces;
    std::vector<uint32_t>               m_vertex_faces_start;
    std::vector<Vec3f>                  m_vertex_normals;

    // Scratch reused between ticks so a stroke allocates nothing.
    std::vector<uint32_t>               m_stamp;      // per-vertex visit epoch
    std::vector<uint32_t>               m_tri_stamp;  // per-triangle visit epoch
    uint32_t                            m_epoch = 0;
    std::vector<float>                  m_scratch_weights;
    std::vector<Vec3f>                  m_scratch_updated;
    std::vector<uint32_t>               m_scratch_ring;
    std::vector<uint32_t>               m_scratch_normal_verts;
};

// ----------------------------------------------------------------------------
// Cursor tracking
// ----------------------------------------------------------------------------
//
// Where the brush cursor is drawn, frame by frame, factored out of the gizmo so
// it can be tested without a GL context, a camera or a mouse.
//
// The rule the paint gizmos follow is "re-raycast on every mouse move and draw
// at the hit" - GLGizmoPainterBase::render_cursor() calls update_raycast_cache()
// with the current mouse position every frame, so its cursor tracks the mouse
// whether or not a button is held. Sculpt cannot follow that rule verbatim for
// Grab: a Grab stroke drags the very surface the cursor sits on, and the AABB
// tree is deliberately left stale for the duration of a stroke, so a fresh
// raycast mid-Grab returns the PRE-stroke surface and the cursor lags the patch
// the user is pulling. Hence:
//
//   * Grab, mid-stroke: the cursor is the stroke anchor moved by the accumulated
//     drag - it stays glued to the patch being pulled.
//   * every other brush, and every hover: the cursor is the fresh hit.
//   * when the ray misses mid-stroke, the cursor holds its last position rather
//     than blinking out - the user is still sculpting, and a stroke that runs
//     off the silhouette should not lose its cursor.
//   * when the ray misses while merely hovering, the cursor is hidden.
//
// Positions are in the mesh's own (volume) coordinates, the space
// BrushParams::center lives in.
enum class CursorMode : unsigned char {
    Hover,      // no button held
    StrokeGrab, // Grab stroke in progress: follow the dragged surface point
    StrokeHit   // Inflate/Deflate/Smooth stroke in progress: follow the fresh hit
};

// One frame's worth of input to the tracker.
struct CursorInput
{
    CursorMode mode      = CursorMode::Hover;
    // Did the ray hit the (stale) mesh this frame?
    bool       hit_valid = false;
    Vec3f      hit       = Vec3f::Zero();
    // StrokeGrab only: the stroke's anchor plus everything the drag has
    // accumulated so far, i.e. where the grabbed patch has moved to.
    Vec3f      grab_anchor = Vec3f::Zero();
};

// Where to draw the cursor this frame.
struct CursorState
{
    bool  visible  = false;
    Vec3f position = Vec3f::Zero();
};

// Pure: next cursor state from the previous one and this frame's input.
CursorState next_cursor_state(const CursorState &prev, const CursorInput &in);

// ----------------------------------------------------------------------------
// Modal brush-parameter adjustment (Blender's F / Shift+F)
// ----------------------------------------------------------------------------
//
// Press F, move the mouse sideways, click or Enter to confirm, Esc or right
// click to go back. The gizmo owns the wx plumbing; the arithmetic and the state
// machine live here so they can be tested without a window.
//
// The mapping is multiplicative for the radius (a drag of AdjustFullScalePx to
// the right doubles it, the same to the left halves it, so the feel is the same
// at 0.5 mm and at 15 mm) and additive for the strength (the whole 0..1 range
// spans AdjustFullScalePx of travel).
enum class AdjustTarget : unsigned char { None, Radius, Strength };

struct AdjustState
{
    AdjustTarget target      = AdjustTarget::None;
    // The value when the modal started; Esc / right click restores exactly this.
    float        start_value = 0.f;
    // Mouse x when the modal started.
    double       start_x     = 0.;
    // The live value: what the panel input and the on-screen circle show.
    float        value       = 0.f;

    bool active() const { return target != AdjustTarget::None; }
};

// Pixels of horizontal travel for one doubling (radius) or for the whole 0..1
// span (strength).
inline constexpr double AdjustFullScalePx = 240.;

// Begin a modal adjustment.
AdjustState adjust_begin(AdjustTarget target, float current_value, double mouse_x);
// One mouse-move frame. `value` is recomputed from the TOTAL travel since the
// start rather than accumulated, so the gesture is exactly reversible: coming
// back to the starting x returns the starting value.
AdjustState adjust_move(const AdjustState &state, double mouse_x, float value_min, float value_max);
// Confirm: the live value stands.
float adjust_confirm(const AdjustState &state);
// Cancel: the value goes back to what it was when the modal began.
float adjust_cancel(const AdjustState &state);

// True when Ctrl inverts this brush. Grab and Smooth are unaffected: there is no
// sensible opposite of dragging a patch, and an "anti-smooth" brush would just
// amplify noise.
bool brush_inverts_with_ctrl(BrushType type);

// Area-weighted plane through the vertices under the brush: `origin` is the
// weighted centroid, `normal` the weighted average of the vertex normals.
// Returns false when the brush covers nothing usable.
bool fit_plane(const indexed_triangle_set  &its,
               const std::vector<Vec3f>    &vertex_normals,
               const std::vector<uint32_t> &vertices,
               const Vec3f                 &center,
               float                        radius,
               bool                         falloff,
               Vec3f                       &origin,
               Vec3f                       &normal);

// Mean squared distance of `vertices` to the plane (origin, normal): the
// flatness measure a Flatten stroke has to reduce.
float plane_distance_variance(const indexed_triangle_set  &its,
                              const std::vector<uint32_t> &vertices,
                              const Vec3f                 &origin,
                              const Vec3f                 &normal);

// Uniform 1:4 midpoint subdivision of the whole mesh. Midpoints are shared
// between the two triangles of an edge, so a manifold mesh stays manifold and
// no cracks appear. Triangle winding is preserved.
// THIS CHANGES its.indices - every facet annotation on the volume is invalidated.
indexed_triangle_set its_subdivide_midpoint(const indexed_triangle_set &its);

// Area-weighted vertex normals (the cross product of the two triangle edges is
// already proportional to twice the triangle area, so summing unnormalised face
// normals gives the area weighting for free).
std::vector<Vec3f> its_vertex_normals(const indexed_triangle_set &its);

// True when `its` can be committed onto `mv` without invalidating the per-facet
// annotations: same triangle count and the same indices, triangle for triangle.
bool indices_match(const ModelVolume &mv, const indexed_triangle_set &its);

// Commit a sculpted mesh onto a ModelVolume, keeping supported_facets,
// seam_facets, mmu_segmentation_facets and fuzzy_skin_facets intact.
// Refuses (returns false, changes nothing) unless indices_match() holds, so a
// caller can never silently misalign existing paint. The caller is still
// responsible for the undo snapshot and for re-scheduling the slice
// (Plater::changed_mesh) - but NOT for Plater::clear_before_change_mesh().
bool commit_sculpted_mesh(ModelVolume &mv, indexed_triangle_set &&its, bool ensure_on_bed = true);

// Sum over vertices of |v - mean(one-ring(v))|^2. A roughness measure: smoothing
// must reduce it.
float its_laplacian_energy(const indexed_triangle_set &its);

} // namespace Sculpt
} // namespace Slic3r

#endif // slic3r_MeshSculpt_hpp_
