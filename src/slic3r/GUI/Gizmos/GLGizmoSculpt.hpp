#ifndef slic3r_GLGizmoSculpt_hpp_
#define slic3r_GLGizmoSculpt_hpp_

#include "GLGizmoBase.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"

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
    // Blender-style modal keys: F / Shift+F size the brush, Ctrl inverts it.
    // Routed from GLGizmosManager::on_char (F, Shift+F, Enter, Esc), which calls
    // this BEFORE its handle_shortcut() fallthrough so bare F does not open the
    // "place face on bed" gizmo while Sculpt is the current one.
    bool on_sculpt_char(int key_code, bool shift_down, bool ctrl_down);
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

public:
    // v3 appends: the numbering is the dropdown's order AND the 1..N keyboard
    // shortcut order, so nothing here may be renumbered without moving both.
    // Public only so the file-static table of dropdown rows in the .cpp can be
    // sized by BrushCount; nothing outside the gizmo has any business with it.
    enum class Brush : int {
        Grab = 0, Inflate = 1, Deflate = 2, Smooth = 3, Flatten = 4, Crease = 5,
        Pinch = 6, Nudge = 7, SnakeHook = 8, ClayStrips = 9, Mask = 10
    };
    static constexpr int BrushCount = 11;

private:

    // --- session / selection ---
    void attach_to_selection();
    void detach();
    ModelVolume *selected_volume(int &object_idx, int &volume_idx, int &mesh_id) const;
    Transform3d volume_trafo() const;
    double      mesh_scale() const;

    // --- stroke ---
    // The brushes whose move IS the mouse delta (Grab, Nudge, Snake Hook): they
    // do nothing on the click itself, they need a drag, and their cursor rides
    // the dragged patch instead of a fresh raycast onto the stale AABB tree.
    static bool brush_is_drag_driven(Brush brush)
    {
        return brush == Brush::Grab || brush == Brush::Nudge || brush == Brush::SnakeHook;
    }
    bool raycast(const Vec2d &mouse_position, Vec3f &hit) const;
    bool start_stroke(const Vec2d &mouse_position, bool shift_down, bool ctrl_down);
    void continue_stroke(const Vec2d &mouse_position, bool shift_down, bool ctrl_down);
    void end_stroke();
    void cancel_stroke();
    Sculpt::BrushParams make_brush(const Vec3f &center_mesh, const Vec3f &displacement_mesh, bool shift_down, bool ctrl_down) const;
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

    // --- brush dropdown ---
    // The 22 sculpt_brush_*.svg icons (a light and a dark variant each), loaded
    // once on the first panel frame, when a GL context is guaranteed. A brush
    // whose texture failed to load falls back to text alone rather than drawing
    // nothing, so a missing file degrades the panel instead of breaking it.
    void        ensure_brush_icons();
    ImTextureID brush_icon(Brush brush) const;
    const wxString &brush_label(Brush brush) const;
    void        draw_brush_combo(float wrap_width);

    // --- mask (phase 3b) ---
    // Re-run the auto detections against the current mesh and push the toggles
    // into the session. Cheap and idempotent: called on attach and after every
    // commit or subdivide, so the mask is never stale.
    void refresh_masks();
    void render_mask_overlay() const;

    // --- modal brush adjust (F / Shift+F) ---
    void begin_adjust(Sculpt::AdjustTarget target);
    void update_adjust(const Vec2d &mouse_position);
    void end_adjust(bool confirm);
    // Ctrl inverts Inflate/Deflate, Flatten and Crease while a stroke runs.
    bool brush_inverted(bool ctrl_down) const;

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
    // Flatten: symmetric by default (bumps down as well as dents up). The Ctrl
    // variant is Blender's "Fill" - only what is below the plane comes up.
    bool  m_flatten_fill_only{false};
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

    // Modal brush adjust. While active every mouse move retargets the value and
    // nothing else; the stroke path is skipped entirely.
    Sculpt::AdjustState m_adjust;
    // The plane a Flatten/Crease stroke works against, pinned at stroke start so
    // the brush levels one plane instead of chasing the surface it is levelling.
    Vec3f m_stroke_plane_normal{Vec3f::Zero()};
    // Clay Strips needs the plane's OFFSET pinned too, not only its direction:
    // refitting it every tick would measure the target standoff from the
    // material the previous tick just laid down, and a held stroke would climb
    // without limit instead of stopping at the plateau that is the brush's whole
    // point. Flatten and Crease deliberately do refit their offset every tick.
    Vec3f m_stroke_plane_origin{Vec3f::Zero()};
    // Ctrl state as of the last tick, so the cursor circle can show the inverted
    // colour on hover and not only mid-stroke.
    bool  m_ctrl_inverted{false};
    // The F/Shift+F modal ends on the left DOWN, so by the time the matching
    // LeftUp arrives m_adjust is no longer active and the event falls straight
    // through the gizmo to GLCanvas3D, whose LeftUp handler deselects the object
    // when the click landed off it - which is exactly where a brush-sizing drag
    // usually ends. This latches "the up half of the click that ended the modal
    // is still owed to me" so it can be swallowed too. Same for the right click
    // that cancels, and for the Esc path (which arms neither, having no up).
    bool  m_swallow_left_up{false};
    bool  m_swallow_right_up{false};

    // v3 icons: brush -> texture id, light and dark. Empty until the first panel
    // frame loads them; a brush missing from the map draws as text.
    std::map<int, ImTextureID> m_brush_icons;
    std::map<int, ImTextureID> m_brush_icons_dark;
    bool                       m_brush_icons_tried{false};

    // v3 mask toggles, session-only (the spec's phase-3b recommendation): the
    // auto detections are pure functions of the current mesh and are recomputed
    // on attach, and a hand-painted mask is deliberately not persisted - that
    // would need a new FacetsAnnotation-style store and a 3MF schema change.
    bool  m_protect_bed{true};
    bool  m_protect_sharp{false};
    float m_sharp_dihedral{60.f};
    // Mask brush strength is its own knob: painting protection is not a sculpt
    // stroke and does not want the sculpt strength.
    float m_mask_strength{0.5f};
    // Clay Strips' plane offset, as a fraction of the brush radius, so it feels
    // the same at any brush size the way Inflate's amount does.
    float m_clay_offset_ratio{0.12f};

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
