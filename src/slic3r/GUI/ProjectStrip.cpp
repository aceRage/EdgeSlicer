#include "ProjectStrip.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <wx/checklst.h>
#include <wx/choicdlg.h>
#include <wx/control.h>
#include <wx/dcbuffer.h>
#include <wx/filedlg.h>
#include <wx/image.h>
#include <wx/menu.h>
#include <wx/mstream.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/button.h>

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/ProjectSwapGuard.hpp"
#include "libslic3r/Utils.hpp"

#include "GUI.hpp"
#include "GLCanvas3D.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MainTabs.hpp"
#include "MsgDialog.hpp"
#include "Notebook.hpp"
#include "NotificationManager.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include "wxExtensions.hpp"
#include "../Utils/Process.hpp"

namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {

namespace {

const char *CFG_COLLAPSED = "project_strip_collapsed";
const char *CFG_TEXT_MODE = "project_strip_text_mode";
const char *CFG_MAX_OPEN  = "project_strip_max_open";
const char *CFG_WIDTH     = "project_strip_width";
const char *CFG_SHELL     = "project_strip_shell_open";
const char *CFG_SESSION   = "project_strip_restore_session";

long long ms_since(std::chrono::steady_clock::time_point t)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t).count();
}

void remove_literal(const std::string &path)
{
    if (path.empty())
        return;
    boost::system::error_code ec;
    fs::remove(fs::path(path), ec);
    if (ec)
        BOOST_LOG_TRIVIAL(warning) << "project strip: could not remove " << path << ": " << ec.message();
}

// Removes a folder only when it is empty (never its content).
void remove_empty_dir(const std::string &path)
{
    boost::system::error_code ec;
    if (fs::is_directory(fs::path(path), ec) && fs::is_empty(fs::path(path), ec) && !ec)
        fs::remove(fs::path(path), ec);
}

wxColour accent_colour() { return wxColour(0, 174, 66); }

// The main tab that shows a Plater's current view.
int tab_for_view(const Plater &plater)
{
    if (plater.is_preview_shown())
        return int(MainTabs::Preview);
    if (plater.is_showcase_shown())
        return int(MainTabs::Showcase);
    return int(MainTabs::Editor3D);
}

} // namespace

// ---- SwitchScope ----------------------------------------------------------------------------------------------------------

ProjectStrip::SwitchScope::SwitchScope(ProjectStrip &strip, bool swap_guard) : m_strip(strip), m_swap_guard(swap_guard)
{
    m_strip.m_switching = true;
    Plater::set_project_switch_busy(true);
    if (m_swap_guard)
        ProjectSwapGuard::hold();
}

ProjectStrip::SwitchScope::~SwitchScope()
{
    Plater::set_project_switch_busy(false);
    m_strip.m_switching = false;
    // The preset refresh queued work for later (the printer tab's nozzle refresh, the height-pattern check): wx runs
    // CallAfter in order, so this release comes after it and the rack memory / variant follow stay out of it too.
    if (m_swap_guard)
        wxGetApp().CallAfter([]() { ProjectSwapGuard::release(); });
    m_strip.sync_active();
    m_strip.Refresh();
}

// ---- ProjectStrip ---------------------------------------------------------------------------------------------------------

ProjectStrip::ProjectStrip(wxWindow *parent, MainFrame *frame)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxFULL_REPAINT_ON_RESIZE | wxBORDER_NONE), m_frame(frame)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    if (AppConfig *cfg = wxGetApp().app_config) {
        m_collapsed = cfg->get(CFG_COLLAPSED) == "1";
        m_text_mode = cfg->get(CFG_TEXT_MODE) == "1";
        m_width_dip = StripLayout::parse_width(cfg->get(CFG_WIDTH));
    }
    refresh_max_open();
    // Session snapshots, per process and per slot, under the app's temp folder (the backup folders live there too).
    m_root = (fs::path(temporary_dir()) / "project_strip" / std::to_string(get_current_pid())).string();

    // The project the window starts with, in the window's first Plater.
    const int first = m_sessions.add("", into_u8(_L("Untitled")));
    m_sessions.select(first);
    m_extra[first].plater = frame != nullptr ? frame->plater() : nullptr;

    // New and Open: two icon buttons at the top (owner feedback on P1), the words in their tooltips.
    m_btn_new  = new ScalableButton(this, wxID_ANY, "add", wxEmptyString, wxDefaultSize, wxDefaultPosition, wxBU_EXACTFIT | wxNO_BORDER, true, 16);
    m_btn_open = new ScalableButton(this, wxID_ANY, "menu_open", wxEmptyString, wxDefaultSize, wxDefaultPosition, wxBU_EXACTFIT | wxNO_BORDER, true, 16);
    m_btn_new->SetToolTip(_L("New project"));
    m_btn_open->SetToolTip(_L("Open project"));
    m_btn_new->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { new_project(); });
    m_btn_open->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { open_project(); });
    apply_colours();

    m_poll_timer.SetOwner(this);
    m_thumb_timer.SetOwner(this);
    Bind(wxEVT_TIMER, &ProjectStrip::on_poll, this, m_poll_timer.GetId());
    Bind(wxEVT_TIMER, &ProjectStrip::on_thumbnail_timer, this, m_thumb_timer.GetId());
    m_poll_timer.Start(500);
    // P3 session restore: once the window and its startup project are up.
    m_session_timer.SetOwner(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent &) { restore_session(); }, m_session_timer.GetId());
    if (wxGetApp().app_config != nullptr && wxGetApp().app_config->get_bool(CFG_SESSION))
        m_session_timer.StartOnce(2500);

    Bind(wxEVT_PAINT, &ProjectStrip::on_paint, this);
    Bind(wxEVT_LEFT_UP, &ProjectStrip::on_left_up, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, &ProjectStrip::on_capture_lost, this);
    Bind(wxEVT_SIZE, &ProjectStrip::on_size, this);
    Bind(wxEVT_LEFT_DOWN, &ProjectStrip::on_left_down, this);
    Bind(wxEVT_MIDDLE_DOWN, &ProjectStrip::on_middle_down, this);
    Bind(wxEVT_RIGHT_DOWN, &ProjectStrip::on_right_down, this);
    Bind(wxEVT_MOTION, &ProjectStrip::on_motion, this);
    Bind(wxEVT_LEAVE_WINDOW, &ProjectStrip::on_leave, this);
    Bind(wxEVT_MOUSEWHEEL, &ProjectStrip::on_wheel, this);
    apply_width();
}

ProjectStrip::~ProjectStrip()
{
    m_poll_timer.Stop();
    m_thumb_timer.Stop();
    m_session_timer.Stop();
}

Plater *ProjectStrip::plater() const { return wxGetApp().plater(); }

Plater *ProjectStrip::plater_of(int id) const
{
    auto it = m_extra.find(id);
    return it == m_extra.end() ? nullptr : it->second.plater;
}

void ProjectStrip::refresh_max_open()
{
    if (AppConfig *cfg = wxGetApp().app_config)
        m_sessions.set_max_open(ProjectSessions::parse_max_open(cfg->get(CFG_MAX_OPEN)));
}

std::string ProjectStrip::slot_dir(int id) const { return (fs::path(m_root) / ("slot-" + std::to_string(id))).string(); }

void ProjectStrip::toast(const wxString &text) const
{
    BOOST_LOG_TRIVIAL(info) << "project strip: " << into_u8(text);
    if (Plater *p = plater())
        if (NotificationManager *nm = p->get_notification_manager())
            nm->push_notification(NotificationType::CustomNotification, NotificationManager::NotificationLevel::RegularNotificationLevel,
                                  into_u8(text));
}

bool ProjectStrip::active_is_blank() const
{
    const ProjectSlot *slot = m_sessions.active();
    Plater            *p    = plater();
    return slot != nullptr && p != nullptr && p->model().objects.empty() && !p->is_project_dirty() &&
           p->get_project_filename(".3mf").IsEmpty();
}

