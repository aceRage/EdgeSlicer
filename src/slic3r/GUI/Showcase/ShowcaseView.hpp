#ifndef slic3r_GUI_ShowcaseView_hpp_
#define slic3r_GUI_ShowcaseView_hpp_

// Showcase tab (phases S1 to S4, design tests/design_showcase_tab.md): the plater's third page, after
// Preview (D1). A canvas of its own (GLCanvas3D::CanvasShowcase, hosted the way AssembleView is)
// next to a side panel with the scene: backdrop, floor, lights and environment, filament materials
// and layer lines, camera and saved views, filament callouts (S3), the video (S4, its half in
// ShowcaseViewVideo.cpp), still export and the user's watermark. The scene belongs to the current
// plate (D17) and is saved in the 3MF (Model::showcase). Nothing here edits the model.

#include "ShowcaseCallouts.hpp"
#include "ShowcaseWatermark.hpp"

#include "libslic3r/Showcase/ShowcasePath.hpp"
#include "libslic3r/Showcase/ShowcaseRenderCli.hpp"
#include "libslic3r/Showcase/ShowcaseScene.hpp"

#include "../Camera.hpp"

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <wx/panel.h>
#include <wx/timer.h>

namespace Slic3r { namespace Showcase { class EncoderWorker; class VideoJob; struct VideoFrame; } }

class wxGLCanvas;
class wxBoxSizer;
class wxButton;
class wxGauge;
class wxSpinCtrlDouble;
class wxStaticBitmap;
class wxCheckBox;
class wxChoice;
class wxColourPickerCtrl;
class wxFlexGridSizer;
class wxImage;
class wxFontPickerCtrl;
class wxScrolledWindow;
class wxSlider;
class wxSpinCtrl;
class wxStaticText;
class wxTextCtrl;
class wxWindow;

namespace Slic3r {

class BackgroundSlicingProcess;
class DynamicPrintConfig;
class Model;

namespace GUI {

class Bed3D;
class GLCanvas3D;
class ShowcaseRenderer;
class ShowcaseVideoProgressDialog;

class ShowcaseView : public wxPanel
{
public:
    ShowcaseView(wxWindow *parent, Bed3D &bed, Model *model, DynamicPrintConfig *config, BackgroundSlicingProcess *process);
    ~ShowcaseView() override;

    wxGLCanvas *get_wxglcanvas() { return m_canvas_widget; }
    GLCanvas3D *get_canvas3d() { return m_canvas; }

    void set_as_dirty();
    bool is_reload_delayed() const;
    void reload_scene(bool refresh_immediately, bool force_full_scene_refresh = false);

    // The tab was entered / left: the Showcase camera replaces the editing camera and back.
    void on_activate();
    void on_deactivate();
    bool is_active() const { return m_active; }
    // The project's scenes were replaced (opened, new, a plate deleted).
    void on_project_changed();
    void on_change_color_mode(bool is_dark);

    // S5, --showcase-render: renders the request's video or still of the current plate (the hidden
    // instance has loaded the project and shown this view). Fills `report`; returns the exit code.
    int run_render_job(const Showcase::RenderRequest &request, Showcase::RenderReport &report);

private:
    Showcase::Project       &project();
    int                      current_plate() const;
    // The current plate's scene: the project's, or a draft until the first change is made.
    Showcase::Scene         &scene();
    bool                     scene_in_project();
    bool                     read_only();
    // Makes the draft the project's scene for this plate (first change, a saved view, an export).
    void                     commit();
    // Applies `change` to the scene (committing it first) and refreshes the view.
    template<class F> void   edit(F &&change, bool resync = false);
    void                     after_change(bool resync);
    void                     mark_project_dirty();

    void                     apply_scene_camera();
    void                     store_scene_camera(bool mark_dirty);
    void                     on_idle_check();
    void                     on_plate_switched();

    void                     build_panel();
    // Sizes the side panel to its widest row (at least the default width, at least what the user
    // dragged the sash to), leaving the canvas some room; the panel scrolls sideways past that.
    void                     fit_side_width();
    void                     sync_controls();
    void                     update_enabled();
    void                     refresh_plates();
    void                     refresh_views();
    void                     refresh_watermark_presets();
    // S2: one row per filament slot, rebuilt when the project's filaments change; their choices
    // set from the scene on a rebuild or when `sync_selection`.
    void                     refresh_materials(bool sync_selection);

