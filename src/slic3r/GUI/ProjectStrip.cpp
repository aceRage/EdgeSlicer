#include "ProjectStrip.hpp"

#include <algorithm>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <wx/checklst.h>
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
#include "../Utils/Process.hpp"

namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {

namespace {

const char *CFG_COLLAPSED = "project_strip_collapsed";
const char *CFG_TEXT_MODE = "project_strip_text_mode";
const char *CFG_MAX_OPEN  = "project_strip_max_open";

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

} // namespace

// ---- SwitchScope ----------------------------------------------------------------------------------------------------------

ProjectStrip::SwitchScope::SwitchScope(ProjectStrip &strip, bool swap_guard) : m_strip(strip), m_swap_guard(swap_guard)
{
    m_strip.m_switching = true;
    if (Plater *p = m_strip.plater())
        p->set_project_switch_busy(true);
    if (m_swap_guard)
        ProjectSwapGuard::hold();
}

ProjectStrip::SwitchScope::~SwitchScope()
{
    if (Plater *p = m_strip.plater())
        p->set_project_switch_busy(false);
    m_strip.m_switching = false;
    // The load queued work for later (the printer tab's nozzle refresh, the height-pattern check): wx runs CallAfter in
    // order, so this release comes after it and the rack memory / variant follow stay out of it too.
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
        m_sessions.set_max_open(ProjectSessions::parse_max_open(cfg->get(CFG_MAX_OPEN)));
    }
    // Per process and per slot, under the app's temp folder (the backup folders live there too).
    m_root = (fs::path(temporary_dir()) / "project_strip" / std::to_string(get_current_pid())).string();

    // The project the window starts with.
    const int first = m_sessions.add("", into_u8(_L("Untitled")));
    m_sessions.select(first);
    m_extra[first];

    Bind(wxEVT_PAINT, &ProjectStrip::on_paint, this);
    Bind(wxEVT_LEFT_DOWN, &ProjectStrip::on_left_down, this);
    Bind(wxEVT_MIDDLE_DOWN, &ProjectStrip::on_middle_down, this);
    Bind(wxEVT_RIGHT_DOWN, &ProjectStrip::on_right_down, this);
    Bind(wxEVT_MOTION, &ProjectStrip::on_motion, this);
    Bind(wxEVT_LEAVE_WINDOW, &ProjectStrip::on_leave, this);
    Bind(wxEVT_MOUSEWHEEL, &ProjectStrip::on_wheel, this);
    apply_width();
}

Plater *ProjectStrip::plater() const { return wxGetApp().plater(); }

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
    if (p == nullptr || slot == nullptr)
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

bool ProjectStrip::can_switch(bool cancel_slicing, bool &slicing_cancelled)
{
    slicing_cancelled = false;
    Plater *p = plater();
    if (p == nullptr || m_switching)
        return false;
    const wxString why = p->project_swap_blocker();
    if (!why.empty()) {
        toast(_L("The project cannot be switched now.") + " " + why);
        return false;
    }
    if (cancel_slicing)
        slicing_cancelled = p->cancel_slicing_for_project_swap();
    return true;
}

void ProjectStrip::load_thumbnail(ProjectSlot &slot)
{
    SlotExtra &extra = m_extra[slot.id];
    extra.thumbnail  = wxBitmap();
    if (slot.snapshot_path.empty())
        return;
    const std::string png = bbs_3mf_get_thumbnail(slot.snapshot_path.c_str());
    if (png.empty())
        return;
    wxMemoryInputStream stream(png.data(), png.size());
    wxImage             image;
    if (!image.LoadFile(stream, wxBITMAP_TYPE_PNG) || !image.IsOk())
        return;
    const int side = FromDIP(72);
    const double scale = std::min(double(side) / image.GetWidth(), double(side) / image.GetHeight());
    image.Rescale(std::max(1, int(image.GetWidth() * scale)), std::max(1, int(image.GetHeight() * scale)), wxIMAGE_QUALITY_HIGH);
    extra.thumbnail = wxBitmap(image);
}