void ProjectStrip::sync_active()
{
    if (m_switching)
        return;
    Plater      *p    = plater();
    ProjectSlot *slot = m_sessions.active();
    if (p == nullptr || slot == nullptr || plater_of(slot->id) != p)
        return;
    // A Save As, or a project opened in place of this one from elsewhere (Home, the hub, another instance), changes them.
    slot->origin_path = into_u8(p->get_project_filename(".3mf"));
    const std::string name = into_u8(p->get_project_name());
    if (!name.empty())
        slot->name = name;
    slot->dirty = p->is_project_dirty();
    slot->empty = p->model().objects.empty();
}

void ProjectStrip::on_active_project_changed()
{
    if (m_switching)
        return;
    sync_active();
    Refresh();
}

bool ProjectStrip::can_leave_active()
{
    Plater *p = plater();
    if (p == nullptr || m_switching || m_frame == nullptr)
        return false;
    const wxString why = p->project_swap_blocker();
    if (!why.empty()) {
        toast(_L("The project cannot be switched now.") + " " + why);
        return false;
    }
    return true;
}

void ProjectStrip::load_thumbnail(int id, const std::string &snapshot_3mf)
{
    if (snapshot_3mf.empty())
        return;
    const std::string png = bbs_3mf_get_thumbnail(snapshot_3mf.c_str());
    if (png.empty())
        return;
    wxMemoryInputStream stream(png.data(), png.size());
    wxImage             image;
    if (!image.LoadFile(stream, wxBITMAP_TYPE_PNG) || !image.IsOk())
        return;
    SlotExtra &extra  = m_extra[id];
    extra.image       = image;
    extra.scaled      = wxBitmap();
    extra.scaled_side = 0;
}

// The tile's thumbnail at `side` px (the thumbnail grows with the strip), scaled once per size.
const wxBitmap *ProjectStrip::tile_bitmap(int id, int side)
{
    auto it = m_extra.find(id);
    if (it == m_extra.end() || !it->second.image.IsOk())
        return nullptr;
    SlotExtra &extra = it->second;
    if (extra.scaled_side != side || !extra.scaled.IsOk()) {
        wxImage      image = extra.image;
        const double scale = std::min(double(side) / image.GetWidth(), double(side) / image.GetHeight());
        image.Rescale(std::max(1, int(image.GetWidth() * scale)), std::max(1, int(image.GetHeight() * scale)), wxIMAGE_QUALITY_HIGH);
        extra.scaled      = wxBitmap(image);
        extra.scaled_side = side;
    }
    return &extra.scaled;
}

void ProjectStrip::stash_active()
{
    ProjectSlot *slot = m_sessions.active();
    Plater      *p    = plater();
    if (slot == nullptr || p == nullptr)
        return;
    sync_active();
    // The tab it was on; from Home or Device, the tab of its current view.
    const int tab  = m_frame != nullptr && m_frame->m_tabpanel != nullptr ? m_frame->m_tabpanel->GetSelection() : int(MainTabs::Editor3D);
    slot->main_tab = MainTabs::hosts_plater(tab) ? tab : tab_for_view(*p);
    slot->plate    = p->get_partplate_list().get_curr_plate_index();
    // Owner P4 hand test: a project opened and left before its live thumbnail was due (1.5 s after the last change) kept
    // the placeholder. Its thumbnail is rendered now, while it is still the one on screen, when the tile's is not current.
    if (const SlotExtra &e = m_extra[slot->id]; tile_thumbnail_stale(e.image.IsOk(), e.thumb_marker, active_thumbnail_marker())) {
        m_thumb_timer.Stop();
        // Off screen too: Recent projects are opened from Home, so the project may be left without its canvas shown (the
        // thumbnail renders into an off-screen buffer, as the 3MF plate thumbnails do).
        render_active_thumbnail(true);
    }
    // Its presets, unsaved edits included: the app-global bundle is the next project's from now on.
    m_extra[slot->id].presets = wxGetApp().preset_bundle->capture_project_presets();
}

long long ProjectStrip::show_slot(int id)
{
    auto it = m_extra.find(id);
    if (it == m_extra.end() || it->second.plater == nullptr || m_frame == nullptr)
        return 0;
    SlotExtra &extra = it->second;
    Plater    *p     = extra.plater;
    m_frame->activate_plater(p);

    const auto     started = std::chrono::steady_clock::now();
    PresetBundle  *bundle  = wxGetApp().preset_bundle;
    if (bundle == nullptr || !extra.presets.valid || bundle->project_presets_current(extra.presets))
        return 0;
    // Its presets back in the bundle, then the Tab pages and the sidebar as a project load refreshes them. The swap guard
    // (SwitchScope) keeps the per-printer nozzle memory and the variant follow out of it: they are the project's own.
    std::vector<std::string> missing;
    bundle->restore_project_presets(extra.presets, &missing);
    wxGetApp().load_current_presets(false, false);
    // The printer tab re-derives some project settings while it loads; the project's own win.
    bundle->project_config = extra.presets.project_config;
    p->sidebar().update_presets(Preset::TYPE_PRINTER);
    p->sidebar().update_presets(Preset::TYPE_FILAMENT);
    p->update_project_dirty_from_presets();
    if (!missing.empty()) {
        wxString names;
        for (const std::string &name : missing)
            names += (names.empty() ? "" : ", ") + from_u8(name);
        toast(wxString::Format(_L("This project's preset %s no longer exists. Its values are kept as unsaved changes."), names));
    }
    return ms_since(started);
}

bool ProjectStrip::switch_to(int id, const char *mode)
{
    if (id == m_sessions.active_id())
        return true;
    if (plater_of(id) == nullptr)
        return false;
    const auto started    = std::chrono::steady_clock::now();
    long long  presets_ms = 0;
    {
        SwitchScope scope(*this, true);
        BOOST_LOG_TRIVIAL(info) << "project strip: switching from slot " << m_sessions.active_id() << " to slot " << id;
        stash_active();
        presets_ms = show_slot(id);
        m_sessions.select(id);
    }
    after_switch(id, started, presets_ms, mode);
    return true;
}

void ProjectStrip::after_switch(int id, std::chrono::steady_clock::time_point started, long long presets_ms, const char *mode)
{
    ProjectSlot *slot = m_sessions.find(id);
    Plater      *p    = plater();
    if (slot != nullptr && p != nullptr && m_frame != nullptr && m_frame->m_tabpanel != nullptr) {
        // On a plater tab: the tab of the project's own view (it kept it). On Home or Device: stay there.
        if (MainTabs::hosts_plater(m_frame->m_tabpanel->GetSelection())) {
            const int want = MainTabs::hosts_plater(slot->main_tab) ? slot->main_tab : tab_for_view(*p);
            if (m_frame->m_tabpanel->GetSelection() != want)
                m_frame->select_tab(size_t(want));
            else if (tab_for_view(*p) != want)
                // The tab is the same but this Plater shows another view: line them up as a tab change does.
                p->select_view_3D(want == int(MainTabs::Preview) ? "Preview" : want == int(MainTabs::Showcase) ? "Showcase" : "3D");
        }
    }
    if (p != nullptr) {
        if (GLCanvas3D *canvas = p->get_current_canvas3D())
            canvas->set_as_dirty();
        // The live thumbnail: none to render while the tile's is current; a project with none yet gets one shortly.
        auto it         = m_extra.find(id);
        m_active_marker = it != m_extra.end() && it->second.image.IsOk() ?
                              p->project_change_marker() * 64 + size_t(std::max(0, p->get_partplate_list().get_curr_plate_index())) :
                              size_t(-1);
    }
    const long long total = ms_since(started);
    BOOST_LOG_TRIVIAL(info) << "project strip: switch to slot " << id << " took " << total << " ms (presets " << presets_ms << " ms, "
                            << m_sessions.size() << " open, " << mode << ")" << log_memory_info();
    wxTimerEvent dummy;
    on_poll(dummy);
    thumbnail_after_load();
}

bool ProjectStrip::select_project(int id)
{
    if (m_sessions.find(id) == nullptr)
        return false;
    if (id == m_sessions.active_id())
        return true;
    if (!can_leave_active())
        return false;
    return switch_to(id, "switch");
}