    void                     on_choose_background_image();
    void                     on_choose_floor_image();
    // D10: the render (centre-cropped, 240 px) as the project's cover picture.
    void                     write_project_cover(const wxImage &image);
    void                     on_choose_logo();
    void                     on_save_view();
    void                     on_delete_view();
    void                     on_save_watermark_preset();
    void                     on_delete_watermark_preset();
    void                     on_apply_watermark_preset();
    void                     on_export(bool to_clipboard);
    bool                     add_image_file(const std::string &path, int max_side, bool always_png, std::string &sha);

    // --- S3: filament callouts -------------------------------------------------------------------
    // The plate's objects (model indices) and its layer changes.
    std::vector<int>         plate_objects() const;
    const CustomGCode::Info *plate_changes() const;
    // S4: anchors something stands in front of, from an ID pass of the current camera.
    std::map<std::string, std::vector<bool>> callouts_compute_occlusion(const Showcase::Scene &scene);
    // S4: the model's box on the safe frame, 0..1 (empty when unknown).
    Showcase::RectD          callouts_model_box() const;
    // Drops anchors on objects that are gone and asks for anchors (and the callout list worked out
    // from the model again) on the next frame. `regenerate`: also re-anchor every callout not placed
    // by hand, from the current view.
    void                     callouts_sync(bool regenerate);
    // GL work at the start of a live frame: anchors, re-anchoring a dropped dot, what is hidden.
    void                     callouts_before_frame();
    void                     callouts_find_anchors(Showcase::Scene &scene, bool regenerate);
    void                     callouts_update_occlusion(const Showcase::Scene &scene);
    // The callouts as drawn now: label text and where the anchors land on the safe frame. S4: a video
    // frame passes its own occlusion and the fade state.
    // S5: `frame_scene` and `visuals` - a moment of a path's timeline (callouts shown, hidden, highlighted).
    std::vector<ShowcaseCalloutDraw> callouts_draw_list(const ShowcaseLabelContext &labels,
                                                        const std::map<std::string, std::vector<bool>> *occluded = nullptr,
                                                        const Showcase::CalloutFade *fade = nullptr,
                                                        const Showcase::Scene *frame_scene = nullptr,
                                                        const std::map<std::string, Showcase::CalloutVisual> *visuals = nullptr);
    double                   callouts_model_center_x() const;
    bool                     callouts_rasterize(int frame_w, int frame_h, ShowcaseImage &image);
    // Draws the callouts onto an exported image (before the watermark).
    void                     callouts_apply_to_export(ShowcaseImage &image, bool has_alpha);
    bool                     callouts_on_mouse(wxMouseEvent &evt);
    void                     callouts_edit(int item);
    void                     callouts_context_menu(int item);
    void                     callouts_select(int item);
    void                     refresh_callout_list();
    // Something the labels show changed: draw them again.
    void                     callouts_changed() { ++m_callouts.version; set_as_dirty(); }

    struct CalloutUi
    {
        bool     pending    = false;   // anchors to find on the next frame
        bool     regenerate = false;
        struct Pick
        {
            std::string key;
            int         anchor = 0;
            double      nx = 0., ny = 0.;   // 0..1 of the frame
        };
        std::vector<Pick> picks;           // dots the user dropped, picked on the next frame
        uint64_t version = 1;              // bumped whenever the labels may look different
        // S5b: each callout's largest visible region in the last ID pass (pixels), for "each colour once".
        std::map<std::string, int> areas;
        uint64_t occlusion_camera = 0;     // the camera the occlusion was worked out for
        std::map<std::string, std::vector<bool>> occluded;
        ShowcaseCalloutHits hits;          // the last live drawing, frame pixels
        int      selected = -1;
        enum class Drag { None, Label, Dot } drag = Drag::None;
        int      drag_item   = -1;
        int      drag_anchor = 0;
        double   grab_dx = 0., grab_dy = 0.;   // label drag: the pointer inside the label
        double   press_x = 0., press_y = 0.;
        double   drag_x  = 0., drag_y  = 0.;   // dot drag: where the dot is now, frame pixels
        bool     moved = false;
        int      cursor = 0;               // 0 arrow, 1 move, 2 hand
        // What the filaments look like, kept while nothing changes (the camera moving included).
        std::shared_ptr<ShowcaseLabelContext> labels;
        uint64_t labels_version = 0;
        bool     labels_usage   = false;
    };
    CalloutUi                m_callouts;
    const ShowcaseLabelContext &callouts_labels();

