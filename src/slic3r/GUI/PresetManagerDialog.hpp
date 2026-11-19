#pragma once

// The Filament manager and the Process manager: one sortable, filterable table of every filament or
// process preset (resources/web/guide/preset_manager, on the same data_table.js as Printer Selection),
// with an "Enabled" box and a favourite star per row, and rename / delete for user presets. Design:
// tests/design_preset_manager_tables.md.
//
// Everything applies at once (owner, Q8): a star is saved when clicked; an Enabled change (the page
// sends ticks in batches, 250 ms after the last one) is stored in the app config and the presets'
// visibility is read again at once, without reloading anything from disk, so the sidebar and the
// settings combos follow while the dialog is open and unsaved edits stay. The one full preset reload
// (with its unsaved-changes prompt) runs once when the Filament manager closes, if the enabled set
// changed: GUI_App::run_preset_manager. There is a single Close button.
//
// Rename (F2 / double-click) and delete (Delete key, row button, several selected rows) apply at once too
// (phase 2). A rename moves the preset's file, re-points user presets that inherit it, the selection,
// the project's filament slots / per-nozzle processes and the favourite star; a user filament renames
// its whole " @printer" family by default. A delete asks first and names what uses the preset; those
// filament slots move to the next visible preset, a nozzle's process falls back to the plate's.
//
// "Create new" and "Edit printers..." open their dialogs over the manager, which stays open and lists
// the result when they return. Only "edit this preset's settings" (from Edit printers...) closes it, so
// the settings window can be used.
//
// Esc never closes the dialog by itself: the page decides (a filter popover first, a rename in
// progress, then it sends pm_close).

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "GUI_Utils.hpp"
#include "libslic3r/PresetManagerModel.hpp"

class wxWebView;
class wxWebViewEvent;

namespace Slic3r {
class PresetCollection;
namespace GUI {

class PresetManagerDialog : public DPIDialog
{
public:
    // What to do once the dialog is closed.
    // EditPresetSettings: "Edit" inside Edit printers...; OpenPresetEditor: the row's edit button.
    enum class Then { Nothing, EditPresetSettings, OpenPresetEditor };

    PresetManagerDialog(wxWindow *parent, PresetManagerKind kind);
    ~PresetManagerDialog() override;

    // false when the web view could not be created (the dialog then shows nothing useful).
    bool               ok() const { return m_view != nullptr; }
    Then               then() const { return m_then; }
    // Then::EditPresetSettings: the filament preset whose settings to open, and its filament_id (the
    // settings window goes back to Edit printers... for it when it closes, as before).
    const std::string &edit_preset_name() const { return m_edit_preset_name; }
    const std::string &edit_filament_id() const { return m_edit_filament_id; }

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    void on_message(wxWebViewEvent &evt);
    void on_char_hook(wxKeyEvent &evt);
    void build_model();
    void send(const std::string &command, nlohmann::json payload);
    void send_rows();
    void send_favorites();
    void apply_enabled(const nlohmann::json &changes);
    // After an Enabled change: re-read each row's state from the bundle; the rows that changed, and the
    // rows the page asked about (so a refused change is undone there), go to the page.
    void refresh_enabled(const std::set<std::string> &asked);
    // The presets changed (a rename, a delete, a child dialog): rebuild the rows and send them.
    void reload_rows();
    void close_with(Then then);
    void save_size();

    // Child dialogs over the manager (filaments).
    void create_filament();
    void edit_filament(const std::string &filament_id);
    // Phase 2: rename / delete of user presets.
    PresetCollection       &collection();
    const PresetManagerRow *user_row(const std::string &key) const;
    PresetRenamePlan        plan_rename(const PresetManagerRow &row, const std::string &typed, bool whole_family);
    void                    check_name(const std::string &key, const std::string &typed, bool whole_family);
    void                    rename(const std::string &key, const std::string &typed, bool whole_family);
    void                    delete_row(const std::string &key);
    // Several rows at once (the selection): one confirmation, one refresh.
    void                    delete_rows(const std::vector<std::string> &keys);
    // The row's edit button: the preset's settings tab (closes the manager; it reopens afterwards).
    void                    open_editor(const std::string &key);
    // The side bar, the settings tab and the other pickers show the presets as they are now.
    void                    refresh_app_presets();

    PresetManagerKind                       m_kind;
    wxWebView                              *m_view{nullptr};
    PresetManagerModel                      m_model;
    std::unordered_map<std::string, size_t> m_row_index;
    int                                     m_fav_subscription{0};
    Then                                    m_then{Then::Nothing};
    std::string                             m_edit_preset_name;
    std::string                             m_edit_filament_id;
    bool                                    m_closing{false};
    bool                                    m_busy{false}; // a child dialog or a confirmation is open
};

}} // namespace Slic3r::GUI