void ProjectStrip::cycle(int step)
{
    const int id = m_sessions.neighbour_id(step);
    if (id >= 0)
        select_project(id);
}

bool ProjectStrip::make_room()
{
    refresh_max_open();
    if (m_sessions.can_add())
        return true;
    // D3 / P4: the cap bounds the live projects (memory). Close one to open another, with the usual unsaved prompt.
    wxArrayString names;
    std::vector<int> ids;
    for (const ProjectSlot &s : m_sessions.slots()) {
        names.Add(from_u8(s.name) + (s.dirty ? wxString(" *") : wxString()));
        ids.push_back(s.id);
    }
    wxSingleChoiceDialog dlg(m_frame,
                             wxString::Format(_L("%d projects are open, the most this window keeps (Preferences > Project). Close one "
                                                 "to open another:"),
                                              m_sessions.max_open()),
                             _L("Too many open projects"), names);
    wxGetApp().UpdateDlgDarkUI(&dlg);
    if (dlg.ShowModal() != wxID_OK || dlg.GetSelection() < 0 || dlg.GetSelection() >= int(ids.size()))
        return false;
    return close_project(ids[size_t(dlg.GetSelection())]) && m_sessions.can_add();
}

void ProjectStrip::new_project()
{
    Plater *p = plater();
    if (p == nullptr || m_frame == nullptr)
        return;
    // An empty Untitled project is simply started again, as before the strip.
    if (active_is_blank() || m_sessions.size() == 0) {
        p->new_project();
        return;
    }
    if (!can_leave_active() || !make_room())
        return;
    const auto started = std::chrono::steady_clock::now();
    int        id      = -1;
    {
        // A brand-new project starts as File > New does: no swap guard (its nozzles come from the printer's memory).
        SwitchScope scope(*this, false);
        stash_active();
        Plater *fresh = m_frame->create_project_plater();
        if (fresh == nullptr)
            return;
        const std::string name = m_sessions.next_untitled_name(into_u8(_L("Untitled")));
        id                     = m_sessions.add("", name);
        m_extra[id].plater     = fresh;
        m_frame->activate_plater(fresh);
        m_sessions.select(id);
        // On the printer of the project it was started from, without that project's unsaved preset edits (they stay with it).
        fresh->new_project(true, false, from_u8(name));
    }
    if (ProjectSlot *slot = m_sessions.find(id))
        slot->main_tab = int(MainTabs::Editor3D);
    after_switch(id, started, 0, "new");
}

void ProjectStrip::open_project(const wxString &file_in)
{
    Plater *p = plater();
    if (p == nullptr || m_frame == nullptr)
        return;
    wxString file = file_in;
    if (file.empty()) {
        wxGetApp().load_project(m_frame, file);
        if (file.empty())
            return;
    }
    // Open already: its slot.
    if (const int existing = m_sessions.find_by_origin(into_u8(file)); existing >= 0) {
        select_project(existing);
        return;
    }
    if (active_is_blank()) {
        p->load_project_unprompted(file);
        sync_active();
        thumbnail_after_load();
        Refresh();
        return;
    }
    if (!can_leave_active() || !make_room())
        return;
    const auto started = std::chrono::steady_clock::now();
    int        id      = -1;
    {
        // Opening a file is opening a file: its 3MF brings its printer and nozzles as always (no swap guard).
        SwitchScope scope(*this, false);
        stash_active();
        Plater *fresh = m_frame->create_project_plater();
        if (fresh == nullptr)
            return;
        id                 = m_sessions.add(into_u8(file), ProjectSessions::name_from_path(into_u8(file)));
        m_extra[id].plater = fresh;
        m_frame->activate_plater(fresh);
        m_sessions.select(id);
        fresh->load_project_unprompted(file);
    }
    if (ProjectSlot *slot = m_sessions.find(id))
        slot->main_tab = int(MainTabs::Editor3D);
    after_switch(id, started, 0, "open");
}

bool ProjectStrip::save_slot(int id, bool save_as)
{
    // A project saves with its own presets in the bundle: a hidden one is shown first.
    if (id != m_sessions.active_id() && !select_project(id))
        return false;
    Plater *p = plater();
    if (p == nullptr)
        return false;
    const bool ok = p->save_project(save_as) == wxID_YES;
    sync_active();
    Refresh();
    return ok;
}

void ProjectStrip::remove_slot_files(int id)
{
    auto it = m_extra.find(id);
    if (it == m_extra.end())
        return;
    for (const std::string &f : it->second.files)
        remove_literal(f);
    it->second.files.clear();
    if (!it->second.dir.empty()) {
        remove_empty_dir(it->second.dir);
        // A restored session's slots sit in the previous run's folder: it goes once empty.
        if (fs::path(it->second.dir).parent_path() != fs::path(m_root))
            remove_empty_dir(fs::path(it->second.dir).parent_path().string());
    }
}

bool ProjectStrip::close_project(int id)
{
    ProjectSlot *slot = m_sessions.find(id);
    Plater      *pl   = plater_of(id);
    if (slot == nullptr || pl == nullptr || m_switching || m_frame == nullptr)
        return false;
    const bool is_active = id == m_sessions.active_id();
    if (is_active) {
        sync_active();
        // A job or an open tool holds the shown project.
        if (!pl->project_swap_blocker().empty()) {
            toast(_L("The project cannot be closed now.") + " " + pl->project_swap_blocker());
            return false;
        }
    }
    if (slot->dirty) {
        MessageDialog dlg(m_frame, wxString::Format(_L("\"%s\" has unsaved changes. Save them before closing it?"), from_u8(slot->name)),
                          _L("Close project"), wxYES_NO | wxCANCEL | wxYES_DEFAULT | wxCENTRE);
        const int answer = dlg.ShowModal();
        if (answer == wxID_CANCEL)
            return false;
        if (answer == wxID_YES && !save_slot(id, false))
            return false;
        slot = m_sessions.find(id);
        if (slot == nullptr)
            return false;
    }
    // A project that is slicing closes once its slice is stopped.
    if (pl->is_background_process_slicing() || pl->is_slicing_in_background()) {
        MessageDialog dlg(m_frame, wxString::Format(_L("\"%s\" is slicing. Stop slicing and close it?"), from_u8(slot->name)),
                          _L("Close project"), wxYES_NO | wxNO_DEFAULT | wxCENTRE);
        if (dlg.ShowModal() != wxID_YES)
            return false;
        pl->cancel_slicing_for_project_close();
    }

    if (id != m_sessions.active_id()) {
        // Hidden: its Plater goes, nothing on screen changes.
        m_frame->destroy_project_plater(pl);
        remove_slot_files(id);
        m_sessions.close(id);
        m_extra.erase(id);
        Refresh();
        return true;
    }

    const int successor = m_sessions.successor_on_close(id);
    if (successor < 0) {
        // The last project: an empty one takes its place in the same Plater, the strip always has one.
        pl->new_project(true);
        remove_slot_files(id);
        m_sessions.close(id);
        m_extra.erase(id);
        const int fresh        = m_sessions.add("", into_u8(_L("Untitled")));
        m_extra[fresh].plater  = pl;
        m_sessions.select(fresh);
        sync_active();
        Refresh();
        return true;
    }
    const auto started    = std::chrono::steady_clock::now();
    long long  presets_ms = 0;
    {
        SwitchScope scope(*this, true);
        presets_ms = show_slot(successor);
        m_frame->destroy_project_plater(pl);
        remove_slot_files(id);
        m_sessions.close(id);
        m_extra.erase(id);
        m_sessions.select(successor);
    }
    after_switch(successor, started, presets_ms, "close");
    return true;
}

void ProjectStrip::close_others(int id)
{
    if (id != m_sessions.active_id() && !select_project(id))
        return;
    std::vector<int> others;
    for (const ProjectSlot &s : m_sessions.slots())
        if (s.id != id)
            others.push_back(s.id);
    for (int other : others)
        if (!close_project(other))
            break;
}

