#ifndef slic3r_GUI_ProjectStrip_hpp_
#define slic3r_GUI_ProjectStrip_hpp_

// Project strip, P1 + P2 of tests/design_multi_project_sidebar.md (owner decisions D1-D10, 2026-11-07): a column at the far left
// of the main window listing the open projects. One Plater shows the active project; the others are parked as a temporary 3MF
// each (Plater::park_project) and come back as their 3MF is opened (Plater::swap_in_project). Home, Device, Printers,
// notifications and the hub stay window-level.
//
// D1 far left, 88 DIP by default, a 24 DIP rail when collapsed; D2 thumbnails by default, names in text mode; D3 at most 8
// projects (2-16, app config "project_strip_max_open"); D4 no slicing in parked projects (a switch cancels a running slice); D5
// disk snapshots only; D6 "Open in new window" in the item menu, multi-window unchanged; D8 undo history is not kept across a
// switch; D9 always shown.
// P2 (owner feedback on the P1 hand test): New / Open as two icon buttons at the top, names on two lines broken at word ends,
// the strip widened by dragging its right edge (88-200 DIP, "project_strip_width"), unsaved and slicing / sliced badges, a live
// thumbnail of the active project, "Send..." in the item menu.

#include <chrono>
#include <future>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <wx/bitmap.h>
#include <wx/image.h>
#include <wx/panel.h>
#include <wx/timer.h>

#include "Camera.hpp"
#include "Plater.hpp"
#include "ProjectSessions.hpp"

class wxMenu;
class ScalableButton; // wxExtensions.hpp, global namespace

namespace Slic3r {
namespace GUI {

class MainFrame;
class Plater;

class ProjectStrip : public wxPanel
{
public:
    ProjectStrip(wxWindow *parent, MainFrame *frame);
    ~ProjectStrip() override;

    // File > New project, Ctrl+N and the strip's "+": a new Untitled project next to the active one.
    void new_project();
    // File > Open project, Ctrl+O and the strip's folder button: the file in a new slot (its own slot when it is open already).
    // An empty name asks for the file.
    void open_project(const wxString &file = wxEmptyString);
    // Makes that project the active one. False when the switch was refused (busy) or failed.
    bool select_project(int id);
    // Ctrl+PgDn (+1) / Ctrl+PgUp (-1), wrapping.
    void cycle(int step);
    // Closes a project, asking about its unsaved changes first. False when cancelled or refused.
    bool close_project(int id);

    // App exit with several projects open: one dialog lists every unsaved one. False when the close was cancelled.
    bool   confirm_close_all();
    // The parked snapshots and their G-code, removed one literal path at a time (exit).
    void   remove_all_slot_files();
    size_t project_count() const { return m_sessions.size(); }
    bool   switching() const { return m_switching; }

    // The active project's name, file or unsaved state changed (Plater title updates call this).
    void on_active_project_changed();
    void rescale();
    void sys_color_changed();

    // P3 (D7): project files from Explorer / Finder (another instance handed them over, or macOS opened them) go into the
    // strip when the preference says so. False when they are not all project files or the preference is "replace".
    bool open_files_from_shell(const wxArrayString &files);
    // P3 session restore (opt-in): at exit, park the active project and write the manifest so the next start brings every
    // project back. Called by MainFrame after the unsaved-projects dialog; the slot files then stay on disk.
    void save_session_for_restart();
    // P3 macOS Window menu: the projects in strip order, and the active one.
    std::vector<std::pair<int, wxString>> menu_entries() const;
    int                                   active_project_id() const { return m_sessions.active_id(); }

private:
    struct SlotExtra
    {
        wxImage               image;      // the thumbnail as read or rendered
        wxBitmap              scaled;     // scaled to the tile
        int                   scaled_side{ 0 };
        std::optional<Camera> camera;
        // Every file the slot wrote, deleted one by one when it closes.
        std::vector<std::string> files;
        // The folder holding them (a restored session's are under the previous run's folder).
        std::string dir;
        // A sliced-file project (.gcode.3mf): a preview of its G-code.
        bool exported_file{ false };
        // P3 warm park: its snapshot read ahead on a worker thread.
        std::shared_future<std::shared_ptr<Plater::PreparsedProject>> warm;
        std::uint64_t parked_at{ 0 };
        size_t        warm_estimate{ 0 };
    };
    enum class HitKind { None, Header, Grip, Item, ItemClose };
    struct Hit
    {
        HitKind kind{ HitKind::None };
        int     id{ -1 };
    };
    // Holds the plater for one switch: opens arriving meanwhile wait, the backup timer stays out, and (when the incoming
    // project is switched in rather than opened or created) the swap guard stays on until the calls the load deferred ran.
    class SwitchScope
    {
    public:
        SwitchScope(ProjectStrip &strip, bool swap_guard);
        ~SwitchScope();

