#ifndef slic3r_GLGizmoSplitToFit_hpp_
#define slic3r_GLGizmoSplitToFit_hpp_

// SPLIT TO FIT, phase 1 (tests/design_split_to_build_volume.md; owner decisions D1-D17, 2026-11-15).
//
// Splits the selected object into parts that each fit the current printer (D8), by an axis-aligned
// grid of plain planar cuts. The panel shows the printer's volume, the margins (D2), the axes, the
// layout (D5), the resulting parts with their sizes, and the warnings; the 3D view shows the cut
// planes on the model and outlines in red the parts that would not fit. Apply runs the cuts as a
// background job (SplitToFitJob), names the parts by grid cell (D7), arranges them over plates (D12)
// and is one undo step.
//
// Phase 2 adds automatic dowels (design 3.6): a "Dowels" group in the panel, the sites as discs on
// the planes, the dowel count per part, and the dowel warnings. The planner places them; the job's
// split_apply() bakes them as Cut-tool Dowels (or Pins) and adds the loose dowels as objects.
//
// Phase 3 adds plane adjustment (design 3.5): "Adjust planes automatically" in the panel (on by
// default, D4), a handle on every plane in the 3D view to drag it along its axis (the panel's
// plane list follows), the part checks (very small part, no flat face), and the "Output Log": the
// parts table and the warnings folded into one section, collapsed by default, with the one-line
// result and a warning count left in view. A dragged plane is re-planned (adjustment, dowel sites)
// when the mouse is released, not on every mouse move: the dowel site search builds an AABB tree
// of the whole mesh.
//
// Phase 5 adds loose parts and matching labels: every piece of material a section falls into is a part
// of its own (its own row, size and checks, its own colour in the 3D view when a section has several,
// its own object after Apply), with "Discard pieces smaller than" (off by default); and a "Labels"
// group: the same code recessed into both faces of every mating pair, drawn on the planes before Apply.
//
// Phase 4 (polish): the plan is made on a worker thread (cancelled and made again when the panel
// changes), with "Updating" shown meanwhile and Apply waiting for the current plan; a click on a dowel
// disc removes it and a click on a cut face adds one (checked by the planner's own rules); "Lay parts on
// a cut face" and "Loose parts on their own plates"; the dowels go on plates of their own and identical
// ones are instances of one object.
//
// Entry points (D1): its own toolbar icon (next to Cut's), a "Split to fit..." button in the Cut panel and
// "Split to fit the printer..." in the object's right-click menu.
//
// The planner (libslic3r/SplitToVolume.hpp) does all the work; this class only gathers its inputs,
// draws what comes back and starts the job.

#include "GLGizmoBase.hpp"
#include "slic3r/GUI/GLModel.hpp"

#include "libslic3r/ObjectID.hpp"
#include "libslic3r/SplitToVolume.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Slic3r {
class ModelObject;

namespace GUI {

// The panel's (and the job's) text for a planner warning.
wxString split_warning_text(const SplitWarning &warning, const SplitPlan &plan);

class GLGizmoSplitToFit : public GLGizmoBase
{
public:
    GLGizmoSplitToFit(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id);
    ~GLGizmoSplitToFit() override;

    void data_changed(bool is_serializing) override;
    bool on_mouse(const wxMouseEvent &mouse_event) override;
    std::string get_tooltip() const override;

protected:
    bool               on_init() override;
    std::string        on_get_name() const override;
    void               on_set_state() override;
    bool               on_is_activable() const override;
    CommonGizmosDataID on_get_requirements() const override;
    void               on_render() override;
    void               on_render_input_window(float x, float y, float bottom_limit) override;
    // P3: the plane handles.
    void               on_start_dragging() override;
    void               on_dragging(const UpdateData &data) override;
    void               on_stop_dragging() override;
    void               on_register_raycasters_for_picking() override;
    void               on_unregister_raycasters_for_picking() override;

private:
    // P3: one handle per plane (grabber i is plane i of the plan), rebuilt with every new plan.
    void sync_grabbers();
    void drop_grabbers();
    void update_grabbers();
    // Where plane `i` is drawn: under the mouse while it is dragged, else where the plan put it.
    double shown_pos(size_t i) const;
    // A dragged plane (3D handle or panel list) becomes a pinned one and the plan is made again.
    void commit_drag();
    // The panel's text for the parts either side of plane `i` at `pos`: "152.0 | 148.0 mm".
    std::string drag_lengths(size_t i, double pos, bool *too_long = nullptr) const;
    // P3 Output Log: show a warning's plane, face or part in the 3D view.
    void show_warning(const SplitWarning &warning);

