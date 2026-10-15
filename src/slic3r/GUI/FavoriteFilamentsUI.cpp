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

std::map<int, std::function<void()>> &subscribers()
{
    static std::map<int, std::function<void()>> subs;
    return subs;
}

std::set<std::string> read_config()
{
    std::set<std::string> keys;
    AppConfig *config = wxGetApp().app_config;
    if (!config)
        return keys;
    const std::string serialized = config->get(FavoriteFilaments::config_key());
    std::vector<std::string> values;
    if (!serialized.empty() && unescape_strings_cstyle(serialized, values))
        for (const std::string &v : values)
            if (!v.empty())
                keys.insert(v);
    return keys;
}

void write_config(const std::set<std::string> &keys)
{
    AppConfig *config = wxGetApp().app_config;
    if (!config)
        return;
    config->set(FavoriteFilaments::config_key(), escape_strings_cstyle(std::vector<std::string>(keys.begin(), keys.end())));
    config->save();
}

// Once per session, after the system presets are loaded: a favourite whose system preset a vendor
// renamed (renamed_from in the new profile) moves to the new name.
void migrate_once(std::set<std::string> &keys)
{
    static bool done = false;
    if (done || keys.empty())
        return;
    PresetBundle *bundle = wxGetApp().preset_bundle;
    if (!bundle || bundle->filaments.size() <= bundle->filaments.num_default_presets())
        return; // not loaded yet; try again on the next read
    done = true;

    std::set<std::string>                            current;
    std::vector<std::pair<std::string, std::string>> renamed;
    for (const Preset &preset : bundle->filaments.get_presets()) {
        const std::string key = key_for(preset);
        if (!key.empty())
            current.insert(key);
        for (const std::string &old_name : preset.renamed_from)
            renamed.emplace_back(key, old_name);
    }
    if (FavoriteFilaments::migrate(keys, current, FavoriteFilaments::rename_map(renamed)))
        write_config(keys);
}

void notify()
{
    // After the event that changed the set has finished: a picker may rebuild its own list.
    wxGetApp().CallAfter([]() {
        // Copy: a subscriber may unsubscribe (a dialog closing) while we iterate.
        const auto subs = subscribers();
        for (const auto &[id, fn] : subs)
            if (subscribers().count(id) > 0 && fn)
                fn();
    });
}

} // namespace

std::string key_for(const Preset &preset)
{
    return FavoriteFilaments::key_for(preset.name, preset.alias, preset.is_system);
}

std::set<std::string> load()
{
    std::set<std::string> keys = read_config();
    migrate_once(keys);
    return keys;
}

bool is_favorite(const std::string &key) { return !key.empty() && load().count(key) > 0; }

bool is_favorite(const Preset &preset) { return is_favorite(key_for(preset)); }

bool toggle(const std::string &key)
{
    if (key.empty())
        return false;
    std::set<std::string> keys  = load();
    const bool            state = FavoriteFilaments::toggle(keys, key);
    write_config(keys);
    notify();
    return state;
}

int subscribe(std::function<void()> on_changed)
{
    static int next_id = 1;
    const int  id      = next_id++;
    subscribers().emplace(id, std::move(on_changed));
    return id;
}

void unsubscribe(int id) { subscribers().erase(id); }

int append_toggle_item(wxMenu &menu, const std::string &key)
{
    if (key.empty())
        return wxID_NONE;
    if (menu.GetMenuItemCount() > 0)
        menu.AppendSeparator();
    const int       id   = wxWindow::NewControlId();
    const bool      fav  = is_favorite(key);
    const wxString  name = from_u8(key);
    // TRN %s is a filament name, e.g. "Bambu PLA Basic"
    menu.Append(id, fav ? wxString::Format(_L("Remove \"%s\" from favorite filaments"), name)
                        : wxString::Format(_L("Add \"%s\" to favorite filaments"), name));
    menu.Bind(wxEVT_MENU, [key](wxCommandEvent &) { toggle(key); }, id);
    return id;
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
    update_stars(); // at once; the other pickers hear about it through notify()
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
