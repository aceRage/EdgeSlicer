#include "FavoriteFilamentsUI.hpp"
#include "FavoriteFilaments.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "GUI.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <map>
#include <utility>
#include <vector>

#include <wx/menu.h>
#include <wx/ctrlsub.h>

#include "Widgets/ComboBox.hpp"
#include "wxExtensions.hpp"

namespace Slic3r { namespace GUI { namespace FavoriteFilamentsUI {

namespace {

struct Subscriber
{
    Kind                  kind;
    std::function<void()> on_changed;
};

std::map<int, Subscriber> &fav_subscribers()
{
    static std::map<int, Subscriber> subs;
    return subs;
}

std::set<std::string> fav_read_config(Kind kind)
{
    std::set<std::string> keys;
    AppConfig *config = wxGetApp().app_config;
    if (!config)
        return keys;
    const std::string serialized = config->get(FavoritePresets::config_key(kind));
    std::vector<std::string> values;
    if (!serialized.empty() && unescape_strings_cstyle(serialized, values))
        for (const std::string &v : values)
            if (!v.empty())
                keys.insert(v);
    return keys;
}

void fav_write_config(Kind kind, const std::set<std::string> &keys)
{
    AppConfig *config = wxGetApp().app_config;
    if (!config)
        return;
    config->set(FavoritePresets::config_key(kind), escape_strings_cstyle(std::vector<std::string>(keys.begin(), keys.end())));
    config->save();
}

// Once per session and kind, after the system presets are loaded: a favourite whose system preset a
// vendor renamed (renamed_from in the new profile) moves to the new name.
void fav_migrate_once(Kind kind, std::set<std::string> &keys)
{
    static bool done[2] = {false, false};
    bool       &kind_done = done[kind == Kind::Process ? 1 : 0];
    if (kind_done || keys.empty())
        return;
    PresetBundle *bundle = wxGetApp().preset_bundle;
    if (!bundle)
        return;
    const PresetCollection &collection = kind == Kind::Process ? bundle->prints : bundle->filaments;
    if (collection.size() <= collection.num_default_presets())
        return; // not loaded yet; try again on the next read
    kind_done = true;

    std::set<std::string>                            current;
    std::vector<std::pair<std::string, std::string>> renamed;
    for (const Preset &preset : collection.get_presets()) {
        const std::string key = key_for(preset);
        if (!key.empty())
            current.insert(key);
        for (const std::string &old_name : preset.renamed_from)
            renamed.emplace_back(key, old_name);
    }
    if (FavoriteFilaments::migrate(keys, current, FavoriteFilaments::rename_map(renamed)))
        fav_write_config(kind, keys);
}

void fav_notify(Kind kind)
{
    // After the event that changed the set has finished: a picker may rebuild its own list.
    wxGetApp().CallAfter([kind]() {
        // Copy: a subscriber may unsubscribe (a dialog closing) while we iterate.
        const auto subs = fav_subscribers();
        for (const auto &[id, sub] : subs) {
            // Only the pickers of this kind: a star on a process preset leaves the filament lists alone.
            if (sub.kind != kind)
                continue;
            // Call the copy: the subscriber may unsubscribe itself from inside the call.
            if (fav_subscribers().count(id) > 0 && sub.on_changed)
                sub.on_changed();
        }
    });
}

} // namespace

bool kind_of(Preset::Type preset_type, Kind &kind)
{
    switch (preset_type) {
    case Preset::TYPE_FILAMENT: kind = Kind::Filament; return true;
    case Preset::TYPE_PRINT:    kind = Kind::Process;  return true;
    default:                    return false;
    }
}

std::string key_for(const Preset &preset)
{
    return FavoriteFilaments::key_for(preset.name, preset.alias, preset.is_system);
}

std::set<std::string> load(Kind kind)
{
    std::set<std::string> keys = fav_read_config(kind);
    fav_migrate_once(kind, keys);
    return keys;
}

bool is_favorite(const std::string &key, Kind kind) { return !key.empty() && load(kind).count(key) > 0; }

bool is_favorite(const Preset &preset)
{
    Kind kind;
    return kind_of(preset.type, kind) && is_favorite(key_for(preset), kind);
}

bool toggle(const std::string &key, Kind kind)
{
    if (key.empty())
        return false;
    std::set<std::string> keys  = load(kind);
    const bool            state = FavoriteFilaments::toggle(keys, key);
    fav_write_config(kind, keys);
    fav_notify(kind);
    return state;
}

int subscribe(std::function<void()> on_changed, Kind kind)
{
    static int next_id = 1;
    const int  id      = next_id++;
    fav_subscribers().emplace(id, Subscriber{kind, std::move(on_changed)});
    return id;
}

void unsubscribe(int id) { fav_subscribers().erase(id); }

int append_toggle_item(wxMenu &menu, const std::string &key, Kind kind)
{
    if (key.empty())
        return wxID_NONE;
    if (menu.GetMenuItemCount() > 0)
        menu.AppendSeparator();
    const int       id   = wxWindow::NewControlId();
    const bool      fav  = is_favorite(key, kind);
    const wxString  name = from_u8(key);
    if (kind == Kind::Process) {
        // TRN %s is a process preset name, e.g. "0.20mm Standard"
        menu.Append(id, fav ? wxString::Format(_L("Remove \"%s\" from favorite process presets"), name)
                            : wxString::Format(_L("Add \"%s\" to favorite process presets"), name));
    } else {
        // TRN %s is a filament name, e.g. "Bambu PLA Basic"
        menu.Append(id, fav ? wxString::Format(_L("Remove \"%s\" from favorite filaments"), name)
                            : wxString::Format(_L("Add \"%s\" to favorite filaments"), name));
    }
    menu.Bind(wxEVT_MENU, [key, kind](wxCommandEvent &) { toggle(key, kind); }, id);
    return id;
}

wxString star_button_tooltip(bool favorite, Kind kind)
{
    if (kind == Kind::Process)
        return favorite ? _L("Remove this process preset from your favorite process presets")
                        : _L("Add this process preset to your favorite process presets (they are listed first in the process list)");
    return favorite ? _L("Remove this filament from your favorite filaments")
                    : _L("Add this filament to your favorite filaments (they are listed first in every filament list)");
}

wxString star_icon_name(bool favorite) { return favorite ? "score_star_light" : "score_star_outline"; }

// ---------------------------------------------------------------------------------------------------
// FavoriteComboList

FavoriteComboList::FavoriteComboList(wxWindow *combo, std::function<void()> on_regrouped)
    : m_window(combo), m_on_regrouped(std::move(on_regrouped)), m_alive(std::make_shared<bool>(true))
{
    std::weak_ptr<bool> alive = m_alive;
    if (auto *cb = dynamic_cast<::ComboBox *>(combo)) {
        cb->Bind(EVT_DROPDOWN_ITEM_ACTION, [this, alive](wxCommandEvent &e) {
            if (alive.expired()) { e.Skip(); return; }
            on_action(e.GetInt());
        });
        cb->Bind(wxEVT_COMBOBOX_CLOSEUP, [this, alive](wxCommandEvent &e) {
            e.Skip();
            if (alive.expired() || !m_regroup_on_close)
                return;
            m_regroup_on_close = false;
            m_window->CallAfter([this, alive]() {
                if (alive.expired())
                    return;
                regroup();
                if (m_on_regrouped)
                    m_on_regrouped();
            });
        });
    }
    combo->Bind(wxEVT_CONTEXT_MENU, [this, alive](wxContextMenuEvent &e) {
        if (alive.expired()) { e.Skip(); return; }
        const int entry = selected_entry();
        if (entry < 0) { e.Skip(); return; }
        wxMenu menu;
        append_toggle_item(menu, m_entries[entry].key);
        m_window->PopupMenu(&menu);
    });
    m_subscription = subscribe([this, alive]() {
        if (alive.expired())
            return;
        auto *cb = dynamic_cast<::ComboBox *>(m_window);
        if (cb && cb->is_drop_down()) {
            // Never move rows under the cursor: the stars change now, the groups when it closes.
            update_stars();
            m_regroup_on_close = true;
            return;
        }
        regroup();
        if (m_on_regrouped)
            m_on_regrouped();
    });
}

FavoriteComboList::~FavoriteComboList()
{
    m_alive.reset();
    unsubscribe(m_subscription);
}

bool FavoriteComboList::is_custom() const { return dynamic_cast<::ComboBox *>(m_window) != nullptr; }

void FavoriteComboList::set_entries(std::vector<Entry> entries, int selected_entry)
{
    m_entries = std::move(entries);
    rebuild(selected_entry);
}

void FavoriteComboList::regroup() { rebuild(selected_entry()); }

void FavoriteComboList::rebuild(int selected)
{
    const std::set<std::string> favorites = load();
    std::vector<std::string>    keys;
    keys.reserve(m_entries.size());
    for (const Entry &e : m_entries)
        keys.push_back(e.key);

    m_row_entry.clear();
    auto *items = dynamic_cast<wxItemContainer *>(m_window);
    if (!items)
        return;
    m_window->Freeze();
    if (auto *cb = dynamic_cast<::ComboBox *>(m_window)) {
        // Clear() first: it resets the selection, so the SetSelection() below is never skipped as
        // "unchanged" while the label still shows the previous filament.
        cb->Clear();
        for (const FavoriteFilaments::Row &row : FavoriteFilaments::group(keys, favorites)) {
            if (row.kind == FavoriteFilaments::Row::Item) {
                cb->Append(m_entries[row.index].text, wxNullBitmap);
                m_row_entry.push_back(int(row.index));
            } else {
                const int n = cb->Append(row.kind == FavoriteFilaments::Row::FavoritesHeader ? _L("Favorite filaments") : _L("All filaments"),
                                         wxNullBitmap);
                cb->SetItemDisabled(unsigned(n));
                m_row_entry.push_back(-1);
            }
        }
        update_stars();
    } else {
        // A native list cannot hold a header or a row icon: the favourites first, marked with a star.
        items->Clear();
        wxArrayString shown;
        const wxString star = wxString::FromUTF8(FavoriteFilaments::star_prefix());
        for (const FavoriteFilaments::Row &row : FavoriteFilaments::flat(keys, favorites)) {
            shown.Add(row.in_favorites ? star + m_entries[row.index].text : m_entries[row.index].text);
            m_row_entry.push_back(int(row.index));
        }
        if (!shown.IsEmpty())
            items->Append(shown);
    }
    select_entry(selected);
    m_window->Thaw();
}

void FavoriteComboList::update_stars()
{
    auto *cb = dynamic_cast<::ComboBox *>(m_window);
    if (!cb)
        return;
    const std::set<std::string> favorites = load();
    const wxBitmap              on        = create_scaled_bitmap(into_u8(star_icon_name(true)), m_window, 16);
    const wxBitmap              off       = create_scaled_bitmap(into_u8(star_icon_name(false)), m_window, 16);
    for (size_t row = 0; row < m_row_entry.size() && row < cb->GetCount(); ++row) {
        const int entry = m_row_entry[row];
        if (entry < 0)
            continue;
        const bool fav = favorites.count(m_entries[entry].key) > 0;
        cb->SetItemActionBitmap(unsigned(row), fav ? on : off);
    }
}

void FavoriteComboList::on_action(int row)
{
    const int entry = entry_of_row(row);
    if (entry < 0)
        return;
    toggle(m_entries[entry].key);
    update_stars(); // at once; the other pickers hear about it through fav_notify()
}

int FavoriteComboList::entry_of_row(int row) const
{
    return row >= 0 && row < int(m_row_entry.size()) ? m_row_entry[row] : -1;
}

int FavoriteComboList::selected_entry() const
{
    const auto *items = dynamic_cast<const wxItemContainerImmutable *>(m_window);
    return items ? entry_of_row(items->GetSelection()) : -1;
}

void FavoriteComboList::select_entry(int entry)
{
    int row = -1;
    for (size_t i = 0; entry >= 0 && i < m_row_entry.size(); ++i)
        if (m_row_entry[i] == entry) {
            row = int(i);
            break;
        }
    if (auto *cb = dynamic_cast<::ComboBox *>(m_window)) {
        cb->SetSelection(row);
        if (row < 0)
            cb->SetValue(wxEmptyString);
    } else if (auto *items = dynamic_cast<wxItemContainerImmutable *>(m_window)) {
        items->SetSelection(row < 0 ? wxNOT_FOUND : row);
    }
}

bool FavoriteComboList::toggle_selected()
{
    const int entry = selected_entry();
    if (entry < 0)
        return false;
    return toggle(m_entries[entry].key);
}

bool FavoriteComboList::selected_is_favorite() const
{
    const int entry = selected_entry();
    return entry >= 0 && is_favorite(m_entries[entry].key);
}

}}} // namespace Slic3r::GUI::FavoriteFilamentsUI
