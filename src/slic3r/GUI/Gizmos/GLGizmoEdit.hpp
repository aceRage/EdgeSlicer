#ifndef slic3r_GLGizmoEdit_hpp_
#define slic3r_GLGizmoEdit_hpp_

#include "GLGizmoBase.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/MeshUtils.hpp"
#include "slic3r/GUI/SceneRaycaster.hpp"

#include "libslic3r/MeshEdit.hpp"
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

// Ultra: CAD-like direct mesh editing - the "Edit" gizmo, phase 1.
// Spec: docs/superpowers/specs/2026-09-11-cad-mode-research.md,
//       docs/superpowers/specs/2026-09-12-cad-edit-phase1.md
//
// PHASE 1 is deliberately the topology-preserving half of the feature:
//
//   * SELECTION - hover a face and the whole planar (or smooth) region lights
//     up; hover near an edge and the whole feature-edge CHAIN lights up, grown
//     by dihedral angle with an adjustable threshold.
//   * PUSH/PULL - drag the selected face region along its own normal, with a
//     numeric field and a snap step. Only vertex POSITIONS move, so the facets
//     outside the region that share a border vertex stretch to follow (a cube's
//     top face pushed up makes the walls taller).
//   * UNDO - a gizmo-local stack, the shape GLGizmoSculpt's stroke undo and the
//     Cut gizmo's curved-sheet undo both use, offered the key ahead of the
//     canvas through GLGizmosManager::on_char.
//
// Because a translate changes NO indices, the commit goes through
// Sculpt::commit_sculpted_mesh() WITHOUT Plater::clear_before_change_mesh() -
// so painted supports, seams, MMU colours and fuzzy skin all survive a push.
// That is the single most valuable property of scoping phase 1 this way, and it
// is guarded by an indices_match() assertion at the commit and by a vertex- and
// facet-count test in tests/libslic3r/test_mesh_edit.cpp.
//
// NOT in phase 1: bevel/chamfer (phase 2), extrude/inset/loop selection (phase
// 3). All three insert geometry and renumber facets, so when they land they must
// take the OTHER commit path, the way Subdivide and Simplify do.
class GLGizmoEdit : public GLGizmoBase
{
public:
    GLGizmoEdit(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id);
    ~GLGizmoEdit() override;

    void data_changed(bool is_serializing) override;
    bool on_mouse(const wxMouseEvent &mouse_event) override;
    // Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z against the gizmo-local stack, and Esc to
    // drop the selection. Routed from GLGizmosManager::on_char, which offers it
    // to the gizmo BEFORE the canvas turns Ctrl+Z into an EVT_GLCANVAS_UNDO -
    // the same first-refusal hook Sculpt and Cut already use. Returns false for
    // anything it does not want, so nothing else is stolen.
    bool on_edit_char(int key_code, bool shift_down, bool ctrl_down);

    bool wants_enter_leave_snapshots() const override { return true; }
    std::string get_gizmo_entering_text() const override;
    std::string get_gizmo_leaving_text() const override;

protected:
    bool on_init() override;
    std::string on_get_name() const override;
    void on_render() override;
    void on_render_input_window(float x, float y, float bottom_limit) override;
    // The push handle is a real pickable object, not decoration: it is
    // registered with the scene raycaster the way GLGizmoFlatten registers its
    // planes and GLGizmoCut its connector cones, so the canvas hit-tests it and
    // hands back m_hover_id. Without these two the handle could never be
    // grabbed, which is exactly the defect they fix.
    void on_register_raycasters_for_picking() override;
    void on_unregister_raycasters_for_picking() override;
    bool on_is_activable() const override;
    bool on_is_selectable() const override { return true; }
    void on_set_state() override;
    CommonGizmosDataID on_get_requirements() const override;
    std::string get_dock_key() const override { return "edit"; }

private:
    // Width of the panel's label column: the widest label the panel can show,
    // plus a gap. Measured once per DPI/font change rather than per frame, and
    // over EVERY label rather than one of them - picking a single label left
    // "Face tolerance" and "Curvature per step" clipped.
    float compute_label_width() const;
    // The width the Select combo needs to show its LONGEST entry, not its first.
    // Cached against the font scaling exactly as the label width is - see
    // compute_label_width() for why that caching exists.
    float compute_mode_combo_width() const;
    mutable float m_label_width{ 0.f };
    mutable float m_label_width_scaling{ 0.f };
    mutable float m_mode_combo_width{ 0.f };
    mutable float m_mode_combo_scaling{ 0.f };

