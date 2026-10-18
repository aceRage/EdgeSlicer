#ifndef slic3r_GUI_ShowcaseView_hpp_
#define slic3r_GUI_ShowcaseView_hpp_

// Showcase tab (phase S1, design tests/design_showcase_tab.md): the plater's third page, after
// Preview (D1). A canvas of its own (GLCanvas3D::CanvasShowcase, hosted the way AssembleView is)
// next to a side panel with the scene: backdrop, floor, light, camera and saved views, still export
// and the user's watermark. The scene belongs to the current plate (D17) and is saved in the 3MF
// (Model::showcase). Nothing here edits the model.

#include "ShowcaseWatermark.hpp"

#include "libslic3r/Showcase/ShowcaseScene.hpp"

#include "../Camera.hpp"

#include <memory>

#include <wx/panel.h>

class wxGLCanvas;
class wxBoxSizer;
class wxButton;
class wxCheckBox;
class wxChoice;
class wxColourPickerCtrl;
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
    void                     sync_controls();
    void                     update_enabled();
    void                     refresh_plates();
    void                     refresh_views();
    void                     refresh_watermark_presets();

    void                     on_choose_background_image();
    void                     on_choose_logo();
    void                     on_save_view();
    void                     on_delete_view();
    void                     on_save_watermark_preset();
    void                     on_delete_watermark_preset();
    void                     on_apply_watermark_preset();
    void                     on_export(bool to_clipboard);
    bool                     add_image_file(const std::string &path, int max_side, bool always_png, std::string &sha);

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

    // The side panel.
    wxScrolledWindow        *m_side = nullptr;
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
    wxChoice                *m_floor_type = nullptr;
    wxColourPickerCtrl      *m_floor_color = nullptr;
    wxSlider                *m_floor_shadow = nullptr;
    wxSpinCtrl              *m_floor_grid = nullptr;
    wxCheckBox              *m_light_fixed = nullptr;
    wxSlider                *m_light_azimuth = nullptr;
    wxSlider                *m_light_elevation = nullptr;
    wxCheckBox              *m_shadows = nullptr;
    wxSlider                *m_shadow_darkness = nullptr;
    wxCheckBox              *m_ssao = nullptr;
    wxChoice                *m_projection = nullptr;
    wxChoice                *m_views = nullptr;
    wxButton                *m_view_delete = nullptr;
    wxChoice                *m_size = nullptr;
    wxSpinCtrl              *m_width = nullptr;
    wxSpinCtrl              *m_height = nullptr;
    wxChoice                *m_quality = nullptr;
    wxChoice                *m_format = nullptr;
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
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_ShowcaseView_hpp_
