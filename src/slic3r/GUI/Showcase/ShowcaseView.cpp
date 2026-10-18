#include "ShowcaseView.hpp"

#include "ShowcaseRenderer.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/Utils.hpp"

#include "../GLCanvas3D.hpp"
#include "../GUI.hpp"
#include "../GUI_App.hpp"
#include "../I18N.hpp"
#include "../OpenGLManager.hpp"
#include "../PartPlate.hpp"
#include "../Plater.hpp"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/clipbrd.h>
#include <wx/clrpicker.h>
#include <wx/dataobj.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/fontpicker.h>
#include <wx/glcanvas.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/spinctrl.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>
#include <wx/utils.h>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace Slic3r {
namespace GUI {

namespace {

struct ShowcaseSizePreset
{
    int         w, h;
    const char *name;   // translated where it is shown
};

// Social and screen sizes; the last row of the choice is "Custom".
const ShowcaseSizePreset SHOWCASE_SIZES[] = {
    { 1280, 720, L("HD") },
    { 1920, 1080, L("Full HD") },
    { 2560, 1440, L("QHD") },
    { 3840, 2160, L("4K UHD") },
    { 1080, 1080, L("Square") },
    { 1080, 1350, L("Portrait 4:5") },
    { 1080, 1920, L("Story 9:16") },
};
constexpr int SHOWCASE_SIZE_COUNT = int(sizeof(SHOWCASE_SIZES) / sizeof(SHOWCASE_SIZES[0]));

wxColour showcase_wx_colour(const std::string &hex)
{
    std::array<float, 4> c{ 1.f, 1.f, 1.f, 1.f };
    Showcase::parse_hex_color(hex, c);
    return wxColour((unsigned char) std::lround(c[0] * 255.f), (unsigned char) std::lround(c[1] * 255.f),
                    (unsigned char) std::lround(c[2] * 255.f));
}

std::string showcase_hex(const wxColour &c)
{
    return Showcase::to_hex_color({ c.Red() / 255.f, c.Green() / 255.f, c.Blue() / 255.f, 1.f });
}

} // namespace

ShowcaseView::ShowcaseView(wxWindow *parent, Bed3D &bed, Model *model, DynamicPrintConfig *config, BackgroundSlicingProcess *process)
    : m_model(model)
{
    if (!Create(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, 0 /* disable wxTAB_TRAVERSAL */))
        return;

    m_canvas_widget = OpenGLManager::create_wxglcanvas(*this);
    if (m_canvas_widget == nullptr)
        return;
    m_canvas = new GLCanvas3D(m_canvas_widget, bed);
    m_canvas->set_context(wxGetApp().init_glcontext(*m_canvas_widget));
    m_canvas->allow_multisample(OpenGLManager::can_multisample());
    // A presentation, never an editor: no picking, moving, gizmos, selection or toolbars.
    m_canvas->enable_picking(false);
    m_canvas->enable_moving(false);
    m_canvas->set_model(model);
    m_canvas->set_process(process);
    m_canvas->set_type(GLCanvas3D::ECanvasType::CanvasShowcase);
    m_canvas->set_config(config);
    m_canvas->enable_gizmos(false);
    m_canvas->enable_selection(false);
    m_canvas->enable_main_toolbar(false);
    m_canvas->enable_labels(false);
    m_canvas->enable_slope(false);
    m_canvas->enable_assemble_view_toolbar(false);
    m_canvas->enable_return_toolbar(false);
    m_canvas->enable_separator_toolbar(false);
    m_renderer = std::make_unique<ShowcaseRenderer>(*m_canvas);
    m_renderer->set_watermark_provider([this](int w, int h, ShowcaseImage &mark, Showcase::RectI &where) {
        return showcase_rasterize_watermark(scene().export_settings.watermark, project(), w, h, mark, where);
    });

    m_watermark_presets.load();

    build_panel();

    auto *sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(m_canvas_widget, 1, wxEXPAND);
    sizer->Add(m_side, 0, wxEXPAND);
    SetSizer(sizer);
    SetMinSize(GetSize());
    GetSizer()->SetSizeHints(this);

    Bind(wxEVT_IDLE, [this](wxIdleEvent &evt) {
        evt.Skip();
        on_idle_check();
    });

    wxGetApp().UpdateDarkUIWin(this);
}

ShowcaseView::~ShowcaseView()
{
    m_renderer.reset();
    delete m_canvas;
    delete m_canvas_widget;
}

// ------------------------------------------------------------------------------------------------
// The scene of the current plate
// ------------------------------------------------------------------------------------------------

Showcase::Project &ShowcaseView::project() { return m_model->showcase; }

int ShowcaseView::current_plate() const
{
    Plater *plater = wxGetApp().plater();
    return plater != nullptr ? plater->get_partplate_list().get_curr_plate_index() : 0;
}

Showcase::Scene &ShowcaseView::scene()
{
    const int plate = current_plate();
    if (Showcase::Scene *s = project().scene_for_plate(plate))
        return *s;
    if (m_draft_plate != plate) {
        m_draft       = Showcase::make_scene(Showcase::PresetId::StudioWhite, plate);
        m_draft_plate = plate;
    }
    return m_draft;
}

bool ShowcaseView::scene_in_project() { return project().scene_for_plate(current_plate()) != nullptr; }

bool ShowcaseView::read_only() { return project().read_only(); }

void ShowcaseView::commit()
{
    if (read_only() || scene_in_project())
        return;
    const int        plate = current_plate();
    Showcase::Scene  draft = scene();
    Showcase::Scene &s     = project().ensure_scene_for_plate(plate);
    const std::string id   = s.id;
    s                      = draft;
    s.id                   = id;
    s.plate_index          = plate;
    project().active       = id;
    m_draft_plate          = -1;
    // ensure_scene_for_plate() may have moved the scenes: hand the renderer the new address.
    m_renderer->set_scene(&scene(), &project());
    mark_project_dirty();
}

template<class F> void ShowcaseView::edit(F &&change, bool resync)
{
    if (m_syncing || read_only())
        return;
    commit();
    change(scene());
    after_change(resync);
}

void ShowcaseView::after_change(bool resync)
{
    m_renderer->set_scene(&scene(), &project());
    m_renderer->invalidate_watermark();
    mark_project_dirty();
    if (resync)
        sync_controls();
    else
        update_enabled();
    set_as_dirty();
}

void ShowcaseView::mark_project_dirty()
{
    if (Plater *plater = wxGetApp().plater()) {
        plater->set_plater_dirty(true);
        plater->update_title_dirty_status();
    }
}

// ------------------------------------------------------------------------------------------------
// Camera
// ------------------------------------------------------------------------------------------------

void ShowcaseView::apply_scene_camera()
{
    Camera                     &camera = wxGetApp().plater()->get_camera();
    const Showcase::CameraView &c      = scene().camera;
    if (c.valid) {
        camera.set_type(c.projection == Showcase::Projection::Ortho ? Camera::EType::Ortho : Camera::EType::Perspective);
        camera.look_at(Vec3d(c.eye[0], c.eye[1], c.eye[2]), Vec3d(c.target[0], c.target[1], c.target[2]), Vec3d(c.up[0], c.up[1], c.up[2]));
        camera.set_zoom(c.zoom);
    } else {
        camera.set_type(Camera::EType::Perspective);
        m_renderer->place_default_camera();
    }
    set_as_dirty();
}

void ShowcaseView::store_scene_camera(bool mark_dirty)
{
    if (read_only())
        return;
    Camera              &camera = wxGetApp().plater()->get_camera();
    Showcase::CameraView v;
    v.valid      = true;
    v.projection = camera.get_type() == Camera::EType::Ortho ? Showcase::Projection::Ortho : Showcase::Projection::Perspective;
    const Vec3d eye = camera.get_position(), target = camera.get_target(), up = camera.get_dir_up();
    v.eye    = { eye.x(), eye.y(), eye.z() };
    v.target = { target.x(), target.y(), target.z() };
    v.up     = { up.x(), up.y(), up.z() };
    v.zoom   = camera.get_zoom();
    Showcase::Scene &s = scene();
    if (s.camera == v)
        return;
    s.camera = v;
    if (mark_dirty && scene_in_project())
        mark_project_dirty();
}

void ShowcaseView::on_activate()
{
    if (m_canvas == nullptr)
        return;
    m_active = true;
    Camera &camera = wxGetApp().plater()->get_camera();
    m_saved_camera     = camera;
    m_has_saved_camera = true;
    // The Showcase camera is the scene's: switching it to orthographic here must not change the
    // user's "use perspective camera" preference.
    camera.enable_update_config_on_type_change(false);
    m_plate = current_plate();
    m_renderer->set_scene(&scene(), &project());
    refresh_plates();
    apply_scene_camera();
    sync_controls();
}

void ShowcaseView::on_deactivate()
{
    if (!m_active)
        return;
    store_scene_camera(true);
    m_active = false;
    if (m_has_saved_camera) {
        // The editing views get their camera (and the preference hook) back exactly as they left it.
        wxGetApp().plater()->get_camera() = m_saved_camera;
        m_has_saved_camera = false;
    }
}

void ShowcaseView::on_project_changed()
{
    m_draft_plate = -1;
    if (m_renderer == nullptr)
        return;
    m_renderer->set_scene(&scene(), &project());
    m_renderer->invalidate_images();
    if (m_active) {
        m_plate = current_plate();
        refresh_plates();
        apply_scene_camera();
        sync_controls();
    }
}

void ShowcaseView::on_idle_check()
{
    if (!m_active || m_renderer == nullptr)
        return;
    if (current_plate() != m_plate)
        on_plate_switched();
    else if (m_plates != nullptr && int(m_plates->GetCount()) != wxGetApp().plater()->get_partplate_list().get_plate_count())
        refresh_plates();
}

// Another plate became current: its own scene, with its own camera.
void ShowcaseView::on_plate_switched()
{
    // Keep the camera of the plate being left (current_plate() already names the new one).
    const int new_plate = current_plate();
    if (!read_only()) {
        Showcase::Scene *old = project().scene_for_plate(m_plate);
        if (old == nullptr && m_draft_plate == m_plate)
            old = &m_draft;
        if (old != nullptr) {
            Camera              &camera = wxGetApp().plater()->get_camera();
            Showcase::CameraView v;
            v.valid      = true;
            v.projection = camera.get_type() == Camera::EType::Ortho ? Showcase::Projection::Ortho : Showcase::Projection::Perspective;
            const Vec3d eye = camera.get_position(), target = camera.get_target(), up = camera.get_dir_up();
            v.eye    = { eye.x(), eye.y(), eye.z() };
            v.target = { target.x(), target.y(), target.z() };
            v.up     = { up.x(), up.y(), up.z() };
            v.zoom   = camera.get_zoom();
            old->camera = v;
        }
    }
    m_plate = new_plate;
    m_renderer->set_scene(&scene(), &project());
    refresh_plates();
    apply_scene_camera();
    sync_controls();
}

// ------------------------------------------------------------------------------------------------
// Canvas
// ------------------------------------------------------------------------------------------------

void ShowcaseView::set_as_dirty()
{
    if (m_canvas != nullptr)
        m_canvas->set_as_dirty();
}

bool ShowcaseView::is_reload_delayed() const { return m_canvas != nullptr && m_canvas->is_reload_delayed(); }

void ShowcaseView::reload_scene(bool refresh_immediately, bool force_full_scene_refresh)
{
    if (m_canvas == nullptr)
        return;
    if (!m_canvas->is_initialized())
        m_canvas->render(true);
    m_canvas->reload_scene(refresh_immediately, force_full_scene_refresh);
}

void ShowcaseView::on_change_color_mode(bool is_dark)
{
    if (m_canvas != nullptr)
        m_canvas->on_change_color_mode(is_dark);
    wxGetApp().UpdateDarkUIWin(this);
    Refresh();
}

// ------------------------------------------------------------------------------------------------
// The side panel
// ------------------------------------------------------------------------------------------------

void ShowcaseView::build_panel()
{
    const int em = wxGetApp().em_unit();
    m_side = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxSize(32 * em, -1), wxVSCROLL);
    m_side->SetScrollRate(0, em);
    auto *col = new wxBoxSizer(wxVERTICAL);