bool ProjectStrip::park_active(std::string &error)
{
    Plater      *p    = plater();
    ProjectSlot *slot = m_sessions.active();
    if (p == nullptr || slot == nullptr) {
        error = "no active project";
        return false;
    }
    sync_active();
    SlotExtra &extra = m_extra[slot->id];
    // The G-code parked last time may belong to plates that are no longer sliced: start from none.
    for (const auto &g : slot->plate_gcodes)
        remove_literal(g.second);
    slot->plate_gcodes.clear();

    const std::string dir      = slot_dir(slot->id);
    const std::string snapshot = (fs::path(dir) / "project.3mf").string();
    boost::system::error_code ec;
    fs::create_directories(fs::path(dir), ec);
    std::vector<std::pair<int, std::string>> gcodes;
    if (!p->park_project(snapshot, dir, gcodes, error))
        return false;
    slot->snapshot_path = snapshot;
    slot->plate_gcodes  = gcodes;
    slot->plate         = p->get_partplate_list().get_curr_plate_index();
    const int tab       = m_frame != nullptr && m_frame->m_tabpanel != nullptr ? m_frame->m_tabpanel->GetSelection() : int(MainTabs::Editor3D);
    slot->main_tab      = MainTabs::hosts_plater(tab) ? tab : (MainTabs::hosts_plater(slot->main_tab) ? slot->main_tab : int(MainTabs::Editor3D));
    extra.camera        = p->get_camera();
    extra.files         = { snapshot, snapshot + ".tmp" };
    for (const auto &g : gcodes)
        extra.files.push_back(g.second);
    load_thumbnail(*slot);
    return true;
}

bool ProjectStrip::restore(ProjectSlot &target)
{
    Plater *p = plater();
    if (p == nullptr)
        return false;
    if (!p->swap_in_project(target.snapshot_path, from_u8(target.origin_path), from_u8(target.name), target.dirty, target.plate_gcodes,
                            target.plate))
        return false;
    // Where it was looked at from.
    auto it = m_extra.find(target.id);
    if (it != m_extra.end() && it->second.camera.has_value()) {
        Camera &camera = p->get_camera();
        camera                         = *it->second.camera;
        camera.requires_zoom_to_bed     = false;
        camera.requires_zoom_to_volumes = false;
        camera.requires_zoom_to_plate   = REQUIRES_ZOOM_TO_PLATE_IDLE;
    }
    return true;
}

void ProjectStrip::after_switch(int id, bool slicing_cancelled, std::chrono::steady_clock::time_point started, long long park_ms)
{
    ProjectSlot *slot = m_sessions.find(id);
    if (slot != nullptr && m_frame != nullptr) {
        // The tab it was on; from Home or Device, the project's last plater tab.
        m_frame->select_tab(size_t(MainTabs::hosts_plater(slot->main_tab) ? slot->main_tab : int(MainTabs::Editor3D)));
    }
    if (Plater *p = plater())
        if (GLCanvas3D *canvas = p->get_current_canvas3D())
            canvas->set_as_dirty();
    const long long total = ms_since(started);
    BOOST_LOG_TRIVIAL(info) << "project strip: switch to slot " << id << " took " << total << " ms (park " << park_ms << " ms, load "
                            << (total - park_ms) << " ms)" << log_memory_info();
    if (slicing_cancelled)
        toast(_L("Slicing was cancelled to switch projects. Slice again when you come back to it."));
}

bool ProjectStrip::select_project(int id)
{
    ProjectSlot *target = m_sessions.find(id);
    if (target == nullptr)
        return false;
    if (id == m_sessions.active_id())
        return true;
    bool slicing_cancelled = false;
    if (!can_switch(true, slicing_cancelled))
        return false;
    const auto  started   = std::chrono::steady_clock::now();
    const int   previous  = m_sessions.active_id();
    long long   park_ms   = 0;
    {
        SwitchScope scope(*this, true);
        BOOST_LOG_TRIVIAL(info) << "project strip: switching from slot " << previous << " to slot " << id << log_memory_info();
        std::string error;
        if (!park_active(error)) {
            MessageDialog(m_frame, _L("The current project could not be set aside, so the switch was cancelled.") + "\n\n" + from_u8(error),
                          _L("Switch project"), wxOK | wxICON_WARNING)
                .ShowModal();
            return false;
        }
        park_ms = ms_since(started);
        if (!restore(*target)) {
            // The project the user left is safe in its snapshot: put it back.
            ProjectSlot *back = m_sessions.find(previous);
            if (back != nullptr)
                restore(*back);
            MessageDialog(m_frame, wxString::Format(_L("\"%s\" could not be opened again, so the switch was cancelled."), from_u8(target->name)),
                          _L("Switch project"), wxOK | wxICON_WARNING)
                .ShowModal();
            return false;
        }
        m_sessions.select(id);
    }
    after_switch(id, slicing_cancelled, started, park_ms);
    return true;
}