    private:
        ProjectStrip &m_strip;
        bool          m_swap_guard;
    };

    Plater     *plater() const;
    bool        can_switch(bool cancel_slicing, bool &slicing_cancelled);
    bool        park_active(std::string &error);
    bool        restore(ProjectSlot &target);
    void        after_switch(int id, bool slicing_cancelled, std::chrono::steady_clock::time_point started, long long park_ms, const char *mode);
    void        reattach_gcode(int id);
    void        start_read_ahead(int id);
    void        drop_warm(SlotExtra &extra);
    void        enforce_warm_budget();
    void        wait_for_read_aheads();
    void        restore_session();
    std::string session_manifest_path() const;
    bool        active_is_blank() const;
    bool        save_slot(int id, bool save_as);
    void        send_project(int id);
    void        open_in_new_window(int id);
    void        close_others(int id);
    void        remove_slot_files(int id);
    void        load_thumbnail(ProjectSlot &slot);
    const wxBitmap *tile_bitmap(int id, int side);
    std::string slot_dir(int id) const;
    void        toast(const wxString &text) const;
    void        sync_active();
    wxString    tooltip_for(const ProjectSlot &slot) const;
    void        on_poll(wxTimerEvent &);
    void        on_thumbnail_timer(wxTimerEvent &);
    bool        plater_tab_shown() const;

    // Layout and drawing.
    int  strip_width() const;
    int  header_height() const;
    int  item_height() const;
    int  thumb_side() const;
    int  grip_width() const;
    void apply_width();
    void layout_buttons();
    void apply_colours();
    Hit  hit_test(const wxPoint &pt) const;
    void on_paint(wxPaintEvent &);
    void on_left_down(wxMouseEvent &);
    void on_left_up(wxMouseEvent &);
    void on_capture_lost(wxMouseCaptureLostEvent &);
    void on_middle_down(wxMouseEvent &);
    void on_right_down(wxMouseEvent &);
    void on_motion(wxMouseEvent &);
    void on_leave(wxMouseEvent &);
    void on_wheel(wxMouseEvent &);
    void on_size(wxSizeEvent &);
    void show_item_menu(int id);
    void show_strip_menu();
    void set_collapsed(bool collapsed);
    void set_text_mode(bool text_mode);
    void end_drag();
    int  drop_index_at(int y) const;

    MainFrame               *m_frame{ nullptr };
    ProjectSessions          m_sessions;
    std::map<int, SlotExtra> m_extra;
    std::string              m_root;
    bool                     m_switching{ false };
    bool                     m_collapsed{ false };
    bool                     m_text_mode{ false };
    int                      m_width_dip{ StripLayout::default_width }; // the dragged width
    int                      m_scroll{ 0 };
    Hit                      m_hover;
    ScalableButton          *m_btn_new{ nullptr };
    ScalableButton          *m_btn_open{ nullptr };
    // Dragging the right edge.
    bool                     m_dragging{ false };
    int                      m_drag_start_x{ 0 };
    int                      m_drag_start_dip{ 0 };
    // Badges and the live thumbnail of the active project.
    wxTimer                  m_poll_timer;
    wxTimer                  m_thumb_timer;
    bool                     m_active_slicing{ false };
    size_t                   m_active_marker{ size_t(-1) };
    int                      m_spin{ 0 };
    // P3: the last switch's read, the slot parked by it, read-aheads being dropped, drag reorder, session restore.
    bool                     m_last_restore_warm{ false };
    int                      m_last_parked{ -1 };
    std::uint64_t            m_park_clock{ 0 };
    std::vector<std::shared_future<std::shared_ptr<Plater::PreparsedProject>>> m_retiring;
    int                      m_press_id{ -1 };
    int                      m_press_y{ 0 };
    bool                     m_reordering{ false };
    int                      m_drop_index{ -1 };
    bool                     m_keep_files_for_session{ false };
    wxTimer                  m_session_timer;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_ProjectStrip_hpp_