    auto header = [this, col, em](const wxString &title) {
        auto *t = new wxStaticText(m_side, wxID_ANY, title);
        t->SetFont(wxGetApp().bold_font());
        col->Add(t, 0, wxLEFT | wxRIGHT | wxTOP, em);
        col->Add(new wxStaticLine(m_side), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em / 2);
    };
    wxFlexGridSizer *grid = nullptr;
    auto begin_grid = [&grid, col, em]() {
        grid = new wxFlexGridSizer(2, em / 2, em);
        grid->AddGrowableCol(1, 1);
        col->Add(grid, 0, wxEXPAND | wxALL, em);
    };
    auto row = [this, &grid](const wxString &label, wxWindow *ctrl) {
        grid->Add(new wxStaticText(m_side, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(ctrl, 1, wxEXPAND);
    };
    auto choice = [this](const wxArrayString &items) {
        auto *c = new wxChoice(m_side, wxID_ANY, wxDefaultPosition, wxDefaultSize, items);
        return c;
    };
    auto slider = [this](int value, int lo, int hi) { return new wxSlider(m_side, wxID_ANY, value, lo, hi); };
    auto picker = [this]() { return new wxColourPickerCtrl(m_side, wxID_ANY, *wxWHITE); };

    m_notice = new wxStaticText(m_side, wxID_ANY, "");
    m_notice->Hide();
    col->Add(m_notice, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em);

    // --- Scene ---------------------------------------------------------------------------------
    header(_L("Scene"));
    begin_grid();
    m_plates = choice({});
    row(_L("Plate"), m_plates);
    wxArrayString presets;
    presets.Add(_L("Studio white"));
    presets.Add(_L("Dark gradient"));
    presets.Add(_L("Build plate"));
    m_presets = choice(presets);
    m_presets->SetToolTip(_L("Applies the preset's backdrop, floor and light. The camera and the export settings stay."));
    row(_L("Look"), m_presets);
    m_hide_plate = new wxCheckBox(m_side, wxID_ANY, _L("Hide the printer's plate"));
    m_hide_wipe_tower = new wxCheckBox(m_side, wxID_ANY, _L("Hide the prime tower"));
    grid->AddSpacer(0);
    grid->Add(m_hide_plate);
    grid->AddSpacer(0);
    grid->Add(m_hide_wipe_tower);

    // --- Background ----------------------------------------------------------------------------
    header(_L("Background"));
    begin_grid();
    wxArrayString bg_types;
    bg_types.Add(_L("Solid colour"));
    bg_types.Add(_L("Vertical gradient"));
    bg_types.Add(_L("Radial gradient"));
    bg_types.Add(_L("Image"));
    bg_types.Add(_L("Transparent"));
    m_bg_type = choice(bg_types);
    row(_L("Type"), m_bg_type);
    m_bg_color1 = picker();
    row(_L("Colour"), m_bg_color1);
    m_bg_color2 = picker();
    row(_L("Second colour"), m_bg_color2);
    m_bg_image = new wxButton(m_side, wxID_ANY, _L("Choose image..."));
    row(_L("Image"), m_bg_image);
    wxArrayString fits;
    fits.Add(_L("Fill"));
    fits.Add(_L("Fit"));
    fits.Add(_L("Tile"));
    m_bg_fit = choice(fits);
    row(_L("Image fit"), m_bg_fit);

    // --- Floor ---------------------------------------------------------------------------------
    header(_L("Floor"));
    begin_grid();
    wxArrayString floor_types;
    floor_types.Add(_L("None"));
    floor_types.Add(_L("Shadow only"));
    floor_types.Add(_L("Solid colour"));
    floor_types.Add(_L("Grid"));
    m_floor_type = choice(floor_types);
    m_floor_type->SetToolTip(_L("Shadow only: an invisible floor that shows just the shadow, over any background."));
    row(_L("Type"), m_floor_type);
    m_floor_color = picker();
    row(_L("Colour"), m_floor_color);
    m_floor_shadow = slider(45, 0, 100);
    row(_L("Shadow"), m_floor_shadow);
    m_floor_grid = new wxSpinCtrl(m_side, wxID_ANY, "10", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 100, 10);
    row(_L("Grid spacing (mm)"), m_floor_grid);

    // --- Light ---------------------------------------------------------------------------------
    header(_L("Light"));
    begin_grid();
    m_light_fixed = new wxCheckBox(m_side, wxID_ANY, _L("Fixed in the scene"));
    m_light_fixed->SetToolTip(_L("The light stays put when you orbit, so the shadows do too. Off: the light follows the camera."));
    grid->AddSpacer(0);
    grid->Add(m_light_fixed);
    m_light_azimuth = slider(225, 0, 359);
    row(_L("Direction"), m_light_azimuth);
    m_light_elevation = slider(55, 5, 90);
    row(_L("Height"), m_light_elevation);
    m_shadows = new wxCheckBox(m_side, wxID_ANY, _L("Shadows"));
    grid->AddSpacer(0);
    grid->Add(m_shadows);
    m_shadow_darkness = slider(35, 0, 100);
    row(_L("Shadow darkness"), m_shadow_darkness);
    m_ssao = new wxCheckBox(m_side, wxID_ANY, _L("Ambient occlusion"));
    grid->AddSpacer(0);
    grid->Add(m_ssao);

    // --- Camera --------------------------------------------------------------------------------
    header(_L("Camera"));
    begin_grid();
    wxArrayString projections;
    projections.Add(_L("Perspective"));
    projections.Add(_L("Orthographic"));
    m_projection = choice(projections);
    row(_L("Projection"), m_projection);
    m_views = choice({});
    row(_L("Saved views"), m_views);
    {
        auto *buttons = new wxBoxSizer(wxHORIZONTAL);
        auto *frame   = new wxButton(m_side, wxID_ANY, _L("Frame model"));
        auto *save    = new wxButton(m_side, wxID_ANY, _L("Save view..."));
        m_view_delete = new wxButton(m_side, wxID_ANY, _L("Delete view"));
        buttons->Add(frame, 0, wxRIGHT, em / 2);
        buttons->Add(save, 0, wxRIGHT, em / 2);
        buttons->Add(m_view_delete, 0);
        col->Add(buttons, 0, wxLEFT | wxRIGHT, em);
        frame->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
            m_renderer->frame_model();
            store_scene_camera(true);
        });
        save->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_save_view(); });
        m_view_delete->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_delete_view(); });
    }

    // --- Export --------------------------------------------------------------------------------
    header(_L("Export"));
    begin_grid();
    wxArrayString sizes;
    for (const ShowcaseSizePreset &p : SHOWCASE_SIZES)
        sizes.Add(wxString::Format("%s (%d x %d)", _(p.name), p.w, p.h));
    sizes.Add(_L("Custom"));
    m_size = choice(sizes);
    row(_L("Size"), m_size);
    m_width  = new wxSpinCtrl(m_side, wxID_ANY, "1920", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, Showcase::MIN_EXPORT_SIDE,
                              Showcase::MAX_EXPORT_SIDE, 1920);
    m_height = new wxSpinCtrl(m_side, wxID_ANY, "1080", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, Showcase::MIN_EXPORT_SIDE,
                              Showcase::MAX_EXPORT_SIDE, 1080);
    row(_L("Width (px)"), m_width);
    row(_L("Height (px)"), m_height);
    wxArrayString qualities;
    qualities.Add(_L("Draft"));
    qualities.Add(_L("High"));
    qualities.Add(_L("Best"));
    m_quality = choice(qualities);
    m_quality->SetToolTip(_L("Draft renders at the image size, High at twice and Best at three times, then scales down. "
                             "Very large images use less where the graphics card cannot."));
    row(_L("Quality"), m_quality);
    wxArrayString formats;
    formats.Add("PNG");
    formats.Add("JPEG");
    m_format = choice(formats);
    row(_L("Format"), m_format);
    {
        auto *buttons = new wxBoxSizer(wxHORIZONTAL);
        m_export = new wxButton(m_side, wxID_ANY, _L("Export image..."));
        m_copy   = new wxButton(m_side, wxID_ANY, _L("Copy to clipboard"));
        buttons->Add(m_export, 0, wxRIGHT, em / 2);
        buttons->Add(m_copy, 0);
        col->Add(buttons, 0, wxLEFT | wxRIGHT, em);
        m_status = new wxStaticText(m_side, wxID_ANY, "");
        col->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em);
        m_export->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_export(false); });
        m_copy->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_export(true); });
    }

    // --- Watermark (D12) -----------------------------------------------------------------------
    header(_L("Watermark"));
    begin_grid();
    m_wm_enabled = new wxCheckBox(m_side, wxID_ANY, _L("Add my watermark"));
    m_wm_enabled->SetToolTip(_L("Your own logo and/or text on the exported images. Off unless you turn it on."));
    grid->AddSpacer(0);
    grid->Add(m_wm_enabled);
    m_wm_presets = choice({});
    row(_L("Preset"), m_wm_presets);
    m_wm_text = new wxTextCtrl(m_side, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    row(_L("Text"), m_wm_text);
    m_wm_font = new wxFontPickerCtrl(m_side, wxID_ANY, wxFont(wxFontInfo(12)), wxDefaultPosition, wxDefaultSize, wxFNTP_FONTDESC_AS_LABEL);
    row(_L("Font"), m_wm_font);
    m_wm_color = picker();
    row(_L("Text colour"), m_wm_color);
    {
        auto *logo   = new wxBoxSizer(wxHORIZONTAL);
        m_wm_logo        = new wxButton(m_side, wxID_ANY, _L("Choose logo..."));
        m_wm_logo_remove = new wxButton(m_side, wxID_ANY, _L("Remove"));
        logo->Add(m_wm_logo, 1, wxRIGHT, em / 2);
        logo->Add(m_wm_logo_remove, 0);
        grid->Add(new wxStaticText(m_side, wxID_ANY, _L("Logo")), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(logo, 1, wxEXPAND);
    }
    wxArrayString anchors;
    anchors.Add(_L("Top left"));
    anchors.Add(_L("Top"));
    anchors.Add(_L("Top right"));
    anchors.Add(_L("Left"));
    anchors.Add(_L("Centre"));
    anchors.Add(_L("Right"));
    anchors.Add(_L("Bottom left"));
    anchors.Add(_L("Bottom"));
    anchors.Add(_L("Bottom right"));
    m_wm_anchor = choice(anchors);
    row(_L("Position"), m_wm_anchor);
    m_wm_size = slider(6, 1, 30);
    row(_L("Size"), m_wm_size);
    m_wm_opacity = slider(85, 0, 100);
    row(_L("Opacity"), m_wm_opacity);
    m_wm_margin = slider(3, 0, 20);
    row(_L("Margin"), m_wm_margin);
    {
        auto *buttons = new wxBoxSizer(wxHORIZONTAL);
        auto *save    = new wxButton(m_side, wxID_ANY, _L("Save as preset..."));
        m_wm_preset_delete = new wxButton(m_side, wxID_ANY, _L("Delete preset"));
        buttons->Add(save, 0, wxRIGHT, em / 2);
        buttons->Add(m_wm_preset_delete, 0);
        col->Add(buttons, 0, wxLEFT | wxRIGHT | wxBOTTOM, em);
        save->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_save_watermark_preset(); });
        m_wm_preset_delete->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_delete_watermark_preset(); });
    }

    m_side->SetSizer(col);
    m_side->FitInside();

    // --- Handlers ------------------------------------------------------------------------------
    m_plates->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        if (m_syncing)
            return;
        const int sel = m_plates->GetSelection();
        if (sel >= 0 && sel != current_plate())
            wxGetApp().plater()->select_plate(sel);
        // on_idle_check() picks the new plate up.
    });
    m_presets->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        const int sel = m_presets->GetSelection();
        if (sel < 0)
            return;
        edit([sel](Showcase::Scene &s) { Showcase::apply_preset(s, Showcase::builtin_presets()[size_t(sel)]); }, true);
        m_renderer->invalidate_images();
    });
    m_hide_plate->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { edit([this](Showcase::Scene &s) { s.floor.hide_plate = m_hide_plate->GetValue(); }); });
    m_hide_wipe_tower->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) {
        edit([this](Showcase::Scene &s) { s.floor.hide_wipe_tower = m_hide_wipe_tower->GetValue(); });
    });

    m_bg_type->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        const int sel = m_bg_type->GetSelection();
        if (sel < 0)
            return;
        if (Showcase::BackgroundType(sel) == Showcase::BackgroundType::Image && scene().background.image.empty()) {
            on_choose_background_image();
            sync_controls();
            return;
        }
        edit([sel](Showcase::Scene &s) { s.background.type = Showcase::BackgroundType(sel); });
    });
    m_bg_color1->Bind(wxEVT_COLOURPICKER_CHANGED, [this](wxColourPickerEvent &) {
        edit([this](Showcase::Scene &s) { s.background.color1 = showcase_hex(m_bg_color1->GetColour()); });
    });
    m_bg_color2->Bind(wxEVT_COLOURPICKER_CHANGED, [this](wxColourPickerEvent &) {
        edit([this](Showcase::Scene &s) { s.background.color2 = showcase_hex(m_bg_color2->GetColour()); });
    });
    m_bg_image->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_choose_background_image(); });
    m_bg_fit->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        const int sel = m_bg_fit->GetSelection();
        if (sel >= 0)
            edit([sel](Showcase::Scene &s) { s.background.fit = Showcase::ImageFit(sel); });
    });

    m_floor_type->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        const int sel = m_floor_type->GetSelection();
        if (sel >= 0)
            edit([sel](Showcase::Scene &s) { s.floor.type = Showcase::FloorType(sel); });
    });
    m_floor_color->Bind(wxEVT_COLOURPICKER_CHANGED, [this](wxColourPickerEvent &) {
        edit([this](Showcase::Scene &s) { s.floor.color = showcase_hex(m_floor_color->GetColour()); });
    });
    m_floor_shadow->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
        edit([this](Showcase::Scene &s) { s.floor.shadow_strength = float(m_floor_shadow->GetValue()) / 100.f; });
    });
    m_floor_grid->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent &) {
        edit([this](Showcase::Scene &s) { s.floor.grid_spacing = float(m_floor_grid->GetValue()); });
    });

    m_light_fixed->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { edit([this](Showcase::Scene &s) { s.lighting.world_fixed = m_light_fixed->GetValue(); }); });
    m_light_azimuth->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
        edit([this](Showcase::Scene &s) { s.lighting.azimuth_deg = float(m_light_azimuth->GetValue()); });
    });
    m_light_elevation->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
        edit([this](Showcase::Scene &s) { s.lighting.elevation_deg = float(m_light_elevation->GetValue()); });
    });
    m_shadows->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { edit([this](Showcase::Scene &s) { s.lighting.shadows = m_shadows->GetValue(); }); });
    m_shadow_darkness->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
        edit([this](Showcase::Scene &s) { s.lighting.shadow_intensity = float(m_shadow_darkness->GetValue()) / 100.f; });
    });
    m_ssao->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { edit([this](Showcase::Scene &s) { s.lighting.ssao = m_ssao->GetValue(); }); });

    m_projection->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        if (m_syncing || read_only())
            return;
        const bool ortho = m_projection->GetSelection() == 1;
        wxGetApp().plater()->get_camera().set_type(ortho ? Camera::EType::Ortho : Camera::EType::Perspective);
        commit();
        store_scene_camera(true);
        set_as_dirty();
    });
    m_views->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        if (m_syncing)
            return;
        const int sel = m_views->GetSelection();
        const std::vector<Showcase::CameraView> &views = scene().views;
        if (sel < 0 || size_t(sel) >= views.size())
            return;
        Showcase::CameraView keep = scene().camera;
        scene().camera            = views[size_t(sel)];
        apply_scene_camera();
        if (scene_in_project() && !(keep == scene().camera))
            mark_project_dirty();
        update_enabled();
    });

    m_size->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        const int sel = m_size->GetSelection();
        if (sel < 0 || sel >= SHOWCASE_SIZE_COUNT) {
            update_enabled();
            return;
        }
        edit([sel](Showcase::Scene &s) {
            s.export_settings.width  = SHOWCASE_SIZES[sel].w;
            s.export_settings.height = SHOWCASE_SIZES[sel].h;
        }, true);
    });
    m_width->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent &) {
        edit([this](Showcase::Scene &s) { s.export_settings.width = Showcase::clamp_export_side(m_width->GetValue()); });
    });
    m_height->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent &) {
        edit([this](Showcase::Scene &s) { s.export_settings.height = Showcase::clamp_export_side(m_height->GetValue()); });
    });
    m_quality->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        const int sel = m_quality->GetSelection();
        if (sel >= 0)
            edit([sel](Showcase::Scene &s) { s.export_settings.quality = Showcase::Quality(sel); });
    });
    m_format->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        const int sel = m_format->GetSelection();
        if (sel >= 0)
            edit([sel](Showcase::Scene &s) { s.export_settings.format = Showcase::ImageFormat(sel); });
    });

    m_wm_enabled->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) {
        edit([this](Showcase::Scene &s) { s.export_settings.watermark.enabled = m_wm_enabled->GetValue(); });
    });
    m_wm_presets->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) { on_apply_watermark_preset(); });
    m_wm_text->Bind(wxEVT_TEXT, [this](wxCommandEvent &) {
        edit([this](Showcase::Scene &s) { s.export_settings.watermark.text = into_u8(m_wm_text->GetValue()); });
    });
    m_wm_font->Bind(wxEVT_FONTPICKER_CHANGED, [this](wxFontPickerEvent &) {
        const wxFont f = m_wm_font->GetSelectedFont();
        edit([&f](Showcase::Scene &s) {
            s.export_settings.watermark.font_face = into_u8(f.GetFaceName());
            s.export_settings.watermark.bold      = f.GetWeight() >= wxFONTWEIGHT_BOLD;
        });
    });
    m_wm_color->Bind(wxEVT_COLOURPICKER_CHANGED, [this](wxColourPickerEvent &) {
        edit([this](Showcase::Scene &s) { s.export_settings.watermark.text_color = showcase_hex(m_wm_color->GetColour()); });
    });
    m_wm_logo->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_choose_logo(); });
    m_wm_logo_remove->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { edit([](Showcase::Scene &s) { s.export_settings.watermark.logo.clear(); }); });
    m_wm_anchor->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        const int sel = m_wm_anchor->GetSelection();
        if (sel >= 0)
            edit([sel](Showcase::Scene &s) { s.export_settings.watermark.anchor = Showcase::Anchor(sel); });
    });
    m_wm_size->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
        edit([this](Showcase::Scene &s) { s.export_settings.watermark.size_pct = float(m_wm_size->GetValue()); });
    });
    m_wm_opacity->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
        edit([this](Showcase::Scene &s) { s.export_settings.watermark.opacity = float(m_wm_opacity->GetValue()) / 100.f; });
    });
    m_wm_margin->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
        edit([this](Showcase::Scene &s) { s.export_settings.watermark.margin_pct = float(m_wm_margin->GetValue()); });
    });
}