void ProjectStrip::cycle(int step)
{
    const int id = m_sessions.neighbour_id(step);
    if (id >= 0)
        select_project(id);
}

void ProjectStrip::new_project()
{
    Plater *p = plater();
    if (p == nullptr)
        return;
    // An empty Untitled project is simply started again, as before the strip.
    if (active_is_blank() || m_sessions.size() == 0) {
        p->new_project();
        return;
    }
    if (!m_sessions.can_add()) {
        toast(wxString::Format(_L("%d projects are open, the most this window keeps. Close one first."), m_sessions.max_open()));
        return;
    }
    bool slicing_cancelled = false;
    if (!can_switch(true, slicing_cancelled))
        return;
    const auto started = std::chrono::steady_clock::now();
    long long  park_ms = 0;
    int        id      = -1;
    {
        // A brand-new project starts as File > New does: no swap guard (its nozzles come from the printer's memory).
        SwitchScope scope(*this, false);
        std::string error;
        if (!park_active(error)) {
            MessageDialog(m_frame, _L("The current project could not be set aside.") + "\n\n" + from_u8(error), _L("New project"),
                          wxOK | wxICON_WARNING)
                .ShowModal();
            return;
        }
        park_ms                 = ms_since(started);
        const std::string name  = m_sessions.next_untitled_name(into_u8(_L("Untitled")));
        id                      = m_sessions.add("", name);
        m_extra[id];
        p->new_project(true, false, from_u8(name));
        m_sessions.select(id);
    }
    if (ProjectSlot *slot = m_sessions.find(id))
        slot->main_tab = int(MainTabs::Editor3D);
    after_switch(id, slicing_cancelled, started, park_ms);
}

void ProjectStrip::open_project(const wxString &file_in)
{
    Plater *p = plater();
    if (p == nullptr)
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
        Refresh();
        return;
    }
    if (!m_sessions.can_add()) {
        toast(wxString::Format(_L("%d projects are open, the most this window keeps. Close one first."), m_sessions.max_open()));
        return;
    }
    bool slicing_cancelled = false;
    if (!can_switch(true, slicing_cancelled))
        return;
    const auto started = std::chrono::steady_clock::now();
    long long  park_ms = 0;
    int        id      = -1;
    {
        // Opening a file is opening a file: its 3MF brings its printer and nozzles as always (no swap guard).
        SwitchScope scope(*this, false);
        std::string error;
        if (!park_active(error)) {
            MessageDialog(m_frame, _L("The current project could not be set aside.") + "\n\n" + from_u8(error), _L("Open project"),
                          wxOK | wxICON_WARNING)
                .ShowModal();
            return;
        }
        park_ms = ms_since(started);
        id      = m_sessions.add(into_u8(file), ProjectSessions::name_from_path(into_u8(file)));
        m_extra[id];
        m_sessions.select(id);
        p->load_project_unprompted(file);
    }
    if (ProjectSlot *slot = m_sessions.find(id))
        slot->main_tab = int(MainTabs::Editor3D);
    after_switch(id, slicing_cancelled, started, park_ms);
}