    // The object the gizmo works on (the selected one), and its selected instance.
    ModelObject *selected_object(int *instance = nullptr) const;
    // Re-reads the model's mesh when the object or its placement changed, and the printer's volume
    // when the printer changed; marks the plan dirty when either did.
    void refresh_inputs();
    // P4: the plan is made off the UI thread. request_plan() asks for a plan of the current inputs
    // (cancelling one under way, which then starts the newest when it stops); plan_finished() takes the
    // worker's result on the UI thread; stop_planner() cancels and joins (closing, destruction).
    void request_plan();
    void start_plan();
    void plan_finished(uint64_t generation);
    void stop_planner();
    void adopt_plan(SplitPlan &&plan);
    // The plan shown is the plan of the current inputs (Apply needs that).
    bool plan_current() const { return !m_plan_dirty && !m_plan_running && m_plan_adopted == m_plan_wanted; }
    // P4: click-to-add / remove a dowel site.
    SplitPickTarget pick_at(const Vec2d &mouse) const;
    void            click_site(const SplitPickTarget &target);
    void rebuild_preview();
    void apply();
    void reset_options();
    void load_settings();
    void save_settings() const;

    SplitOptions          m_options;
    SplitPrinterVolume    m_printer;
    SplitPlan             m_plan;
    bool                  m_plan_dirty{ true };

    // The mesh in the grid frame, and what it was taken from. P4: shared with the planner thread (a new
    // mesh is a new object; the one a running plan reads stays alive until it is done).
    std::shared_ptr<const indexed_triangle_set> m_mesh;
    // P5: its face neighbours, worked out once per mesh (the piece search needs them on every plan).
    std::shared_ptr<const std::vector<Vec3i32>> m_mesh_neighbors;
    ObjectID              m_mesh_object;
    Transform3d           m_mesh_trafo{ Transform3d::Identity() };
    size_t                m_mesh_volumes{ 0 };

    GLModel               m_planes_fill;
    GLModel               m_pinned_fill;
    GLModel               m_planes_outline;
    GLModel               m_bad_parts;
    // P2: the dowel sites, drawn on top of the model (discs on the planes, their axes as lines).
    GLModel               m_sites_fill;
    GLModel               m_sites_axes;
    // P4: the dowel sites added by hand (another colour).
    GLModel               m_sites_manual;
    // P5: the pieces of sections that fall apart (one colour each), and the labels' outlines.
    // Six colours and grey (discarded), filled in place: a GLModel must not be copied.
    std::array<GLModel, 7> m_piece_boxes;
    GLModel               m_labels;
    bool                  m_preview_dirty{ true };

    // P3: the plane being dragged (index in m_plan.planes, -1 for none) and where it is now.
    int                   m_drag_plane{ -1 };
    double                m_drag_pos{ 0. };
    double                m_drag_start_pos{ 0. };
    double                m_drag_start_t{ 0. };
    bool                  m_drag_have_t{ false };
    // The adjustment's on / off and limit are kept for the session (no app config key); the first
    // opening uses the defaults (D4: on, 10 mm). P5: so are the label settings and the discard option.
    bool                  m_adjust_seen{ false };
    // The Output Log's fold: collapsed by default, remembered for the session.
    bool                  m_output_log_open{ false };
    // Panel height: the body scrolls, the action row stays put. The last frame's measurements size this
    // frame's body (content height, the parts table's width, the action row's height); the open / closed
    // state of every collapsible section is read from the app config once and kept here.
    float                 m_body_content_h{ 0.f };
    float                 m_table_w{ 0.f };
    float                 m_footer_h{ 0.f };
    std::map<std::string, bool> m_section_open;
    // A warning clicked in the Output Log: its box, outlined in the 3D view.
    BoundingBoxf3         m_highlight;
    GLModel               m_highlight_model;

    // P4: the planner thread. m_plan_wanted counts requests; the thread plans for m_plan_running_gen and
    // leaves its result (under m_plan_mutex) for plan_finished(); m_plan_adopted is the request the shown
    // plan answers. Only the UI thread starts, joins or reads these, except the result and the cancel flag.
    std::thread                        m_plan_thread;
    std::shared_ptr<std::atomic<bool>> m_plan_cancel;
    std::mutex                         m_plan_mutex;
    std::unique_ptr<SplitPlan>         m_plan_result;        // guarded by m_plan_mutex
    uint64_t                           m_plan_result_gen{ 0 }; // guarded by m_plan_mutex
    uint64_t                           m_plan_wanted{ 0 };
    uint64_t                           m_plan_running_gen{ 0 };
    uint64_t                           m_plan_adopted{ 0 };
    bool                               m_plan_running{ false };
    // Expires with the gizmo: a finished plan's callback that arrives later does nothing.
    std::shared_ptr<int>               m_alive{ std::make_shared<int>(0) };

    // P4: a click on a disc or a face, waiting for the button to come up where it went down.
    bool            m_click_pending{ false };
    Vec2d           m_click_start{ Vec2d::Zero() };
    SplitPickTarget m_click_target;
    SplitPickTarget m_hover_pick;
    // Why the last added dowel was refused (shown in the panel), empty when it was not.
    std::string     m_manual_note;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoSplitToFit_hpp_