void ShowcaseView::refresh_plates()
{
    if (m_plates == nullptr)
        return;
    const bool syncing = m_syncing;
    m_syncing          = true;
    const int count    = wxGetApp().plater()->get_partplate_list().get_plate_count();
    m_plates->Clear();
    for (int i = 0; i < count; ++i)
        m_plates->Append(wxString::Format(_L("Plate %d"), i + 1));
    m_plates->SetSelection(std::min(current_plate(), count - 1));
    m_syncing = syncing;
}

void ShowcaseView::refresh_views()
{
    const bool syncing = m_syncing;
    m_syncing          = true;
    m_views->Clear();
    int selected = -1;
    const Showcase::Scene &s = scene();
    for (size_t i = 0; i < s.views.size(); ++i) {
        m_views->Append(wxString::FromUTF8(s.views[i].name.empty() ? std::to_string(i + 1) : s.views[i].name));
        if (s.camera == s.views[i] || (s.views[i].eye == s.camera.eye && s.views[i].target == s.camera.target && s.views[i].zoom == s.camera.zoom))
            selected = int(i);
    }
    m_views->SetSelection(selected);
    m_syncing = syncing;
}

void ShowcaseView::refresh_watermark_presets()
{
    const bool syncing = m_syncing;
    m_syncing          = true;
    m_wm_presets->Clear();
    m_wm_presets->Append(_L("(none)"));
    int selected = 0;
    const std::string &current = scene().export_settings.watermark.name;
    for (const WatermarkPresets::Preset &p : m_watermark_presets.presets()) {
        m_wm_presets->Append(wxString::FromUTF8(p.name));
        if (!current.empty() && p.name == current)
            selected = int(m_wm_presets->GetCount()) - 1;
    }
    m_wm_presets->SetSelection(selected);
    m_syncing = syncing;
}