    // --- S4: the user's saved callout styles ------------------------------------------------------
    Showcase::CalloutStyleLibrary m_callout_styles;
    static std::string       callout_styles_path();   // <data dir>/showcase/callout_styles.json
    void                     load_callout_styles();
    bool                     save_callout_styles();
    void                     refresh_callout_styles();
    void                     on_save_callout_style();
    void                     on_delete_callout_style();

    // --- S4: the videographer (ShowcaseViewVideo.cpp) -----------------------------------------------
    void                     build_video_section(wxBoxSizer *root);
    // S5c: the camera of a video at t: the path, a static zoom-out framed on the build-up.
    Showcase::CameraPose     video_pose(const Showcase::VideoSettings &v, const Showcase::CameraView &base, double t,
                                        const std::vector<Showcase::PathEvent> &events);
    // S5c: before a preview or an export: the callouts' first heights and, for static callouts, the
    // anchors of the frame they keep.
    void                     video_prepare_callouts(const Showcase::VideoSettings &v, const Showcase::CameraView &base,
                                                    const std::vector<Showcase::PathEvent> &events);
    void                     video_set_callout_pass(const Showcase::VideoSettings &v, double build, Showcase::BuildSource source);
    void                     apply_section_header_colours();
    // S5b: the side panel's collapsible sections. Adds a section's clickable header to `root` and
    // returns the sizer its rows go in; whether it is folded is kept in the app config.
    wxBoxSizer              *add_panel_section(wxBoxSizer *root, const wxString &title, const std::string &key);
    void                     toggle_panel_section(size_t index);
    // Hides the rows of the folded sections again (after rows were shown or rebuilt).
    void                     apply_panel_sections();
    void                     sync_video_controls();
    void                     update_video_enabled();
    void                     refresh_keyframes();
    // The live camera as a scene camera.
    Showcase::CameraView     live_camera_view() const;
    // The video's size: the resolution at the safe frame's aspect.
    void                     video_size(int &w, int &h);
    // Shows the path's pose at `t` seconds without keeping it: the camera (and, for a turntable, the
    // lights) are borrowed until video_restore_view().
    void                     video_show_pose(double t);
    void                     video_restore_view();
    bool                     video_camera_borrowed() const { return m_video.borrowed; }
    void                     video_toggle_preview();
    void                     video_stop_preview();
    void                     on_video_timer(wxTimerEvent &evt);
    // The export: asks where to, picks the encoder, then renders a frame per timer tick.
    void                     on_export_video();
    bool                     video_start_export(Showcase::VideoFormat format, const std::string &path, std::string &error);
    void                     video_tick();
    bool                     video_render_frame(int index, Showcase::VideoFrame &out);
    void                     video_finish(bool cancelled);
    void                     video_cancel();
    bool                     video_busy() const { return m_video.job != nullptr; }

    // --- S5: path files, the timeline, the build-up and the toolpath source (ShowcaseViewPath.cpp) ---
    void                     on_import_path();
    void                     on_export_path();
    void                     refresh_path_info();
    // What the paths' moves need: the camera to start from, the plate's objects and filaments, the
    // saved views; zooms through `frame_height_px` (the live safe frame's height in the tab).
    Showcase::PathContext    path_context(double frame_height_px);
    // The video's events, parsed once per change of their text.
    const std::vector<Showcase::PathEvent> &video_events(const Showcase::VideoSettings &v);
    // The scene's view frame height, noted whenever its camera, views or keyframes are set.
    void                     note_frame_height(Showcase::Scene &s);
    // S5: shows the path at `t` seconds (the scrubber, the drawn timeline).
    void                     video_scrub(double t);
    void                     paint_timeline();
    void                     on_timeline_mouse(wxMouseEvent &evt);
    // A frame of the export at time `t` (callouts, watermark and the timeline included).
    bool                     video_render_frame_at(double t, int index, Showcase::VideoFrame &out);

