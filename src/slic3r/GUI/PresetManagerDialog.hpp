#pragma once

// The Filament manager and the Process manager: one sortable, filterable table of every filament or
// process preset (resources/web/guide/preset_manager, on the same data_table.js as Printer Selection),
// with an "Enabled" box and a favourite star per row. Design: tests/design_preset_manager_tables.md.
//
// Everything applies at once (owner, Q8): a star is saved when clicked; an Enabled change (the page
// sends ticks in batches, 250 ms after the last one) is stored in the app config and the presets'
// visibility is read again at once, without reloading anything from disk, so the sidebar and the
// settings combos follow while the dialog is open and unsaved edits stay. The one full preset reload
// (with its unsaved-changes prompt) runs once when the Filament manager closes, if the enabled set
// changed: GUI_App::run_preset_manager. There is a single Close button.
//
// Esc never closes the dialog by itself: the page decides (a filter popover first, an edit in phase 2,
// then it sends pm_close).

#include <map>
#include <set>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "GUI_Utils.hpp"
#include "libslic3r/PresetManagerModel.hpp"

class wxWebView;
class wxWebViewEvent;

namespace Slic3r { namespace GUI {

class PresetManagerDialog : public DPIDialog
{
public:
    // What to do once the dialog is closed (the filament buttons hand over to other dialogs).
    enum class Then { Nothing, CreateFilament, EditFilament };

    PresetManagerDialog(wxWindow *parent, PresetManagerKind kind);
    ~PresetManagerDialog() override;

    // false when the web view could not be created (the dialog then shows nothing useful).
    bool               ok() const { return m_view != nullptr; }
    Then               then() const { return m_then; }
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
    void close_with(Then then);
    void save_size();

    PresetManagerKind                       m_kind;
    wxWebView                              *m_view{nullptr};
    PresetManagerModel                      m_model;
    std::unordered_map<std::string, size_t> m_row_index;
    int                                     m_fav_subscription{0};
    Then                                    m_then{Then::Nothing};
    std::string                             m_edit_filament_id;
    bool                                    m_closing{false};
};

}} // namespace Slic3r::GUI