void ShowcaseView::sync_controls()
{
    if (m_side == nullptr)
        return;
    m_syncing = true;
    const Showcase::Scene &s = scene();

    m_notice->Show(read_only() || !ShowcaseRenderer::is_supported());
    if (!ShowcaseRenderer::is_supported())
        m_notice->SetLabel(_L("Showcase needs OpenGL 3.2 or newer to export images. This graphics driver only shows the preview."));
    else if (read_only())
        m_notice->SetLabel(_L("These scenes were saved by a newer version of EdgeSlicer. They are shown as saved and cannot be changed here."));
    m_notice->Wrap(30 * wxGetApp().em_unit());

    m_presets->SetSelection(-1);
    Showcase::PresetId preset;
    if (Showcase::from_string(s.preset_base, preset))
        m_presets->SetSelection(int(preset));
    m_hide_plate->SetValue(s.floor.hide_plate);
    m_hide_wipe_tower->SetValue(s.floor.hide_wipe_tower);

    m_bg_type->SetSelection(int(s.background.type));
    m_bg_color1->SetColour(showcase_wx_colour(s.background.color1));
    m_bg_color2->SetColour(showcase_wx_colour(s.background.color2));
    m_bg_fit->SetSelection(int(s.background.fit));

    m_floor_type->SetSelection(int(s.floor.type));
    m_floor_color->SetColour(showcase_wx_colour(s.floor.color));
    m_floor_shadow->SetValue(int(std::lround(s.floor.shadow_strength * 100.f)));
    m_floor_grid->SetValue(int(std::lround(s.floor.grid_spacing)));

    m_light_fixed->SetValue(s.lighting.world_fixed);
    int az = int(std::lround(s.lighting.azimuth_deg)) % 360;
    if (az < 0)
        az += 360;
    m_light_azimuth->SetValue(az);
    m_light_elevation->SetValue(std::clamp(int(std::lround(s.lighting.elevation_deg)), 5, 90));
    m_shadows->SetValue(s.lighting.shadows);
    m_shadow_darkness->SetValue(int(std::lround(s.lighting.shadow_intensity * 100.f)));
    m_ssao->SetValue(s.lighting.ssao);

    m_projection->SetSelection(wxGetApp().plater()->get_camera().get_type() == Camera::EType::Ortho ? 1 : 0);
    refresh_views();

    int size_sel = SHOWCASE_SIZE_COUNT;
    for (int i = 0; i < SHOWCASE_SIZE_COUNT; ++i)
        if (SHOWCASE_SIZES[i].w == s.export_settings.width && SHOWCASE_SIZES[i].h == s.export_settings.height)
            size_sel = i;
    m_size->SetSelection(size_sel);
    m_width->SetValue(s.export_settings.width);
    m_height->SetValue(s.export_settings.height);
    m_quality->SetSelection(int(s.export_settings.quality));
    m_format->SetSelection(int(s.export_settings.format));

    const Showcase::Watermark &wm = s.export_settings.watermark;
    m_wm_enabled->SetValue(wm.enabled);
    refresh_watermark_presets();
    m_wm_text->ChangeValue(wxString::FromUTF8(wm.text));
    wxFontInfo font_info(12);
    if (!wm.font_face.empty())
        font_info.FaceName(wxString::FromUTF8(wm.font_face));
    font_info.Bold(wm.bold);
    m_wm_font->SetSelectedFont(wxFont(font_info));
    m_wm_color->SetColour(showcase_wx_colour(wm.text_color));
    m_wm_anchor->SetSelection(int(wm.anchor));
    m_wm_size->SetValue(std::clamp(int(std::lround(wm.size_pct)), 1, 30));
    m_wm_opacity->SetValue(int(std::lround(wm.opacity * 100.f)));
    m_wm_margin->SetValue(std::clamp(int(std::lround(wm.margin_pct)), 0, 20));

    m_syncing = false;
    update_enabled();
    m_side->FitInside();
    m_side->Layout();
}