    struct VideoUi
    {
        // Preview and peek: the camera and the scene the renderer had before.
        bool                     borrowed = false;
        Camera                   saved_camera;
        Showcase::Scene          pose_scene;      // the scene with a turntable's lights turned
        Showcase::CameraView     base;            // what the path starts from
        bool                     previewing = false;
        std::chrono::steady_clock::time_point preview_start;
        double                   preview_offset = 0.;
        int                      peek_ticks = 0;  // >0: restore the view after so many timer ticks
        int                      selected_key = -1;

        // The export.
        std::unique_ptr<Showcase::VideoJob>      job;
        std::unique_ptr<Showcase::EncoderWorker> worker;
        std::unique_ptr<Showcase::VideoFrame>    pending;   // rendered, waiting for room in the queue
        Showcase::VideoSettings  settings;
        Showcase::Scene          frame_scene;
        Showcase::CalloutFade    fade;
        std::map<int, int>       sides;
        std::string              path;
        Showcase::VideoFormat    format = Showcase::VideoFormat::MP4;
        bool                     alpha  = false;
        int                      width  = 0;
        int                      height = 0;
        std::chrono::steady_clock::time_point started;
        ShowcaseVideoProgressDialog *progress = nullptr;

        // S5: the timeline. The export's events; the preview's state of the moment it shows.
        std::vector<Showcase::PathEvent> events;
        std::string              events_text;          // what events_cache was parsed from
        std::vector<Showcase::PathEvent> events_cache;
        bool                     pose_timeline = false;
        std::map<std::string, Showcase::CalloutVisual> pose_visuals;

        // S5c/S5d: the callouts of a clip. Static ones keep their labels where the layout of one frame
        // put them (top-left, 0..1 of the frame, per callout key) while their leaders follow the model;
        // "as used" ones need each callout's first print height.
        bool                     static_valid = false;
        std::map<std::string, Showcase::PointD> static_labels;
        std::map<std::string, double> first_z;
    };
    // S5c: what the callouts of the video frame being drawn follow (the defaults: a still or the live view).
    struct CalloutVideoPass
    {
        double built_z = 1e30;   // the build-up's height; anchors above it are not built yet
        bool   as_used = false;  // a callout waits for its filament's first height
        bool   fixed   = false;  // static callouts: labels at VideoUi::static_labels
        double fade_mm = 1.;     // as used: the height a callout fades in over
    };
    CalloutVideoPass         m_co_video;
    uint64_t                 m_co_video_rev = 0;   // bumped when m_co_video changes
    VideoUi                  m_video;
    wxTimer                  m_video_timer;

    wxGLCanvas              *m_canvas_widget = nullptr;
    GLCanvas3D              *m_canvas        = nullptr;
    std::unique_ptr<ShowcaseRenderer> m_renderer;
    Model                   *m_model = nullptr;

    bool                     m_active  = false;
    bool                     m_syncing = false;
    int                      m_plate   = -1;
    Showcase::Scene          m_draft;
    int                      m_draft_plate = -1;
    Camera                   m_saved_camera;
    bool                     m_has_saved_camera = false;
    WatermarkPresets         m_watermark_presets;

