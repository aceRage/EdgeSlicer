#include "ShowcaseView.hpp"

#include "ShowcaseRenderer.hpp"
#include "ShowcaseTextures.hpp"

#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Showcase/GlowFilament.hpp"
#include "libslic3r/Showcase/ShowcaseEncoder.hpp"
#include "libslic3r/Showcase/ShowcaseWebP.hpp"
#include "libslic3r/Utils.hpp"

#include "../GLCanvas3D.hpp"
#include "../GUI.hpp"
#include "../GUI_App.hpp"
#include "../FilamentColorUtils.hpp"
#include "../GUI_Utils.hpp"   // _3MF_COVER_SIZE
#include "../I18N.hpp"
#include "../OpenGLManager.hpp"
#include "../PartPlate.hpp"
#include "../Plater.hpp"
#include "../PresetComboBoxes.hpp"
#include <wx/statbmp.h>
#include "../Widgets/StateColor.hpp"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/clipbrd.h>
#include <wx/clrpicker.h>
#include <wx/cursor.h>
#include <wx/dataobj.h>
#include <wx/dcclient.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/fontpicker.h>
#include <wx/glcanvas.h>
#include <wx/menu.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/spinctrl.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>
#include <wx/utils.h>

#include <limits>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <unordered_map>

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

wxString showcase_preset_name(Showcase::PresetId id)
{
    switch (id) {
    case Showcase::PresetId::DeskWood: return _L("Desk wood");
    case Showcase::PresetId::DarkGradient: return _L("Dark gradient");
    case Showcase::PresetId::Outdoor: return _L("Outdoor");
    case Showcase::PresetId::BuildPlate: return _L("Build plate");
    default: return _L("Studio white");
    }
}

wxString showcase_light_rig_name(Showcase::LightRig rig)
{
    switch (rig) {
    case Showcase::LightRig::ClassicThreePoint: return _L("Classic three-point");
    case Showcase::LightRig::Dramatic: return _L("Dramatic (key and rim)");
    case Showcase::LightRig::KeyOnly: return _L("Key light only");
    case Showcase::LightRig::Sun: return _L("Hard sun");
    default: return _L("Soft three-point");
    }
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
    // S3: the filament callouts over the live view, their GL work and their mouse handling.
    m_renderer->set_callout_provider([this](int w, int h, ShowcaseImage &image) { return callouts_rasterize(w, h, image); },
                                     [this]() { return m_callouts.version * 1000003ull + m_co_video_rev; });
    m_renderer->set_before_frame([this]() { callouts_before_frame(); });
    m_renderer->set_mouse_handler([this](wxMouseEvent &evt) {
        // S4: a video being written owns the view; a preview or a peek ends at the first click or wheel.
        if (video_busy())
            return true;
        if (m_video.borrowed && (evt.ButtonDown() || evt.GetWheelRotation() != 0))
            video_restore_view();
        return callouts_on_mouse(evt);
    });
    // A callout drag holds the mouse; losing it ends the drag where it is.
    m_canvas_widget->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent &evt) {
        m_callouts.drag = CalloutUi::Drag::None;
        evt.Skip();
    });

    m_watermark_presets.load();
    load_callout_styles();
    m_video_timer.SetOwner(this);
    Bind(wxEVT_TIMER, &ShowcaseView::on_video_timer, this, m_video_timer.GetId());

    build_panel();

    // The sash on the panel's left edge: drag it to widen the panel, double-click it to fit the panel
    // to its contents again. The width is kept in tenths of an em, so it carries over between
    // displays of different scaling.
    const int em = wxGetApp().em_unit();
    m_side_width = std::max(0, std::atoi(wxGetApp().app_config->get("showcase_panel_width").c_str()) * em / 10);
    m_sash = new wxWindow(this, wxID_ANY, wxDefaultPosition, wxSize(std::max(4, em / 2), -1));
    m_sash->SetCursor(wxCursor(wxCURSOR_SIZEWE));
    m_sash->SetToolTip(_L("Drag to resize the panel. Double-click to fit it to its contents."));
    m_sash->Bind(wxEVT_PAINT, [this](wxPaintEvent &) {
        wxPaintDC dc(m_sash);
        // The colours of the Plater's sidebar sash.
        dc.SetBackground(wxBrush(wxGetApp().dark_mode() ? wxColour(38, 46, 48) : wxColour(206, 206, 206)));
        dc.Clear();
    });
    m_sash->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &e) {
        m_sash_start_x     = m_sash->ClientToScreen(e.GetPosition()).x;
        m_sash_start_width = m_side->GetSize().x;
        if (!m_sash->HasCapture())
            m_sash->CaptureMouse();
    });
    m_sash->Bind(wxEVT_MOTION, [this](wxMouseEvent &e) {
        if (!m_sash->HasCapture())
            return;
        // The panel is on the right: dragging left widens it. It never gets narrower than its contents.
        m_side_width = std::max(0, m_sash_start_width - (m_sash->ClientToScreen(e.GetPosition()).x - m_sash_start_x));
        fit_side_width();
        // Where the canvas' share stops it, remember the stop, so dragging back responds at once.
        m_side_width = std::min(m_side_width, m_side->GetMinSize().x);
    });
    auto save_side_width = [this]() {
        wxGetApp().app_config->set("showcase_panel_width", std::to_string(m_side_width * 10 / std::max(1, wxGetApp().em_unit())));
    };
    m_sash->Bind(wxEVT_LEFT_UP, [this, save_side_width](wxMouseEvent &) {
        if (!m_sash->HasCapture())
            return;
        m_sash->ReleaseMouse();
        save_side_width();
    });
    m_sash->Bind(wxEVT_MOUSE_CAPTURE_LOST, [](wxMouseCaptureLostEvent &) {});
    m_sash->Bind(wxEVT_LEFT_DCLICK, [this, save_side_width](wxMouseEvent &) {
        m_side_width = 0;
        fit_side_width();
        save_side_width();
    });

    auto *sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(m_canvas_widget, 1, wxEXPAND);
    sizer->Add(m_sash, 0, wxEXPAND);
    sizer->Add(m_side, 0, wxEXPAND);
    SetSizer(sizer);
    SetMinSize(GetSize());
    GetSizer()->SetSizeHints(this);
    // After the size hints: the view's own minimum stays the default panel width's.
    fit_side_width();
    Bind(wxEVT_SIZE, [this](wxSizeEvent &evt) {
        fit_side_width();
        evt.Skip();
    });

    Bind(wxEVT_IDLE, [this](wxIdleEvent &evt) {
        evt.Skip();
        on_idle_check();
    });

    wxGetApp().UpdateDarkUIWin(this);
}

ShowcaseView::~ShowcaseView()
{
    // S4: a video still being written stops (its partial file goes).
    m_video_timer.Stop();
    if (m_video.worker)
        m_video.worker->cancel();
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
    note_frame_height(scene());
    m_renderer->set_scene(&scene(), &project());
    m_renderer->invalidate_watermark();
    ++m_callouts.version;
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
    // S4: a preview's camera is borrowed, never the scene's.
    if (read_only() || m_video.borrowed)
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
    note_frame_height(s);
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
    // The model or the print settings may have changed in Prepare: new layer heights.
    m_renderer->invalidate_layers();
    refresh_plates();
    apply_scene_camera();
    // S3: the filament assignments may have changed too, and so may the slice's weights.
    callouts_sync(false);
    sync_controls();
}

void ShowcaseView::on_deactivate()
{
    if (!m_active)
        return;
    video_cancel();
    video_restore_view();
    store_scene_camera(true);
    m_active = false;
    // S5: the toolpath source borrowed Preview's viewer.
    if (m_renderer != nullptr)
        m_renderer->set_build(-1., Showcase::BuildSource::Auto);
    ShowcaseRenderer::release_toolpaths();
    m_callouts.drag = CalloutUi::Drag::None;
    if (m_canvas_widget != nullptr && m_canvas_widget->HasCapture())
        m_canvas_widget->ReleaseMouse();
    if (m_has_saved_camera) {
        // The editing views get their camera (and the preference hook) back exactly as they left it.
        wxGetApp().plater()->get_camera() = m_saved_camera;
        m_has_saved_camera = false;
    }
}