void ShowcaseView::update_enabled()
{
    if (m_side == nullptr)
        return;
    const Showcase::Scene &s        = scene();
    const bool             editable = !read_only();
    const bool             gradient = s.background.type == Showcase::BackgroundType::VerticalGradient ||
                          s.background.type == Showcase::BackgroundType::RadialGradient;
    const bool image    = s.background.type == Showcase::BackgroundType::Image;
    const bool floor_on = s.floor.hide_plate;

    for (wxWindow *w : std::initializer_list<wxWindow *>{ m_presets, m_hide_plate, m_hide_wipe_tower, m_bg_type, m_light_fixed, m_shadows,
                                                          m_ssao, m_projection, m_size, m_quality, m_format, m_wm_enabled })
        w->Enable(editable);
    m_bg_color1->Enable(editable && s.background.type != Showcase::BackgroundType::Transparent);
    m_bg_color2->Enable(editable && gradient);
    m_bg_image->Enable(editable && image);
    m_bg_fit->Enable(editable && image);
    m_floor_type->Enable(editable && floor_on);
    m_floor_color->Enable(editable && floor_on &&
                          (s.floor.type == Showcase::FloorType::Solid || s.floor.type == Showcase::FloorType::Grid));
    m_floor_shadow->Enable(editable && floor_on && s.floor.type != Showcase::FloorType::None);
    m_floor_grid->Enable(editable && floor_on && s.floor.type == Showcase::FloorType::Grid);
    m_light_azimuth->Enable(editable && s.lighting.world_fixed);
    m_light_elevation->Enable(editable && s.lighting.world_fixed);
    m_shadow_darkness->Enable(editable && s.lighting.shadows);
    m_views->Enable(!s.views.empty());
    m_view_delete->Enable(editable && m_views->GetSelection() >= 0);
    const bool custom = m_size->GetSelection() == SHOWCASE_SIZE_COUNT;
    m_width->Enable(editable && custom);
    m_height->Enable(editable && custom);
    const bool supported = ShowcaseRenderer::is_supported();
    m_export->Enable(supported);
    m_copy->Enable(supported);
    const bool wm = editable && s.export_settings.watermark.enabled;
    for (wxWindow *w : std::initializer_list<wxWindow *>{ m_wm_presets, m_wm_text, m_wm_font, m_wm_color, m_wm_logo, m_wm_anchor, m_wm_size,
                                                          m_wm_opacity, m_wm_margin })
        w->Enable(wm);
    m_wm_logo_remove->Enable(wm && !s.export_settings.watermark.logo.empty());
    m_wm_preset_delete->Enable(m_wm_presets->GetSelection() > 0);
}