bool ProjectStrip::save_slot(int id, bool save_as)
{
    ProjectSlot *slot = m_sessions.find(id);
    Plater      *p    = plater();
    if (slot == nullptr || p == nullptr)
        return false;
    if (id == m_sessions.active_id()) {
        const bool ok = p->save_project(save_as) == wxID_YES;
        sync_active();
        Refresh();
        return ok;
    }
    // A parked project is saved by copying its snapshot: it was written as File > Save writes a project.
    std::string target = save_as ? std::string() : slot->origin_path;
    if (target.empty()) {
        wxFileDialog dlg(m_frame, _L("Save project as"), from_u8(fs::path(slot->origin_path).parent_path().string()),
                         from_u8(slot->name) + ".3mf", file_wildcards(FT_3MF), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() != wxID_OK)
            return false;
        target = into_u8(dlg.GetPath());
    }
    if (slot->snapshot_path.empty())
        return false;
    const std::string tmp = target + ".tmp";
    boost::system::error_code ec;
    fs::copy_file(fs::path(slot->snapshot_path), fs::path(tmp), fs::copy_options::overwrite_existing, ec);
    if (!ec)
        fs::rename(fs::path(tmp), fs::path(target), ec);
    if (ec) {
        remove_literal(tmp);
        MessageDialog(m_frame, _L("Failed to save the project.") + "\n\n" + from_u8(ec.message()), _L("Save project"), wxOK | wxICON_WARNING)
            .ShowModal();
        return false;
    }
    slot->origin_path = target;
    slot->name        = ProjectSessions::name_from_path(target);
    slot->dirty       = false;
    if (m_frame != nullptr)
        m_frame->add_to_recent_projects(from_u8(target));
    Refresh();
    return true;
}

void ProjectStrip::remove_slot_files(int id)
{
    auto it = m_extra.find(id);
    if (it != m_extra.end()) {
        for (const std::string &f : it->second.files)
            remove_literal(f);
        it->second.files.clear();
    }
    if (const ProjectSlot *slot = m_sessions.find(id))
        for (const auto &g : slot->plate_gcodes)
            remove_literal(g.second);
    remove_empty_dir(slot_dir(id));
}

bool ProjectStrip::close_project(int id)
{
    ProjectSlot *slot = m_sessions.find(id);
    Plater      *p    = plater();
    if (slot == nullptr || p == nullptr || m_switching)
        return false;
    const bool is_active = id == m_sessions.active_id();
    if (is_active)
        sync_active();
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
    if (!is_active) {
        remove_slot_files(id);
        m_sessions.close(id);
        m_extra.erase(id);
        Refresh();
        return true;
    }

    bool slicing_cancelled = false;
    if (!can_switch(true, slicing_cancelled))
        return false;
    const int successor = m_sessions.successor_on_close(id);
    if (successor < 0) {
        // The last project: an empty one takes its place, the strip always has one.
        p->new_project(true);
        remove_slot_files(id);
        m_sessions.close(id);
        m_extra.erase(id);
        const int fresh = m_sessions.add("", into_u8(_L("Untitled")));
        m_extra[fresh];
        m_sessions.select(fresh);
        sync_active();
        Refresh();
        return true;
    }
    const auto started = std::chrono::steady_clock::now();
    {
        SwitchScope  scope(*this, true);
        ProjectSlot *target = m_sessions.find(successor);
        if (target == nullptr || !restore(*target)) {
            MessageDialog(m_frame, _L("The next project could not be opened, so this one stays open."), _L("Close project"),
                          wxOK | wxICON_WARNING)
                .ShowModal();
            return false;
        }
        remove_slot_files(id);
        m_sessions.close(id);
        m_extra.erase(id);
        m_sessions.select(successor);
    }
    after_switch(successor, slicing_cancelled, started, 0);
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
    Plater                *p     = plater();
    if (dirty.empty() || p == nullptr)
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
        for (unsigned i = 0; i < list->GetCount(); ++i)
            if (list->IsChecked(i) && !save_slot(dirty[i], false))
                return false;
    }
    // What close_with_confirm() does once answered: the active project's crash backup goes.
    p->model().set_backup_path("");
    p->up_to_date(true, false);
    p->up_to_date(true, true);
    return true;
}

void ProjectStrip::remove_all_slot_files()
{
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
    tip += "\n\n" + _L("Undo history is not kept when switching projects.");
    return tip;
}

void ProjectStrip::rescale()
{
    for (auto &kv : m_extra)
        if (ProjectSlot *slot = m_sessions.find(kv.first))
            load_thumbnail(*slot);
    apply_width();
    Refresh();
}

// ---- Layout and drawing ---------------------------------------------------------------------------------------------------

int ProjectStrip::strip_width() const { return FromDIP(m_collapsed ? 24 : (m_text_mode ? 160 : 88)); }
int ProjectStrip::header_height() const { return FromDIP(24); }
int ProjectStrip::item_height() const { return FromDIP(m_collapsed ? 22 : (m_text_mode ? 32 : 112)); }
int ProjectStrip::button_height() const { return FromDIP(m_collapsed ? 22 : 28); }