void ShowcaseView::on_project_changed()
{
    video_cancel();
    m_video.borrowed = false;   // the scene it belonged to is gone
    m_video.previewing = false;
    m_draft_plate = -1;
    if (m_renderer == nullptr)
        return;
    m_renderer->set_scene(&scene(), &project());
    m_renderer->invalidate_images();
    m_callouts.selected = -1;
    if (m_active) {
        m_plate = current_plate();
        refresh_plates();
        apply_scene_camera();
        callouts_sync(false);
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
    else
        refresh_materials(false);   // the filament list may have changed in the sidebar
}

// Another plate became current: its own scene, with its own camera.
void ShowcaseView::on_plate_switched()
{
    video_cancel();
    if (m_video.borrowed) {
        // The plate being left gets its own camera back, not the preview's.
        wxGetApp().plater()->get_camera() = m_video.saved_camera;
        m_video.borrowed   = false;
        m_video.previewing = false;
    }
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
    m_callouts.selected = -1;
    m_callouts.drag     = CalloutUi::Drag::None;
    refresh_plates();
    apply_scene_camera();
    callouts_sync(false);
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
    if (m_renderer != nullptr)
        m_renderer->invalidate_layers();
    m_canvas->reload_scene(refresh_immediately, force_full_scene_refresh);
}

void ShowcaseView::on_change_color_mode(bool is_dark)
{
    if (m_canvas != nullptr)
        m_canvas->on_change_color_mode(is_dark);
    wxGetApp().UpdateDarkUIWin(this);
    apply_section_header_colours();
    Refresh();
    if (m_sash != nullptr)
        m_sash->Refresh();
}

// ------------------------------------------------------------------------------------------------
// The side panel
// ------------------------------------------------------------------------------------------------

void ShowcaseView::build_panel()
{
    const int em = wxGetApp().em_unit();
    // 32 em is the default width; fit_side_width() widens the panel to its widest row and to what the
    // user drags the sash to. Sideways scrolling only shows where the window is too narrow for that.
    m_side = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxSize(32 * em, -1), wxVSCROLL | wxHSCROLL);
    m_side->SetScrollRate(em, em);
    auto *root  = new wxBoxSizer(wxVERTICAL);
    m_side_root = root;
    m_sections.clear();
    // Rows go into the current section's sizer (S5b: each section folds).
    wxBoxSizer *col = root;

    auto header = [this, root, &col](const wxString &title, const char *key) { col = add_panel_section(root, title, key); };
    wxFlexGridSizer *grid = nullptr;
    auto begin_grid = [&grid, &col, em]() {
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
    // For names the user picks (plates, saved views, watermark presets): a set width, so a long name
    // is cut short instead of widening the panel. Wider when the panel is.
    auto named_choice = [this, em]() { return new wxChoice(m_side, wxID_ANY, wxDefaultPosition, wxSize(12 * em, -1)); };
    auto slider = [this](int value, int lo, int hi) { return new wxSlider(m_side, wxID_ANY, value, lo, hi); };
    auto picker = [this]() { return new wxColourPickerCtrl(m_side, wxID_ANY, *wxWHITE); };
    // The scene's one environment, listed twice: in Background (the backdrop) and in Light (the lighting
    // behind a colour, gradient or picture backdrop). The baked ones, then the bundled HDRIs.
    auto env_choice = [choice]() {
        wxArrayString envs;
        envs.Add(_L("None"));
        for (const Showcase::EnvironmentInfo &e : showcase_environments().environments)
            envs.Add(showcase_environment_name(e));
        for (const Showcase::HdriInfo &h : showcase_hdris().hdris)
            envs.Add(showcase_hdri_name(h));
        return choice(envs);
    };

    m_notice = new wxStaticText(m_side, wxID_ANY, "");
    m_notice->Hide();
    col->Add(m_notice, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em);

    // --- Scene ---------------------------------------------------------------------------------
    header(_L("Scene"), "scene");
    begin_grid();
    m_plates = named_choice();
    row(_L("Plate"), m_plates);
    wxArrayString presets;
    for (Showcase::PresetId id : Showcase::builtin_presets())
        presets.Add(showcase_preset_name(id));
    m_presets = choice(presets);
    m_presets->SetToolTip(_L("Applies the preset's backdrop, floor, lights and environment. The camera, the materials and the export settings stay."));
    row(_L("Look"), m_presets);
    m_hide_plate = new wxCheckBox(m_side, wxID_ANY, _L("Hide the printer's plate"));
    m_hide_wipe_tower = new wxCheckBox(m_side, wxID_ANY, _L("Hide the prime tower"));
    grid->AddSpacer(0);
    grid->Add(m_hide_plate);
    grid->AddSpacer(0);
    grid->Add(m_hide_wipe_tower);

    // --- Background ----------------------------------------------------------------------------
    header(_L("Background"), "background");
    begin_grid();
    wxArrayString bg_types;
    bg_types.Add(_L("Solid colour"));
    bg_types.Add(_L("Vertical gradient"));
    bg_types.Add(_L("Radial gradient"));
    bg_types.Add(_L("Image"));
    bg_types.Add(_L("Transparent"));
    bg_types.Add(_L("Environment"));
    bg_types.Add(_L("Photo backplate"));
    m_bg_type = choice(bg_types);
    m_bg_type->SetToolTip(_L("Environment: the scene's lighting environment behind the model. Photo backplate: your own photo, "
                             "with the model's shadow on it."));
    row(_L("Type"), m_bg_type);
    m_environment = env_choice();
    m_environment->SetToolTip(_L("The surrounding shown behind the model. It also lights the model and shows in its reflections."));
    row(_L("Environment"), m_environment);
    // 1h-2: where the environment sits (the box first: it decides which rows below apply).
    m_bg_ground = new wxCheckBox(m_side, wxID_ANY, _L("Stand on the environment's floor"));
    m_bg_ground->SetToolTip(_L("Projects the environment onto a dome with a flat floor, so the model stands on its floor "
                               "and casts its shadow there."));
    grid->AddSpacer(0);
    grid->Add(m_bg_ground);
    m_bg_ground_height = new wxSpinCtrl(m_side, wxID_ANY, "150", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 10, 20000, 150);
    m_bg_ground_height->SetToolTip(_L("How high the environment's camera stood above its floor, at the model's scale. Lower makes the "
                                      "surroundings larger."));
    row(_L("Camera height (mm)"), m_bg_ground_height);
    m_bg_ground_radius = new wxSpinCtrl(m_side, wxID_ANY, "1500", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 50, 1000000, 1500);
    m_bg_ground_radius->SetToolTip(_L("How far away the walls and the horizon are."));
    row(_L("Dome radius (mm)"), m_bg_ground_radius);
    m_bg_ground_scale = slider(100, 25, 400);
    m_bg_ground_scale->SetToolTip(_L("Makes the whole surrounding larger or smaller around the model."));
    row(_L("World scale"), m_bg_ground_scale);
    m_bg_pitch = slider(0, -45, 45);
    m_bg_pitch->SetToolTip(_L("Tilts the backdrop: right shows more sky, left more floor. The lighting does not move."));
    row(_L("Backdrop tilt"), m_bg_pitch);
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
    // 1h-2: the photo backplate's camera match.
    m_plate_horizon = slider(50, 0, 100);
    m_plate_horizon->SetToolTip(_L("Where the photo's horizon is, from the bottom of the frame. The live view draws it as a line."));
    row(_L("Photo horizon"), m_plate_horizon);
    m_plate_focal = new wxSpinCtrl(m_side, wxID_ANY, "50", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 8, 600, 50);
    m_plate_focal->SetToolTip(_L("The focal length the photo was taken with (35 mm equivalent; a phone's main camera is about 26 mm)."));
    row(_L("Photo lens (mm)"), m_plate_focal);
    m_plate_match = new wxButton(m_side, wxID_ANY, _L("Match camera"));
    m_plate_match->SetToolTip(_L("Turns the camera so its horizon and lens match the photo. The model keeps its size."));
    grid->AddSpacer(0);
    grid->Add(m_plate_match);
    m_bg_blur = slider(0, 0, 100);
    m_bg_blur->SetToolTip(_L("Softens the image or the environment behind the model, like a shallow depth of field."));
    row(_L("Blur"), m_bg_blur);

    // --- Floor ---------------------------------------------------------------------------------
    header(_L("Floor"), "floor");
    begin_grid();
    wxArrayString floor_types;
    floor_types.Add(_L("None"));
    floor_types.Add(_L("Shadow only"));
    floor_types.Add(_L("Solid colour"));
    floor_types.Add(_L("Grid"));
    floor_types.Add(_L("Photo"));
    floor_types.Add(_L("My picture"));
    m_floor_type = choice(floor_types);
    m_floor_type->SetToolTip(_L("Shadow only: an invisible floor that shows just the shadow, over any background."));
    row(_L("Type"), m_floor_type);
    m_floor_color = picker();
    row(_L("Colour"), m_floor_color);
    m_floor_shadow = slider(45, 0, 100);
    row(_L("Shadow"), m_floor_shadow);
    m_floor_grid = new wxSpinCtrl(m_side, wxID_ANY, "10", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 100, 10);
    row(_L("Grid spacing (mm)"), m_floor_grid);
    wxArrayString floor_textures;
    for (const Showcase::FloorTextureInfo &f : showcase_floors().floors)
        floor_textures.Add(showcase_floor_name(f));
    m_floor_texture = choice(floor_textures);
    row(_L("Photo"), m_floor_texture);
    m_floor_image = new wxButton(m_side, wxID_ANY, _L("Choose picture..."));
    row(_L("Picture"), m_floor_image);
    {
        wxArrayString turns;
        for (const char *t : { "0", "90", "180", "270" })
            turns.Add(wxString::FromUTF8(t));
        m_floor_rotation = choice(turns);
        m_floor_rotation->SetToolTip(_L("Turn the photo or picture counter-clockwise, seen from above. The top of the picture points to the back of the plate at 0."));
        row(_L("Rotate"), m_floor_rotation);
    }
    m_floor_flip = new wxCheckBox(m_side, wxID_ANY, _L("Flip left to right"));
    grid->AddSpacer(0);
    grid->Add(m_floor_flip);
    m_floor_tile = new wxSpinCtrl(m_side, wxID_ANY, "300", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 10, 5000, 300);
    m_floor_tile->SetToolTip(_L("How large one copy of the photo or picture is on the floor."));
    row(_L("Tile size (mm)"), m_floor_tile);
    m_floor_reflection = slider(0, 0, 100);
    m_floor_reflection->SetToolTip(_L("A glossy floor: how strongly the model is mirrored in it."));
    row(_L("Reflection"), m_floor_reflection);
    m_floor_contact = slider(60, 0, 100);
    m_floor_contact->SetToolTip(_L("The soft shade right under the model where it touches the floor. Kept in transparent exports."));
    row(_L("Contact shadow"), m_floor_contact);

    // --- Light ---------------------------------------------------------------------------------
    header(_L("Light"), "light");
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
    wxArrayString rigs;
    for (Showcase::LightRig rig : Showcase::light_rigs())
        rigs.Add(showcase_light_rig_name(rig));
    m_light_rig = choice(rigs);
    m_light_rig->SetToolTip(_L("Places the fill and rim lights around the key light and sets how soft the shadows are."));
    row(_L("Setup"), m_light_rig);
    m_key_intensity = slider(100, 0, 300);
    row(_L("Key brightness"), m_key_intensity);
    m_key_temperature = slider(56, 20, 100);
    m_key_temperature->SetToolTip(_L("Colour of the key light: warm on the left, cool on the right (2000 to 10000 K)."));
    row(_L("Key colour"), m_key_temperature);
    m_fill = new wxCheckBox(m_side, wxID_ANY, _L("Fill light"));
    m_fill->SetToolTip(_L("A soft light from the other side that lifts the shadows."));
    m_fill_intensity = slider(35, 0, 300);
    grid->Add(m_fill, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(m_fill_intensity, 1, wxEXPAND);
    m_rim = new wxCheckBox(m_side, wxID_ANY, _L("Rim light"));
    m_rim->SetToolTip(_L("A light from behind that outlines the model."));
    m_rim_intensity = slider(45, 0, 300);
    grid->Add(m_rim, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(m_rim_intensity, 1, wxEXPAND);
    m_shadow_softness = slider(10, 5, 40);
    row(_L("Shadow softness"), m_shadow_softness);
    // 1h-1: the HDRI environments follow the baked ones (their ids share the scene's environment field).
    // Shown while the backdrop is not the environment; with the Environment backdrop the Background
    // section's picker is this same value.
    m_light_env = env_choice();
    m_light_env->SetToolTip(_L("Light and reflections from a photographed surrounding, behind the colour, gradient or picture."));
    row(_L("Lighting environment"), m_light_env);
    m_env_intensity = slider(70, 0, 300);
    row(_L("Environment brightness"), m_env_intensity);
    m_env_rotation = slider(0, 0, 359);
    row(_L("Environment rotation"), m_env_rotation);
    m_sun_light = new wxCheckBox(m_side, wxID_ANY, _L("Use the environment's sun as the key light"));
    m_sun_light->SetToolTip(_L("For an HDRI environment: the brightest light in the panorama becomes the key light, so the "
                               "highlights and the shadows agree with the backdrop. It turns with the environment rotation."));
    row(_L("Sun"), m_sun_light);
    m_exposure = slider(0, -30, 30);
    m_exposure->SetToolTip(_L("Brightness of the model and the environment, in tenths of a stop."));
    row(_L("Exposure"), m_exposure);
    // Realism 1a (D-R1): Neutral keeps filament colours true; Filmic rolls bright highlights off
    // softly, like a camera. The order follows Showcase::ToneCurve.
    wxArrayString tones;
    tones.Add(_L("Neutral"));
    tones.Add(_L("Filmic"));
    m_tone = choice(tones);
    m_tone->SetToolTip(_L("How light is turned into the picture's colours. Neutral keeps the filament colours true; "
                          "Filmic rolls very bright highlights off softly, like a camera."));
    row(_L("Tone"), m_tone);

    // --- Materials (D4) -----------------------------------------------------------------------
    header(_L("Materials"), "materials");
    begin_grid();
    // First in the section: it decides whether Layer lines or the smoothing rows apply (a row never
    // hides behind a row below it).
    // S5: the print's real extrusions instead of the model (once the plate is sliced), with D13's
    // smoothing: rounded beads and blended top skin. Showcase only; Preview keeps its look.
    wxArrayString sources;
    sources.Add(_L("Model"));
    sources.Add(_L("Toolpaths (sliced plate)"));
    m_source = choice(sources);
    m_source->SetToolTip(_L("Toolpaths show the plate's slice as it will print: seams, the top pattern, supports and colour changes. "
                            "Slice the plate first; until then the model is shown."));
    row(_L("Show"), m_source);
    wxArrayString layer_lines;
    layer_lines.Add(_L("Off"));
    layer_lines.Add(_L("Subtle"));
    layer_lines.Add(_L("Real"));
    layer_lines.Add(_L("Exaggerated"));
    m_layer_lines = choice(layer_lines);
    m_layer_lines->SetToolTip(_L("Printed layer lines on the model at its real layer height. A layer is often thinner than a pixel "
                                 "when the whole model is in view, so the lines show in close-ups and in large, high quality exports. "
                                 "Model only: the toolpaths view already shows the real layers."));
    row(_L("Layer lines"), m_layer_lines);
    // Multi-colour P2 (design 4.4): the coextruded look of dual- and tri-colour filaments.
    m_coextruded = new wxCheckBox(m_side, wxID_ANY, _L("Coextruded colours"));
    m_coextruded->SetToolTip(_L("Shows a dual- or tri-colour filament the way it prints: its colours side by side, with the "
                                "proportions and rotation set in the filament's colour dialog. Off: one plain colour per filament. "
                                "Before slicing the model shows an approximation: the walls are right, but the top stripes "
                                "are not at the real line positions, and seams and ironing are not shown. "
                                "Visual only; the G-code is not affected."));
    grid->AddSpacer(0);
    grid->Add(m_coextruded);
    // Glow-in-the-dark G1 (design section 10): the lights-off view.
    m_lights_off = new wxCheckBox(m_side, wxID_ANY, _L("Lights off (glow)"));
    m_lights_off->SetToolTip(_L("Turns the lights out. Filaments go dim; filaments set to glow in their colour dialog shine in their "
                                "glow colour. Visual only; the G-code is not affected."));
    grid->AddSpacer(0);
    grid->Add(m_lights_off);
    m_lights_ambient = slider(5, 0, 50);
    m_lights_ambient->SetToolTip(_L("How much of the light is left with the lights off, in percent. Applies to the filaments, "
                                    "the floor and the backdrop."));
    row(_L("Light left"), m_lights_ambient);
    m_glow_intensity = slider(100, 0, 300);
    m_glow_intensity->SetToolTip(_L("How strongly every glow filament shines, in percent. Saved with the project; Preview uses it too."));
    row(_L("Glow intensity"), m_glow_intensity);
    m_smooth_auto = new wxCheckBox(m_side, wxID_ANY, _L("By finish"));
    m_smooth_auto->SetToolTip(_L("Each filament's finish picks how smooth its toolpaths look (silk smoothest, matte least)."));
    m_smooth = slider(60, 0, 100);
    m_smooth->SetToolTip(_L("Toolpaths only. Left: crisp extrusions as in Preview. Right: rounded beads, and top and bottom "
                            "skin widened and flattened into one surface."));
    grid->Add(m_smooth_auto, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(m_smooth, 1, wxEXPAND);
    {
        auto *hint = new wxStaticText(m_side, wxID_ANY, _L("The finish of each filament is guessed from its preset name and type. "
                                                           "Pick another one where the guess is wrong."));
        hint->Wrap(29 * em);
        col->Add(hint, 0, wxLEFT | wxRIGHT, em);
    }
    begin_grid();
    m_mat_grid = grid;

    // --- Camera --------------------------------------------------------------------------------
    header(_L("Camera"), "camera");
    begin_grid();
    wxArrayString projections;
    projections.Add(_L("Perspective"));
    projections.Add(_L("Orthographic"));
    m_projection = choice(projections);
    row(_L("Projection"), m_projection);
    // 1h-2: an action (the selection goes back to empty): the camera moves closer or further so the
    // frame has that lens's angle of view, the model keeping its size.
    wxArrayString lenses;
    for (int mm : { 24, 35, 50, 85, 135 })
        lenses.Add(wxString::Format(_L("%d mm"), mm));
    m_lens = choice(lenses);
    m_lens->SetToolTip(_L("A wide lens shows more of the surroundings, a long one less. The model keeps its size in the frame."));
    row(_L("Lens"), m_lens);
    m_views = named_choice();
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
    // 1h-2: depth of field.
    begin_grid();
    m_dof = new wxCheckBox(m_side, wxID_ANY, _L("Depth of field"));
    m_dof->SetToolTip(_L("Blurs what is in front of or behind the focus, like a camera lens."));
    grid->AddSpacer(0);
    grid->Add(m_dof);
    wxArrayString stops;
    for (const char *f : { "1.4", "2", "2.8", "4", "5.6", "8", "11", "16", "22" })
        stops.Add(wxString::FromUTF8("f/") + wxString::FromUTF8(f));
    m_dof_fnumber = choice(stops);
    m_dof_fnumber->SetToolTip(_L("A smaller number blurs more."));
    row(_L("Aperture"), m_dof_fnumber);
    m_dof_on_model = new wxCheckBox(m_side, wxID_ANY, _L("Focus on the model"));
    grid->AddSpacer(0);
    grid->Add(m_dof_on_model);
    m_dof_focus = new wxSpinCtrl(m_side, wxID_ANY, "300", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 10, 100000, 300);
    m_dof_focus->SetToolTip(_L("How far from the camera the sharpest plane is."));
    row(_L("Focus distance (mm)"), m_dof_focus);
    wxArrayString bokeh;
    bokeh.Add(_L("Draft"));
    bokeh.Add(_L("Good"));
    bokeh.Add(_L("Best"));
    m_dof_quality = choice(bokeh);
    m_dof_quality->SetToolTip(_L("Smoother blur, slower to render."));
    row(_L("Bokeh quality"), m_dof_quality);

    // --- Callouts (S3) ------------------------------------------------------------------------
    header(_L("Callouts"), "callouts");
    begin_grid();
    m_co_enabled = new wxCheckBox(m_side, wxID_ANY, _L("Show filament callouts"));
    m_co_enabled->SetToolTip(_L("Labels that name each filament on the model: made from the filaments of objects, parts, "
                                "painted areas, height ranges and layer changes."));
    grid->AddSpacer(0);
    grid->Add(m_co_enabled);
    wxArrayString co_layouts;
    co_layouts.Add(_L("Automatic"));
    co_layouts.Add(_L("Columns"));
    co_layouts.Add(_L("Numbered legend"));
    co_layouts.Add(_L("Rows above and below"));
    m_co_layout = choice(co_layouts);
    m_co_layout->SetToolTip(_L("Automatic: labels in columns left and right of the model, in rows above and below a wide, flat "
                               "model, a numbered legend when there are too many."));
    row(_L("Layout"), m_co_layout);
    m_co_once = new wxCheckBox(m_side, wxID_ANY, _L("Each colour once"));
    m_co_once->SetToolTip(_L("One callout per colour on the plate, with one leader to where that colour shows most, instead of a "
                             "callout per filament with a leader to each place it shows."));
    grid->AddSpacer(0);
    grid->Add(m_co_once);
    wxArrayString co_styles;
    for (Showcase::CalloutStylePreset preset : Showcase::callout_style_presets()) {
        switch (preset) {
        case Showcase::CalloutStylePreset::CardLight: co_styles.Add(_L("Card, light")); break;
        case Showcase::CalloutStylePreset::CardDark: co_styles.Add(_L("Card, dark")); break;
        case Showcase::CalloutStylePreset::Minimal: co_styles.Add(_L("Minimal")); break;
        case Showcase::CalloutStylePreset::Pill: co_styles.Add(_L("Pill")); break;
        case Showcase::CalloutStylePreset::Technical: co_styles.Add(_L("Technical")); break;
        }
    }
    m_co_style = choice(co_styles);
    m_co_style->SetToolTip(_L("Sets the label shape, colours, leaders and size. A built-in style keeps your font; a style you saved "
                              "brings its own."));
    row(_L("Style"), m_co_style);
    {
        // S4: the user's own styles, kept across projects.
        auto *buttons    = new wxBoxSizer(wxHORIZONTAL);
        m_co_style_save   = new wxButton(m_side, wxID_ANY, _L("Save style..."));
        m_co_style_delete = new wxButton(m_side, wxID_ANY, _L("Delete style"));
        buttons->Add(m_co_style_save, 0, wxRIGHT, em / 2);
        buttons->Add(m_co_style_delete, 0);
        grid->AddSpacer(0);
        grid->Add(buttons);
        m_co_style_save->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_save_callout_style(); });
        m_co_style_delete->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_delete_callout_style(); });
    }
    m_co_font = new wxFontPickerCtrl(m_side, wxID_ANY, wxFont(wxFontInfo(12)), wxDefaultPosition, wxSize(12 * em, -1),
                                     wxFNTP_FONTDESC_AS_LABEL);
    row(_L("Font"), m_co_font);
    m_co_size = slider(22, 10, 60);
    m_co_size->SetToolTip(_L("Text height, as a share of the image height: the labels look the same at every export size."));
    row(_L("Text size"), m_co_size);
    wxArrayString co_leaders;
    co_leaders.Add(_L("Straight"));
    co_leaders.Add(_L("Elbow"));
    m_co_leader = choice(co_leaders);
    row(_L("Leader lines"), m_co_leader);
    m_co_auto_colors = new wxCheckBox(m_side, wxID_ANY, _L("Colours that stand out from the background"));
    grid->AddSpacer(0);
    grid->Add(m_co_auto_colors);
    m_co_text = picker();
    row(_L("Text colour"), m_co_text);
    m_co_line = picker();
    row(_L("Line colour"), m_co_line);
    m_co_bg = picker();
    row(_L("Label background"), m_co_bg);
    m_co_bg_opacity = slider(92, 0, 100);
    row(_L("Background opacity"), m_co_bg_opacity);
    {
        // D7: brand, product, colour name, hex, swatch, type and finish on; family, weight and price off.
        auto *fields = new wxFlexGridSizer(2, em / 4, em);
        const wxString names[] = { _L("Brand"), _L("Product"), _L("Material family"), _L("Colour name"), _L("Hex code"), _L("Swatch"),
                                   _L("Type and finish"), _L("Weight"), _L("Price") };
        for (const wxString &name : names) {
            auto *cb = new wxCheckBox(m_side, wxID_ANY, name);
            fields->Add(cb);
            m_co_fields.push_back(cb);
        }
        m_co_fields[2]->SetToolTip(_L("Just the material, such as PLA, PETG or ABS. With Product on, the product name is shown instead "
                                      "(it names the material too)."));
        m_co_fields[7]->SetToolTip(_L("Filament used, from the plate's slice. Shown once the plate is sliced."));
        m_co_fields[8]->SetToolTip(_L("Filament cost, from the plate's slice and your filament prices. Shown once the plate is sliced."));
        grid->Add(new wxStaticText(m_side, wxID_ANY, _L("Show")), 0, wxALIGN_TOP | wxTOP, em / 4);
        grid->Add(fields, 1, wxEXPAND);
    }
    m_co_separator = new wxTextCtrl(m_side, wxID_ANY, "", wxDefaultPosition, wxSize(6 * em, -1));
    m_co_separator->SetToolTip(_L("What goes between the name and the colour name on the first line. Spaces count."));
    row(_L("Separator"), m_co_separator);
    m_co_list = named_choice();
    row(_L("Callout"), m_co_list);
    {
        auto *buttons = new wxBoxSizer(wxHORIZONTAL);
        m_co_edit = new wxButton(m_side, wxID_ANY, _L("Edit..."));
        m_co_hide = new wxButton(m_side, wxID_ANY, _L("Hide"));
        buttons->Add(m_co_edit, 0, wxRIGHT, em / 2);
        buttons->Add(m_co_hide, 0);
        col->Add(buttons, 0, wxLEFT | wxRIGHT, em);
        auto *more = new wxBoxSizer(wxHORIZONTAL);
        m_co_regenerate = new wxButton(m_side, wxID_ANY, _L("Regenerate"));
        m_co_regenerate->SetToolTip(_L("Finds the filaments again and places every callout you did not place by hand on what the view shows now."));
        m_co_unpin = new wxButton(m_side, wxID_ANY, _L("Unpin all"));
        m_co_unpin->SetToolTip(_L("Returns every label you dragged to the automatic layout."));
        more->Add(m_co_regenerate, 0, wxRIGHT, em / 2);
        more->Add(m_co_unpin, 0);
        col->Add(more, 0, wxLEFT | wxRIGHT | wxTOP, em / 2);
        m_co_hint = new wxStaticText(m_side, wxID_ANY, _L("On the view: drag a label to move it, drag its dot to another spot on the model, "
                                                          "double-click a label to edit it, right-click it for more."));
        m_co_hint->Wrap(29 * em);
        col->Add(m_co_hint, 0, wxLEFT | wxRIGHT | wxTOP, em);
        m_co_edit->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { callouts_edit(m_callouts.selected); });
        m_co_hide->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
            const int item = m_callouts.selected;
            edit([item](Showcase::Scene &s) {
                if (item >= 0 && size_t(item) < s.callouts.items.size())
                    s.callouts.items[size_t(item)].hidden = !s.callouts.items[size_t(item)].hidden;
            });
            refresh_callout_list();
        });
        m_co_regenerate->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { callouts_sync(true); });
        m_co_unpin->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
            edit([](Showcase::Scene &s) {
                for (Showcase::Callout &c : s.callouts.items)
                    c.pinned = false;
            });
        });
    }

    // --- Video (S4) --------------------------------------------------------------------------------
    build_video_section(root);

    // --- Export --------------------------------------------------------------------------------
    header(_L("Export"), "export");
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
    formats.Add(_L("WebP (lossless)"));
    m_format = choice(formats);
    row(_L("Format"), m_format);
    m_use_as_cover = new wxCheckBox(m_side, wxID_ANY, _L("Use as project cover"));
    m_use_as_cover->SetToolTip(_L("Each export also becomes the cover picture of the project file. Off unless you turn it on."));
    grid->AddSpacer(0);
    grid->Add(m_use_as_cover);
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
    header(_L("Watermark"), "watermark");
    begin_grid();
    m_wm_enabled = new wxCheckBox(m_side, wxID_ANY, _L("Add my watermark"));
    m_wm_enabled->SetToolTip(_L("Your own logo and/or text on the exported images. Off unless you turn it on."));
    grid->AddSpacer(0);
    grid->Add(m_wm_enabled);
    m_wm_presets = named_choice();
    row(_L("Preset"), m_wm_presets);
    m_wm_text = new wxTextCtrl(m_side, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    row(_L("Text"), m_wm_text);
    // A set width too: the button shows the font's name, which can be long.
    m_wm_font = new wxFontPickerCtrl(m_side, wxID_ANY, wxFont(wxFontInfo(12)), wxDefaultPosition, wxSize(12 * em, -1),
                                     wxFNTP_FONTDESC_AS_LABEL);
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

    m_side->SetSizer(root);
    index_panel_rows();
    // S5c: the panel's width is that of its widest row, folded or not, so it never jumps.
    m_side_content_w = root->CalcMin().x;
    apply_panel_sections();
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
        const Showcase::BackgroundType type = Showcase::BackgroundType(sel);
        if ((type == Showcase::BackgroundType::Image || type == Showcase::BackgroundType::Backplate) && scene().background.image.empty()) {
            on_choose_background_image(type);
            sync_controls();
            return;
        }
        edit([type](Showcase::Scene &s) {
            s.background.type = type;
            // 1h-2: the photo is the floor: the plate goes and the floor only catches the shadow.
            if (type == Showcase::BackgroundType::Backplate) {
                s.floor.hide_plate = true;
                if (s.floor.type != Showcase::FloorType::None)
                    s.floor.type = Showcase::FloorType::ShadowCatcher;
            }
        }, true);
    });
    m_bg_ground->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) {
        const bool on = m_bg_ground->GetValue();
        edit([on](Showcase::Scene &s) {
            s.background.ground = on;
            // Turned on: automatic sizes; the environment's floor is the floor, catching the shadow.
            if (on) {
                s.background.ground_height = 0.f;
                s.background.ground_radius = 0.f;
                s.background.ground_scale  = 1.f;
                s.floor.hide_plate         = true;
                if (s.floor.type != Showcase::FloorType::None)
                    s.floor.type = Showcase::FloorType::ShadowCatcher;
            }
        }, true);
    });
    m_bg_ground_height->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent &) {
        const float mm = float(m_bg_ground_height->GetValue());
        edit([mm](Showcase::Scene &s) { s.background.ground_height = mm; });
    });
    m_bg_ground_radius->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent &) {
        const float mm = float(m_bg_ground_radius->GetValue());
        edit([mm](Showcase::Scene &s) { s.background.ground_radius = mm; });
    });
    m_bg_ground_scale->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
        const float k = float(m_bg_ground_scale->GetValue()) / 100.f;
        edit([k](Showcase::Scene &s) { s.background.ground_scale = k; });
    });
    m_bg_pitch->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
        const float deg = float(m_bg_pitch->GetValue());
        edit([deg](Showcase::Scene &s) { s.background.backdrop_pitch = deg; });
    });
    m_plate_horizon->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
        const float h = float(m_plate_horizon->GetValue()) / 100.f;
        edit([h](Showcase::Scene &s) { s.background.plate_horizon = h; });
    });
    m_plate_focal->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent &) {
        const float mm = float(m_plate_focal->GetValue());
        edit([mm](Showcase::Scene &s) { s.background.plate_focal = mm; });
    });
    m_plate_match->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        const Showcase::Scene &s = scene();
        const Showcase::RectI  f = m_renderer->live_frame();
        if (f.empty())
            return;
        const double fov_y = Showcase::fov_y_from_focal(double(s.background.plate_focal), double(f.w) / double(f.h));
        const double pitch = Showcase::pitch_for_horizon(double(s.background.plate_horizon), fov_y);
        apply_camera_lens(double(s.background.plate_focal), &pitch);
    });
    m_bg_color1->Bind(wxEVT_COLOURPICKER_CHANGED, [this](wxColourPickerEvent &) {
        edit([this](Showcase::Scene &s) { s.background.color1 = showcase_hex(m_bg_color1->GetColour()); });
    });
    m_bg_color2->Bind(wxEVT_COLOURPICKER_CHANGED, [this](wxColourPickerEvent &) {
        edit([this](Showcase::Scene &s) { s.background.color2 = showcase_hex(m_bg_color2->GetColour()); });
    });
    m_bg_image->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        on_choose_background_image(scene().background.type == Showcase::BackgroundType::Backplate ? Showcase::BackgroundType::Backplate :
                                                                                                     Showcase::BackgroundType::Image);
    });
    m_bg_fit->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        const int sel = m_bg_fit->GetSelection();
        if (sel >= 0)
            edit([sel](Showcase::Scene &s) { s.background.fit = Showcase::ImageFit(sel); });
    });
    m_bg_blur->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
        edit([this](Showcase::Scene &s) { s.background.blur = float(m_bg_blur->GetValue()) / 100.f; });
    });

    m_floor_type->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        const int sel = m_floor_type->GetSelection();
        if (sel < 0)
            return;
        if (Showcase::FloorType(sel) == Showcase::FloorType::Image && scene().floor.image.empty()) {
            on_choose_floor_image();
            sync_controls();
            return;
        }
        edit([sel](Showcase::Scene &s) { s.floor.type = Showcase::FloorType(sel); }, true);
    });
    m_floor_texture->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        const int sel = m_floor_texture->GetSelection();
        const std::vector<Showcase::FloorTextureInfo> &floors = showcase_floors().floors;
        if (sel < 0 || size_t(sel) >= floors.size())
            return;
        const std::string id = floors[size_t(sel)].id;
        edit([&id](Showcase::Scene &s) {
            s.floor.texture = id;
            s.floor.type    = Showcase::FloorType::Texture;
        }, true);
    });
    m_floor_image->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_choose_floor_image(); });
    m_floor_rotation->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        const int sel = m_floor_rotation->GetSelection();
        if (sel >= 0 && sel < 4)
            edit([sel](Showcase::Scene &s) { s.floor.rotation = sel * 90; });
    });
    m_floor_flip->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { edit([this](Showcase::Scene &s) { s.floor.flip = m_floor_flip->GetValue(); }); });
    m_floor_tile->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent &) {
        const float mm = float(m_floor_tile->GetValue());
        edit([mm](Showcase::Scene &s) {
            if (s.floor.type == Showcase::FloorType::Image)
                s.floor.texture_size = mm;
            else if (const Showcase::FloorTextureInfo *f = showcase_floors().find(s.floor.texture))
                s.floor.texture_scale = std::clamp(mm / f->tile_mm, 0.1f, 10.f);
        });
    });
    m_floor_reflection->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
        edit([this](Showcase::Scene &s) { s.floor.reflection = float(m_floor_reflection->GetValue()) / 100.f; });
    });
    m_floor_contact->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
        edit([this](Showcase::Scene &s) { s.floor.contact_shadow = float(m_floor_contact->GetValue()) / 100.f; });
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
    m_light_rig->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        const int sel = m_light_rig->GetSelection();
        const std::vector<Showcase::LightRig> rigs = Showcase::light_rigs();
        if (sel >= 0 && size_t(sel) < rigs.size())
            edit([&rigs, sel](Showcase::Scene &s) { Showcase::apply_light_rig(s.lighting, rigs[size_t(sel)]); }, true);
    });
    m_key_intensity->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
        edit([this](Showcase::Scene &s) { s.lighting.key_intensity = float(m_key_intensity->GetValue()) / 100.f; });
    });
    m_key_temperature->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
        edit([this](Showcase::Scene &s) { s.lighting.key_temperature = float(m_key_temperature->GetValue()) * 100.f; });
    });
    m_fill->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { edit([this](Showcase::Scene &s) { s.lighting.fill = m_fill->GetValue(); }); });
    m_fill_intensity->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
        edit([this](Showcase::Scene &s) { s.lighting.fill_intensity = float(m_fill_intensity->GetValue()) / 100.f; });
    });
    m_rim->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { edit([this](Showcase::Scene &s) { s.lighting.rim = m_rim->GetValue(); }); });
    m_rim_intensity->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
        edit([this](Showcase::Scene &s) { s.lighting.rim_intensity = float(m_rim_intensity->GetValue()) / 100.f; });
    });
    m_shadow_softness->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
        edit([this](Showcase::Scene &s) { s.lighting.shadow_softness = float(m_shadow_softness->GetValue()) / 10.f; });
    });
    for (wxChoice *envc : { m_environment, m_light_env })
    envc->Bind(wxEVT_CHOICE, [this, envc](wxCommandEvent &) {
        const int sel = envc->GetSelection();
        const std::vector<Showcase::EnvironmentInfo> &envs = showcase_environments().environments;
        const std::vector<Showcase::HdriInfo>        &hdris = showcase_hdris().hdris;
        if (sel < 0)
            return;
        std::string id;
        if (sel >= 1 && size_t(sel) <= envs.size())
            id = envs[size_t(sel - 1)].id;
        else if (sel >= 1 && size_t(sel) <= envs.size() + hdris.size())
            id = hdris[size_t(sel) - 1 - envs.size()].id;
        // Choosing an HDRI turns its sun on as the key light; choosing anything else turns it off.
        const Showcase::HdriInfo *info = showcase_hdris().find(id);
        const bool                hdri = info != nullptr;
        // 1h-2: and its backdrop tilt (the camera usually looks down, into the panorama's floor).
        const float pitch = info != nullptr ? info->backdrop_pitch : 0.f;
        edit([&id, hdri, pitch](Showcase::Scene &s) {
            s.lighting.environment     = id;
            s.lighting.sun_from_env    = hdri;
            s.background.backdrop_pitch = pitch;
        }, true);
    });
    m_sun_light->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { edit([this](Showcase::Scene &s) { s.lighting.sun_from_env = m_sun_light->GetValue(); }); });
    m_env_intensity->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
        edit([this](Showcase::Scene &s) { s.lighting.env_intensity = float(m_env_intensity->GetValue()) / 100.f; });
    });
    m_env_rotation->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
        edit([this](Showcase::Scene &s) { s.lighting.env_rotation_deg = float(m_env_rotation->GetValue()); });
    });
    m_exposure->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
        edit([this](Showcase::Scene &s) { s.lighting.exposure = float(m_exposure->GetValue()) / 10.f; });
    });
    m_tone->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        const int sel = m_tone->GetSelection();
        if (sel >= 0)
            edit([sel](Showcase::Scene &s) { s.lighting.tone = sel == 1 ? Showcase::ToneCurve::Filmic : Showcase::ToneCurve::Neutral; });
    });
    m_layer_lines->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        const int sel = m_layer_lines->GetSelection();
        if (sel >= 0)
            edit([sel](Showcase::Scene &s) { s.materials.layer_lines = Showcase::LayerLines(sel); });
    });
    m_coextruded->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) {
        const bool on = m_coextruded->GetValue();
        edit([on](Showcase::Scene &s) { s.materials.coextruded_look = on; });
    });
    m_lights_off->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) {
        const bool on = m_lights_off->GetValue();
        edit([on](Showcase::Scene &s) { s.materials.lights_off = on; });
    });
    m_lights_ambient->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
        const float level = float(m_lights_ambient->GetValue()) / 100.f;
        edit([level](Showcase::Scene &s) { s.materials.lights_off_ambient = level; });
    });
    m_glow_intensity->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
        if (m_syncing || read_only())
            return;
        PresetBundle *bundle = wxGetApp().preset_bundle;
        if (bundle == nullptr)
            return;
        bundle->project_config.set_key_value("filament_glow_intensity", new ConfigOptionFloat(double(m_glow_intensity->GetValue())));
        if (Plater *plater = wxGetApp().plater())
            plater->update_project_dirty_from_presets();
        set_as_dirty();
    });
    m_source->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        const int sel = m_source->GetSelection();
        if (sel >= 0)
            edit([sel](Showcase::Scene &s) { s.source = sel == 1 ? "toolpaths" : "model"; });
    });
    m_smooth_auto->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) {
        const bool by_finish = m_smooth_auto->GetValue();
        const int  v         = m_smooth->GetValue();
        edit([by_finish, v](Showcase::Scene &s) { s.materials.toolpath_smoothness = by_finish ? -1.f : float(v) / 100.f; });
    });
    m_smooth->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
        const int v = m_smooth->GetValue();
        edit([v](Showcase::Scene &s) { s.materials.toolpath_smoothness = float(v) / 100.f; });
    });

    m_lens->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        const int sel = m_lens->GetSelection();
        const int mm[] = { 24, 35, 50, 85, 135 };
        m_lens->SetSelection(-1);   // an action, not a state
        if (sel >= 0 && sel < 5)
            apply_camera_lens(double(mm[sel]));
    });
    m_dof->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) {
        const bool on = m_dof->GetValue();
        edit([on](Showcase::Scene &s) { s.dof.enabled = on; });
    });
    m_dof_fnumber->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        const int   sel     = m_dof_fnumber->GetSelection();
        const float stops[] = { 1.4f, 2.f, 2.8f, 4.f, 5.6f, 8.f, 11.f, 16.f, 22.f };
        if (sel >= 0 && sel < 9)
            edit([&stops, sel](Showcase::Scene &s) { s.dof.f_number = stops[sel]; });
    });
    m_dof_on_model->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) {
        const bool on = m_dof_on_model->GetValue();
        edit([on](Showcase::Scene &s) { s.dof.focus_on_model = on; });
    });
    m_dof_focus->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent &) {
        const float mm = float(m_dof_focus->GetValue());
        edit([mm](Showcase::Scene &s) { s.dof.focus_mm = mm; });
    });
    m_dof_quality->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        const int sel = m_dof_quality->GetSelection();
        if (sel >= 0 && sel < 3)
            edit([sel](Showcase::Scene &s) { s.dof.quality = Showcase::BokehQuality(sel); });
    });
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
    m_use_as_cover->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) {
        edit([this](Showcase::Scene &s) { s.export_settings.use_as_cover = m_use_as_cover->GetValue(); });
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

    // S3: callouts.
    m_co_enabled->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) {
        edit([this](Showcase::Scene &s) { s.callouts.enabled = m_co_enabled->GetValue(); });
        // Turned on: the callouts are worked out from the model and placed on the next frame.
        callouts_sync(false);
        refresh_callout_list();
    });
    m_co_layout->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        const int sel = m_co_layout->GetSelection();
        if (sel >= 0)
            edit([sel](Showcase::Scene &s) { s.callouts.layout = Showcase::CalloutLayout(sel); });
    });
    m_co_style->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        const int sel = m_co_style->GetSelection();
        const std::vector<Showcase::CalloutStylePreset> presets = Showcase::callout_style_presets();
        if (sel >= 0 && size_t(sel) < presets.size())
            edit([&presets, sel](Showcase::Scene &s) { Showcase::apply_callout_style_preset(s.callouts.style, presets[size_t(sel)]); }, true);
        else if (sel >= 0 && size_t(sel) - presets.size() < m_callout_styles.styles().size()) {
            // S4: one of the user's styles, font included.
            const Showcase::CalloutStyle style = m_callout_styles.styles()[size_t(sel) - presets.size()].style;
            edit([&style](Showcase::Scene &s) { s.callouts.style = style; }, true);
        }
    });
    m_co_font->Bind(wxEVT_FONTPICKER_CHANGED, [this](wxFontPickerEvent &) {
        const wxFont f = m_co_font->GetSelectedFont();
        edit([&f](Showcase::Scene &s) {
            s.callouts.style.font_face = into_u8(f.GetFaceName());
            s.callouts.style.bold      = f.GetWeight() >= wxFONTWEIGHT_BOLD;
        });
    });
    m_co_size->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
        edit([this](Showcase::Scene &s) { s.callouts.style.size_pct = float(m_co_size->GetValue()) / 10.f; });
    });
    m_co_leader->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        const int sel = m_co_leader->GetSelection();
        if (sel >= 0)
            edit([sel](Showcase::Scene &s) { s.callouts.style.leader = Showcase::LeaderStyle(sel); });
    });
    m_co_auto_colors->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) {
        edit([this](Showcase::Scene &s) { s.callouts.style.auto_colors = m_co_auto_colors->GetValue(); });
    });
    m_co_text->Bind(wxEVT_COLOURPICKER_CHANGED, [this](wxColourPickerEvent &) {
        edit([this](Showcase::Scene &s) { s.callouts.style.text_color = showcase_hex(m_co_text->GetColour()); });
    });
    m_co_line->Bind(wxEVT_COLOURPICKER_CHANGED, [this](wxColourPickerEvent &) {
        edit([this](Showcase::Scene &s) { s.callouts.style.line_color = showcase_hex(m_co_line->GetColour()); });
    });
    m_co_bg->Bind(wxEVT_COLOURPICKER_CHANGED, [this](wxColourPickerEvent &) {
        edit([this](Showcase::Scene &s) { s.callouts.style.background = showcase_hex(m_co_bg->GetColour()); });
    });
    m_co_bg_opacity->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
        edit([this](Showcase::Scene &s) { s.callouts.style.background_opacity = float(m_co_bg_opacity->GetValue()) / 100.f; });
    });
    for (size_t i = 0; i < m_co_fields.size(); ++i)
        m_co_fields[i]->Bind(wxEVT_CHECKBOX, [this, i](wxCommandEvent &) {
            const bool on = m_co_fields[i]->GetValue();
            edit([i, on](Showcase::Scene &s) {
                Showcase::CalloutFields &f = s.callouts.fields;
                bool *fields[] = { &f.brand, &f.product, &f.family, &f.color_name, &f.hex, &f.swatch, &f.type, &f.weight, &f.price };
                *fields[i] = on;
            });
            refresh_callout_list();
        });
    m_co_separator->Bind(wxEVT_TEXT, [this](wxCommandEvent &) {
        edit([this](Showcase::Scene &s) { s.callouts.fields.separator = into_u8(m_co_separator->GetValue()); });
        refresh_callout_list();
    });
    m_co_once->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) {
        edit([this](Showcase::Scene &s) { s.callouts.once_per_color = m_co_once->GetValue(); });
    });
    m_co_list->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        if (m_syncing)
            return;
        const int sel = m_co_list->GetSelection();
        callouts_select(sel >= 0 ? int(reinterpret_cast<intptr_t>(m_co_list->GetClientData(unsigned(sel)))) : -1);
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

