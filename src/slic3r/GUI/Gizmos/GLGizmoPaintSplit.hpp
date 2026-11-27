#ifndef slic3r_GLGizmoPaintSplit_hpp_
#define slic3r_GLGizmoPaintSplit_hpp_

// Paint Split (tests/design_paint_split.md, phase 2): select a region of a part by painting or by picking
// a painted colour or segment, and split it off with a cut. The selection (the "mask") belongs to the
// gizmo, not to the volume: it is never written to the model, it is archived in the undo snapshots
// through on_save / on_load (design 3.6), and closing the gizmo with a selection asks first (D13).

#include "GLGizmoPainterBase.hpp"

#include "libslic3r/ObjectID.hpp"
#include "libslic3r/PaintSplit.hpp"
#include "libslic3r/PaintSplitPreview.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "slic3r/GUI/GLModel.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r::GUI {

class GLGizmoPaintSplit : public GLGizmoPainterBase
{
public:
    GLGizmoPaintSplit(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id);

    void render_painter_gizmo() override;
    bool gizmo_event(SLAGizmoEventType action, const Vec2d &mouse_position, bool shift_down, bool alt_down, bool control_down) override;
    // Ctrl+click in the pick modes adds a patch (the painter base keeps Ctrl clicks for the camera).
    bool on_mouse(const wxMouseEvent &mouse_event) override;

    // Whether any part has selected surface.
    bool has_selection() const;
    // D13: a close the USER asked for (toolbar button, Esc). With a selection this asks "Discard?" and runs
    // `close` on Yes, returning true (the caller does nothing now); without one it returns false.
    bool defer_close(std::function<void()> close);

protected:
    void on_set_state() override;
    void on_render_input_window(float x, float y, float bottom_limit) override;
    std::string on_get_name() const override;
    PainterGizmoType get_painter_type() const override { return PainterGizmoType::PAINT_SPLIT; }
    wxString handle_snapshot_action_name(bool shift_down, Button button_down) const override;

    std::string get_gizmo_entering_text() const override { return _u8L("Entering Paint Split"); }
    std::string get_gizmo_leaving_text() const override { return _u8L("Leaving Paint Split"); }
    std::string get_action_snapshot_name() const override { return _u8L("Paint Split selection"); }

    // The right button does nothing here (Shift + left erases), so it stays free for the camera.
    EnforcerBlockerType get_right_button_state_type() const override { return EnforcerBlockerType(-1); }

    void on_save(cereal::BinaryOutputArchive &ar) const override;
    void on_load(cereal::BinaryInputArchive &ar) override;
    bool paint_differs_from_model() const override;

private:
    bool on_init() override;
    void update_model_object() override;
    void update_from_model_object(bool first_update) override;
    void on_opening() override;
    void on_shutdown() override;

    enum class Mode : int { Brush, Bucket, SmartFill, PickColour, PickSegment };
    enum class Style : int { Auto, Plane };

    bool pick_mode() const { return m_mode == Mode::PickColour || m_mode == Mode::PickSegment; }
    bool raycast(const Vec2d &mouse_position, int &mesh_id, size_t &facet, Vec3f &hit) const;
    // `clear`: Shift+click empties the part. `add`: Ctrl+click adds the patch, or takes it out when the click
    // lands on the selection (owner 2026-11-25).
    void pick(int mesh_id, size_t facet, const Vec3f &hit, bool clear, bool add);
    void clear_selection();
    void apply();


    // The preview: the selection's region (rebuilt when the mask or the hole fill changes), the fitted planes,
    // their cut faces and the tint (rebuilt when the cut style changes too).
    void refresh_preview();
    void rebuild_preview();
    void render_preview();
    PaintSplitSkinParams skin_params() const;
    // R2 (E5/E6): the region and Show cut run on m_worker. request_preview() starts it for the current inputs (a new
    // generation; the one under way is cancelled); on_preview_results() adopts what it stored for that generation.
    void request_preview(bool region_too);
    void start_preview();
    void on_preview_results();
    void cancel_show_cut();
    void compute_cut_preview(); // Show cut: turns the cut preview on and requests it
    const PaintSplitRegion &region() const;
    bool cuts_ok(size_t comp) const; // the component can be split
    int  masked_part(int *count = nullptr) const;
    Transform3d part_to_cut_frame(int part) const;