// ------------------------------------------------------------------------------------------------
// Images, views, presets
// ------------------------------------------------------------------------------------------------

bool ShowcaseView::add_image_file(const std::string &path, int max_side, bool always_png, std::string &sha)
{
    std::vector<uint8_t> bytes;
    std::string          ext, error;
    if (!showcase_load_image_file(path, max_side, always_png, bytes, ext, error)) {
        show_error(this, _L("The image could not be used.") + "\n" + wxString::FromUTF8(error));
        return false;
    }
    sha = project().add_image(std::move(bytes), ext);
    // Design section 5: warn when the project's pictures grow large.
    size_t total = 0;
    for (const auto &it : project().images)
        total += it.second.bytes.size();
    if (total > size_t(20) * 1024 * 1024)
        show_info(this, _L("The pictures in this project's Showcase scenes are larger than 20 MB in total. "
                           "They are saved in the project file."),
                  _L("Showcase"));
    return !sha.empty();
}

void ShowcaseView::on_choose_background_image()
{
    if (read_only())
        return;
    wxFileDialog dlg(this, _L("Choose a background image"), "", "", _L("Images") + " (*.png;*.jpg;*.jpeg;*.bmp)|*.png;*.jpg;*.jpeg;*.bmp",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK)
        return;
    commit();
    std::string sha;
    if (!add_image_file(into_u8(dlg.GetPath()), 4096, false, sha))
        return;
    edit([&sha](Showcase::Scene &s) {
        s.background.image = sha;
        s.background.type  = Showcase::BackgroundType::Image;
    }, true);
    m_renderer->invalidate_images();
}