void ShowcaseView::refresh_materials(bool sync_selection)
{
    if (m_mat_grid == nullptr)
        return;
    const std::vector<ShowcaseFilament> filaments = showcase_filaments();
    // Multi-colour P2: a split slot shows its make-up as a chip (colours, proportions, rotation).
    struct MakeUpChip
    {
        std::vector<std::string>            colors;
        FilamentColorUtils::ColourMakeUp    makeUp;
        bool                                shown = false;
    };
    std::vector<MakeUpChip> chips(filaments.size());
    // Glow G1: which slots glow, shown beside the name (set in the colour dialog).
    std::vector<Glow::Slot> glow;
    if (const PresetBundle *bundle = wxGetApp().preset_bundle)
        glow = Glow::slots_from_bundle(*bundle);
    if (const PresetBundle *bundle = wxGetApp().preset_bundle) {
        const DynamicPrintConfig &pc = bundle->project_config;
        const bool look = scene().materials.coextruded_look;
        for (size_t i = 0; i < filaments.size(); ++i) {
            const size_t slot = size_t(filaments[i].slot - 1);
            const FilamentColor c = FilamentColorUtils::GetFilamentColorFromConfig(&pc, slot, filaments[i].color);
            chips[i].colors = c.colors;
            chips[i].makeUp = FilamentColorUtils::GetColourMakeUp(&pc, slot);
            chips[i].shown  = look && c.colors.size() > 1 && c.NormalizedMode() == FilamentColorMode::Segment;
        }
    }
    std::string signature;
    for (size_t i = 0; i < filaments.size(); ++i) {
        const ShowcaseFilament &f = filaments[i];
        signature += f.preset_name + "\n" + f.type + "\n" + f.color + "\n";
        if (size_t(f.slot - 1) < glow.size() && glow[size_t(f.slot - 1)].on)
            signature += "glow\n";
        if (chips[i].shown) {
            for (const std::string &c : chips[i].colors)
                signature += c + ",";
            signature += "|" + chips[i].makeUp.split + "|" + std::to_string(int(chips[i].makeUp.angle)) + "\n";
        }
    }
    const bool rebuild = signature != m_mat_signature || m_mat_choices.size() != filaments.size();
    if (!rebuild && !sync_selection)
        return;
    const bool syncing = m_syncing;
    m_syncing          = true;
    if (rebuild) {
        m_mat_signature = signature;
        m_mat_grid->Clear(true);
        m_mat_choices.clear();
        const int em = wxGetApp().em_unit();
        for (size_t fi = 0; fi < filaments.size(); ++fi) {
            const ShowcaseFilament &f = filaments[fi];
            auto *label  = new wxBoxSizer(wxHORIZONTAL);
            if (chips[fi].shown) {
                wxBitmap *split_icon = FilamentColorUtils::GetSplitFilamentColorIcon(chips[fi].colors, chips[fi].makeUp.split,
                                                                                    chips[fi].makeUp.angle, std::string(),
                                                                                    int(2.2 * em), int(1.4 * em));
                auto *chip = new wxStaticBitmap(m_side, wxID_ANY, *split_icon);
                chip->SetToolTip(_L("The colour make-up of this filament. Click to edit it (colours, proportions, rotation)."));
                chip->SetCursor(wxCursor(wxCURSOR_HAND));
                const int chip_slot = f.slot;
                chip->Bind(wxEVT_LEFT_UP, [chip_slot](wxMouseEvent &) {
                    auto &combos = wxGetApp().sidebar().combos_filament();
                    if (chip_slot >= 1 && size_t(chip_slot) <= combos.size())
                        combos[size_t(chip_slot - 1)]->ChangeExtruderColor();
                });
                label->Add(chip, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em / 2);
            } else {
                auto *swatch = new wxPanel(m_side, wxID_ANY, wxDefaultPosition, wxSize(int(1.4 * em), int(1.4 * em)), wxBORDER_SIMPLE);
                swatch->SetBackgroundColour(showcase_wx_colour(f.color));
                swatch->SetToolTip(_L("Click to edit this filament's colour, make-up and glow."));
                swatch->SetCursor(wxCursor(wxCURSOR_HAND));
                const int swatch_slot = f.slot;
                swatch->Bind(wxEVT_LEFT_UP, [swatch_slot](wxMouseEvent &) {
                    auto &combos = wxGetApp().sidebar().combos_filament();
                    if (swatch_slot >= 1 && size_t(swatch_slot) <= combos.size())
                        combos[size_t(swatch_slot - 1)]->ChangeExtruderColor();
                });
                label->Add(swatch, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em / 2);
            }
            wxString name = f.preset_name.empty() ? wxString::Format(_L("Filament %d"), f.slot)
                                                  : wxString::Format("%d  %s", f.slot, wxString::FromUTF8(f.preset_name));
            if (size_t(f.slot - 1) < glow.size() && glow[size_t(f.slot - 1)].on)
                name += _L("  (glows)");
            auto *text = new wxStaticText(m_side, wxID_ANY, name, wxDefaultPosition, wxSize(14 * em, -1), wxST_ELLIPSIZE_END);
            text->SetToolTip(wxString::FromUTF8(f.preset_name + (f.type.empty() ? std::string() : " (" + f.type + ")")));
            label->Add(text, 1, wxALIGN_CENTER_VERTICAL);
            m_mat_grid->Add(label, 0, wxALIGN_CENTER_VERTICAL);

            wxArrayString items;
            items.Add(wxString::Format(_L("Auto: %s"), showcase_finish_name(f.inferred)));
            for (Showcase::Finish finish : Showcase::all_finishes())
                items.Add(showcase_finish_name(finish));
            auto *c = new wxChoice(m_side, wxID_ANY, wxDefaultPosition, wxDefaultSize, items);
            const int slot = f.slot;
            c->Bind(wxEVT_CHOICE, [this, c, slot](wxCommandEvent &) {
                const int sel = c->GetSelection();
                const std::vector<Showcase::Finish> finishes = Showcase::all_finishes();
                edit([&finishes, sel, slot](Showcase::Scene &s) {
                    if (sel <= 0 || size_t(sel) > finishes.size())
                        s.materials.set_override(slot, nullptr);
                    else
                        s.materials.set_override(slot, &finishes[size_t(sel - 1)]);
                });
            });
            m_mat_grid->Add(c, 1, wxEXPAND);
            m_mat_choices.push_back(c);
        }
        wxGetApp().UpdateDarkUIWin(m_side);
        fit_side_width();
        m_side->Layout();
        // S3: the labels name these filaments.
        callouts_changed();
        refresh_callout_list();
    }
    const Showcase::Scene &s = scene();
    const std::vector<Showcase::Finish> finishes = Showcase::all_finishes();
    for (size_t i = 0; i < m_mat_choices.size(); ++i) {
        const Showcase::Finish *o = s.materials.override_for(int(i) + 1);
        int sel = 0;
        if (o != nullptr)
            sel = int(std::find(finishes.begin(), finishes.end(), *o) - finishes.begin()) + 1;
        m_mat_choices[i]->SetSelection(sel);
    }
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
    if (Showcase::from_string(s.preset_base, preset)) {
        const std::vector<Showcase::PresetId> presets = Showcase::builtin_presets();
        if (auto it = std::find(presets.begin(), presets.end(), preset); it != presets.end())
            m_presets->SetSelection(int(it - presets.begin()));
    }
    m_hide_plate->SetValue(s.floor.hide_plate);
    m_hide_wipe_tower->SetValue(s.floor.hide_wipe_tower);

    m_bg_type->SetSelection(int(s.background.type));
    m_bg_color1->SetColour(showcase_wx_colour(s.background.color1));
    m_bg_color2->SetColour(showcase_wx_colour(s.background.color2));
    m_bg_fit->SetSelection(int(s.background.fit));
    m_bg_blur->SetValue(int(std::lround(s.background.blur * 100.f)));
    // 1h-2
    m_bg_ground->SetValue(s.background.ground);
    {
        double auto_h = 0., auto_r = 0.;
        ground_auto_values(auto_h, auto_r);
        const double h = s.background.ground_height > 0.f ? double(s.background.ground_height) : auto_h;
        const double r = s.background.ground_radius > 0.f ? double(s.background.ground_radius) : auto_r;
        m_bg_ground_height->SetValue(std::clamp(int(std::lround(h)), 10, 20000));
        m_bg_ground_radius->SetValue(std::clamp(int(std::lround(r)), 50, 1000000));
    }
    m_bg_ground_scale->SetValue(std::clamp(int(std::lround(s.background.ground_scale * 100.f)), 25, 400));
    m_bg_pitch->SetValue(std::clamp(int(std::lround(s.background.backdrop_pitch)), -45, 45));
    m_plate_horizon->SetValue(std::clamp(int(std::lround(s.background.plate_horizon * 100.f)), 0, 100));
    m_plate_focal->SetValue(std::clamp(int(std::lround(s.background.plate_focal)), 8, 600));

    m_floor_type->SetSelection(int(s.floor.type));
    m_floor_color->SetColour(showcase_wx_colour(s.floor.color));
    m_floor_shadow->SetValue(int(std::lround(s.floor.shadow_strength * 100.f)));
    m_floor_grid->SetValue(int(std::lround(s.floor.grid_spacing)));
    {
        const std::vector<Showcase::FloorTextureInfo> &floors = showcase_floors().floors;
        int sel = -1;
        for (size_t i = 0; i < floors.size(); ++i)
            if (floors[i].id == s.floor.texture)
                sel = int(i);
        m_floor_texture->SetSelection(sel);
        const Showcase::FloorTextureInfo *f = showcase_floors().find(s.floor.texture);
        const float tile = s.floor.type == Showcase::FloorType::Image ? s.floor.texture_size :
                                                                         (f != nullptr ? f->tile_mm * s.floor.texture_scale : 300.f);
        m_floor_tile->SetValue(std::clamp(int(std::lround(tile)), 10, 5000));
    }
    m_floor_rotation->SetSelection(std::clamp(Showcase::floor_rotation_snap(double(s.floor.rotation)) / 90, 0, 3));
    m_floor_flip->SetValue(s.floor.flip);
    m_floor_reflection->SetValue(int(std::lround(s.floor.reflection * 100.f)));
    m_floor_contact->SetValue(int(std::lround(s.floor.contact_shadow * 100.f)));

    m_light_fixed->SetValue(s.lighting.world_fixed);
    int az = int(std::lround(s.lighting.azimuth_deg)) % 360;
    if (az < 0)
        az += 360;
    m_light_azimuth->SetValue(az);
    m_light_elevation->SetValue(std::clamp(int(std::lround(s.lighting.elevation_deg)), 5, 90));
    m_shadows->SetValue(s.lighting.shadows);
    m_shadow_darkness->SetValue(int(std::lround(s.lighting.shadow_intensity * 100.f)));
    m_ssao->SetValue(s.lighting.ssao);
    m_light_rig->SetSelection(-1);   // an action, not a state
    m_key_intensity->SetValue(int(std::lround(s.lighting.key_intensity * 100.f)));
    m_key_temperature->SetValue(std::clamp(int(std::lround(s.lighting.key_temperature / 100.f)), 20, 100));
    m_fill->SetValue(s.lighting.fill);
    m_fill_intensity->SetValue(int(std::lround(s.lighting.fill_intensity * 100.f)));
    m_rim->SetValue(s.lighting.rim);
    m_rim_intensity->SetValue(int(std::lround(s.lighting.rim_intensity * 100.f)));
    m_shadow_softness->SetValue(std::clamp(int(std::lround(s.lighting.shadow_softness * 10.f)), 5, 40));
    {
        const std::vector<Showcase::EnvironmentInfo> &envs = showcase_environments().environments;
        const std::vector<Showcase::HdriInfo>        &hdris = showcase_hdris().hdris;
        int sel = 0;
        for (size_t i = 0; i < envs.size(); ++i)
            if (envs[i].id == s.lighting.environment)
                sel = int(i) + 1;
        for (size_t i = 0; i < hdris.size(); ++i)
            if (hdris[i].id == s.lighting.environment)
                sel = int(envs.size() + i) + 1;
        m_environment->SetSelection(sel);
        m_light_env->SetSelection(sel);
    }
    m_env_intensity->SetValue(int(std::lround(s.lighting.env_intensity * 100.f)));
    int env_rot = int(std::lround(s.lighting.env_rotation_deg)) % 360;
    if (env_rot < 0)
        env_rot += 360;
    m_env_rotation->SetValue(env_rot);
    m_exposure->SetValue(std::clamp(int(std::lround(s.lighting.exposure * 10.f)), -30, 30));
    m_sun_light->SetValue(s.lighting.sun_from_env);
    m_tone->SetSelection(s.lighting.tone == Showcase::ToneCurve::Filmic ? 1 : 0);
    m_layer_lines->SetSelection(int(s.materials.layer_lines));
    m_coextruded->SetValue(s.materials.coextruded_look);
    m_lights_off->SetValue(s.materials.lights_off);
    m_lights_ambient->SetValue(int(std::lround(s.materials.lights_off_ambient * 100.f)));
    m_glow_intensity->SetValue(int(std::lround(Glow::intensity_from_config(wxGetApp().preset_bundle->project_config) * 100.f)));
    m_source->SetSelection(s.source == "toolpaths" ? 1 : 0);
    m_smooth_auto->SetValue(s.materials.toolpath_smoothness < 0.f);
    if (s.materials.toolpath_smoothness >= 0.f)
        m_smooth->SetValue(int(std::lround(s.materials.toolpath_smoothness * 100.f)));
    refresh_materials(true);

    m_projection->SetSelection(wxGetApp().plater()->get_camera().get_type() == Camera::EType::Ortho ? 1 : 0);
    refresh_views();
    // 1h-2
    m_lens->SetSelection(-1);   // an action, not a state
    m_dof->SetValue(s.dof.enabled);
    {
        const float stops[] = { 1.4f, 2.f, 2.8f, 4.f, 5.6f, 8.f, 11.f, 16.f, 22.f };
        int         best    = 2;
        for (int i = 0; i < 9; ++i)
            if (std::abs(std::log(stops[i] / std::max(s.dof.f_number, 0.5f))) < std::abs(std::log(stops[best] / std::max(s.dof.f_number, 0.5f))))
                best = i;
        m_dof_fnumber->SetSelection(best);
    }
    m_dof_on_model->SetValue(s.dof.focus_on_model);
    m_dof_focus->SetValue(std::clamp(int(std::lround(s.dof.focus_mm)), 10, 100000));
    m_dof_quality->SetSelection(int(s.dof.quality));

    int size_sel = SHOWCASE_SIZE_COUNT;
    for (int i = 0; i < SHOWCASE_SIZE_COUNT; ++i)
        if (SHOWCASE_SIZES[i].w == s.export_settings.width && SHOWCASE_SIZES[i].h == s.export_settings.height)
            size_sel = i;
    m_size->SetSelection(size_sel);
    m_width->SetValue(s.export_settings.width);
    m_height->SetValue(s.export_settings.height);
    m_quality->SetSelection(int(s.export_settings.quality));
    m_format->SetSelection(int(s.export_settings.format));
    m_use_as_cover->SetValue(s.export_settings.use_as_cover);

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

    // S3
    const Showcase::Callouts &co = s.callouts;
    m_co_enabled->SetValue(co.enabled);
    m_co_layout->SetSelection(int(co.layout));
    refresh_callout_styles();
    wxFontInfo co_font(12);
    if (!co.style.font_face.empty())
        co_font.FaceName(wxString::FromUTF8(co.style.font_face));
    co_font.Bold(co.style.bold);
    m_co_font->SetSelectedFont(wxFont(co_font));
    m_co_size->SetValue(std::clamp(int(std::lround(co.style.size_pct * 10.f)), 10, 60));
    m_co_leader->SetSelection(int(co.style.leader));
    m_co_auto_colors->SetValue(co.style.auto_colors);
    m_co_text->SetColour(showcase_wx_colour(co.style.text_color));
    m_co_line->SetColour(showcase_wx_colour(co.style.line_color));
    m_co_bg->SetColour(showcase_wx_colour(co.style.background));
    m_co_bg_opacity->SetValue(int(std::lround(co.style.background_opacity * 100.f)));
    {
        const bool fields[] = { co.fields.brand, co.fields.product, co.fields.family, co.fields.color_name, co.fields.hex,
                                co.fields.swatch, co.fields.type, co.fields.weight, co.fields.price };
        for (size_t i = 0; i < m_co_fields.size() && i < sizeof(fields) / sizeof(fields[0]); ++i)
            m_co_fields[i]->SetValue(fields[i]);
    }
    if (into_u8(m_co_separator->GetValue()) != co.fields.separator)
        m_co_separator->ChangeValue(wxString::FromUTF8(co.fields.separator));
    m_co_once->SetValue(co.once_per_color);
    refresh_callout_list();
    sync_video_controls();

    m_syncing = false;
    update_enabled();
    fit_side_width();
    m_side->Layout();
}

