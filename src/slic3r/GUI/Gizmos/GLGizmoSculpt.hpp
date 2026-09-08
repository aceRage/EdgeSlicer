#ifndef slic3r_GLGizmoSculpt_hpp_
#define slic3r_GLGizmoSculpt_hpp_

#include "GLGizmoBase.hpp"

#include "libslic3r/MeshSculpt.hpp"
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/Point.hpp"

#include <map>
#include <memory>
#include <string>

namespace Slic3r {
class ModelVolume;

namespace GUI {

enum class SLAGizmoEventType : unsigned char;
struct Camera;

// Ultra: brush sculpting on the selected part.
//
// v1 is deliberately vertex-only: a stroke rewrites positions in
// indexed_triangle_set::vertices and never touches ::indices. That is what lets
// the commit path go through Sculpt::commit_sculpted_mesh() (ModelVolume::set_mesh
// + changed_mesh) WITHOUT Plater::clear_before_change_mesh(), so painted supports,
// seams, colours and fuzzy skin survive a sculpt.
//
// The one topology-changing operation, "Subdivide", is an explicit separate
// button, a separate undo step, and it DOES clear the paint the way Simplify does.
class GLGizmoSculpt : public GLGizmoBase
{
public:
    GLGizmoSculpt(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id);
    ~GLGizmoSculpt() override;

    void data_changed(bool is_serializing) override;
    bool on_mouse(const wxMouseEvent &mouse_event) override;
    // Ctrl + wheel resizes the brush, like the paint gizmos.
    bool gizmo_event(SLAGizmoEventType action, const Vec2d &mouse_position, bool shift_down, bool alt_down, bool control_down);

    bool wants_enter_leave_snapshots() const override { return true; }
    std::string get_gizmo_entering_text() const override;
    std::string get_gizmo_leaving_text() const override;

protected:
    bool on_init() override;
    std::string on_get_name() const override;
    void on_render() override;
    void on_render_input_window(float x, float y, float bottom_limit) override;
    bool on_is_activable() const override;
    bool on_is_selectable() const override { return true; }
    void on_set_state() override;
    CommonGizmosDataID on_get_requirements() const override;

private:
    enum class Brush : int { Grab = 0, Inflate = 1, Deflate = 2, Smooth = 3 };

    // --- session / selection ---
    void attach_to_selection();
    void detach();
    ModelVolume *selected_volume(int &object_idx, int &volume_idx, int &mesh_id) const;
    Transform3d volume_trafo() const;
    double      mesh_scale() const;

    // --- stroke ---
    bool raycast(const Vec2d &mouse_position, Vec3f &hit) const;
    bool start_stroke(const Vec2d &mouse_position, bool shift_down);
    void continue_stroke(const Vec2d &mouse_position, bool shift_down);
    void end_stroke();
    void cancel_stroke();
    Sculpt::BrushParams make_brush(const Vec3f &center_mesh, const Vec3f &displacement_mesh, bool shift_down) const;
    // Project the mouse onto the plane through the stroke anchor facing the camera.
    bool project_on_drag_plane(const Vec2d &mouse_position, Vec3d &out) const;

    // --- rendering ---
    // Re-evaluate where the cursor sphere belongs for the current mouse
    // position. Called on hover AND on every drag tick, which is what makes
    // the sphere follow the mouse through a stroke.
    void update_cursor(const Vec2d &mouse_position);
    void render_cursor_sphere() const;
    // Push the touched triangles into the volume's vertex buffer, or - if that
    // buffer cannot be patched - rebuild the model at a throttled rate.
    void refresh_render_volumes(const std::vector<uint32_t> &dirty_triangles, bool force);

    // --- subdivision ---
    bool  needs_subdivision();
    void  do_subdivide();
    size_t subdivided_triangle_count() const;

    ModelVolume                          *m_volume{nullptr};
    int                                   m_object_idx{-1};
    int                                   m_volume_idx{-1};
    // Index of m_volume among the object's model-part volumes: that is how the
    // shared Raycaster indexes its meshes.
    int                                   m_mesh_id{-1};
    ObjectID                              m_volume_id;
    std::unique_ptr<Sculpt::SculptSession> m_session;
    // Reused every tick so a drag allocates nothing.
    Sculpt::StrokeStep                     m_stroke_step;

    // Brush settings. The radius is in world millimetres, like the paint gizmos'.
    Brush m_brush{Brush::Grab};
    float m_cursor_radius{2.f};
    float m_strength{0.5f};
    bool  m_falloff{true};
    bool  m_taubin{false};
    // Offer to subdivide when the mesh under the brush is coarser than
    // radius * m_subdivide_ratio.
    float m_subdivide_ratio{0.25f};

    static constexpr float CursorRadiusMin  = 0.4f;
    static constexpr float CursorRadiusMax  = 20.f;
    static constexpr float CursorRadiusStep = 0.2f;
    // The floor BrushParams::strength is documented at; the panel shows it as 5%.
    static constexpr float StrengthMin      = 0.05f;
    // A midpoint subdivision quadruples the triangle count; refuse past this.
    static constexpr size_t MaxTrianglesAfterSubdivision = 2000000;

    // Stroke state.
    bool  m_stroke_active{false};
    bool  m_pending_commit{false};
    Vec3f m_stroke_center_mesh{Vec3f::Zero()};
    Vec3d m_stroke_anchor_world{Vec3d::Zero()};
    Vec3d m_drag_prev_world{Vec3d::Zero()};
    // Union of everything a stroke touched, for the buffer refresh fallback.
    std::vector<uint32_t> m_stroke_dirty_triangles;
    int64_t m_last_render_refresh{0};
    bool    m_partial_gpu_update{true};

    // Hover / cursor. m_cursor is driven by Sculpt::next_cursor_state() from
    // both the hover path and the drag path; m_hit_valid/m_hit stay as the raw
    // last raycast result, which the subdivide check wants.
    mutable Vec2d               m_last_mouse{Vec2d::Zero()};
    mutable bool                m_hit_valid{false};
    mutable Vec3f               m_hit{Vec3f::Zero()};
    mutable Sculpt::CursorState m_cursor;

    std::map<std::string, wxString> m_desc;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoSculpt_hpp_