void ProjectStrip::open_in_new_window(int id)
{
    ProjectSlot *slot = m_sessions.find(id);
    if (slot == nullptr)
        return;
    if (id == m_sessions.active_id())
        sync_active();
    if (slot->dirty || slot->origin_path.empty()) {
        MessageDialog dlg(m_frame,
                          wxString::Format(_L("\"%s\" has to be saved to open it in a new window. Save it now?"), from_u8(slot->name)),
                          _L("Open in new window"), wxYES_NO | wxYES_DEFAULT | wxCENTRE);
        if (dlg.ShowModal() != wxID_YES || !save_slot(id, false))
            return;
        slot = m_sessions.find(id);
        if (slot == nullptr || slot->origin_path.empty())
            return;
    }
    const wxString path = from_u8(slot->origin_path);
    if (!close_project(id))
        return;
    start_new_slicer(&path);
}

bool ProjectStrip::confirm_close_all()
{
    sync_active();
    const std::vector<int> dirty = m_sessions.dirty_ids();
    if (dirty.empty() || plater() == nullptr)
        return true;

    wxDialog dlg(m_frame, wxID_ANY, _L("Unsaved projects"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE);
    auto    *sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(new wxStaticText(&dlg, wxID_ANY, _L("These projects have unsaved changes. Save the ticked ones before closing?")), 0,
               wxALL, FromDIP(12));
    wxArrayString labels;
    for (int id : dirty) {
        const ProjectSlot *slot = m_sessions.find(id);
        labels.Add(from_u8(slot->name) + (slot->origin_path.empty() ? wxString() : "  (" + from_u8(slot->origin_path) + ")"));
    }
    auto *list = new wxCheckListBox(&dlg, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(420), FromDIP(160)), labels);
    for (unsigned i = 0; i < list->GetCount(); ++i)
        list->Check(i, true);
    sizer->Add(list, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    auto *buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->AddStretchSpacer();
    buttons->Add(new wxButton(&dlg, wxID_YES, _L("Save")), 0, wxRIGHT, FromDIP(8));
    buttons->Add(new wxButton(&dlg, wxID_NO, _L("Don't save")), 0, wxRIGHT, FromDIP(8));
    buttons->Add(new wxButton(&dlg, wxID_CANCEL, _L("Cancel")), 0);
    sizer->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(12));
    dlg.Bind(wxEVT_BUTTON, [&dlg](wxCommandEvent &e) { dlg.EndModal(e.GetId()); });
    dlg.SetSizerAndFit(sizer);
    dlg.CentreOnParent();
    wxGetApp().UpdateDlgDarkUI(&dlg);
    const int answer = dlg.ShowModal();
    if (answer != wxID_YES && answer != wxID_NO)
        return false;
    if (answer == wxID_YES) {
        // Each one is saved shown (with its own presets in the bundle).
        for (unsigned i = 0; i < list->GetCount(); ++i)
            if (list->IsChecked(i) && !save_slot(dirty[i], false))
                return false;
    }
    // What close_with_confirm() does once answered: the shown project's crash backup goes (the hidden ones' go with their
    // Platers as the window closes).
    Plater *p = plater();
    p->model().set_backup_path("");
    p->up_to_date(true, false);
    p->up_to_date(true, true);
    return true;
}

void ProjectStrip::remove_all_slot_files()
{
    // Session restore: the snapshots are what the next start reads.
    if (m_keep_files_for_session)
        return;
    std::vector<int> ids;
    for (const ProjectSlot &s : m_sessions.slots())
        ids.push_back(s.id);
    for (int id : ids)
        remove_slot_files(id);
    remove_empty_dir(m_root);
    remove_empty_dir(fs::path(m_root).parent_path().string());
}

wxString ProjectStrip::tooltip_for(const ProjectSlot &slot) const
{
    wxString tip = from_u8(slot.name);
    if (!slot.origin_path.empty())
        tip += "\n" + from_u8(slot.origin_path);
    if (slot.dirty)
        tip += "\n" + _L("Unsaved changes");
    if (slot.slicing)
        tip += "\n" + _L("Slicing");
    return tip;
}

void ProjectStrip::rescale()
{
    // Same images, new tile sizes.
    for (auto &kv : m_extra) {
        kv.second.scaled      = wxBitmap();
        kv.second.scaled_side = 0;
    }
    if (m_btn_new != nullptr)
        m_btn_new->msw_rescale();
    if (m_btn_open != nullptr)
        m_btn_open->msw_rescale();
    apply_width();
    Refresh();
}

void ProjectStrip::sys_color_changed()
{
    apply_colours();
    if (m_btn_new != nullptr)
        m_btn_new->msw_rescale();
    if (m_btn_open != nullptr)
        m_btn_open->msw_rescale();
    Refresh();
}

void ProjectStrip::apply_colours()
{
    const wxColour bg = wxGetApp().dark_mode() ? wxColour(0x2B, 0x2B, 0x30) : wxColour(0xF0, 0xF0, 0xF0);
    SetBackgroundColour(bg);
    for (ScalableButton *b : { m_btn_new, m_btn_open })
        if (b != nullptr)
            b->SetBackgroundColour(bg);
}

// ---- P2: badges, live thumbnail, Send ---------------------------------------------------------------------------------------

bool ProjectStrip::plater_tab_shown() const
{
    return m_frame != nullptr && m_frame->m_tabpanel != nullptr && m_frame->IsShown() && !m_frame->IsIconized() &&
           MainTabs::hosts_plater(m_frame->m_tabpanel->GetSelection());
}

void ProjectStrip::on_poll(wxTimerEvent &)
{
    Plater *p = plater();
    if (m_switching || p == nullptr || !IsShown())
        return;
    // P4: every project is live, the hidden ones' badges too (a hidden project's slice goes on).
    bool changed      = false;
    bool any_slicing  = false;
    for (const ProjectSlot &cs : m_sessions.slots()) {
        ProjectSlot *slot = m_sessions.find(cs.id);
        Plater      *pl   = plater_of(cs.id);
        if (slot == nullptr || pl == nullptr)
            continue;
        const bool shown = pl == p;
        int with_objects = 0, sliced = 0;
        pl->count_sliced_plates(with_objects, sliced);
        const bool slicing = shown ? pl->is_background_process_slicing() : pl->is_slicing_in_background();
        const bool dirty   = pl->is_project_dirty();
        changed = changed || slot->plates_with_objects != with_objects || slot->plates_sliced != sliced || slot->slicing != slicing ||
                  slot->dirty != dirty;
        slot->plates_with_objects = with_objects;
        slot->plates_sliced       = sliced;
        slot->slicing             = slicing;
        slot->dirty               = dirty;
        any_slicing               = any_slicing || slicing;
    }
    if (any_slicing) {
        m_spin  = (m_spin + 1) % 8;
        changed = true;
    }
    // The live thumbnail: rendered again once the project stops changing for a moment (an undo snapshot or another plate).
    const size_t marker = p->project_change_marker() * 64 + size_t(std::max(0, p->get_partplate_list().get_curr_plate_index()));
    if (marker != m_active_marker) {
        m_active_marker = marker;
        m_thumb_timer.StartOnce(1500);
    }
    if (changed)
        Refresh();
}

void ProjectStrip::on_thumbnail_timer(wxTimerEvent &)
{
    if (m_switching)
        return;
    // A tile with no picture yet gets one off screen too (a project opened from Home is not on screen).
    const ProjectSlot *active = m_sessions.active();
    auto               it     = active != nullptr ? m_extra.find(active->id) : m_extra.end();
    const bool         none   = it == m_extra.end() || !it->second.image.IsOk();
    if (render_active_thumbnail(none))
        Refresh();
}

void ProjectStrip::thumbnail_after_load()
{
    // Owner P4 re-test: a project opened from Recent showed no thumbnail until something changed. Once the load and what
    // it deferred ran, the tile is rendered, on screen or not.
    CallAfter([this]() {
        if (m_switching)
            return;
        const ProjectSlot *active = m_sessions.active();
        if (active == nullptr)
            return;
        const SlotExtra &e = m_extra[active->id];
        if (tile_thumbnail_stale(e.image.IsOk(), e.thumb_marker, active_thumbnail_marker()) && render_active_thumbnail(true))
            Refresh();
    });
}