void ShowcaseView::fit_side_width()
{
    if (m_side == nullptr || m_side->GetSizer() == nullptr)
        return;
    // Rows shown or rebuilt since (a path's rows, the materials) stay hidden in a folded section.
    apply_panel_sections();
    const int em = wxGetApp().em_unit();
    // Measured, not assumed: labels and controls come out wider in macOS's system font, and in longer
    // languages, than an em count tuned on Windows allows for. The vertical scroll bar takes its
    // share too where it is not an overlay.
    // S5c: never narrower than the widest the rows have been, so folding a section keeps the width.
    m_side_content_w  = std::max(m_side_content_w, m_side->GetSizer()->CalcMin().x);
    const int content = m_side_content_w + wxSystemSettings::GetMetric(wxSYS_VSCROLL_X, m_side);
    int       width   = std::max({ 32 * em, content, m_side_width });
    // Leave the canvas some room; in a window too narrow for both the panel scrolls sideways.
    if (const int client = GetClientSize().x; client > 0)
        width = std::min(width, std::max(client - (m_sash != nullptr ? m_sash->GetSize().x : 0) - 24 * em, 20 * em));
    if (width != m_side->GetMinSize().x) {
        m_side->SetMinSize(wxSize(width, -1));
        Layout();
    }
    m_side->FitInside();
}