void ProjectStrip::apply_width()
{
    const int w = strip_width();
    SetMinSize(wxSize(w, -1));
    SetMaxSize(wxSize(w, -1));
    SetSize(wxSize(w, GetSize().GetHeight()));
    if (wxWindow *parent = GetParent())
        parent->Layout();
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
    if (pt.y < header_height())
        return { HitKind::Header, -1 };
    const int bh      = button_height();
    const int buttons = size.y - 2 * bh;
    if (pt.y >= buttons)
        return { pt.y < buttons + bh ? HitKind::New : HitKind::Open, -1 };
    const int y = pt.y - header_height() + m_scroll;
    const int i = y / item_height();
    if (i < 0 || size_t(i) >= m_sessions.size())
        return {};
    const int id = m_sessions.slots()[size_t(i)].id;
    if (!m_collapsed) {
        // The close cross in the item's top-right corner.
        const int cross = FromDIP(14);
        const int top   = header_height() + i * item_height() - m_scroll;
        if (pt.x >= size.x - cross - FromDIP(4) && pt.y < top + cross + FromDIP(4))
            return { HitKind::ItemClose, id };
    }
    return { HitKind::Item, id };
}

void ProjectStrip::on_paint(wxPaintEvent &)
{
    wxAutoBufferedPaintDC dc(this);
    const bool   dark   = wxGetApp().dark_mode();
    const wxSize size   = GetClientSize();
    const wxColour bg     = dark ? wxColour(0x2B, 0x2B, 0x30) : wxColour(0xF0, 0xF0, 0xF0);
    const wxColour item_bg = dark ? wxColour(0x36, 0x36, 0x3C) : wxColour(0xFF, 0xFF, 0xFF);
    const wxColour hover_bg = dark ? wxColour(0x42, 0x42, 0x4A) : wxColour(0xE4, 0xF4, 0xEA);
    const wxColour text   = dark ? wxColour(0xE0, 0xE0, 0xE0) : wxColour(0x26, 0x2E, 0x30);
    const wxColour muted  = dark ? wxColour(0x90, 0x90, 0x98) : wxColour(0x8A, 0x8A, 0x8A);
    const wxColour line   = dark ? wxColour(0x4A, 0x4A, 0x50) : wxColour(0xD8, 0xD8, 0xD8);
    dc.SetBackground(wxBrush(bg));
    dc.Clear();
    dc.SetFont(wxGetApp().small_font());

    // Header: the collapse chevron.
    dc.SetTextForeground(muted);
    const wxString chevron = m_collapsed ? wxString::FromUTF8("\xC2\xBB") : wxString::FromUTF8("\xC2\xAB");
    const wxSize   cs      = dc.GetTextExtent(chevron);
    dc.DrawText(chevron, (size.x - cs.x) / 2, (header_height() - cs.y) / 2);
    dc.SetPen(wxPen(line));
    dc.DrawLine(0, header_height() - 1, size.x, header_height() - 1);
    dc.DrawLine(size.x - 1, 0, size.x - 1, size.y);

    const int bh      = button_height();
    const int list_top = header_height();
    const int list_bottom = size.y - 2 * bh;
    dc.SetClippingRegion(0, list_top, size.x, std::max(0, list_bottom - list_top));
    const int ih = item_height();
    for (size_t i = 0; i < m_sessions.size(); ++i) {
        const ProjectSlot &slot   = m_sessions.slots()[i];
        const bool         active = slot.id == m_sessions.active_id();
        const bool         hover  = m_hover.id == slot.id;
        const int          top    = list_top + int(i) * ih - m_scroll;
        if (top + ih < list_top || top > list_bottom)
            continue;
        const wxRect card(FromDIP(m_collapsed ? 3 : 4), top + FromDIP(2), size.x - FromDIP(m_collapsed ? 6 : 8), ih - FromDIP(4));
        dc.SetBrush(wxBrush(hover ? hover_bg : item_bg));
        dc.SetPen(active ? wxPen(accent_colour(), FromDIP(2)) : wxPen(line));
        dc.DrawRoundedRectangle(card, FromDIP(4));

        if (m_collapsed) {
            // The rail: one mark per project, filled for the active one.
            const int r = FromDIP(4);
            dc.SetBrush(active ? wxBrush(accent_colour()) : wxBrush(muted));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawCircle(card.x + card.width / 2, card.y + card.height / 2, r);
            if (slot.dirty) {
                dc.SetBrush(wxBrush(wxColour(0xF5, 0x9E, 0x0B)));
                dc.DrawCircle(card.x + card.width - FromDIP(3), card.y + FromDIP(3), FromDIP(2));
            }
            continue;
        }

        int text_top = card.y + FromDIP(6);
        if (!m_text_mode) {
            // A project card: the plate thumbnail of its snapshot, or a file shape with a folded corner (not a plate tile).
            const int    side = FromDIP(72);
            const wxRect thumb(card.x + (card.width - side) / 2, card.y + FromDIP(6), side, side);
            auto         it = m_extra.find(slot.id);
            if (it != m_extra.end() && it->second.thumbnail.IsOk()) {
                const wxBitmap &bmp = it->second.thumbnail;
                dc.DrawBitmap(bmp, thumb.x + (side - bmp.GetWidth()) / 2, thumb.y + (side - bmp.GetHeight()) / 2, true);
            } else {
                const int fold = FromDIP(14);
                dc.SetPen(wxPen(muted));
                dc.SetBrush(*wxTRANSPARENT_BRUSH);
                const wxRect page(thumb.x + FromDIP(14), thumb.y + FromDIP(8), side - FromDIP(28), side - FromDIP(16));
                wxPoint      outline[] = { { page.x, page.y }, { page.GetRight() - fold, page.y }, { page.GetRight(), page.y + fold },
                                           { page.GetRight(), page.GetBottom() }, { page.x, page.GetBottom() } };
                dc.DrawPolygon(5, outline);
                dc.DrawLine(page.GetRight() - fold, page.y, page.GetRight() - fold, page.y + fold);
                dc.DrawLine(page.GetRight() - fold, page.y + fold, page.GetRight(), page.y + fold);
            }
            text_top = thumb.GetBottom() + FromDIP(4);
        }

        // The name: two lines under the thumbnail, one line in text mode; ellipsised.
        dc.SetTextForeground(active ? text : (dark ? wxColour(0xC8, 0xC8, 0xC8) : wxColour(0x44, 0x4C, 0x4E)));
        const int      dot_w   = slot.dirty ? FromDIP(10) : 0;
        const int      avail   = card.width - FromDIP(10) - dot_w - (m_text_mode ? FromDIP(14) : 0);
        const wxString name    = from_u8(slot.name);
        const int      lh      = dc.GetCharHeight();
        int            x       = card.x + FromDIP(5) + dot_w;
        if (slot.dirty) {
            dc.SetBrush(wxBrush(wxColour(0xF5, 0x9E, 0x0B)));
            dc.SetPen(*wxTRANSPARENT_PEN);
            const int cy = m_text_mode ? card.y + card.height / 2 : text_top + lh / 2;
            dc.DrawCircle(card.x + FromDIP(8), cy, FromDIP(3));
        }
        if (m_text_mode) {
            const wxString shown = wxControl::Ellipsize(name, dc, wxELLIPSIZE_END, avail);
            dc.DrawText(shown, x, card.y + (card.height - lh) / 2);
        } else {
            // First line: as many characters as fit, broken at a space when there is one.
            wxString first = name, rest;
            if (dc.GetTextExtent(name).x > avail) {
                size_t n = 0;
                while (n < name.length() && dc.GetTextExtent(name.Left(n + 1)).x <= avail)
                    ++n;
                const size_t space = name.Left(n).find_last_of(' ');
                if (space != wxString::npos && space > 0)
                    n = space + 1;
                first = name.Left(n);
                rest  = name.Mid(n);
            }
            dc.DrawText(first, x, text_top);
            if (!rest.empty())
                dc.DrawText(wxControl::Ellipsize(rest, dc, wxELLIPSIZE_END, avail), card.x + FromDIP(5), text_top + lh);
        }

        // The close cross, while hovered.
        if (hover) {
            dc.SetTextForeground(muted);
            const wxString cross = wxString::FromUTF8("\xC3\x97");
            const wxSize   xs    = dc.GetTextExtent(cross);
            dc.DrawText(cross, card.GetRight() - xs.x - FromDIP(3), card.y + FromDIP(1));
        }
    }
    dc.DestroyClippingRegion();

    // "+ New" and "Open..." at the bottom.
    dc.SetPen(wxPen(line));
    dc.DrawLine(0, list_bottom, size.x, list_bottom);
    const wxString labels[2] = { m_collapsed ? wxString("+") : "+ " + _L("New"),
                                 m_collapsed ? wxString::FromUTF8("\xE2\x80\xA6") : _L("Open...") };
    for (int b = 0; b < 2; ++b) {
        const int    top = list_bottom + b * bh;
        const bool   hot = (b == 0 && m_hover.kind == HitKind::New) || (b == 1 && m_hover.kind == HitKind::Open);
        if (hot) {
            dc.SetBrush(wxBrush(hover_bg));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(0, top, size.x - 1, bh);
        }
        dc.SetTextForeground(hot ? accent_colour() : text);
        const wxString shown = wxControl::Ellipsize(labels[b], dc, wxELLIPSIZE_END, size.x - FromDIP(4));
        const wxSize   ts    = dc.GetTextExtent(shown);
        dc.DrawText(shown, (size.x - ts.x) / 2, top + (bh - ts.y) / 2);
    }
}