size_t ProjectStrip::active_thumbnail_marker() const
{
    Plater *p = plater();
    return p == nullptr ? size_t(-1) : p->project_change_marker() * 64 + size_t(std::max(0, p->get_partplate_list().get_curr_plate_index()));
}

bool ProjectStrip::render_active_thumbnail(bool off_screen)
{
    Plater      *p      = plater();
    ProjectSlot *active = m_sessions.active();
    if (p == nullptr || active == nullptr || m_collapsed || m_text_mode || plater_of(active->id) != p)
        return false;
    if (!off_screen && !plater_tab_shown()) {
        // The canvas is not on screen: try again after the next change, or when the plater is back.
        m_active_marker = size_t(-1);
        return false;
    }
    ThumbnailData data;
    const unsigned size = unsigned(std::max(128, FromDIP(StripLayout::thumb_side(StripLayout::max_width))));
    if (!p->render_current_plate_thumbnail(data, size))
        return false;
    wxImage image(int(data.width), int(data.height));
    image.InitAlpha();
    for (unsigned r = 0; r < data.height; ++r) {
        const unsigned rr = (data.height - 1 - r) * data.width; // the GL image is bottom-up
        for (unsigned c = 0; c < data.width; ++c) {
            const unsigned char *px = data.pixels.data() + 4 * (rr + c);
            image.SetRGB(int(c), int(r), px[0], px[1], px[2]);
            image.SetAlpha(int(c), int(r), px[3]);
        }
    }
    SlotExtra &extra   = m_extra[active->id];
    extra.image        = image;
    extra.scaled       = wxBitmap();
    extra.scaled_side  = 0;
    extra.thumb_marker = active_thumbnail_marker();
    return true;
}

void ProjectStrip::send_project(int id)
{
    if (id != m_sessions.active_id() && !select_project(id))
        return;
    if (m_frame == nullptr)
        return;
    // After the switch the window's buttons follow the project shown; send once that ran.
    wxGetApp().CallAfter([this]() {
        if (m_frame != nullptr && !m_frame->send_current_plate())
            toast(_L("Slice the plate first, then send it."));
    });
}

// ---- Layout and drawing ---------------------------------------------------------------------------------------------------

int ProjectStrip::strip_width() const { return FromDIP(StripLayout::width(m_collapsed, m_text_mode, m_width_dip)); }
int ProjectStrip::thumb_side() const { return FromDIP(StripLayout::thumb_side(StripLayout::width(false, false, m_width_dip))); }
int ProjectStrip::grip_width() const { return m_collapsed ? 0 : FromDIP(5); }
// Expanded: one row, the chevron at the left, New and Open at the right. The rail: the three stacked.
int ProjectStrip::header_height() const { return FromDIP(m_collapsed ? 3 * 24 : 30); }

int ProjectStrip::item_height() const
{
    if (m_collapsed)
        return FromDIP(22);
    const int line = std::max(FromDIP(12), wxGetApp().small_font().GetPixelSize().GetHeight() + FromDIP(2));
    if (m_text_mode)
        return std::max(FromDIP(32), line + FromDIP(14));
    // Padding, the thumbnail, a gap, two name lines, padding.
    return FromDIP(6) + thumb_side() + FromDIP(4) + 2 * line + FromDIP(8);
}

void ProjectStrip::apply_width()
{
    const int w = strip_width();
    SetMinSize(wxSize(w, -1));
    SetMaxSize(wxSize(w, -1));
    SetSize(wxSize(w, GetSize().GetHeight()));
    layout_buttons();
    if (wxWindow *parent = GetParent())
        parent->Layout();
}

void ProjectStrip::layout_buttons()
{
    if (m_btn_new == nullptr || m_btn_open == nullptr)
        return;
    const int    w   = GetClientSize().x > 0 ? GetClientSize().x : strip_width();
    const wxSize bsz = m_btn_new->GetBestSize();
    const int    b   = std::max(bsz.x, bsz.y);
    if (m_collapsed) {
        const int row = FromDIP(24);
        m_btn_new->SetSize((w - b) / 2, row + (row - b) / 2, b, b);
        m_btn_open->SetSize((w - b) / 2, 2 * row + (row - b) / 2, b, b);
    } else {
        const int top = (header_height() - b) / 2;
        const int gap = FromDIP(4);
        m_btn_open->SetSize(w - grip_width() - b - gap, top, b, b);
        m_btn_new->SetSize(w - grip_width() - 2 * b - 2 * gap, top, b, b);
    }
}

void ProjectStrip::on_size(wxSizeEvent &evt)
{
    layout_buttons();
    Refresh();
    evt.Skip();
}

void ProjectStrip::set_collapsed(bool collapsed)
{
    m_collapsed = collapsed;
    if (AppConfig *cfg = wxGetApp().app_config)
        cfg->set(CFG_COLLAPSED, collapsed ? "1" : "0");
    m_scroll = 0;
    apply_width();
    Refresh();
}

void ProjectStrip::set_text_mode(bool text_mode)
{
    m_text_mode = text_mode;
    if (AppConfig *cfg = wxGetApp().app_config)
        cfg->set(CFG_TEXT_MODE, text_mode ? "1" : "0");
    m_scroll = 0;
    apply_width();
    Refresh();
}

ProjectStrip::Hit ProjectStrip::hit_test(const wxPoint &pt) const
{
    const wxSize size = GetClientSize();
    if (!m_collapsed && pt.x >= size.x - grip_width())
        return { HitKind::Grip, -1 };
    if (pt.y < header_height())
        return (m_collapsed ? pt.y < FromDIP(24) : pt.x < FromDIP(26)) ? Hit{ HitKind::Header, -1 } : Hit{};
    const int y = pt.y - header_height() + m_scroll;
    const int i = y / item_height();
    if (y < 0 || i < 0 || size_t(i) >= m_sessions.size())
        return {};
    const int id = m_sessions.slots()[size_t(i)].id;
    if (!m_collapsed) {
        // The close cross in the tile's top-right corner.
        const int top   = header_height() + i * item_height() - m_scroll;
        const int cross = FromDIP(18);
        if (pt.x >= size.x - grip_width() - cross - FromDIP(6) && pt.y < top + cross + FromDIP(6))
            return { HitKind::ItemClose, id };
    }
    return { HitKind::Item, id };
}