// ------------------------------------------------------------------------------------------------
// S5b: collapsible sections
// ------------------------------------------------------------------------------------------------

namespace {
wxString panel_section_arrow(bool collapsed)
{
    // U+25B8 / U+25BE, small right- and down-pointing triangles.
    return wxString::FromUTF8(collapsed ? "\xE2\x96\xB8" : "\xE2\x96\xBE");
}
} // namespace

wxBoxSizer *ShowcaseView::add_panel_section(wxBoxSizer *root, const wxString &title, const std::string &key)
{
    const int    em = wxGetApp().em_unit();
    PanelSection sec;
    sec.key = key;
    // S5c: folded until the user opens a section (then remembered either way).
    sec.collapsed = wxGetApp().app_config->get("showcase_collapsed_" + key) != "0";
    // A full-width row on the theme's button colour.
    sec.head   = new wxPanel(m_side, wxID_ANY);
    auto *head = new wxBoxSizer(wxHORIZONTAL);
    sec.arrow  = new wxStaticText(sec.head, wxID_ANY, panel_section_arrow(sec.collapsed));
    sec.arrow->SetMinSize(wxSize(int(1.2 * em), -1));
    sec.title = new wxStaticText(sec.head, wxID_ANY, title);
    sec.title->SetFont(wxGetApp().bold_font());
    sec.arrow->SetFont(wxGetApp().bold_font());
    head->Add(sec.arrow, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, em / 2);
    head->Add(sec.title, 1, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, em / 2);
    sec.head->SetSizer(head);
    root->Add(sec.head, 0, wxEXPAND | wxTOP, em / 2);
    sec.body = new wxBoxSizer(wxVERTICAL);
    root->Add(sec.body, 0, wxEXPAND);
    const size_t index = m_sections.size();
    for (wxWindow *w : { static_cast<wxWindow *>(sec.head), static_cast<wxWindow *>(sec.arrow), static_cast<wxWindow *>(sec.title) }) {
        w->SetCursor(wxCursor(wxCURSOR_HAND));
        w->SetToolTip(_L("Click to show or hide this section."));
        w->Bind(wxEVT_LEFT_DOWN, [this, index](wxMouseEvent &) { toggle_panel_section(index); });
    }
    m_sections.push_back(sec);
    apply_section_header_colours();
    return sec.body;
}

void ShowcaseView::apply_section_header_colours()
{
    // The regular button's background (Widgets/Button.cpp), themed like it.
    const bool     dark = wxGetApp().dark_mode();
    const wxColour bg   = dark ? StateColor::darkModeColorFor(wxColour("#DFDFDF")) : wxColour("#DFDFDF");
    const wxColour fg   = dark ? StateColor::darkModeColorFor(wxColour("#262E30")) : wxColour("#262E30");
    for (PanelSection &sec : m_sections) {
        if (sec.head == nullptr)
            continue;
        if (sec.head->GetBackgroundColour() != bg) {
            sec.head->SetBackgroundColour(bg);
            sec.head->Refresh();
        }
        for (wxStaticText *t : { sec.arrow, sec.title })
            if (t != nullptr) {
                t->SetBackgroundColour(bg);
                t->SetForegroundColour(fg);
            }
    }
}

void ShowcaseView::apply_panel_sections()
{
    if (m_side_root == nullptr)
        return;
    // A rebuilt row may have had the dark theme applied over the headers' colours.
    apply_section_header_colours();
    for (const PanelSection &sec : m_sections)
        if (sec.collapsed && sec.body != nullptr)
            m_side_root->Hide(sec.body);
}

void ShowcaseView::toggle_panel_section(size_t index)
{
    if (index >= m_sections.size() || m_side_root == nullptr)
        return;
    PanelSection &sec = m_sections[index];
    sec.collapsed     = !sec.collapsed;
    wxGetApp().app_config->set("showcase_collapsed_" + sec.key, sec.collapsed ? "1" : "0");
    sec.arrow->SetLabel(panel_section_arrow(sec.collapsed));
    if (sec.collapsed) {
        m_side_root->Hide(sec.body);
        m_side->Layout();
        fit_side_width();
    } else {
        // Showing the section shows every row in it: the controls' own state hides the rows that
        // should stay hidden again (a camera path's rows, the read-only notice).
        m_side_root->Show(sec.body);
        sync_controls();
    }
    m_side->Refresh();
}

void ShowcaseView::update_enabled()
{
    if (m_side == nullptr)
        return;
    const Showcase::Scene &s        = scene();
    const bool             editable = !read_only();
    const bool             gradient = s.background.type == Showcase::BackgroundType::VerticalGradient ||
                          s.background.type == Showcase::BackgroundType::RadialGradient;
    const bool image    = s.background.type == Showcase::BackgroundType::Image || s.background.type == Showcase::BackgroundType::Backplate;
    const bool floor_on = s.floor.hide_plate;

    for (wxWindow *w : std::initializer_list<wxWindow *>{ m_presets, m_hide_plate, m_hide_wipe_tower, m_bg_type, m_light_fixed, m_shadows,
                                                          m_ssao, m_projection, m_size, m_quality, m_format, m_wm_enabled, m_light_rig,
                                                          m_key_intensity, m_key_temperature, m_fill, m_rim, m_shadow_softness, m_environment, m_light_env,
                                                          m_exposure, m_tone, m_coextruded, m_lights_off, m_use_as_cover, m_source, m_smooth_auto,
                                                          m_bg_ground, m_bg_ground_height, m_bg_ground_radius, m_bg_ground_scale, m_bg_pitch,
                                                          m_plate_horizon, m_plate_focal, m_plate_match, m_lens, m_dof, m_dof_fnumber,
                                                          m_dof_on_model, m_dof_focus, m_dof_quality })
        w->Enable(editable);
    m_lights_ambient->Enable(editable && s.materials.lights_off);
    m_glow_intensity->Enable(editable);
    // The toolpaths are the real layers, so the procedural lines only apply to the model.
    m_layer_lines->Enable(editable && s.source != "toolpaths");
    m_smooth->Enable(editable && s.materials.toolpath_smoothness >= 0.f);
    for (wxChoice *c : m_mat_choices)
        c->Enable(editable);
    m_fill_intensity->Enable(editable && s.lighting.fill);
    m_rim_intensity->Enable(editable && s.lighting.rim);
    m_env_intensity->Enable(editable && !s.lighting.environment.empty());
    m_env_rotation->Enable(editable && !s.lighting.environment.empty());
    m_sun_light->Enable(editable && showcase_hdris().find(s.lighting.environment) != nullptr);
    m_bg_blur->Enable(editable && (image || s.background.type == Showcase::BackgroundType::Environment));
    const bool photo   = s.floor.type == Showcase::FloorType::Texture;
    const bool picture = s.floor.type == Showcase::FloorType::Image;
    m_floor_texture->Enable(editable && floor_on && !showcase_floors().floors.empty());
    m_floor_image->Enable(editable && floor_on);
    m_floor_tile->Enable(editable && floor_on && (photo || picture));
    m_floor_rotation->Enable(editable && floor_on && (photo || picture));
    m_floor_flip->Enable(editable && floor_on && (photo || picture));
    m_floor_reflection->Enable(editable && floor_on && s.floor.type != Showcase::FloorType::None);
    m_floor_contact->Enable(editable && floor_on && s.floor.type != Showcase::FloorType::None);
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

    // S3
    const Showcase::Callouts &co = s.callouts;
    const bool co_on  = editable && co.enabled;
    const bool boxed  = co.style.shape == Showcase::CalloutShape::Card || co.style.shape == Showcase::CalloutShape::Pill;
    m_co_enabled->Enable(editable);
    for (wxWindow *w : std::initializer_list<wxWindow *>{ m_co_layout, m_co_style, m_co_font, m_co_size, m_co_leader, m_co_auto_colors,
                                                          m_co_regenerate, m_co_once })
        w->Enable(co_on);
    for (wxCheckBox *cb : m_co_fields)
        cb->Enable(co_on);
    m_co_separator->Enable(co_on && co.fields.color_name);
    m_co_text->Enable(co_on && !co.style.auto_colors);
    m_co_line->Enable(co_on && !co.style.auto_colors);
    m_co_bg->Enable(co_on && boxed);
    m_co_bg_opacity->Enable(co_on && boxed);
    const bool has_items = co.enabled && !co.items.empty();
    m_co_list->Enable(has_items);
    const bool selected = has_items && m_callouts.selected >= 0 && size_t(m_callouts.selected) < co.items.size();
    m_co_edit->Enable(editable && selected);
    m_co_hide->Enable(editable && selected);
    m_co_hide->SetLabel(selected && co.items[size_t(m_callouts.selected)].hidden ? _L("Show") : _L("Hide"));
    m_co_unpin->Enable(editable && has_items &&
                       std::any_of(co.items.begin(), co.items.end(), [](const Showcase::Callout &c) { return c.pinned; }));
    m_co_style_save->Enable(co_on);
    m_co_style_delete->Enable(co_on && m_callout_styles.match(co.style) != nullptr);

    update_video_enabled();
    apply_row_visibility();
}

// ------------------------------------------------------------------------------------------------
// Rows that do not apply are hidden (Showcase::panel_visibility); their values stay as they are.
// ------------------------------------------------------------------------------------------------

namespace {
const void *row_key(wxWindow *w) { return static_cast<const void *>(w); }
const void *row_key(wxSizer *s) { return static_cast<const void *>(s); }
} // namespace

void ShowcaseView::index_panel_rows()
{
    m_rows.clear();
    for (size_t i = 0; i < m_sections.size(); ++i) {
        if (m_sections[i].body == nullptr)
            continue;
        const wxSizerItemList &items = m_sections[i].body->GetChildren();
        for (wxSizerItemList::compatibility_iterator node = items.GetFirst(); node; node = node->GetNext()) {
            wxSizerItem *item = node->GetData();
            // A full-width item of the section (a button row, a hint): the row is the item itself.
            const void *key = item->GetSizer() != nullptr ? row_key(item->GetSizer()) : (item->GetWindow() != nullptr ? row_key(item->GetWindow()) : nullptr);
            if (key != nullptr) {
                PanelRow r;
                r.a       = item;
                r.section = int(i);
                m_rows[key] = r;
            }
            // A grid of label + control rows: a row is two cells, found by either of them. The materials
            // grid is rebuilt with the project's filaments, so it is left alone.
            auto *grid = dynamic_cast<wxFlexGridSizer *>(item->GetSizer());
            if (grid == nullptr || grid == m_mat_grid)
                continue;
            const wxSizerItemList &cells = grid->GetChildren();
            for (wxSizerItemList::compatibility_iterator cell = cells.GetFirst(); cell; cell = cell->GetNext()) {
                wxSizerItem *left  = cell->GetData();
                cell               = cell->GetNext();
                wxSizerItem *right = cell ? cell->GetData() : nullptr;
                PanelRow     r;
                r.a       = left;
                r.b       = right;
                r.section = int(i);
                for (wxSizerItem *it : { left, right }) {
                    if (it == nullptr)
                        continue;
                    if (it->GetWindow() != nullptr)
                        m_rows[row_key(it->GetWindow())] = r;
                    else if (it->GetSizer() != nullptr)
                        m_rows[row_key(it->GetSizer())] = r;
                }
                if (!cell)
                    break;
            }
        }
    }
}