    // The mask as of the last undo snapshot, per model part (what on_save archives).
    std::vector<TriangleSelector::TriangleSplittingData> m_committed;
    size_t                                               m_committed_object{ 0 };
    // What on_load read, waiting for update_from_model_object().
    std::vector<TriangleSelector::TriangleSplittingData> m_loaded;
    size_t                                               m_loaded_object{ 0 };
    bool                                                 m_loaded_valid{ false };
    // The picked filament per part (D6), 1-based; 0 = none picked.
    std::vector<int>                                     m_picked_filament;

    // The model's paint, shown under the mask (one per model part, like m_triangle_selectors).
    std::vector<std::unique_ptr<TriangleSelectorGUI>>    m_paint_views;

    Mode  m_mode{ Mode::Brush };
    Style m_style{ Style::Auto };
    float m_fill_holes_pct{ 10.f };
    // Follow paint (the cut style since the owner's hand test 2026-11-26).
    float m_depth{ 0.f };      // mm under the selected skin; 0 = unlimited
    float m_lip_angle{ 0.f };  // unused since the nearest-skin cut (kept in the undo archive's layout)
    float m_smoothing{ 0.5f }; // "Smooth cut face" (D22, R1): 0..1, the seam stays pinned
    float m_skin_smoothing{ 0.5f }; // R2, Advanced "Smooth along the skin": 0..1 (0 = R1)
    bool  m_advanced_open{ false };
    bool  m_snap_to_bed{ false }; // the flat core level
    bool  m_keep_as_parts{ false };
    bool  m_cut_only_painted_part{ false };

    bool  m_close_confirmed{ false };
    bool  m_close_prompt_pending{ false };
    bool  m_was_on{ false };

    // Preview state.
    bool                         m_preview_dirty{ true }; // planes, caps, tint
    bool                         m_region_dirty{ true };  // the region itself
    Transform3d                  m_preview_trafo{ Transform3d::Identity() };
    int                          m_preview_part{ -1 };
    int                          m_masked_parts{ 0 };
    std::shared_ptr<const PaintSplitRegion> m_region;   // of m_preview_part, in the cut frame (null until the first one)
    std::shared_ptr<const PaintSplitPart>   m_part;     // that part in the cut frame, prepared
    std::vector<indexed_triangle_set> m_claim_cut;      // Show cut: per component cut, the cut surface inside the part
    std::vector<int>             m_claim_comps;         // ... and which components those are
    std::vector<std::shared_ptr<const PaintSplitClaim>> m_claims; // the fine claims (for Apply), per m_claim_comps entry
    bool                         m_claims_coarse{ false };
    std::vector<std::string>     m_show_log;            // Show cut's time lines, for the Output Log
    bool                         m_output_log_open{ false };
    // R2: the worker and its generations. m_preview_gen is the inputs' newest generation; a result of another one is stale.
    std::unique_ptr<PaintSplitPreviewWorker> m_worker;
    std::shared_ptr<int>         m_alive{ std::make_shared<int>(0) };
    uint64_t                     m_preview_gen{ 0 };
    bool                         m_region_pending{ false }; // the region of m_preview_gen is not in yet
    bool                         m_show_cut{ false };       // Show cut is on: the cut follows every change
    bool                         m_cut_pending{ false };    // ... and its fine cut is not in yet
    bool                         m_restart_pending{ false }; // a request waits for the cancelled computation to end
    bool                         m_restart_region{ false };  // ... and needs the region computed again
    GLModel                      m_plane_fill;          // the cut surfaces (band, skirt, core)
    GLModel                      m_plane_outline;       // the paint lines they follow
    Vec3d                        m_world_offset{ Vec3d::Zero() };

    std::map<std::string, wxString> m_desc;
};

} // namespace Slic3r::GUI

#endif // slic3r_GLGizmoPaintSplit_hpp_