void ShowcaseView::on_choose_logo()
{
    if (read_only())
        return;
    wxFileDialog dlg(this, _L("Choose a logo"), "", "", _L("Images") + " (*.png;*.jpg;*.jpeg;*.bmp)|*.png;*.jpg;*.jpeg;*.bmp",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK)
        return;
    commit();
    std::string sha;
    if (!add_image_file(into_u8(dlg.GetPath()), 1024, true, sha))
        return;
    edit([&sha](Showcase::Scene &s) { s.export_settings.watermark.logo = sha; }, true);
}

void ShowcaseView::on_save_view()
{
    if (read_only())
        return;
    const Showcase::Scene &s = scene();
    wxTextEntryDialog dlg(this, _L("Name of the view:"), _L("Save view"), wxString::Format(_L("View %d"), int(s.views.size()) + 1));
    if (dlg.ShowModal() != wxID_OK)
        return;
    const std::string name = into_u8(dlg.GetValue());
    commit();
    store_scene_camera(false);
    edit([&name](Showcase::Scene &sc) {
        Showcase::CameraView v = sc.camera;
        v.name                 = name;
        for (Showcase::CameraView &existing : sc.views)
            if (existing.name == name) {
                existing = v;
                return;
            }
        sc.views.push_back(v);
    }, true);
}

void ShowcaseView::on_delete_view()
{
    const int sel = m_views->GetSelection();
    if (sel < 0)
        return;
    edit([sel](Showcase::Scene &s) {
        if (size_t(sel) < s.views.size())
            s.views.erase(s.views.begin() + sel);
    }, true);
}