bool ShowcaseView::show_row(const void *key, bool show)
{
    auto it = m_rows.find(key);
    if (it == m_rows.end())
        return false;
    const PanelRow &r = it->second;
    // A folded section keeps its rows hidden; opening it runs the rules again.
    if (r.section >= 0 && size_t(r.section) < m_sections.size() && m_sections[size_t(r.section)].collapsed)
        return false;
    bool changed = false;
    for (wxSizerItem *item : { r.a, r.b })
        if (item != nullptr && item->IsShown() != show) {
            item->Show(show);
            changed = true;
        }
    return changed;
}

void ShowcaseView::apply_row_visibility()
{
    if (m_side == nullptr || m_rows.empty())
        return;
    const Showcase::Scene &s    = scene();
    const bool             hdri = showcase_hdris().find(s.lighting.environment) != nullptr;
    const Showcase::PanelVisibility v = Showcase::panel_visibility(s, hdri);

    bool changed = false;
    auto set = [&](wxWindow *w, bool on) {
        if (w != nullptr)
            changed |= show_row(row_key(w), on);
    };
    // For a control that sits in a sizer of its own (a row of buttons).
    auto set_box = [&](wxWindow *w, bool on) {
        if (w != nullptr && w->GetContainingSizer() != nullptr)
            changed |= show_row(row_key(w->GetContainingSizer()), on);
    };

    // Background
    set(m_environment, v.bg_environment);
    set(m_bg_color1, v.bg_colour1);
    set(m_bg_color2, v.bg_colour2);
    set(m_bg_image, v.bg_image);
    set(m_bg_fit, v.bg_fit);
    set(m_bg_blur, v.bg_blur);
    set(m_bg_ground, v.bg_ground);
    set(m_bg_ground_height, v.bg_ground_detail);
    set(m_bg_ground_radius, v.bg_ground_detail);
    set(m_bg_ground_scale, v.bg_ground_detail);
    set(m_bg_pitch, v.bg_pitch);
    set(m_plate_horizon, v.bg_plate_match);
    set(m_plate_focal, v.bg_plate_match);
    set(m_plate_match, v.bg_plate_match);
    // Camera
    set(m_dof_fnumber, v.cam_dof_detail);
    set(m_dof_on_model, v.cam_dof_detail);
    set(m_dof_quality, v.cam_dof_detail);
    set(m_dof_focus, v.cam_dof_focus);
    // Floor
    set(m_floor_color, v.floor_colour);
    set(m_floor_shadow, v.floor_shadow);
    set(m_floor_grid, v.floor_grid);
    set(m_floor_texture, v.floor_photo);
    set(m_floor_image, v.floor_picture);
    set(m_floor_rotation, v.floor_orientation);
    set(m_floor_flip, v.floor_orientation);
    set(m_floor_tile, v.floor_orientation);
    set(m_floor_reflection, v.floor_reflection);
    set(m_floor_contact, v.floor_contact);
    // Light
    set(m_light_azimuth, v.light_direction);
    set(m_light_elevation, v.light_direction);
    set(m_shadow_darkness, v.light_shadow_dark);
    set(m_light_env, v.light_environment);
    set(m_env_intensity, v.light_env_levels);
    set(m_env_rotation, v.light_env_levels);
    set(m_sun_light, v.light_sun);
    // Materials
    set(m_layer_lines, v.mat_layer_lines);
    set(m_smooth_auto, v.mat_toolpath);
    set(m_lights_ambient, v.mat_light_left);
    // Callouts
    for (wxWindow *w : std::initializer_list<wxWindow *>{ m_co_layout, m_co_once, m_co_style, m_co_font, m_co_size, m_co_leader,
                                                          m_co_auto_colors, m_co_list, m_co_hint })
        set(w, v.co_detail);
    set_box(m_co_style_save, v.co_detail);
    set_box(m_co_edit, v.co_detail);
    set_box(m_co_regenerate, v.co_detail);
    if (!m_co_fields.empty())
        set_box(m_co_fields.front(), v.co_detail);
    set(m_co_text, v.co_plain_colours);
    set(m_co_line, v.co_plain_colours);
    set(m_co_bg, v.co_box);
    set(m_co_bg_opacity, v.co_box);
    set(m_co_separator, v.co_separator);
    // Video
    set(m_vid_co_motion, v.vid_co_position);
    set(m_vid_co_appear, v.vid_co_appearance);
    set_box(m_vid_build_infill, v.vid_build_parts);
    // Export
    const bool custom = m_size->GetSelection() == SHOWCASE_SIZE_COUNT;
    set(m_width, custom);
    set(m_height, custom);
    // Watermark
    for (wxWindow *w : std::initializer_list<wxWindow *>{ m_wm_presets, m_wm_text, m_wm_font, m_wm_color, m_wm_anchor, m_wm_size,
                                                          m_wm_opacity, m_wm_margin })
        set(w, v.wm_detail);
    set_box(m_wm_logo, v.wm_detail);
    set_box(m_wm_preset_delete, v.wm_detail);

    if (!changed)
        return;
    // The panel follows its rows and stays where it was scrolled to.
    int x = 0, y = 0;
    m_side->GetViewStart(&x, &y);
    m_side->Layout();
    fit_side_width();
    m_side->Scroll(x, y);
    m_side->Refresh();
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

void ShowcaseView::on_choose_background_image(Showcase::BackgroundType type)
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
    edit([&sha, type](Showcase::Scene &s) {
        s.background.image = sha;
        s.background.type  = type;
        // 1h-2: a photo backplate is the floor: the plate goes and the floor only catches the shadow.
        if (type == Showcase::BackgroundType::Backplate) {
            s.floor.hide_plate = true;
            if (s.floor.type != Showcase::FloorType::None)
                s.floor.type = Showcase::FloorType::ShadowCatcher;
        }
    }, true);
    m_renderer->invalidate_images();
}

void ShowcaseView::apply_camera_lens(double focal_mm, const double *pitch)
{
    if (read_only() || m_renderer == nullptr)
        return;
    Camera               &camera = wxGetApp().plater()->get_camera();
    const Showcase::RectI f      = m_renderer->live_frame();
    if (f.empty())
        return;
    // A lens is a perspective camera.
    if (camera.get_type() != Camera::EType::Perspective)
        camera.set_type(Camera::EType::Perspective);
    const double fov_y = Showcase::fov_y_from_focal(focal_mm, double(f.w) / double(f.h));
    const double dist  = Showcase::distance_for_fov(double(f.h), camera.get_zoom(), fov_y);
    if (!(dist > 0.) || !std::isfinite(dist))
        return;
    const Vec3d  target = camera.get_target();
    const Vec3d  fwd    = camera.get_dir_forward();
    const double p      = pitch != nullptr ? *pitch : std::asin(std::clamp(-fwd.z(), -1., 1.));
    const Showcase::LensVec eye = Showcase::matched_eye({ target.x(), target.y(), target.z() }, { fwd.x(), fwd.y(), fwd.z() }, p, dist);
    camera.look_at(Vec3d(eye[0], eye[1], eye[2]), target, Vec3d::UnitZ());
    commit();
    store_scene_camera(true);
    sync_controls();
    set_as_dirty();
}

void ShowcaseView::ground_auto_values(double &height, double &radius)
{
    height = 150.;
    radius = 1500.;
    if (m_renderer == nullptr)
        return;
    const Showcase::Scene &s = scene();
    double hdri_h = 1600., hdri_r = 16000.;
    if (const Showcase::HdriInfo *h = showcase_hdris().find(s.lighting.environment)) {
        hdri_h = h->ground_height_mm;
        hdri_r = h->ground_radius_mm;
    }
    BoundingBoxf3 box = m_renderer->objects_box();
    if (!box.defined)
        box = BoundingBoxf3(Vec3d(-50., -50., 0.), Vec3d(50., 50., 50.));
    // The dome as the renderer draws it with the automatic values and a world scale of 1.
    Showcase::Background bg = s.background;
    bg.ground_height = 0.f;
    bg.ground_radius = 0.f;
    bg.ground_scale  = 1.f;
    const Showcase::GroundDome d = Showcase::ground_dome(bg, hdri_h, hdri_r, { box.min.x(), box.min.y(), box.min.z() },
                                                         { box.max.x(), box.max.y(), box.max.z() });
    height = d.height;
    // A set height keeps the HDRI's room shape for the automatic radius.
    const double h0 = s.background.ground_height > 0.f ? double(s.background.ground_height) : d.height;
    radius = std::max(h0 * (hdri_r / hdri_h), 1.25 * h0);
}