    // What a hover or a click picks.
    enum class PickMode : int {
        // A face region: the coplanar connected set (a CAD face).
        Face = 0,
        // The same grow with a per-step angle as well, so a gently curved face
        // comes out whole instead of shattering into one-triangle regions.
        SmoothFace = 1,
        // A feature-edge chain, grown by dihedral angle from the edge under the
        // cursor.
        EdgeChain = 2
    };

    // --- session / selection ---
    void attach_to_selection();
    void detach();
    ModelVolume *selected_volume(int &object_idx, int &volume_idx, int &mesh_id) const;
    Transform3d volume_trafo() const;
    double      mesh_scale() const;

    // --- picking ---
    bool raycast(const Vec2d &mouse_position, Vec3f &hit, Vec3f &normal, size_t &facet) const;
    // Re-evaluate what is under the cursor. Cheap enough for a hover because the
    // region grow is bounded and the chain walk is linear in the feature edges.
    void update_hover(const Vec2d &mouse_position);
    void commit_hover_to_selection();
    void clear_selection();
    MeshEdit::RegionParams region_params() const;

    // --- push/pull handle ---
    // id of the one pickable element this gizmo owns. Any non-negative value
    // works; 0 keeps the decode in GLCanvas3D trivial.
    static constexpr int PushHandleId = 0;
    // Build the stem+cone arrow once. Two PickingModels rather than one so the
    // stem and the head can be scaled independently (a cone squashed to the
    // stem's radius is not an arrow head).
    void  init_handle_models();
    // World transform of the stem and of the head for the CURRENT selection and
    // live drag distance. Returns false when there is nothing to draw.
    bool  handle_transforms(Transform3d &stem, Transform3d &head) const;
    // Keep the registered raycasters on top of the drawn arrow.
    void  update_handle_raycasters();
    // Length of the arrow in world mm, from the mesh size - so it is visible on
    // a 5 mm part and not a mast on a 300 mm one.
    double handle_length() const;

    // --- push/pull ---
    bool  begin_drag(const Vec2d &mouse_position);
    void  update_drag(const Vec2d &mouse_position);
    void  end_drag();
    void  cancel_drag();
    // The signed distance the mouse has travelled along the push axis, obtained
    // by projecting the ray onto the plane that contains the push axis and faces
    // the camera as squarely as it can.
    bool  drag_distance(const Vec2d &mouse_position, float &out) const;
    // Apply `distance` to the PRE-DRAG mesh (a drag is absolute, so every tick
    // re-applies the whole displacement rather than accumulating) and show it.
    // `final_apply` runs the self-intersection guard and commits; a preview tick
    // skips the guard, which is what keeps a drag interactive.
    void  apply_push(float distance, bool final_apply);
    // The numeric field's Apply button: the same path as the end of a drag.
    void  apply_push_from_field();
    void  commit_to_volume();
    void  refresh_render_volume();

    // --- bevel / chamfer (phase 2) ---
    // The params the panel's Width / Segments / Profile controls describe.
    MeshEdit::BevelParams bevel_params() const;
    // Recompute the live preview from the CURRENT selection and params and show
    // it. The brief asks for a strip preview if feasible and the result mesh
    // otherwise; the result mesh is what this does, because the bevel is linear
    // in the SELECTED edges (not in the mesh) and so is fast enough to re-run on
    // a slider tick, and because showing the actual result cannot disagree with
    // what Apply will produce.
    void  update_bevel_preview();
    void  clear_bevel_preview();
    // Apply the bevel for real: session undo entry, commit, rebuild.
    // The solve itself goes to a BevelJob on the plater's worker - it used to
    // run here, on the UI thread, which is what let an expensive selection
    // freeze the whole application - and this only queues it.
    void  apply_bevel();
    // The worker's result, delivered on the UI thread by BevelJob::finalize().
    void  on_bevel_done(const MeshEdit::BevelResult &r);
    // True while a BevelJob queued by this gizmo is still running: the panel
    // disables Apply and shows progress plus Cancel instead.
    bool  bevel_running() const { return m_bevel_job_running; }
    void  cancel_bevel_job();
    // The bevel RENUMBERS every facet, so unlike a push it cannot go through
    // Sculpt::commit_sculpted_mesh() - it takes the clear_before_change_mesh()
    // path Subdivide and Simplify take, and the painted data is dropped.
    void  commit_bevelled_mesh(indexed_triangle_set &&its);