void ProjectStrip::on_paint(wxPaintEvent &)
{
    wxAutoBufferedPaintDC dc(this);
    const bool     dark     = wxGetApp().dark_mode();
    const wxSize   size     = GetClientSize();
    const wxColour bg       = dark ? wxColour(0x2B, 0x2B, 0x30) : wxColour(0xF0, 0xF0, 0xF0);
    const wxColour hover_bg = dark ? wxColour(0x38, 0x38, 0x40) : wxColour(0xE4, 0xF4, 0xEA);
    const wxColour text     = dark ? wxColour(0xE0, 0xE0, 0xE0) : wxColour(0x26, 0x2E, 0x30);
    const wxColour text_dim = dark ? wxColour(0xB8, 0xB8, 0xBE) : wxColour(0x50, 0x58, 0x5A);
    const wxColour muted    = dark ? wxColour(0x90, 0x90, 0x98) : wxColour(0x8A, 0x8A, 0x8A);
    const wxColour line     = dark ? wxColour(0x4A, 0x4A, 0x50) : wxColour(0xD8, 0xD8, 0xD8);
    const wxColour frame_bg = dark ? wxColour(0x36, 0x36, 0x3C) : wxColour(0xFF, 0xFF, 0xFF);
    const wxColour dirty_c  = wxColour(0xF5, 0x9E, 0x0B);
    dc.SetBackground(wxBrush(bg));
    dc.Clear();
    dc.SetFont(wxGetApp().small_font());

    // Header: the collapse chevron (New / Open are child buttons).
    dc.SetTextForeground(m_hover.kind == HitKind::Header ? accent_colour() : muted);
    const wxString chevron = m_collapsed ? wxString::FromUTF8("\xC2\xBB") : wxString::FromUTF8("\xC2\xAB");
    const wxSize   cs      = dc.GetTextExtent(chevron);
    if (m_collapsed)
        dc.DrawText(chevron, (size.x - cs.x) / 2, (FromDIP(24) - cs.y) / 2);
    else
        dc.DrawText(chevron, FromDIP(8), (header_height() - cs.y) / 2);
    dc.SetPen(wxPen(line));
    dc.DrawLine(0, header_height() - 1, size.x, header_height() - 1);
    // The right edge: the grip to widen the strip.
    const bool grip_hot = m_dragging || m_hover.kind == HitKind::Grip;
    dc.SetPen(wxPen(grip_hot ? accent_colour() : line, grip_hot ? FromDIP(2) : 1));
    dc.DrawLine(size.x - 1, 0, size.x - 1, size.y);

    const int list_top = header_height();
    dc.SetClippingRegion(0, list_top, std::max(0, size.x - 1), std::max(0, size.y - list_top));
    const int ih    = item_height();
    const int side  = thumb_side();
    const int inner = size.x - grip_width(); // what the tiles may use
    for (size_t i = 0; i < m_sessions.size(); ++i) {
        const ProjectSlot &slot   = m_sessions.slots()[i];
        const bool         active = slot.id == m_sessions.active_id();
        const bool         hover  = m_hover.id == slot.id && (m_hover.kind == HitKind::Item || m_hover.kind == HitKind::ItemClose);
        const int          top    = list_top + int(i) * ih - m_scroll;
        if (top + ih < list_top || top > size.y)
            continue;
        const SliceBadge badge = slice_badge(slot.slicing, slot.plates_with_objects, slot.plates_sliced);

        if (m_collapsed) {
            // The rail: one mark per project, filled for the active one; the unsaved dot next to it.
            const int cx = size.x / 2, cy = top + ih / 2;
            if (hover) {
                dc.SetBrush(wxBrush(hover_bg));
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.DrawRectangle(0, top, size.x - 1, ih);
            }
            dc.SetPen(badge == SliceBadge::Sliced ? wxPen(accent_colour(), FromDIP(2)) : *wxTRANSPARENT_PEN);
            dc.SetBrush(active ? wxBrush(accent_colour()) : wxBrush(muted));
            dc.DrawCircle(cx, cy, FromDIP(4));
            if (slot.dirty) {
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.SetBrush(wxBrush(dirty_c));
                dc.DrawCircle(cx + FromDIP(6), cy - FromDIP(5), FromDIP(2));
            }
            continue;
        }

        // The tile's frame: the thumbnail (thumbnail mode) or the row (text mode). The active project's highlight is this frame.
        wxRect frame;
        if (m_text_mode)
            frame = wxRect(FromDIP(4), top + FromDIP(3), inner - FromDIP(8), ih - FromDIP(6));
        else
            frame = wxRect((inner - side) / 2, top + FromDIP(6), side, side);
        if (hover) {
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(hover_bg));
            dc.DrawRoundedRectangle(wxRect(FromDIP(2), top + FromDIP(1), inner - FromDIP(4), ih - FromDIP(2)), FromDIP(4));
        }
        dc.SetBrush(wxBrush(frame_bg));
        dc.SetPen(active ? wxPen(accent_colour(), FromDIP(2)) : wxPen(line));
        dc.DrawRoundedRectangle(frame, FromDIP(4));

        if (!m_text_mode) {
            if (const wxBitmap *bmp = tile_bitmap(slot.id, side - FromDIP(6)); bmp != nullptr && bmp->IsOk()) {
                dc.DrawBitmap(*bmp, frame.x + (side - bmp->GetWidth()) / 2, frame.y + (side - bmp->GetHeight()) / 2, true);
            } else {
                // No thumbnail yet: a file shape with a folded corner (a project, not a plate tile).
                const int    fold = side / 5;
                const wxRect page(frame.x + side / 5, frame.y + side / 8, side - 2 * (side / 5), side - 2 * (side / 8));
                wxPoint      outline[] = { { page.x, page.y }, { page.GetRight() - fold, page.y }, { page.GetRight(), page.y + fold },
                                           { page.GetRight(), page.GetBottom() }, { page.x, page.GetBottom() } };
                dc.SetPen(wxPen(muted));
                dc.SetBrush(*wxTRANSPARENT_BRUSH);
                dc.DrawPolygon(5, outline);
                dc.DrawLine(page.GetRight() - fold, page.y, page.GetRight() - fold, page.y + fold);
                dc.DrawLine(page.GetRight() - fold, page.y + fold, page.GetRight(), page.y + fold);
            }
        }

        // Corners: the unsaved dot top-left, the slice badge bottom-right, the close cross top-right while hovered.
        const int r = FromDIP(5);
        if (slot.dirty) {
            dc.SetPen(wxPen(frame_bg, FromDIP(1)));
            dc.SetBrush(wxBrush(dirty_c));
            const wxPoint c = m_text_mode ? wxPoint(frame.x + FromDIP(10), frame.y + frame.height / 2) : wxPoint(frame.x + r + FromDIP(3), frame.y + r + FromDIP(3));
            dc.DrawCircle(c, FromDIP(4));
        }
        if (badge != SliceBadge::None && !(hover && m_text_mode)) {
            const wxPoint c = m_text_mode ? wxPoint(frame.GetRight() - FromDIP(12), frame.y + frame.height / 2)
                                          : wxPoint(frame.GetRight() - r - FromDIP(3), frame.GetBottom() - r - FromDIP(3));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(frame_bg));
            dc.DrawCircle(c, r + FromDIP(2));
            if (badge == SliceBadge::Sliced) {
                dc.SetBrush(wxBrush(accent_colour()));
                dc.DrawCircle(c, r);
                dc.SetPen(wxPen(*wxWHITE, FromDIP(2)));
                dc.DrawLine(c.x - r / 2, c.y, c.x - r / 6, c.y + r / 2);
                dc.DrawLine(c.x - r / 6, c.y + r / 2, c.x + r / 2, c.y - r / 3);
            } else {
                // Slicing: eight dots going round.
                for (int k = 0; k < 8; ++k) {
                    const double a = k * 3.14159265358979 / 4.;
                    dc.SetBrush(wxBrush(k == m_spin ? accent_colour() : muted));
                    dc.DrawCircle(c.x + int(std::lround(r * std::cos(a))), c.y + int(std::lround(r * std::sin(a))), std::max(1, FromDIP(1)));
                }
            }
        }
        if (hover) {
            const int     cr = FromDIP(7);
            const wxPoint c  = m_text_mode ? wxPoint(frame.GetRight() - FromDIP(12), frame.y + frame.height / 2)
                                           : wxPoint(frame.GetRight() - cr - FromDIP(2), frame.y + cr + FromDIP(2));
            const bool hot = m_hover.kind == HitKind::ItemClose;
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(hot ? wxColour(0xE0, 0x4B, 0x4B) : (dark ? wxColour(0x55, 0x55, 0x5C) : wxColour(0xC8, 0xC8, 0xC8))));
            dc.DrawCircle(c, cr);
            dc.SetPen(wxPen(*wxWHITE, FromDIP(2)));
            const int d = cr / 2;
            dc.DrawLine(c.x - d, c.y - d, c.x + d, c.y + d);
            dc.DrawLine(c.x - d, c.y + d, c.x + d, c.y - d);
        }

        // The name: under the thumbnail on at most two lines, broken at word ends and ellipsised (the full name is the tooltip).
        dc.SetTextForeground(active ? text : text_dim);
        const int lh = dc.GetCharHeight();
        if (m_text_mode) {
            const int x     = frame.x + (slot.dirty ? FromDIP(20) : FromDIP(8));
            const int avail = frame.GetRight() - FromDIP(24) - x;
            dc.DrawText(wxControl::Ellipsize(from_u8(slot.name), dc, wxELLIPSIZE_END, std::max(10, avail)), x, frame.y + (frame.height - lh) / 2);
        } else {
            const int avail = inner - FromDIP(2 * StripLayout::padding);
            const auto measure = [&dc](const std::string &s) { return dc.GetTextExtent(from_u8(s)).x; };
            const auto ellipsize = [&dc](const std::string &s, int w) {
                return into_u8(wxControl::Ellipsize(from_u8(s), dc, wxELLIPSIZE_END, w));
            };
            const std::vector<std::string> lines = wrap_name_two_lines(slot.name, avail, measure, ellipsize);
            int y = frame.GetBottom() + FromDIP(4);
            for (const std::string &l : lines) {
                const wxString shown = from_u8(l);
                const int      tw    = dc.GetTextExtent(shown).x;
                dc.DrawText(shown, (inner - tw) / 2, y);
                y += lh;
            }
        }
    }
    // Drag reorder: where the tile goes.
    if (m_reordering && m_drop_index >= 0) {
        const int y = list_top + m_drop_index * ih - m_scroll;
        dc.SetPen(wxPen(accent_colour(), FromDIP(3)));
        dc.DrawLine(FromDIP(4), y, inner - FromDIP(4), y);
    }
    dc.DestroyClippingRegion();
}

