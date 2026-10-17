#ifndef slic3r_GLGizmoFdmSupports_hpp_
#define slic3r_GLGizmoFdmSupports_hpp_

#include "GLGizmoPainterBase.hpp"
//BBS
#include "libslic3r/Print.hpp"
#include "libslic3r/ObjectID.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "libslic3r/Support/SupportOverhangHighlight.hpp"

#include <boost/thread.hpp>

#include <atomic>
#include <memory>
#include <thread>

namespace Slic3r::GUI {

class GLGizmoFdmSupports : public GLGizmoPainterBase
{
public:
    GLGizmoFdmSupports(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);
    ~GLGizmoFdmSupports() override;

    void render_painter_gizmo() override;

    //BBS: add edit state
    enum EditState {
        state_idle = 0,
        state_generating = 1,
        state_ready
    };

    //BBS
    bool on_key_down_select_tool_type(int keyCode);

protected:
    void on_render_input_window(float x, float y, float bottom_limit) override;
    std::string on_get_name() const override;

    void on_set_state() override;
    void show_tooltip_information(float caption_max, float x, float y);
    wxString handle_snapshot_action_name(bool shift_down, Button button_down) const override;

    std::string get_gizmo_entering_text() const override { return "Entering Paint-on supports"; }
    std::string get_gizmo_leaving_text() const override { return "Leaving Paint-on supports"; }
    std::string get_action_snapshot_name() const override { return "Paint-on supports editing"; }

    // Painting stabilizer points (EnforcerBlockerType::STABILIZER) instead of enforcers and blockers:
    // left paints them, right erases.
    EnforcerBlockerType get_left_button_state_type() const override
    {
        return m_paint_stabilizers ? EnforcerBlockerType::STABILIZER : EnforcerBlockerType::ENFORCER;
    }
    EnforcerBlockerType get_right_button_state_type() const override
    {
        return m_paint_stabilizers ? EnforcerBlockerType::NONE : EnforcerBlockerType::BLOCKER;
    }
    ColorRGBA get_cursor_sphere_left_button_color() const override
    {
        return m_paint_stabilizers ? ColorRGBA{ 0.62f, 0.36f, 0.90f, 0.25f } : GLGizmoPainterBase::get_cursor_sphere_left_button_color();
    }

    // BBS
    wchar_t                           m_current_tool = 0;
    bool                              m_paint_stabilizers = false;

private:
    bool on_init() override;

    //BBS: remove const
    void update_model_object() override;
    //BBS: add logic to distinguish the first_time_update and later_update
    void update_from_model_object(bool first_update) override;
    void tool_changed(wchar_t old_tool, wchar_t new_tool);

    void on_opening() override;
    void on_shutdown() override;
    PainterGizmoType get_painter_type() const override;

    void select_facets_by_angle(float threshold, bool block);
    // "Threshold Angle" / "Threshold Overlap": the support settings the highlight uses. With
    // "Simulate Auto Supports" they mirror the effective settings of the selected object (its own
    // override, else the process) and follow them; off, they are free preview values. The face
    // tint angle is the automatic supports' layer rule for those values
    // (support_overhang_rule_for(), Support/SupportOverhangHighlight.hpp): the threshold angle
    // (+1, inclusive, as the generators), or the overlap at this layer height and outer wall width,
    // or the tree 30 degree fallback.
    struct ThresholdSource
    {
        int    angle { 0 };
        double overlap_pct { 50. };   // percent of the outer wall width
        bool   angle_from_object { false };
        bool   overlap_from_object { false };
        bool   overlap_absolute { false }; // the setting is in mm; shown converted to percent
    };
    ThresholdSource effective_thresholds(const ModelObject &mo) const;
    SupportOverhangRule current_rule(const ModelObject &mo) const;
    void   update_highlight_angle_from_rule();
    wxString highlight_rule_description() const;
    wxString threshold_source_text(const ModelObject &mo) const;

    bool   m_highlight_reset_pending = true;  // set the sliders from the settings on the next frame
    bool   m_simulate_auto           = true;  // remembered in AppConfig "support_paint_highlight_mode"
    float  m_threshold_angle         = 30.f;  // degrees, as support_threshold_angle
    float  m_threshold_overlap       = 50.f;  // percent, as support_threshold_overlap