    // --- rendering ---
    // init_plane_data-style highlight of a facet list, the way
    // GLGizmoMeasure::init_plane_glmodel builds its plane overlay.
    void  rebuild_region_model(const MeshEdit::FaceRegion &region, GLModel &model, int &cached_key, int key) const;
    void  rebuild_chain_model(const MeshEdit::EdgeChain &chain, GLModel &model, int &cached_key, int key) const;
    void  render_highlights();
    void  render_push_axis() const;
    void  invalidate_highlight_models();

    // --- undo ---
    bool  do_undo();
    bool  do_redo();

    ModelVolume                            *m_volume{nullptr};
    int                                     m_object_idx{-1};
    int                                     m_volume_idx{-1};
    // Index of m_volume among the object's model-part volumes: how the shared
    // Raycaster indexes its meshes.
    int                                     m_mesh_id{-1};
    ObjectID                                m_volume_id;
    std::unique_ptr<MeshEdit::EditSession>  m_session;

    // --- picking state ---
    PickMode m_pick_mode{PickMode::Face};
    // Dihedral threshold, in degrees, above which an edge is a feature edge.
    float    m_feature_angle{30.f};
    // How far a chain may turn at a vertex and still continue.
    float    m_chain_continuation{35.f};
    // Planar mode's tolerance against the seed normal.
    float    m_planar_tol{1.f};
    // Smooth mode's per-step tolerance, and its cap against the seed.
    float    m_smooth_step{8.f};
    float    m_smooth_cap{20.f};

    bool                   m_hover_valid{false};
    Vec3f                  m_hover_point{Vec3f::Zero()};
    size_t                 m_hover_facet{0};
    MeshEdit::FaceRegion   m_hover_region;
    MeshEdit::EdgeChain    m_hover_chain;

    bool                   m_has_selection{false};
    MeshEdit::FaceRegion   m_selected_region;
    MeshEdit::EdgeChain    m_selected_chain;
    // Which of the two the selection is. A chain cannot be pushed in phase 1 -
    // it is selection and highlight only, the substrate phase 2's bevel will use
    // - so the panel says so rather than offering a control that does nothing.
    bool                   m_selection_is_chain{false};

    // --- push/pull state ---
    // The typed distance, in world millimetres. Shared by the field and the
    // drag, and both go through MeshEdit::snap_to_step so they agree exactly.
    float m_push_distance{0.f};
    float m_push_step{0.5f};
    bool  m_push_snap{true};
    static constexpr float PushStepMin = 0.f;
    static constexpr float PushStepMax = 10.f;

    // The arrow: stem (cylinder) + head (cone), each with its own raycaster.
    PickingModel m_handle_stem;
    PickingModel m_handle_head;
    std::vector<std::shared_ptr<SceneRaycasterItem>> m_handle_raycasters;

    // NOTE: the push drag deliberately uses GLGizmoBase::m_dragging rather than a
    // member of its own. A shadowing bool here would leave GLGizmosManager::
    // is_dragging() reporting false for the whole drag, so GLCanvas3D would keep
    // running its picking pass (GLCanvas3D.cpp:7377) and let the camera rotate
    // under the gesture.
    // The mesh as it stood when the drag began: every tick re-applies the whole
    // displacement to this, so a drag is absolute and exactly reversible.
    indexed_triangle_set m_drag_base_mesh;
    // The region as it stood when the drag began, in the base mesh's indices.
    MeshEdit::FaceRegion m_drag_region;
    Vec3d m_drag_anchor_world{Vec3d::Zero()};
    Vec3d m_drag_axis_world{Vec3d::UnitZ()};
    float m_drag_start_distance{0.f};
    // The live distance the preview is showing, so the panel's field tracks the
    // drag and the drag picks up where the field left off.
    float m_drag_distance{0.f};