    // The side panel, and the sash on its left edge that makes it wider.
    wxScrolledWindow        *m_side = nullptr;
    wxWindow                *m_sash = nullptr;
    int                      m_side_width = 0;      // dragged to, in px; 0 = fit the contents
    int                      m_sash_start_x = 0;    // drag start, screen px
    int                      m_sash_start_width = 0;
    wxStaticText            *m_notice = nullptr;
    wxChoice                *m_plates = nullptr;
    wxChoice                *m_presets = nullptr;
    wxCheckBox              *m_hide_plate = nullptr;
    wxCheckBox              *m_hide_wipe_tower = nullptr;
    wxChoice                *m_bg_type = nullptr;
    wxColourPickerCtrl      *m_bg_color1 = nullptr;
    wxColourPickerCtrl      *m_bg_color2 = nullptr;
    wxButton                *m_bg_image = nullptr;
    wxChoice                *m_bg_fit = nullptr;
    wxSlider                *m_bg_blur = nullptr;
    wxChoice                *m_floor_type = nullptr;
    wxColourPickerCtrl      *m_floor_color = nullptr;
    wxSlider                *m_floor_shadow = nullptr;
    wxSpinCtrl              *m_floor_grid = nullptr;
    wxChoice                *m_floor_texture = nullptr;
    wxButton                *m_floor_image = nullptr;
    wxSpinCtrl              *m_floor_tile = nullptr;
    wxSlider                *m_floor_reflection = nullptr;
    wxSlider                *m_floor_contact = nullptr;
    wxCheckBox              *m_light_fixed = nullptr;
    wxSlider                *m_light_azimuth = nullptr;
    wxSlider                *m_light_elevation = nullptr;
    wxCheckBox              *m_shadows = nullptr;
    wxSlider                *m_shadow_darkness = nullptr;
    wxCheckBox              *m_ssao = nullptr;
    wxChoice                *m_light_rig = nullptr;
    wxSlider                *m_key_intensity = nullptr;
    wxSlider                *m_key_temperature = nullptr;
    wxCheckBox              *m_fill = nullptr;
    wxSlider                *m_fill_intensity = nullptr;
    wxCheckBox              *m_rim = nullptr;
    wxSlider                *m_rim_intensity = nullptr;
    wxChoice                *m_environment = nullptr;
    wxSlider                *m_env_intensity = nullptr;
    wxSlider                *m_env_rotation = nullptr;
    wxSlider                *m_exposure = nullptr;
    wxSlider                *m_shadow_softness = nullptr;
    wxChoice                *m_layer_lines = nullptr;
    wxFlexGridSizer         *m_mat_grid = nullptr;
    std::vector<wxChoice *>  m_mat_choices;
    std::string              m_mat_signature;
    wxChoice                *m_projection = nullptr;
    wxChoice                *m_views = nullptr;
    wxButton                *m_view_delete = nullptr;
    wxChoice                *m_size = nullptr;
    wxSpinCtrl              *m_width = nullptr;
    wxSpinCtrl              *m_height = nullptr;
    wxChoice                *m_quality = nullptr;
    wxChoice                *m_format = nullptr;
    wxCheckBox              *m_use_as_cover = nullptr;
    wxButton                *m_export = nullptr;
    wxButton                *m_copy = nullptr;
    wxStaticText            *m_status = nullptr;
    wxCheckBox              *m_wm_enabled = nullptr;
    wxChoice                *m_wm_presets = nullptr;
    wxButton                *m_wm_preset_delete = nullptr;
    wxTextCtrl              *m_wm_text = nullptr;
    wxFontPickerCtrl        *m_wm_font = nullptr;
    wxColourPickerCtrl      *m_wm_color = nullptr;
    wxButton                *m_wm_logo = nullptr;
    wxButton                *m_wm_logo_remove = nullptr;
    wxChoice                *m_wm_anchor = nullptr;
    wxSlider                *m_wm_size = nullptr;
    wxSlider                *m_wm_opacity = nullptr;
    wxSlider                *m_wm_margin = nullptr;
    // S3
    wxCheckBox              *m_co_enabled = nullptr;
    wxChoice                *m_co_layout = nullptr;
    wxChoice                *m_co_style = nullptr;
    wxFontPickerCtrl        *m_co_font = nullptr;
    wxSlider                *m_co_size = nullptr;
    wxChoice                *m_co_leader = nullptr;
    wxCheckBox              *m_co_auto_colors = nullptr;
    wxColourPickerCtrl      *m_co_text = nullptr;
    wxColourPickerCtrl      *m_co_line = nullptr;
    wxColourPickerCtrl      *m_co_bg = nullptr;
    wxSlider                *m_co_bg_opacity = nullptr;
    std::vector<wxCheckBox *> m_co_fields;   // brand, product, family, colour name, hex, swatch, type, weight, price
    wxTextCtrl              *m_co_separator = nullptr;   // S5b
    wxCheckBox              *m_co_once = nullptr;        // S5b: each colour once
    struct PanelSection
    {
        std::string   key;
        wxBoxSizer   *body      = nullptr;
        wxPanel      *head      = nullptr;   // S5c: the header row, on the theme's button colour
        wxStaticText *arrow     = nullptr;
        wxStaticText *title     = nullptr;
        bool          collapsed = false;
    };
    int                      m_side_content_w = 0;      // S5c: the widest the panel's rows have been (px)
    std::vector<PanelSection> m_sections;               // S5b
    wxBoxSizer              *m_side_root = nullptr;     // S5b: the panel's sizer
    wxChoice                *m_co_list = nullptr;
    wxButton                *m_co_edit = nullptr;
    wxButton                *m_co_hide = nullptr;
    wxButton                *m_co_regenerate = nullptr;
    wxButton                *m_co_unpin = nullptr;
    wxStaticText            *m_co_hint = nullptr;
    // S4
    wxButton                *m_co_style_save = nullptr;
    wxButton                *m_co_style_delete = nullptr;
    wxChoice                *m_vid_path = nullptr;
    wxSpinCtrlDouble        *m_vid_duration = nullptr;
    wxChoice                *m_vid_fps = nullptr;
    wxChoice                *m_vid_resolution = nullptr;
    wxChoice                *m_vid_quality = nullptr;
    wxSlider                *m_vid_bitrate = nullptr;
    wxChoice                *m_vid_format = nullptr;
    wxChoice                *m_vid_easing = nullptr;
    wxSpinCtrl              *m_vid_sweep = nullptr;
    wxCheckBox              *m_vid_clockwise = nullptr;
    wxSpinCtrl              *m_vid_bob = nullptr;
    wxSpinCtrl              *m_vid_bob_cycles = nullptr;
    wxSpinCtrlDouble        *m_vid_hold_start = nullptr;
    wxSpinCtrlDouble        *m_vid_hold_end = nullptr;
    wxCheckBox              *m_vid_loop = nullptr;
    wxCheckBox              *m_vid_callouts = nullptr;
    wxChoice                *m_vid_keys = nullptr;
    wxSpinCtrlDouble        *m_vid_key_time = nullptr;
    wxChoice                *m_vid_key_ease = nullptr;
    wxButton                *m_vid_key_add = nullptr;
    wxButton                *m_vid_key_update = nullptr;
    wxButton                *m_vid_key_delete = nullptr;
    wxButton                *m_vid_key_goto = nullptr;
    wxSlider                *m_vid_time = nullptr;
    wxStaticText            *m_vid_time_label = nullptr;
    wxButton                *m_vid_preview = nullptr;
    wxButton                *m_vid_export = nullptr;
    wxStaticText            *m_vid_info = nullptr;
    std::vector<wxWindow *>  m_vid_orbit_rows;     // shown for orbit paths
    std::vector<wxWindow *>  m_vid_bob_rows;       // shown for the bob
    std::vector<wxWindow *>  m_vid_key_rows;       // shown for keyframes
    // S5
    wxCheckBox              *m_vid_build = nullptr;
    wxChoice                *m_vid_co_motion = nullptr;   // S5c
    wxChoice                *m_vid_co_appear = nullptr;   // S5c
    wxCheckBox              *m_vid_build_infill = nullptr;     // S5b
    wxCheckBox              *m_vid_build_supports = nullptr;   // S5b
    wxButton                *m_vid_path_import = nullptr;
    wxButton                *m_vid_path_export = nullptr;
    wxButton                *m_vid_path_clear = nullptr;
    wxStaticText            *m_vid_path_info = nullptr;
    wxWindow                *m_vid_timeline = nullptr;   // the drawn timeline: keyframes, events, the moment shown
    wxChoice                *m_source = nullptr;
    wxCheckBox              *m_smooth_auto = nullptr;
    wxSlider                *m_smooth = nullptr;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_ShowcaseView_hpp_