void ShowcaseView::on_choose_floor_image()
{
    if (read_only())
        return;
    wxFileDialog dlg(this, _L("Choose a floor picture"), "", "", _L("Images") + " (*.png;*.jpg;*.jpeg;*.bmp)|*.png;*.jpg;*.jpeg;*.bmp",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK)
        return;
    commit();
    std::string sha;
    if (!add_image_file(into_u8(dlg.GetPath()), 4096, false, sha))
        return;
    edit([&sha](Showcase::Scene &s) {
        s.floor.image = sha;
        s.floor.type  = Showcase::FloorType::Image;
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

void ShowcaseView::write_project_cover(const wxImage &image)
{
    // The project's cover picture is the one the Project page's "set as cover" writes:
    // <auxiliary folder>/.thumbnails/thumbnail_3mf.png, saved with the project (D10). The printer
    // thumbnails next to it are left alone.
    if (!image.IsOk() || image.GetWidth() <= 0 || image.GetHeight() <= 0)
        return;
    const int side = std::min(image.GetWidth(), image.GetHeight());
    wxImage   cover = image.GetSubImage(wxRect((image.GetWidth() - side) / 2, (image.GetHeight() - side) / 2, side, side));
    cover.Rescale(_3MF_COVER_SIZE.GetWidth(), _3MF_COVER_SIZE.GetHeight(), wxIMAGE_QUALITY_HIGH);
    boost::system::error_code ec;
    const boost::filesystem::path dir = boost::filesystem::path(m_model->get_auxiliary_file_temp_path()) / ".thumbnails";
    boost::filesystem::create_directories(dir, ec);
    const std::string file = (dir / "thumbnail_3mf.png").string();
    if (ec || !cover.SaveFile(wxString::FromUTF8(file), wxBITMAP_TYPE_PNG)) {
        BOOST_LOG_TRIVIAL(warning) << "Showcase: the project cover could not be written to " << file;
        return;
    }
    mark_project_dirty();
    BOOST_LOG_TRIVIAL(info) << "Showcase: the export is the project cover now (" << file << ")";
}

void ShowcaseView::on_export(bool to_clipboard)
{
    if (m_renderer == nullptr || !ShowcaseRenderer::is_supported())
        return;
    // Exporting a scene keeps it: its camera and settings are saved with the project.
    commit();
    store_scene_camera(true);
    const Showcase::Scene &s    = scene();
    const bool             jpeg = s.export_settings.format == Showcase::ImageFormat::JPEG;
    const bool             webp = s.export_settings.format == Showcase::ImageFormat::WebP;

    wxString path;
    if (!to_clipboard) {
        wxString name = wxGetApp().plater()->get_project_name();
        if (name.empty())
            name = "showcase";
        name += wxString::Format("_plate%d", current_plate() + 1);
        name += jpeg ? ".jpg" : webp ? ".webp" : ".png";
        wxFileDialog dlg(this, _L("Export Showcase image"), wxString::FromUTF8(wxGetApp().app_config->get_last_dir()), name,
                         jpeg ? "JPEG (*.jpg)|*.jpg;*.jpeg" : webp ? "WebP (*.webp)|*.webp" : "PNG (*.png)|*.png", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
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
        // S3: the callouts at the export's own size (after the supersampled render was scaled
        // down, so the text is sharp), then the watermark over everything.
        callouts_apply_to_export(image, s.background.type == Showcase::BackgroundType::Transparent);
        showcase_apply_watermark(image, s.export_settings.watermark, project());
    }
    set_as_dirty();

    const bool transparent = !jpeg && !to_clipboard && s.background.type == Showcase::BackgroundType::Transparent;
    // WebP: our own lossless encoder (no WebP library in the dependencies), alpha included.
    std::vector<uint8_t> webp_bytes;
    if (webp && !to_clipboard) {
        if (!transparent)
            Showcase::flatten_rgba(image.rgba, 255, 255, 255);
        webp_bytes = Showcase::encode_webp_lossless(image.rgba, image.width, image.height);
    }
    if (jpeg || to_clipboard)
        Showcase::flatten_rgba(image.rgba, 255, 255, 255);
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

    // D10: opt-in only.
    if (s.export_settings.use_as_cover)
        write_project_cover(out);

    if (to_clipboard) {
        if (wxTheClipboard->Open()) {
            wxTheClipboard->SetData(new wxBitmapDataObject(wxBitmap(out)));
            wxTheClipboard->Close();
            m_status->SetLabel(wxString::Format(_L("Copied a %d x %d image."), image.width, image.height));
        } else
            show_error(this, _L("The clipboard is in use by another program."));
        return;
    }

    bool saved = false;
    if (webp) {
        boost::nowide::ofstream f(into_u8(path), std::ios::binary | std::ios::trunc);
        if (f && !webp_bytes.empty()) {
            f.write(reinterpret_cast<const char *>(webp_bytes.data()), std::streamsize(webp_bytes.size()));
            saved = f.good();
        }
    } else {
        if (jpeg)
            out.SetOption(wxIMAGE_OPTION_QUALITY, s.export_settings.jpeg_quality);
        saved = out.SaveFile(path, jpeg ? wxBITMAP_TYPE_JPEG : wxBITMAP_TYPE_PNG);
    }
    if (!saved) {
        m_status->SetLabel("");
        show_error(this, _L("The image could not be saved."));
        return;
    }
    wxGetApp().app_config->update_skein_dir(into_u8(wxFileName(path).GetPath()));
    m_status->SetLabel(wxString::Format(_L("Saved %s (%d x %d)."), wxFileName(path).GetFullName(), image.width, image.height));
    m_status->Wrap(30 * wxGetApp().em_unit());
    fit_side_width();
    BOOST_LOG_TRIVIAL(info) << "Showcase: exported " << image.width << "x" << image.height << " to " << into_u8(path);
}


// ------------------------------------------------------------------------------------------------
// S3: filament callouts
// ------------------------------------------------------------------------------------------------

namespace {

const ModelInstance *showcase_instance(const Model &model, int object_idx, int instance_idx)
{
    if (object_idx < 0 || size_t(object_idx) >= model.objects.size())
        return nullptr;
    const ModelObject *mo = model.objects[size_t(object_idx)];
    if (instance_idx < 0 || size_t(instance_idx) >= mo->instances.size())
        return nullptr;
    return mo->instances[size_t(instance_idx)];
}

bool showcase_anchor_world(const Model &model, const Showcase::CalloutAnchor &a, Vec3d &world)
{
    const ModelInstance *inst = showcase_instance(model, a.object_idx, a.instance_idx);
    if (inst == nullptr)
        return false;
    world = inst->get_matrix() * Vec3d(a.local[0], a.local[1], a.local[2]);
    return true;
}

Showcase::CalloutAnchor showcase_make_anchor(const ModelInstance &inst, int object_idx, int instance_idx, const Vec3d &world)
{
    Showcase::CalloutAnchor a;
    a.object_idx   = object_idx;
    a.instance_idx = instance_idx;
    const Vec3d local = inst.get_matrix().inverse() * world;
    a.local = { local.x(), local.y(), local.z() };
    return a;
}

uint64_t showcase_matrix_key(const Matrix4d &m)
{
    uint64_t key = 1469598103934665603ull;
    for (int i = 0; i < 16; ++i) {
        const double d = m.data()[i];
        uint64_t     bits;
        std::memcpy(&bits, &d, sizeof(bits));
        key = (key ^ bits) * 1099511628211ull;
    }
    return key;
}

} // namespace

std::vector<int> ShowcaseView::plate_objects() const
{
    std::vector<int> out;
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr || m_model == nullptr)
        return out;
    PartPlate *plate = plater->get_partplate_list().get_curr_plate();
    if (plate == nullptr)
        return out;
    for (size_t o = 0; o < m_model->objects.size(); ++o)
        for (size_t i = 0; i < m_model->objects[o]->instances.size(); ++i)
            if (plate->contain_instance(int(o), int(i))) {
                out.push_back(int(o));
                break;
            }
    return out;
}

const CustomGCode::Info *ShowcaseView::plate_changes() const
{
    if (m_model == nullptr)
        return nullptr;
    auto it = m_model->plates_custom_gcodes.find(current_plate());
    return it == m_model->plates_custom_gcodes.end() ? nullptr : &it->second;
}

const ShowcaseLabelContext &ShowcaseView::callouts_labels()
{
    const Showcase::Scene &s     = scene();
    const bool             usage = s.callouts.fields.weight || s.callouts.fields.price;
    if (!m_callouts.labels || m_callouts.labels_version != m_callouts.version || m_callouts.labels_usage != usage) {
        m_callouts.labels         = std::make_shared<ShowcaseLabelContext>(ShowcaseLabelContext::current(current_plate(), usage));
        m_callouts.labels_version = m_callouts.version;
        m_callouts.labels_usage   = usage;
    }
    return *m_callouts.labels;
}

void ShowcaseView::callouts_sync(bool regenerate)
{
    if (m_renderer == nullptr || m_model == nullptr || read_only())
        return;
    Showcase::Scene &s = scene();
    if (!s.callouts.enabled)
        return;
    int filament_count = 0;
    if (PresetBundle *bundle = wxGetApp().preset_bundle)
        filament_count = int(bundle->mixed_filaments.total_filaments(bundle->filament_presets.size()));
    if (Showcase::prune_callouts(s.callouts, m_model->objects.size(), filament_count) > 0 && scene_in_project())
        mark_project_dirty();
    // The ID pass needs the canvas' context and the live camera: the next frame does the rest.
    m_callouts.pending          = true;
    m_callouts.regenerate       = m_callouts.regenerate || regenerate;
    m_callouts.occlusion_camera = 0;
    callouts_changed();
}

void ShowcaseView::callouts_before_frame()
{
    if (!m_active || m_renderer == nullptr || m_model == nullptr || read_only())
        return;
    Showcase::Scene &s = scene();
    if (!s.callouts.enabled)
        return;
    if (m_callouts.pending) {
        const bool regenerate = m_callouts.regenerate;
        m_callouts.pending    = false;
        m_callouts.regenerate = false;
        callouts_find_anchors(s, regenerate);
    }
    if (!m_callouts.picks.empty()) {
        // Dots the user dropped: on the model where they landed, else back where they were.
        ShowcaseIdPass pass;
        const bool     ok      = m_renderer->render_id_pass(1024, pass);
        bool           changed = false;
        for (const CalloutUi::Pick &pick : m_callouts.picks) {
            Showcase::Callout *c = s.callouts.find(pick.key);
            if (!ok || c == nullptr)
                continue;
            const int x = std::clamp(int(pick.nx * pass.width), 0, pass.width - 1);
            const int y = std::clamp(int(pick.ny * pass.height), 0, pass.height - 1);
            const ShowcaseIdPass::Entry *e = pass.entry_at(x, y);
            Vec3d world;
            if (e == nullptr || !pass.world_at(x, y, world))
                continue;
            const ModelInstance *inst = showcase_instance(*m_model, e->object_idx, e->instance_idx);
            if (inst == nullptr)
                continue;
            const Showcase::CalloutAnchor a = showcase_make_anchor(*inst, e->object_idx, e->instance_idx, world);
            if (pick.anchor >= 0 && size_t(pick.anchor) < c->anchors.size())
                c->anchors[size_t(pick.anchor)] = a;
            else
                c->anchors.push_back(a);
            c->user_anchor = true;
            changed        = true;
        }
        m_callouts.picks.clear();
        if (changed) {
            ++m_callouts.version;
            m_callouts.occlusion_camera = 0;
            CallAfter([this]() { mark_project_dirty(); });
        }
    }
    callouts_update_occlusion(s);
}

void ShowcaseView::callouts_find_anchors(Showcase::Scene &s, bool regenerate)
{
    const Showcase::CalloutRegions             regions(*m_model, plate_objects(), plate_changes());
    const std::vector<Showcase::CalloutTarget> targets = regions.targets();

    // Design 3.3: a low-resolution ID pass, each visible pixel sorted into its callout, and per
    // callout the point deepest inside each separate region it covers.
    ShowcaseIdPass pass;
    const bool     have_pass = !targets.empty() && m_renderer->render_id_pass(512, pass);
    std::vector<std::vector<Showcase::AnchorPixel>> found;
    if (have_pass) {
        std::map<std::string, int> index;
        for (size_t i = 0; i < targets.size(); ++i)
            index[targets[i].key] = int(i);
        // The callout of a pixel depends only on its entry and its height: remember each answer.
        std::unordered_map<uint64_t, int> memo;
        std::vector<int> classes(pass.ids.size(), -1);
        for (int y = 0; y < pass.height; ++y)
            for (int x = 0; x < pass.width; ++x) {
                const int id = pass.ids[size_t(y) * size_t(pass.width) + size_t(x)];
                Vec3d     world;
                if (id < 0 || !pass.world_at(x, y, world))
                    continue;
                const uint64_t key = (uint64_t(uint32_t(id)) << 32) | uint64_t(uint32_t(std::max(0LL, std::llround(world.z() * 100.))));
                auto           it  = memo.find(key);
                if (it == memo.end()) {
                    const ShowcaseIdPass::Entry &e = pass.entries[size_t(id)];
                    const Showcase::CalloutTarget t = regions.classify(e.object_idx, e.base, world.z());
                    auto                           ix = index.find(t.key);
                    it = memo.emplace(key, ix == index.end() ? -1 : ix->second).first;
                }
                classes[size_t(y) * size_t(pass.width) + size_t(x)] = it->second;
            }
        found = Showcase::find_anchor_pixels(classes, pass.width, pass.height, int(targets.size()), Showcase::MAX_CALLOUT_LEADERS,
                                             std::max(6, pass.width * pass.height / 1500));
        // S5b: how much of each shows, for "each colour once" (the regions come largest first).
        m_callouts.areas.clear();
        for (size_t i = 0; i < targets.size() && i < found.size(); ++i)
            m_callouts.areas[targets[i].key] = found[i].empty() ? 0 : found[i].front().area;
    }

    std::vector<Showcase::Callout> items;
    for (size_t i = 0; i < targets.size(); ++i) {
        const Showcase::CalloutTarget &t = targets[i];
        Showcase::Callout              c;
        // The user's edits stay with their callout.
        if (const Showcase::Callout *old = s.callouts.find(t.key))
            c = *old;
        else
            c.key = t.key;
        c.filament = t.filament;
        c.source   = t.source;
        c.z_from   = t.z_from;
        c.color    = t.color;
        // An anchor must still be on (or right next to) its object: after objects were deleted an index
        // can name another one.
        const bool valid = !c.anchors.empty() && std::all_of(c.anchors.begin(), c.anchors.end(), [this](const Showcase::CalloutAnchor &a) {
                               Vec3d world;
                               if (!showcase_anchor_world(*m_model, a, world))
                                   return false;
                               const BoundingBoxf3 box = m_model->objects[size_t(a.object_idx)]->instance_bounding_box(size_t(a.instance_idx));
                               const double        m   = 1.;
                               return box.defined && world.x() >= box.min.x() - m && world.y() >= box.min.y() - m && world.z() >= box.min.z() - m &&
                                      world.x() <= box.max.x() + m && world.y() <= box.max.y() + m && world.z() <= box.max.z() + m;
                           });
        if (!(valid && (c.user_anchor || !regenerate))) {
            c.anchors.clear();
            c.user_anchor = false;
            if (have_pass && i < found.size())
                for (const Showcase::AnchorPixel &ap : found[i]) {
                    const ShowcaseIdPass::Entry *e = pass.entry_at(ap.x, ap.y);
                    Vec3d world;
                    if (e == nullptr || !pass.world_at(ap.x, ap.y, world))
                        continue;
                    if (const ModelInstance *inst = showcase_instance(*m_model, e->object_idx, e->instance_idx))
                        c.anchors.push_back(showcase_make_anchor(*inst, e->object_idx, e->instance_idx, world));
                }
            if (c.anchors.empty() && !t.objects.empty()) {
                // Not in view from here: the middle of the first object showing it (of the part above
                // a layer change).
                const int    o     = t.objects.front();
                const ModelObject *mo = m_model->objects[size_t(o)];
                PartPlate   *plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
                for (size_t k = 0; k < mo->instances.size(); ++k) {
                    if (plate != nullptr && !plate->contain_instance(o, int(k)))
                        continue;
                    const BoundingBoxf3 box = mo->instance_bounding_box(k);
                    Vec3d centre = box.center();
                    if (t.z_from >= 0.)
                        centre.z() = 0.5 * (std::clamp(t.z_from, box.min.z(), box.max.z()) + box.max.z());
                    c.anchors.push_back(showcase_make_anchor(*mo->instances[k], o, int(k), centre));
                    break;
                }
            }
        }
        items.push_back(std::move(c));
    }
    if (items != s.callouts.items) {
        s.callouts.items = std::move(items);
        if (m_callouts.selected >= int(s.callouts.items.size()))
            m_callouts.selected = -1;
        ++m_callouts.version;
        m_callouts.occlusion_camera = 0;
        // Not from inside the frame: the title bar and the panel are windows of their own.
        CallAfter([this]() {
            if (scene_in_project())
                mark_project_dirty();
            refresh_callout_list();
            update_enabled();
            fit_side_width();
        });
    }
}

void ShowcaseView::callouts_update_occlusion(const Showcase::Scene &s)
{
    if (s.callouts.items.empty() || m_callouts.drag != CalloutUi::Drag::None)
        return;
    Matrix4d vp;
    if (!m_renderer->frame_view_projection(vp))
        return;
    const uint64_t key = showcase_matrix_key(vp);
    if (key == m_callouts.occlusion_camera)
        return;
    m_callouts.occlusion_camera = key;
    std::map<std::string, std::vector<bool>> occluded = callouts_compute_occlusion(s);
    if (occluded != m_callouts.occluded) {
        m_callouts.occluded = std::move(occluded);
        ++m_callouts.version;
    }
}

std::map<std::string, std::vector<bool>> ShowcaseView::callouts_compute_occlusion(const Showcase::Scene &s)
{
    std::map<std::string, std::vector<bool>> occluded;
    ShowcaseIdPass pass;
    if (s.callouts.items.empty() || !m_renderer->render_id_pass(384, pass))
        return occluded;
    // Design 3.3, 6.: an anchor something stands in front of gets a dashed leader. Compared as
    // distances along the view (ortho or perspective alike), with a tolerance for the coarse pass.
    const Transform3d  &view = wxGetApp().plater()->get_camera().get_view_matrix();
    const BoundingBoxf3 box  = m_renderer->objects_box();
    const double        tol  = box.defined ? std::max(0.3, 0.015 * box.radius()) : 0.5;
    for (const Showcase::Callout &c : s.callouts.items) {
        std::vector<bool> flags;
        for (const Showcase::CalloutAnchor &a : c.anchors) {
            Vec3d world;
            bool  hidden = false;
            if (showcase_anchor_world(*m_model, a, world)) {
                const Vec4d clip = pass.view_projection * Vec4d(world.x(), world.y(), world.z(), 1.);
                if (clip.w() > 1e-9) {
                    const int    px    = int(std::floor((clip.x() / clip.w() + 1.) * 0.5 * pass.width));
                    const int    py    = int(std::floor((1. - clip.y() / clip.w()) * 0.5 * pass.height));
                    const double depth = -(view * world).z();
                    hidden = px >= 0 && py >= 0 && px < pass.width && py < pass.height;
                    // Hidden only when everything around it is clearly in front of it.
                    for (int dy = -1; dy <= 1 && hidden; ++dy)
                        for (int dx = -1; dx <= 1 && hidden; ++dx) {
                            Vec3d seen;
                            if (!pass.world_at(std::clamp(px + dx, 0, pass.width - 1), std::clamp(py + dy, 0, pass.height - 1), seen) ||
                                -(view * seen).z() >= depth - tol)
                                hidden = false;
                        }
                }
            }
            flags.push_back(hidden);
        }
        occluded[c.key] = std::move(flags);
    }
    return occluded;
}

std::vector<ShowcaseCalloutDraw> ShowcaseView::callouts_draw_list(const ShowcaseLabelContext &labels,
                                                                  const std::map<std::string, std::vector<bool>> *occluded_in,
                                                                  const Showcase::CalloutFade *fade, const Showcase::Scene *frame_scene,
                                                                  const std::map<std::string, Showcase::CalloutVisual> *visuals)
{
    const std::map<std::string, std::vector<bool>> &occluded = occluded_in != nullptr ? *occluded_in : m_callouts.occluded;
    std::vector<ShowcaseCalloutDraw> out;
    // S5: a preview of a path shows its timeline's callouts.
    if (frame_scene == nullptr && m_video.borrowed && m_video.pose_timeline) {
        frame_scene = &m_video.pose_scene;
        visuals     = &m_video.pose_visuals;
    }
    const Showcase::Scene &s = frame_scene != nullptr ? *frame_scene : scene();
    Matrix4d               vp;
    if (m_model == nullptr || !m_renderer->frame_view_projection(vp))
        return out;
    const Showcase::RectI frame = m_renderer->live_frame();
    // S5b, each colour once: per colour the callout that shows most (its largest region in the last
    // ID pass), with one leader, to that region.
    const bool        once = s.callouts.once_per_color;
    std::vector<bool> keep(s.callouts.items.size(), true);
    if (once) {
        std::vector<std::string> colors;
        std::vector<double>      scores;
        for (const Showcase::Callout &c : s.callouts.items) {
            bool shown = !c.hidden;
            if (visuals != nullptr)
                if (auto v = visuals->find(c.key); v != visuals->end() && v->second.alpha <= 1e-3)
                    shown = false;
            std::string key;
            if (shown)
                for (const std::string &col : labels.colors_of(c))
                    key += (key.empty() ? "" : ",") + col;
            colors.push_back(key);
            auto a = m_callouts.areas.find(c.key);
            scores.push_back(a == m_callouts.areas.end() ? 0. : double(a->second));
        }
        keep = Showcase::once_per_color_keep(colors, scores);
    }
    for (size_t i = 0; i < s.callouts.items.size(); ++i) {
        const Showcase::Callout &c = s.callouts.items[i];
        if (c.hidden || !keep[i])
            continue;
        // S5c, as used: not before the build-up has printed its filament, then fading in.
        double appear = 1.;
        if (m_co_video.as_used && m_co_video.built_z < 1e29) {
            auto         fz    = m_video.first_z.find(c.key);
            const double first = fz != m_video.first_z.end() ? fz->second : 0.;
            appear             = std::clamp((m_co_video.built_z - first) / std::max(1e-3, m_co_video.fade_mm), 0., 1.);
            if (appear <= 1e-3)
                continue;
        }
        // S5d, static: the label stays where the clip's layout put it; its leaders still follow the model.
        const Showcase::PointD *fixed_label = nullptr;
        if (m_co_video.fixed && m_video.static_valid)
            if (auto it = m_video.static_labels.find(c.key); it != m_video.static_labels.end())
                fixed_label = &it->second;
        ShowcaseCalloutDraw d;
        d.item  = int(i);
        d.label = Showcase::build_callout_label(c, labels.filament(s, c), s.callouts.fields, labels.words());
        auto         occ     = occluded.find(c.key);
        const size_t anchors = once ? std::min<size_t>(1, c.anchors.size()) : c.anchors.size();
        for (size_t a = 0; a < anchors; ++a) {
            if (m_callouts.drag == CalloutUi::Drag::Dot && m_callouts.moved && m_callouts.drag_item == int(i) &&
                m_callouts.drag_anchor == int(a) && !frame.empty()) {
                // The dot being dragged follows the pointer.
                d.anchors.push_back({ m_callouts.drag_x / frame.w, m_callouts.drag_y / frame.h });
                d.occluded.push_back(false);
                continue;
            }
            Vec3d world;
            if (!showcase_anchor_world(*m_model, c.anchors[a], world))
                continue;
            // S5c: a spot the build-up has not printed yet gets no leader (it would point at air): the
            // label stays, without a line, until it is built.
            if (world.z() > m_co_video.built_z + 0.5)
                continue;
            const Vec4d clip = vp * Vec4d(world.x(), world.y(), world.z(), 1.);
            if (clip.w() <= 1e-9)
                continue;   // behind the camera
            d.anchors.push_back({ (clip.x() / clip.w() + 1.) * 0.5, (1. - clip.y() / clip.w()) * 0.5 });
            // Design 3.3: an anchor the model hides from this view gets a dashed leader and a hollow dot
            // (stills and the live view; a video fades it instead).
            d.occluded.push_back(occ != occluded.end() && a < occ->second.size() && occ->second[a]);
            if (fade != nullptr)
                d.fade.push_back(fade->alpha(c.key, int(a)));
        }
        if (fade != nullptr)
            d.alpha = fixed_label != nullptr ? 1. : fade->label_alpha(c.key, int(anchors));
        if (visuals != nullptr) {
            auto v = visuals->find(c.key);
            if (v != visuals->end()) {
                d.visual    = v->second.alpha;
                d.highlight = v->second.highlight;
            }
        }
        d.visual *= appear;
        d.pinned     = c.pinned;
        d.pinned_pos = { c.label_pos[0], c.label_pos[1] };
        if (fixed_label != nullptr && !c.pinned) {
            // Laid out like a label the user pinned there: a leader to its nearest free side, every frame.
            d.pinned     = true;
            d.pinned_pos = *fixed_label;
        }
        out.push_back(std::move(d));
    }
    return out;
}

// S5c (the video panel's, here for showcase_anchor_world())
void ShowcaseView::video_prepare_callouts(const Showcase::VideoSettings &v, const Showcase::CameraView &base,
                                          const std::vector<Showcase::PathEvent> &events)
{
    m_video.first_z.clear();
    m_video.static_valid = false;
    m_video.static_labels.clear();
    if (m_model == nullptr || m_renderer == nullptr)
        return;
    for (const Showcase::CalloutTarget &t : Showcase::CalloutRegions(*m_model, plate_objects(), plate_changes()).targets())
        m_video.first_z[t.key] = t.first_z;
    if (v.callout_motion != Showcase::CalloutMotion::Static)
        return;
    // S5d: static callouts keep the labels where the layout of one frame puts them - the frame that
    // shows the whole model: the end of the motion when the print builds up (a static zoom-out ends
    // framed on all of it), else the first. Their leaders follow the model every frame.
    const double t     = v.build.enabled ? v.hold_start + v.duration : 0.;
    Camera      &camera = wxGetApp().plater()->get_camera();
    const Camera saved  = camera;
    const Showcase::CameraPose pose = video_pose(v, base, t, events);
    camera.look_at(Vec3d(pose.eye[0], pose.eye[1], pose.eye[2]), Vec3d(pose.target[0], pose.target[1], pose.target[2]),
                   Vec3d(pose.up[0], pose.up[1], pose.up[2]));
    camera.set_zoom(pose.zoom);
    m_renderer->prepare_live_camera();
    const CalloutVideoPass pass = m_co_video;
    m_co_video                  = CalloutVideoPass();   // every label, as on the finished model
    const Showcase::RectI frame = m_renderer->live_frame();
    const int             w     = frame.empty() ? 1920 : frame.w;
    const int             h     = frame.empty() ? 1080 : frame.h;
    const std::vector<ShowcaseCalloutDraw> draws = callouts_draw_list(callouts_labels());
    if (!draws.empty()) {
        ShowcaseImage layout;
        layout.width  = w;
        layout.height = h;
        layout.rgba.assign(size_t(w) * size_t(h) * 4, 0);
        ShowcaseCalloutOptions opt;
        opt.reserved       = showcase_watermark_rect(scene().export_settings.watermark, project(), w, h);
        opt.model_center_x = callouts_model_center_x();
        opt.model_box      = callouts_model_box();
        ShowcaseCalloutHits hits;
        showcase_draw_callouts(layout, true, scene(), draws, opt, &hits);
        if (!hits.legend) {   // a numbered legend stays put anyway
            for (const ShowcaseCalloutHits::Item &it : hits.items)
                if (it.item >= 0 && size_t(it.item) < scene().callouts.items.size())
                    m_video.static_labels[scene().callouts.items[size_t(it.item)].key] = { it.box.x / double(w), it.box.y / double(h) };
            m_video.static_valid = !m_video.static_labels.empty();
        }
    }
    m_co_video = pass;
    camera     = saved;
    m_renderer->prepare_live_camera();
}

Showcase::RectD ShowcaseView::callouts_model_box() const
{
    Matrix4d vp;
    if (m_renderer == nullptr || !m_renderer->frame_view_projection(vp))
        return {};
    const BoundingBoxf3 box = m_renderer->objects_box();
    if (!box.defined)
        return {};
    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (int i = 0; i < 8; ++i) {
        const Vec3d c((i & 1) ? box.max.x() : box.min.x(), (i & 2) ? box.max.y() : box.min.y(), (i & 4) ? box.max.z() : box.min.z());
        const Vec4d clip = vp * Vec4d(c.x(), c.y(), c.z(), 1.);
        if (clip.w() <= 1e-9)
            return {};   // partly behind the camera: no useful box
        const double x = (clip.x() / clip.w() + 1.) * 0.5, y = (1. - clip.y() / clip.w()) * 0.5;
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
    }
    x0 = std::clamp(x0, 0., 1.);
    y0 = std::clamp(y0, 0., 1.);
    x1 = std::clamp(x1, 0., 1.);
    y1 = std::clamp(y1, 0., 1.);
    return { x0, y0, x1 - x0, y1 - y0 };
}

double ShowcaseView::callouts_model_center_x() const
{
    Matrix4d vp;
    if (m_renderer == nullptr || !m_renderer->frame_view_projection(vp))
        return -1.;
    const BoundingBoxf3 box = m_renderer->objects_box();
    if (!box.defined)
        return -1.;
    const Vec3d c    = box.center();
    const Vec4d clip = vp * Vec4d(c.x(), c.y(), c.z(), 1.);
    if (clip.w() <= 1e-9)
        return -1.;
    return std::clamp((clip.x() / clip.w() + 1.) * 0.5, 0., 1.);
}

bool ShowcaseView::callouts_rasterize(int frame_w, int frame_h, ShowcaseImage &image)
{
    const Showcase::Scene &s = scene();
    // S5c: a video's preview follows its "Show Callouts" (S4 drew them in the preview regardless), and
    // the path's timeline may switch them on or off.
    const Showcase::Scene &shown   = m_video.borrowed && m_video.pose_timeline ? m_video.pose_scene : s;
    const bool             in_clip = m_video.borrowed && s.video.callouts;
    if (!m_active || (m_video.borrowed && !in_clip) || !shown.callouts.enabled || shown.callouts.items.empty() || frame_w <= 0 ||
        frame_h <= 0) {
        m_callouts.hits = ShowcaseCalloutHits();
        return false;
    }
    const std::vector<ShowcaseCalloutDraw> draws = callouts_draw_list(callouts_labels());
    image.width  = frame_w;
    image.height = frame_h;
    image.rgba.assign(size_t(frame_w) * size_t(frame_h) * 4, 0);
    ShowcaseCalloutOptions opt;
    opt.overlay        = true;
    opt.selected       = m_callouts.selected;
    opt.reserved       = showcase_watermark_rect(s.export_settings.watermark, project(), frame_w, frame_h);
    opt.model_center_x = callouts_model_center_x();
    opt.model_box      = callouts_model_box();
    showcase_draw_callouts(image, true, s, draws, opt, &m_callouts.hits);
    return true;
}

void ShowcaseView::callouts_apply_to_export(ShowcaseImage &image, bool has_alpha)
{
    const Showcase::Scene &s = scene();
    if (!s.callouts.enabled || s.callouts.items.empty() || image.empty())
        return;
    // The same anchors, layout and text as the screen, drawn at the export's size.
    const ShowcaseLabelContext labels = ShowcaseLabelContext::current(current_plate(), s.callouts.fields.weight || s.callouts.fields.price);
    ShowcaseCalloutOptions opt;
    opt.reserved       = showcase_watermark_rect(s.export_settings.watermark, project(), image.width, image.height);
    opt.model_center_x = callouts_model_center_x();
    opt.model_box      = callouts_model_box();
    showcase_draw_callouts(image, has_alpha, s, callouts_draw_list(labels), opt);
}

bool ShowcaseView::callouts_on_mouse(wxMouseEvent &evt)
{
    if (!m_active || m_renderer == nullptr || m_canvas_widget == nullptr)
        return false;
    CalloutUi &ui = m_callouts;
    auto set_cursor = [this, &ui](int kind) {
        if (kind == ui.cursor)
            return;
        ui.cursor = kind;
        m_canvas_widget->SetCursor(wxCursor(kind == 2 ? wxCURSOR_HAND : kind == 1 ? wxCURSOR_SIZING : wxCURSOR_ARROW));
    };
    const Showcase::Scene &s = scene();
    const Showcase::RectI  f = m_renderer->live_frame();
    if (!s.callouts.enabled || s.callouts.items.empty() || read_only() || f.empty()) {
        set_cursor(0);
        ui.drag = CalloutUi::Drag::None;
        return false;
    }
    const double x = double(evt.GetX() - f.x), y = double(evt.GetY() - f.y);

    // A drag under way.
    if (ui.drag != CalloutUi::Drag::None) {
        if (evt.Dragging() && evt.LeftIsDown()) {
            if (std::hypot(x - ui.press_x, y - ui.press_y) > 3.)
                ui.moved = true;
            if (ui.moved) {
                if (ui.drag == CalloutUi::Drag::Label) {
                    Showcase::Scene &sc = scene();
                    if (ui.drag_item >= 0 && size_t(ui.drag_item) < sc.callouts.items.size()) {
                        Showcase::Callout &c = sc.callouts.items[size_t(ui.drag_item)];
                        c.pinned    = true;
                        c.label_pos = { std::clamp((x - ui.grab_dx) / double(f.w), 0., 1.), std::clamp((y - ui.grab_dy) / double(f.h), 0., 1.) };
                    }
                } else {
                    ui.drag_x = std::clamp(x, 0., double(f.w));
                    ui.drag_y = std::clamp(y, 0., double(f.h));
                }
                callouts_changed();
            }
            return true;
        }
        if (evt.LeftUp() || !evt.LeftIsDown()) {
            const CalloutUi::Drag drag = ui.drag;
            const int             item = ui.drag_item;
            ui.drag                    = CalloutUi::Drag::None;
            if (m_canvas_widget->HasCapture())
                m_canvas_widget->ReleaseMouse();
            if (ui.moved && item >= 0 && size_t(item) < s.callouts.items.size()) {
                if (drag == CalloutUi::Drag::Label) {
                    mark_project_dirty();
                    update_enabled();
                } else
                    // Dropped: picked on the model in the next frame (it needs the GL context).
                    ui.picks.push_back({ s.callouts.items[size_t(item)].key, ui.drag_anchor, ui.drag_x / double(f.w), ui.drag_y / double(f.h) });
                ui.occlusion_camera = 0;
                callouts_changed();
            } else
                callouts_select(item);
            return true;
        }
        return true;
    }

    // What is under the pointer: dots first (they are small), then labels, topmost first.
    int  hit_item = -1, dot_index = 0;
    bool on_dot   = false;
    Showcase::RectD hit_box;
    const double reach = std::max(6., 0.012 * double(f.h));
    for (auto it = ui.hits.items.rbegin(); it != ui.hits.items.rend() && hit_item < 0; ++it)
        for (size_t d = 0; d < it->dots.size(); ++d)
            if (std::hypot(it->dots[d].x - x, it->dots[d].y - y) <= reach) {
                hit_item  = it->item;
                dot_index = int(d);
                on_dot    = true;
                hit_box   = it->box;
                break;
            }
    for (auto it = ui.hits.items.rbegin(); it != ui.hits.items.rend() && hit_item < 0; ++it)
        if (it->box.contains({ x, y })) {
            hit_item = it->item;
            hit_box  = it->box;
        }
    set_cursor(hit_item < 0 ? 0 : on_dot ? 2 : 1);
    if (evt.Leaving())
        set_cursor(0);
    if (hit_item < 0) {
        if (evt.LeftDown() && ui.selected >= 0)
            callouts_select(-1);   // and the camera orbits as usual
        return false;
    }
    if (evt.LeftDClick()) {
        CallAfter([this, hit_item]() { callouts_edit(hit_item); });
        return true;
    }
    if (evt.LeftDown() && !on_dot && ui.hits.legend) {
        // The legend's rows stay in the key: a click only selects.
        callouts_select(hit_item);
        return true;
    }
    if (evt.LeftDown()) {
        commit();
        ui.drag        = on_dot ? CalloutUi::Drag::Dot : CalloutUi::Drag::Label;
        ui.drag_item   = hit_item;
        ui.drag_anchor = dot_index;
        ui.moved       = false;
        ui.press_x     = x;
        ui.press_y     = y;
        ui.grab_dx     = x - hit_box.x;
        ui.grab_dy     = y - hit_box.y;
        ui.drag_x      = x;
        ui.drag_y      = y;
        if (!m_canvas_widget->HasCapture())
            m_canvas_widget->CaptureMouse();
        return true;
    }
    if (evt.RightDown())
        return true;
    if (evt.RightUp()) {
        callouts_select(hit_item);
        CallAfter([this, hit_item]() { callouts_context_menu(hit_item); });
        return true;
    }
    return false;
}

void ShowcaseView::callouts_select(int item)
{
    if (item != m_callouts.selected) {
        m_callouts.selected = item;
        callouts_changed();
    }
    if (m_co_list != nullptr) {
        const bool syncing = m_syncing;
        m_syncing          = true;
        int sel = -1;
        for (unsigned i = 0; i < m_co_list->GetCount(); ++i)
            if (int(reinterpret_cast<intptr_t>(m_co_list->GetClientData(i))) == item)
                sel = int(i);
        m_co_list->SetSelection(sel);
        m_syncing = syncing;
    }
    update_enabled();
}

void ShowcaseView::callouts_edit(int item)
{
    if (read_only())
        return;
    const Showcase::Scene &s = scene();
    if (item < 0 || size_t(item) >= s.callouts.items.size())
        return;
    callouts_select(item);
    const Showcase::Callout    c      = s.callouts.items[size_t(item)];
    const ShowcaseLabelContext labels = ShowcaseLabelContext::current(current_plate(), false);
    // What the label says without the user's text.
    Showcase::Callout plain = c;
    plain.title.clear();
    plain.color_name.clear();
    plain.note.clear();
    const Showcase::CalloutLabel auto_label = Showcase::build_callout_label(plain, labels.filament(s, plain), Showcase::CalloutFields(), labels.words());
    const std::string            current    = Showcase::callout_first_line(Showcase::build_callout_label(c, labels.filament(s, c),
                                                                                                         Showcase::CalloutFields(), labels.words()));
    const wxString heading = current.empty() ? wxString::Format(_L("Filament %d"), c.filament) : wxString::FromUTF8(current);
    ShowcaseCalloutDialog dlg(this, c, heading, auto_label.title, auto_label.color_name, labels.library_name(c));
    if (dlg.ShowModal() != wxID_OK)
        return;
    const Showcase::Callout r = dlg.result();
    edit([item, &r](Showcase::Scene &sc) {
        if (size_t(item) < sc.callouts.items.size())
            sc.callouts.items[size_t(item)] = r;
    });
    refresh_callout_list();
    update_enabled();
}

void ShowcaseView::callouts_context_menu(int item)
{
    const Showcase::Scene &s = scene();
    if (read_only() || item < 0 || size_t(item) >= s.callouts.items.size() || m_canvas_widget == nullptr)
        return;
    const Showcase::Callout &c = s.callouts.items[size_t(item)];
    enum { ID_EDIT = 1, ID_HIDE, ID_UNPIN, ID_REANCHOR };
    wxMenu menu;
    menu.Append(ID_EDIT, _L("Edit..."));
    menu.Append(ID_HIDE, _L("Hide"));
    if (c.pinned)
        menu.Append(ID_UNPIN, _L("Back to the automatic layout"));
    if (c.user_anchor)
        menu.Append(ID_REANCHOR, _L("Place the dot automatically"));
    const int chosen = m_canvas_widget->GetPopupMenuSelectionFromUser(menu);
    switch (chosen) {
    case ID_EDIT: callouts_edit(item); break;
    case ID_HIDE:
        edit([item](Showcase::Scene &sc) {
            if (size_t(item) < sc.callouts.items.size())
                sc.callouts.items[size_t(item)].hidden = true;
        });
        refresh_callout_list();
        break;
    case ID_UNPIN:
        edit([item](Showcase::Scene &sc) {
            if (size_t(item) < sc.callouts.items.size())
                sc.callouts.items[size_t(item)].pinned = false;
        });
        break;
    case ID_REANCHOR:
        edit([item](Showcase::Scene &sc) {
            if (size_t(item) < sc.callouts.items.size()) {
                sc.callouts.items[size_t(item)].anchors.clear();
                sc.callouts.items[size_t(item)].user_anchor = false;
            }
        });
        callouts_sync(false);
        break;
    default: break;
    }
}

void ShowcaseView::refresh_callout_list()
{
    if (m_co_list == nullptr)
        return;
    const bool syncing = m_syncing;
    m_syncing          = true;
    m_co_list->Clear();
    int                    sel = -1;
    const Showcase::Scene &s   = scene();
    if (s.callouts.enabled && !s.callouts.items.empty()) {
        const ShowcaseLabelContext &labels = callouts_labels();
        for (size_t i = 0; i < s.callouts.items.size(); ++i) {
            const Showcase::Callout &c    = s.callouts.items[i];
            const std::string        text = Showcase::callout_first_line(
                Showcase::build_callout_label(c, labels.filament(s, c), Showcase::CalloutFields(), labels.words()));
            wxString label = wxString::Format("%d  ", c.filament) + (text.empty() ? wxString::Format(_L("Filament %d"), c.filament) : wxString::FromUTF8(text));
            if (c.z_from >= 0.)
                label += wxString::Format(" (%s)", wxString::FromUTF8(Showcase::build_callout_label(c, Showcase::LabelFilament(),
                                                                                                    Showcase::CalloutFields{ false, false, false, false, false, false, false },
                                                                                                    labels.words()).details));
            if (c.hidden)
                label += " " + _L("(hidden)");
            m_co_list->Append(label, reinterpret_cast<void *>(intptr_t(i)));
            if (int(i) == m_callouts.selected)
                sel = int(m_co_list->GetCount()) - 1;
        }
    }
    m_co_list->SetSelection(sel);
    m_syncing = syncing;
}

} // namespace GUI
} // namespace Slic3r
