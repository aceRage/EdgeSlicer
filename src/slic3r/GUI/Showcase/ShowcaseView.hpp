#ifndef slic3r_GUI_ShowcaseView_hpp_
#define slic3r_GUI_ShowcaseView_hpp_

// Showcase tab (phases S1, S2, design tests/design_showcase_tab.md): the plater's third page, after
// Preview (D1). A canvas of its own (GLCanvas3D::CanvasShowcase, hosted the way AssembleView is)
// next to a side panel with the scene: backdrop, floor, lights and environment, filament materials
// and layer lines, camera and saved views, filament callouts (S3), still export and the user's
// watermark. The scene belongs to the current plate (D17) and is saved in the 3MF
// (Model::showcase). Nothing here edits the model.

#include "ShowcaseCallouts.hpp"
#include "ShowcaseWatermark.hpp"

#include "libslic3r/Showcase/ShowcaseScene.hpp"

#include "../Camera.hpp"

#include <memory>
#include <string>
#include <vector>

#include <wx/panel.h>

class wxGLCanvas;
class wxBoxSizer;
class wxButton;
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
    // Drops anchors on objects that are gone and asks for anchors (and the callout list worked out
    // from the model again) on the next frame. `regenerate`: also re-anchor every callout not placed
    // by hand, from the current view.
    void                     callouts_sync(bool regenerate);
    // GL work at the start of a live frame: anchors, re-anchoring a dropped dot, what is hidden.
    void                     callouts_before_frame();
    void                     callouts_find_anchors(Showcase::Scene &scene, bool regenerate);
    void                     callouts_update_occlusion(const Showcase::Scene &scene);
    // The callouts as drawn now: label text and where the anchors land on the safe frame.
    std::vector<ShowcaseCalloutDraw> callouts_draw_list(const ShowcaseLabelContext &labels);
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
    std::vector<wxCheckBox *> m_co_fields;   // brand, product, colour name, hex, type, weight, price
    wxChoice                *m_co_list = nullptr;
    wxButton                *m_co_edit = nullptr;
    wxButton                *m_co_hide = nullptr;
    wxButton                *m_co_regenerate = nullptr;
    wxButton                *m_co_unpin = nullptr;
    wxStaticText            *m_co_hint = nullptr;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_ShowcaseView_hpp_