void ShowcaseView::on_apply_watermark_preset()
{
    if (m_syncing || read_only())
        return;
    const int sel = m_wm_presets->GetSelection();
    if (sel <= 0) {
        update_enabled();
        return;
    }
    const WatermarkPresets::Preset *p = m_watermark_presets.find(into_u8(m_wm_presets->GetString(sel)));
    if (p == nullptr)
        return;
    commit();
    std::string logo;
    if (!p->logo.empty())
        logo = project().add_image(p->logo, p->logo_ext);
    edit([p, &logo](Showcase::Scene &s) {
        Showcase::Watermark wm = p->watermark;
        wm.name                = p->name;
        wm.enabled             = true;
        wm.logo                = logo;
        s.export_settings.watermark = wm;
    }, true);
}

void ShowcaseView::on_save_watermark_preset()
{
    const Showcase::Watermark &wm = scene().export_settings.watermark;
    wxTextEntryDialog dlg(this, _L("Name of the watermark preset:"), _L("Save watermark preset"),
                          wm.name.empty() ? _L("My watermark") : wxString::FromUTF8(wm.name));
    if (dlg.ShowModal() != wxID_OK)
        return;
    const std::string name = into_u8(dlg.GetValue());
    if (name.empty())
        return;
    WatermarkPresets::Preset p;
    p.name              = name;
    p.watermark         = wm;
    p.watermark.name    = name;
    p.watermark.enabled = true;
    if (const Showcase::ImageBlob *blob = project().find_image(wm.logo)) {
        p.logo     = blob->bytes;
        p.logo_ext = blob->ext;
    } else
        p.watermark.logo.clear();
    m_watermark_presets.put(std::move(p));
    std::string error;
    if (!m_watermark_presets.save(&error)) {
        show_error(this, _L("The watermark preset could not be saved.") + "\n" + wxString::FromUTF8(error));
        return;
    }
    if (!read_only() && scene().export_settings.watermark.name != name)
        edit([&name](Showcase::Scene &s) { s.export_settings.watermark.name = name; }, true);
    else
        refresh_watermark_presets();
    update_enabled();
}

void ShowcaseView::on_delete_watermark_preset()
{
    const int sel = m_wm_presets->GetSelection();
    if (sel <= 0)
        return;
    m_watermark_presets.remove(into_u8(m_wm_presets->GetString(sel)));
    std::string error;
    if (!m_watermark_presets.save(&error))
        show_error(this, _L("The watermark presets could not be saved.") + "\n" + wxString::FromUTF8(error));
    refresh_watermark_presets();
    update_enabled();
}

// ------------------------------------------------------------------------------------------------
// Export
// ------------------------------------------------------------------------------------------------

void ShowcaseView::on_export(bool to_clipboard)
{
    if (m_renderer == nullptr || !ShowcaseRenderer::is_supported())
        return;
    // Exporting a scene keeps it: its camera and settings are saved with the project.
    commit();
    store_scene_camera(true);
    const Showcase::Scene &s    = scene();
    const bool             jpeg = s.export_settings.format == Showcase::ImageFormat::JPEG;

    wxString path;
    if (!to_clipboard) {
        wxString name = wxGetApp().plater()->get_project_name();
        if (name.empty())
            name = "showcase";
        name += wxString::Format("_plate%d", current_plate() + 1);
        name += jpeg ? ".jpg" : ".png";
        wxFileDialog dlg(this, _L("Export Showcase image"), wxString::FromUTF8(wxGetApp().app_config->get_last_dir()), name,
                         jpeg ? "JPEG (*.jpg)|*.jpg;*.jpeg" : "PNG (*.png)|*.png", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() != wxID_OK)
            return;
        path = dlg.GetPath();
    }

    ShowcaseImage image;
    std::string   report;
    {
        wxBusyCursor busy;
        m_status->SetLabel(_L("Rendering..."));
        m_status->Update();
        const bool ok = m_renderer->render_export(image, report);
        if (!ok || image.empty()) {
            m_status->SetLabel("");
            show_error(this, _L("The image could not be rendered.") + "\n" + wxString::FromUTF8(report));
            set_as_dirty();
            return;
        }
        showcase_apply_watermark(image, s.export_settings.watermark, project());
    }
    set_as_dirty();

    if (jpeg || to_clipboard)
        Showcase::flatten_rgba(image.rgba, 255, 255, 255);
    const bool transparent = !jpeg && !to_clipboard && s.background.type == Showcase::BackgroundType::Transparent;
    wxImage    out(image.width, image.height, false);
    if (transparent)
        out.InitAlpha();
    unsigned char *rgb   = out.GetData();
    unsigned char *alpha = transparent ? out.GetAlpha() : nullptr;
    for (size_t i = 0; i < size_t(image.width) * size_t(image.height); ++i) {
        rgb[i * 3 + 0] = image.rgba[i * 4 + 0];
        rgb[i * 3 + 1] = image.rgba[i * 4 + 1];
        rgb[i * 3 + 2] = image.rgba[i * 4 + 2];
        if (alpha)
            alpha[i] = image.rgba[i * 4 + 3];
    }

    if (to_clipboard) {
        if (wxTheClipboard->Open()) {
            wxTheClipboard->SetData(new wxBitmapDataObject(wxBitmap(out)));
            wxTheClipboard->Close();
            m_status->SetLabel(wxString::Format(_L("Copied a %d x %d image."), image.width, image.height));
        } else
            show_error(this, _L("The clipboard is in use by another program."));
        return;
    }

    if (jpeg)
        out.SetOption(wxIMAGE_OPTION_QUALITY, s.export_settings.jpeg_quality);
    if (!out.SaveFile(path, jpeg ? wxBITMAP_TYPE_JPEG : wxBITMAP_TYPE_PNG)) {
        m_status->SetLabel("");
        show_error(this, _L("The image could not be saved."));
        return;
    }
    wxGetApp().app_config->update_skein_dir(into_u8(wxFileName(path).GetPath()));
    m_status->SetLabel(wxString::Format(_L("Saved %s (%d x %d)."), wxFileName(path).GetFullName(), image.width, image.height));
    m_status->Wrap(30 * wxGetApp().em_unit());
    m_side->FitInside();
    BOOST_LOG_TRIVIAL(info) << "Showcase: exported " << image.width << "x" << image.height << " to " << into_u8(path);
}

} // namespace GUI
} // namespace Slic3r