    // "By layer (before slicing)": a background job runs the support generator's own overhang
    // detection on a private copy of the object (phase 5: support_overhang_preview_exact(); the
    // support type, the object's overrides, bridges, small-overhang removal, build plate only,
    // painted enforcers / blockers all as the slice will have them) and the result is drawn over
    // the facets (red: the generator holds it up, amber: an overhang it leaves). A layer stepper
    // cuts the object at one layer and draws that layer's areas on the cut. Remembered in AppConfig
    // "support_paint_highlight_by_layer".
    struct LayerPreviewJob
    {
        std::atomic<bool>                       cancel { false };
        std::atomic<bool>                       done   { false };
        std::unique_ptr<Model>                  model;   // the object alone, one instance
        DynamicPrintConfig                      config;  // the full config
        std::unique_ptr<Print>                  print;   // private, so cancel() reaches it
        std::unique_ptr<SupportOverhangPreview> result;
        std::string                             error;
    };
    void   update_layer_preview();
    void   stop_layer_preview_job();
    void   clear_layer_preview();
    void   rebuild_layer_preview_models();
    void   set_layer_preview_layer(int layer);
    void   render_layer_preview() const;
    void   paint_layer_preview_facets(bool block);
    std::string layer_preview_key(const ModelObject &mo, int instance_idx) const;
    bool   layer_preview_settings_changed(const ModelObject &mo);

    bool                                    m_highlight_by_layer = false;
    std::shared_ptr<LayerPreviewJob>        m_layer_preview_job;
    std::thread                             m_layer_preview_thread;
    std::string                             m_layer_preview_key;      // the inputs of the job started last
    std::string                             m_layer_preview_pending_key; // changed inputs waiting for the debounce
    long long                               m_layer_preview_pending_ms = 0;
    unsigned int                            m_layer_preview_settings_rev = 0; // bumped when a setting changes
    DynamicPrintConfig                      m_layer_preview_print_cfg;   // the settings the last job saw
    DynamicPrintConfig                      m_layer_preview_printer_cfg;
    DynamicPrintConfig                      m_layer_preview_project_cfg;
    DynamicPrintConfig                      m_layer_preview_object_cfg;
    std::vector<std::string>                m_layer_preview_filaments;
    std::unique_ptr<SupportOverhangPreview> m_layer_preview;          // the result on screen
    std::vector<std::pair<GLModel, GLModel>> m_layer_preview_models;  // per model part (mesh_id): red, amber
    GLModel                                 m_layer_cut_supported;    // the stepped layer's areas, world
    GLModel                                 m_layer_cut_unsupported;
    int                                     m_layer_preview_layer = 0; // 0: whole object, else 1-based layer
    size_t                                  m_layer_preview_facets = 0;
    bool                                    m_layer_preview_too_big = false;

    //BBS: add support preview logic
    void init_print_instance();
    void update_support_volumes();
    void invalid_support_volumes(bool invalid_step = false);
    bool need_regenerate_support_volumes();
    void generate_support_volume();
    void run_thread();
    float m_angle_threshold_deg = 40.f;
    bool m_volume_valid = false;


    GLVolume *m_support_volume = NULL;
    mutable bool m_volume_ready = false;
    bool m_is_tree_support = false;
    bool m_cancel = false;
    size_t m_object_id;
    std::vector<ObjectBase::Timestamp> m_volume_timestamps;
    PrintInstance m_print_instance;
    mutable EditState m_edit_state;
    //thread
    boost::thread   m_thread;
    // Mutex and condition variable to synchronize m_thread with the UI thread.
    std::mutex      m_mutex;
    int m_generate_count;

    // This map holds all translated description texts, so they can be easily referenced during layout calculations
    // etc. When language changes, GUI is recreated and this class constructed again, so the change takes effect.
    std::map<std::string, wxString> m_desc;
};



} // namespace Slic3r::GUI


#endif // slic3r_GLGizmoFdmSupports_hpp_
