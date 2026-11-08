#ifndef slic3r_GUI_ProjectStrip_hpp_
#define slic3r_GUI_ProjectStrip_hpp_

// Project strip (tests/design_multi_project_sidebar.md): a column at the far left of the main window listing the open
// projects. Home, Device, Printers, notifications and the hub stay window-level.
//
// P4 (owner, 2026-11-07): one live Plater per open project, browser-tab style. MainFrame shows one of them
// (MainFrame::activate_plater); the others keep their model, undo history, slice, preview, selection, camera and
// notifications out of sight, and a slice running in one of them goes on. A switch never saves or loads anything. The
// presets are app-global: each project's are captured when it goes out of sight (PresetBundle::capture_project_presets)
// and put back when it is shown (restore_project_presets); its unsaved preset edits ride with it.
//
// D1 far left, 88 DIP by default, a 24 DIP rail when collapsed; D2 thumbnails by default, names in text mode; D3 at most 8
// projects (2-16, app config "project_strip_max_open", Preferences > Project), now the bound on live Platers (memory): at the
// limit, opening another one asks which to close; D6 "Open in new window" in the item menu, multi-window unchanged; D9
// always shown.
// P2: New / Open icon buttons at the top, names on two lines broken at word ends, the strip widened by dragging its right
// edge (88-200 DIP, "project_strip_width"), unsaved and slicing / sliced badges, a live thumbnail of the active project,
// "Send..." in the item menu.
// P3: drag reorder, Explorer / Finder opening into the strip, opt-in session restore, the macOS Window menu.

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <wx/bitmap.h>
#include <wx/image.h>
#include <wx/panel.h>
#include <wx/timer.h>

#include "libslic3r/PresetBundle.hpp"
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
    // Makes that project the active one. False when the switch was refused (busy).
    bool select_project(int id);
    // Ctrl+PgDn (+1) / Ctrl+PgUp (-1), wrapping.
    void cycle(int step);
    // Closes a project, asking about its unsaved changes first. False when cancelled or refused.
    bool close_project(int id);

    // App exit with several projects open: one dialog lists every unsaved one. False when the close was cancelled.
    bool   confirm_close_all();
    // The session snapshots (exit without session restore: none), removed one literal path at a time.
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
    // P3 session restore (opt-in): at exit, write every project's snapshot and the manifest so the next start brings them
    // back. Called by MainFrame after the unsaved-projects dialog; the snapshots then stay on disk.
    void save_session_for_restart();
    // P3 macOS Window menu: the projects in strip order, and the active one.
    std::vector<std::pair<int, wxString>> menu_entries() const;
    int                                   active_project_id() const { return m_sessions.active_id(); }

private:
    struct SlotExtra
    {
        Plater  *plater{ nullptr }; // the project's own Plater
        // The project's presets while another project is shown (captured when it goes out of sight).
        PresetBundle::ProjectPresetState presets;
        wxImage  image;             // the thumbnail as rendered (or read from a session snapshot)
        wxBitmap scaled;            // scaled to the tile
        int      scaled_side{ 0 };
        size_t   thumb_marker{ size_t(-1) }; // the project's change marker when `image` was rendered
        // Session restore: the snapshot files of the previous run, deleted one by one once the project is back.
        std::vector<std::string> files;
        std::string              dir;
    };
    enum class HitKind { None, Header, Grip, Item, ItemClose };
    struct Hit
    {
        HitKind kind{ HitKind::None };
        int     id{ -1 };
    };
    // Holds the window for one switch or strip open: opens arriving meanwhile wait, the backup timer stays out, and (when
    // a project's presets are put back) the swap guard stays on until the calls the preset refresh deferred ran.
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
    Plater     *plater_of(int id) const;
    // The shown project may be left (no job or tool holds it). Says why not in a notice.
    bool        can_leave_active();
    // The shown project goes out of sight: its tab and presets are kept with its slot.
    void        stash_active();
    // Shows that slot's Plater and puts its presets back. Returns the preset part's time in ms.
    long long   show_slot(int id);
    // stash_active + show_slot + select, timed and logged.
    bool        switch_to(int id, const char *mode);
    void        after_switch(int id, std::chrono::steady_clock::time_point started, long long presets_ms, const char *mode);
    // At the limit (D3): asks which project to close to make room. False when none was closed.
    bool        make_room();
    void        restore_session();
    std::string session_manifest_path() const;
    std::string slot_dir(int id) const;
    bool        active_is_blank() const;
    bool        save_slot(int id, bool save_as);
    void        send_project(int id);
    void        open_in_new_window(int id);
    void        close_others(int id);
    void        remove_slot_files(int id);
    void        load_thumbnail(int id, const std::string &snapshot_3mf);
    const wxBitmap *tile_bitmap(int id, int side);
    void        toast(const wxString &text) const;
    void        sync_active();
    wxString    tooltip_for(const ProjectSlot &slot) const;
    void        on_poll(wxTimerEvent &);
    void        on_thumbnail_timer(wxTimerEvent &);
    // Renders the shown project's plate into its tile. False when it cannot (no canvas on screen, names-only mode).
    bool        render_active_thumbnail();
    size_t      active_thumbnail_marker() const;
    bool        plater_tab_shown() const;
    void        refresh_max_open();

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
    size_t                   m_active_marker{ size_t(-1) };
    int                      m_spin{ 0 };
    // P3: drag reorder, session restore.
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