void ProjectStrip::on_left_down(wxMouseEvent &evt)
{
    const Hit hit = hit_test(evt.GetPosition());
    switch (hit.kind) {
    case HitKind::Header: set_collapsed(!m_collapsed); break;
    case HitKind::Item: select_project(hit.id); break;
    case HitKind::ItemClose: close_project(hit.id); break;
    case HitKind::New: new_project(); break;
    case HitKind::Open: open_project(); break;
    default: break;
    }
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
    const Hit hit = hit_test(evt.GetPosition());
    if (hit.kind != m_hover.kind || hit.id != m_hover.id) {
        m_hover = hit;
        wxString tip;
        if (hit.kind == HitKind::Item || hit.kind == HitKind::ItemClose) {
            if (const ProjectSlot *slot = m_sessions.find(hit.id))
                tip = hit.kind == HitKind::ItemClose ? _L("Close project") : tooltip_for(*slot);
        } else if (hit.kind == HitKind::Header)
            tip = m_collapsed ? _L("Show the project strip") : _L("Collapse the project strip");
        else if (hit.kind == HitKind::New)
            tip = _L("New project");
        else if (hit.kind == HitKind::Open)
            tip = _L("Open project");
        SetToolTip(tip);
        Refresh();
    }
    evt.Skip();
}