    // The last refusal, so the panel can say WHY a push did not happen instead
    // of silently doing nothing.
    MeshEdit::TranslateStatus m_last_status{MeshEdit::TranslateStatus::Ok};
    bool  m_show_last_status{false};

    // --- bevel / chamfer state (phase 2) ---
    float m_bevel_width{1.f};
    int   m_bevel_segments{1};
    // 0 = chamfer, 1 = round. An int because that is what the radio buttons want.
    int   m_bevel_profile{0};
    static constexpr float BevelWidthMin = 0.01f;
    static constexpr float BevelWidthMax = 50.f;
    // The SLIDER's range, which is deliberately narrower than the value's. 0..10 mm
    // covers every bevel anyone puts on a printed part, and a slider that spanned
    // the full 0..50 would put all the useful widths in its first fifth. Values
    // past the slider's top are still reachable by typing into the field beside it,
    // which is clamped to BevelWidthMax instead.
    static constexpr float BevelWidthSliderMin = 0.f;
    static constexpr float BevelWidthSliderMax = 10.f;
    // The step the numeric field moves in, and the brief's 0.1 mm.
    static constexpr float BevelWidthStep      = 0.1f;

    // The live preview. `m_bevel_preview_mesh` is the bevelled mesh the render
    // volume is currently showing; empty when no preview is up. The key is what
    // the preview was built from, so a redraw that changes nothing rebuilds
    // nothing.
    indexed_triangle_set m_bevel_preview_mesh;
    bool                 m_bevel_preview_valid{false};
    float                m_bevel_preview_width{-1.f};
    int                  m_bevel_preview_segments{-1};
    int                  m_bevel_preview_profile{-1};
    int                  m_bevel_preview_seed{-1};

    // What the last solve/preview decided, so the panel can show the clamped
    // width and the corner count before the user commits.
    // Which construction the last solve used, so the panel can say "geometric" or
    // "voxel fallback". The two produce visibly different meshes and the user is
    // entitled to know which they are looking at.
    MeshEdit::BevelPath   m_bevel_path{MeshEdit::BevelPath::None};
    MeshEdit::BevelStatus m_bevel_status{MeshEdit::BevelStatus::EmptyChain};
    float                 m_bevel_applied_width{0.f};
    bool                  m_bevel_clamped{false};
    size_t                m_bevel_corner_patches{0};
    // Edges the solve dropped for being concave, so the panel can point at "Round
    // all edges" rather than reporting a bare "too flat".
    size_t                m_bevel_dropped_concave{0};
    bool                  m_show_bevel_status{false};

    // --- the bevel worker ---
    // Set when a BevelJob is queued, cleared by on_bevel_done(). The panel reads
    // it to disable Apply (one bevel at a time) and to show Cancel.
    bool  m_bevel_job_running{false};
    // What a CurvedSurface refusal reported, for the panel's message.
    size_t m_bevel_curved_facets{0};
    float  m_bevel_curved_spread{0.f};

    // --- render models ---
    // Cached highlight geometry. The key is what the model was built from - the
    // seed facet for a region, the seed edge for a chain - so a hover that stays
    // inside the same region rebuilds nothing.
    GLModel m_hover_region_model;
    int     m_hover_region_key{-1};
    GLModel m_selected_region_model;
    int     m_selected_region_key{-1};
    GLModel m_hover_chain_model;
    int     m_hover_chain_key{-1};
    GLModel m_selected_chain_model;
    int     m_selected_chain_key{-1};

    Vec2d m_last_mouse{Vec2d::Zero()};

    std::map<std::string, wxString> m_desc;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoEdit_hpp_