void ProjectStrip::on_left_down(wxMouseEvent &evt)
{
    const Hit hit = hit_test(evt.GetPosition());
    switch (hit.kind) {
    case HitKind::Grip:
        m_dragging       = true;
        m_drag_start_x   = ClientToScreen(evt.GetPosition()).x;
        m_drag_start_dip = StripLayout::width(false, m_text_mode, m_width_dip);
        CaptureMouse();
        break;
    case HitKind::Header: set_collapsed(!m_collapsed); break;
    case HitKind::Item:
        // Selected on release; dragged, it moves (P3 reorder).
        m_press_id   = hit.id;
        m_press_y    = evt.GetPosition().y;
        m_reordering = false;
        m_drop_index = -1;
        CaptureMouse();
        break;
    case HitKind::ItemClose: close_project(hit.id); break;
    default: break;
    }
}

void ProjectStrip::end_drag()
{
    if (!m_dragging)
        return;
    m_dragging = false;
    if (HasCapture())
        ReleaseMouse();
    if (AppConfig *cfg = wxGetApp().app_config)
        cfg->set(CFG_WIDTH, std::to_string(m_width_dip));
    Refresh();
}

void ProjectStrip::on_left_up(wxMouseEvent &evt)
{
    end_drag();
    if (m_press_id >= 0) {
        const int id = m_press_id;
        m_press_id   = -1;
        if (HasCapture())
            ReleaseMouse();
        if (m_reordering) {
            const int from = m_sessions.index_of(id);
            int       to   = m_drop_index;
            if (from >= 0 && to >= 0) {
                if (to > from)
                    --to;
                to = std::max(0, std::min(int(m_sessions.size()) - 1, to));
                m_sessions.move(id, to);
            }
            m_reordering = false;
            m_drop_index = -1;
            Refresh();
        } else
            select_project(id);
    }
    evt.Skip();
}

// Where a dragged tile would go: the gap nearest to y, 0..count.
int ProjectStrip::drop_index_at(int y) const
{
    const int ih = item_height();
    const int i  = (y - header_height() + m_scroll + ih / 2) / std::max(1, ih);
    return std::max(0, std::min(int(m_sessions.size()), i));
}

void ProjectStrip::on_capture_lost(wxMouseCaptureLostEvent &)
{
    m_press_id   = -1;
    m_reordering = false;
    m_drop_index = -1;
    m_dragging = false;
    if (AppConfig *cfg = wxGetApp().app_config)
        cfg->set(CFG_WIDTH, std::to_string(m_width_dip));
}

void ProjectStrip::on_middle_down(wxMouseEvent &evt)
{
    const Hit hit = hit_test(evt.GetPosition());
    if (hit.kind == HitKind::Item || hit.kind == HitKind::ItemClose)
        close_project(hit.id);
}

void ProjectStrip::on_right_down(wxMouseEvent &evt)
{
    const Hit hit = hit_test(evt.GetPosition());
    if (hit.kind == HitKind::Item || hit.kind == HitKind::ItemClose)
        show_item_menu(hit.id);
    else
        show_strip_menu();
}

void ProjectStrip::on_motion(wxMouseEvent &evt)
{
    if (m_dragging) {
        // DIP from the drag distance in pixels, clamped to 88-200.
        const double px_per_dip = FromDIP(100) / 100.;
        const int    dx         = ClientToScreen(evt.GetPosition()).x - m_drag_start_x;
        const int    wanted     = StripLayout::clamp_width(m_drag_start_dip + int(std::lround(dx / px_per_dip)));
        if (wanted != m_width_dip) {
            m_width_dip = wanted;
            apply_width();
            Refresh();
        }
        return;
    }
    if (m_press_id >= 0 && evt.LeftIsDown()) {
        if (!m_reordering && std::abs(evt.GetPosition().y - m_press_y) > FromDIP(6) && m_sessions.size() > 1)
            m_reordering = true;
        if (m_reordering) {
            const int drop = drop_index_at(evt.GetPosition().y);
            if (drop != m_drop_index) {
                m_drop_index = drop;
                Refresh();
            }
            SetCursor(wxCursor(wxCURSOR_HAND));
            return;
        }
    }
    const Hit hit = hit_test(evt.GetPosition());
    SetCursor(hit.kind == HitKind::Grip ? wxCursor(wxCURSOR_SIZEWE) : wxNullCursor);
    if (hit.kind != m_hover.kind || hit.id != m_hover.id) {
        m_hover = hit;
        wxString tip;
        if (hit.kind == HitKind::Item) {
            if (const ProjectSlot *slot = m_sessions.find(hit.id))
                tip = tooltip_for(*slot);
        } else if (hit.kind == HitKind::ItemClose)
            tip = _L("Close project");
        else if (hit.kind == HitKind::Header)
            tip = m_collapsed ? _L("Show the project strip") : _L("Collapse the project strip");
        else if (hit.kind == HitKind::Grip)
            tip = _L("Drag to widen the project strip");
        SetToolTip(tip);
        Refresh();
    }
    evt.Skip();
}

void ProjectStrip::on_leave(wxMouseEvent &evt)
{
    if (!m_dragging) {
        m_hover = {};
        SetCursor(wxNullCursor);
        Refresh();
    }
    evt.Skip();
}

void ProjectStrip::on_wheel(wxMouseEvent &evt)
{
    const int content = int(m_sessions.size()) * item_height();
    const int view    = GetClientSize().y - header_height();
    const int max     = std::max(0, content - view);
    m_scroll          = std::max(0, std::min(max, m_scroll - evt.GetWheelRotation() / std::max(1, evt.GetWheelDelta()) * item_height() / 2));
    Refresh();
}

void ProjectStrip::show_item_menu(int id)
{
    const ProjectSlot *slot = m_sessions.find(id);
    if (slot == nullptr)
        return;
    wxMenu    menu;
    const int id_save = wxWindow::NewControlId(), id_save_as = wxWindow::NewControlId(), id_send = wxWindow::NewControlId(),
              id_close = wxWindow::NewControlId(), id_close_others = wxWindow::NewControlId(), id_reveal = wxWindow::NewControlId(),
              id_window = wxWindow::NewControlId();
    menu.Append(id_save, _L("Save"));
    menu.Append(id_save_as, _L("Save as..."));
    menu.AppendSeparator();
    // P2: switch to it, then the print / send of its plate (as Ctrl+Shift+G).
    menu.Append(id_send, _L("Send..."));
    menu.Enable(id_send, slot->plates_sliced > 0 || id == m_sessions.active_id());
    menu.AppendSeparator();
    menu.Append(id_close, _L("Close"));
    menu.Append(id_close_others, _L("Close others"));
    menu.Enable(id_close_others, m_sessions.size() > 1);
    menu.AppendSeparator();
#ifdef __APPLE__
    menu.Append(id_reveal, _L("Reveal in Finder"));
#else
    menu.Append(id_reveal, _L("Show in Explorer"));
#endif
    menu.Enable(id_reveal, !slot->origin_path.empty());
    menu.Append(id_window, _L("Open in new window"));
    const int chosen = GetPopupMenuSelectionFromUser(menu);
    if (chosen == id_save)
        save_slot(id, false);
    else if (chosen == id_save_as)
        save_slot(id, true);
    else if (chosen == id_send)
        send_project(id);
    else if (chosen == id_close)
        close_project(id);
    else if (chosen == id_close_others)
        close_others(id);
    else if (chosen == id_reveal) {
        if (const ProjectSlot *s = m_sessions.find(id); s != nullptr && !s->origin_path.empty())
            desktop_open_any_folder(s->origin_path);
    } else if (chosen == id_window)
        open_in_new_window(id);
}