void ProjectStrip::on_leave(wxMouseEvent &evt)
{
    m_hover = {};
    Refresh();
    evt.Skip();
}

void ProjectStrip::on_wheel(wxMouseEvent &evt)
{
    const int content = int(m_sessions.size()) * item_height();
    const int view    = GetClientSize().y - header_height() - 2 * button_height();
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
    const int id_save = wxWindow::NewControlId(), id_save_as = wxWindow::NewControlId(), id_close = wxWindow::NewControlId(),
              id_close_others = wxWindow::NewControlId(), id_reveal = wxWindow::NewControlId(), id_window = wxWindow::NewControlId();
    menu.Append(id_save, _L("Save"));
    menu.Append(id_save_as, _L("Save as..."));
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
    const int id_thumbs = wxWindow::NewControlId(), id_names = wxWindow::NewControlId(), id_collapse = wxWindow::NewControlId();
    menu.AppendRadioItem(id_thumbs, _L("Show thumbnails"));
    menu.AppendRadioItem(id_names, _L("Show names only"));
    menu.Check(m_text_mode ? id_names : id_thumbs, true);
    menu.AppendSeparator();
    menu.Append(id_collapse, m_collapsed ? _L("Show the project strip") : _L("Collapse the project strip"));
    const int chosen = GetPopupMenuSelectionFromUser(menu);
    if (chosen == id_thumbs)
        set_text_mode(false);
    else if (chosen == id_names)
        set_text_mode(true);
    else if (chosen == id_collapse)
        set_collapsed(!m_collapsed);
}

} // namespace GUI
} // namespace Slic3r
