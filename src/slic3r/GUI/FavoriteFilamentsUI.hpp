#pragma once

// The app side of favourite filaments: reads and writes the one per-user set in the app config
// (local only), follows vendor renames once per session, and tells every open filament picker to
// regroup when the set changes. The rules themselves are in FavoriteFilaments.hpp (GUI-free).

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <wx/string.h>

class wxWindow;
class wxMenu;

namespace Slic3r {
class Preset;
namespace GUI {
namespace FavoriteFilamentsUI {

// The key of a filament preset: a system preset's alias, a user preset's full name.
std::string key_for(const Preset &preset);

std::set<std::string> load();
bool                  is_favorite(const std::string &key);
bool                  is_favorite(const Preset &preset);
// Stars or un-stars the key, saves the app config and notifies the pickers. Returns the new state.
bool                  toggle(const std::string &key);

// Pickers that list filaments subscribe to hear about a change made anywhere (a star in the AMS
// dialog regroups the sidebar too). The returned id unsubscribes.
int  subscribe(std::function<void()> on_changed);
void unsubscribe(int id);

// "Add to favorite filaments" / "Remove from favorite filaments" for the key: an item at the end of
// the menu, the keyboard and screen-reader path where a star cannot be clicked (context
// menu key / Shift+F10, macOS native combos). Returns the item id, or wxID_NONE for an empty key.
int append_toggle_item(wxMenu &menu, const std::string &key);

// The star bitmaps the pickers draw (filled = favourite), at 16 px for the window's DPI.
wxString star_icon_name(bool favorite);

// A filament list in a plain combo box (not a PresetComboBox) with the favourites on top.
//
// On our ::ComboBox (every platform's custom widget) the list gets a "Favorite filaments" header, the
// favourites, an "All filaments" header and every filament, each row with a star that toggles it; a
// star clicked while the list is open regroups the list when it closes, so rows never move under the
// cursor. On a native combo (the AMS slot dialog on macOS) there are no headers or row icons: the
// favourites come first with a star prefix, and the dialog offers a star button instead. Both get a
// context menu (right-click, the context-menu key, Shift+F10) on the current filament.
//
// The owner keeps its own per-filament data in entry order and asks entry_of_row() for the selection:
// a row index is not an entry index any more (headers, the favourites' second copies).
class FavoriteComboList
{
public:
    struct Entry
    {
        wxString    text; // what the list shows (without any star)
        std::string key;  // FavoriteFilamentsUI::key_for() of the preset behind it
    };

    // combo is a ::ComboBox or a native wxComboBox / wxChoice. on_regrouped runs after a rebuild
    // the owner did not ask for (a favourite changed); the selected entry is kept.
    explicit FavoriteComboList(wxWindow *combo, std::function<void()> on_regrouped = nullptr);
    ~FavoriteComboList();
    FavoriteComboList(const FavoriteComboList &)            = delete;
    FavoriteComboList &operator=(const FavoriteComboList &) = delete;

    // Replaces the list and selects the entry (-1: none). Does not send a selection event.
    void set_entries(std::vector<Entry> entries, int selected_entry = -1);
    // Rebuilds with the current favourites, keeping the selected entry.
    void regroup();

    const std::vector<Entry> &entries() const { return m_entries; }
    int  entry_of_row(int row) const; // -1 for a header or an out-of-range row
    int  selected_entry() const;
    void select_entry(int entry);     // the first row showing it (the favourites copy when starred)
    // Toggles the favourite of the selected entry (a star button's action). Returns the new state.
    bool toggle_selected();
    bool selected_is_favorite() const;

private:
    void rebuild(int selected_entry);
    void update_stars();
    void on_action(int row);
    bool is_custom() const;

    wxWindow             *m_window{nullptr};
    std::vector<Entry>    m_entries;
    std::vector<int>      m_row_entry; // row -> entry, -1 for headers
    std::function<void()> m_on_regrouped;
    int                   m_subscription{0};
    bool                  m_regroup_on_close{false};
    std::shared_ptr<bool> m_alive; // handlers bound to the combo check it: the list may go first
};

} // namespace FavoriteFilamentsUI
} // namespace GUI
} // namespace Slic3r