void ProjectStrip::show_strip_menu()
{
    wxMenu    menu;
    const int id_thumbs = wxWindow::NewControlId(), id_names = wxWindow::NewControlId(), id_collapse = wxWindow::NewControlId(),
              id_reset = wxWindow::NewControlId();
    menu.AppendRadioItem(id_thumbs, _L("Show thumbnails"));
    menu.AppendRadioItem(id_names, _L("Show names only"));
    menu.Check(m_text_mode ? id_names : id_thumbs, true);
    menu.AppendSeparator();
    menu.Append(id_reset, _L("Default width"));
    menu.Enable(id_reset, !m_collapsed && m_width_dip != StripLayout::default_width);
    menu.Append(id_collapse, m_collapsed ? _L("Show the project strip") : _L("Collapse the project strip"));
    const int chosen = GetPopupMenuSelectionFromUser(menu);
    if (chosen == id_thumbs)
        set_text_mode(false);
    else if (chosen == id_names)
        set_text_mode(true);
    else if (chosen == id_collapse)
        set_collapsed(!m_collapsed);
    else if (chosen == id_reset) {
        m_width_dip = StripLayout::default_width;
        if (AppConfig *cfg = wxGetApp().app_config)
            cfg->set(CFG_WIDTH, std::to_string(m_width_dip));
        apply_width();
        Refresh();
    }
}

// ---- P3: Explorer / Finder, session restore, the macOS Window menu -----------------------------------------------------------

bool ProjectStrip::open_files_from_shell(const wxArrayString &files)
{
    if (files.empty() || wxGetApp().app_config == nullptr || !shell_open_into_strip(wxGetApp().app_config->get(CFG_SHELL)))
        return false;
    for (const wxString &f : files)
        if (!f.Lower().EndsWith(".3mf"))
            return false;
    for (const wxString &f : files)
        open_project(f);
    return true;
}

std::vector<std::pair<int, wxString>> ProjectStrip::menu_entries() const
{
    std::vector<std::pair<int, wxString>> out;
    for (const ProjectSlot &s : m_sessions.slots())
        out.emplace_back(s.id, from_u8(s.name) + (s.dirty ? wxString(" *") : wxString()));
    return out;
}

std::string ProjectStrip::session_manifest_path() const
{
    return (fs::path(m_root).parent_path() / "session.json").string();
}

void ProjectStrip::save_session_for_restart()
{
    if (wxGetApp().app_config == nullptr || !wxGetApp().app_config->get_bool(CFG_SESSION) || m_sessions.size() < 2)
        return;
    // Each project is written shown: the 3MF takes its settings from the app-global presets, which are the shown
    // project's. The one shown at exit is written last, so it is the one selected at the next start.
    const int        active = m_sessions.active_id();
    std::vector<int> order;
    for (const ProjectSlot &s : m_sessions.slots())
        if (s.id != active)
            order.push_back(s.id);
    order.push_back(active);
    for (int id : order) {
        if (id != m_sessions.active_id() && !switch_to(id, "session save"))
            continue;
        ProjectSlot *slot = m_sessions.find(id);
        Plater      *pl   = plater_of(id);
        if (slot == nullptr || pl == nullptr)
            continue;
        sync_active();
        slot->plate                 = pl->get_partplate_list().get_curr_plate_index();
        const std::string snapshot = (fs::path(slot_dir(id)) / "project.3mf").string();
        std::string       error;
        if (!pl->save_project_snapshot(snapshot, error)) {
            BOOST_LOG_TRIVIAL(warning) << "project strip: \"" << slot->name << "\" is not in the session: " << error;
            continue;
        }
        slot->snapshot_path = snapshot;
        SlotExtra &extra    = m_extra[id];
        extra.dir           = slot_dir(id);
        extra.files         = { snapshot, snapshot + ".tmp" };
    }
    SessionManifest m;
    for (const ProjectSlot &s : m_sessions.slots()) {
        if (s.snapshot_path.empty())
            continue;
        if (s.id == active)
            m.active = int(m.slots.size());
        m.slots.push_back(s);
    }
    if (m.slots.empty())
        return;
    try {
        save_string_file(fs::path(session_manifest_path()), m.to_json());
        m_keep_files_for_session = true;
        BOOST_LOG_TRIVIAL(info) << "project strip: session of " << m.slots.size() << " projects saved for the next start";
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(warning) << "project strip: session not saved: " << e.what();
    }
}

void ProjectStrip::restore_session()
{
    const std::string path = session_manifest_path();
    boost::system::error_code ec;
    if (!fs::exists(fs::path(path), ec) || plater() == nullptr || m_frame == nullptr)
        return;
    std::string text;
    try {
        load_string_file(fs::path(path), text);
    } catch (...) {
    }
    remove_literal(path);
    SessionManifest m;
    if (!SessionManifest::from_json(text, m))
        return;
    const SessionManifest r = m.restorable([](const std::string &f) {
        boost::system::error_code e;
        return fs::exists(fs::path(f), e);
    });
    if (r.slots.empty())
        return;
    // A project opened meanwhile (Explorer, the hub) stays in front; the start's empty project gives way.
    const bool blank      = active_is_blank();
    const int  blank_id   = m_sessions.active_id();
    const int  front_id   = m_sessions.active_id();
    int        target_id  = -1;
    const auto started    = std::chrono::steady_clock::now();
    {
        SwitchScope scope(*this, true);
        for (size_t i = 0; i < r.slots.size(); ++i) {
            refresh_max_open();
            if (!m_sessions.can_add())
                break;
            const ProjectSlot &saved = r.slots[i];
            stash_active();
            Plater *fresh = m_frame->create_project_plater();
            if (fresh == nullptr)
                break;
            const int id       = m_sessions.add_restored(saved);
            SlotExtra &extra   = m_extra[id];
            extra.plater       = fresh;
            extra.dir          = fs::path(saved.snapshot_path).parent_path().string();
            extra.files        = { saved.snapshot_path, saved.snapshot_path + ".tmp" };
            m_frame->activate_plater(fresh);
            m_sessions.select(id);
            // Its own presets come with its 3MF (as an opened project); the swap guard keeps the printer memory out.
            const bool ok = fresh->load_project_snapshot(saved.snapshot_path, from_u8(saved.origin_path), from_u8(saved.name), saved.dirty,
                                                         saved.plate);
            load_thumbnail(id, saved.snapshot_path);
            // Back from its snapshot: the files go now (the next exit writes new ones if asked to).
            remove_slot_files(id);
            extra.files.clear();
            extra.dir.clear();
            if (ProjectSlot *slot = m_sessions.find(id)) {
                slot->snapshot_path.clear();
                slot->main_tab = int(MainTabs::Editor3D);
            }
            if (!ok)
                BOOST_LOG_TRIVIAL(warning) << "project strip: \"" << saved.name << "\" could not be restored";
            if (int(i) == r.active)
                target_id = id;
        }
        // The project to show: the one that was shown, unless another one was opened meanwhile.
        const int show = blank ? target_id : front_id;
        if (show >= 0 && show != m_sessions.active_id()) {
            stash_active();
            show_slot(show);
            m_sessions.select(show);
        }
        if (blank && blank_id != m_sessions.active_id())
            if (Plater *pl = plater_of(blank_id)) {
                m_frame->destroy_project_plater(pl);
                m_sessions.close(blank_id);
                m_extra.erase(blank_id);
            }
    }
    BOOST_LOG_TRIVIAL(info) << "project strip: " << r.slots.size() << " projects of the previous session restored";
    after_switch(m_sessions.active_id(), started, 0, "session");
    Refresh();
}

} // namespace GUI
} // namespace Slic3r
