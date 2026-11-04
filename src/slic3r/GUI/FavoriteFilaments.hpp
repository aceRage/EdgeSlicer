#pragma once

// Favourite filaments: the rules every filament picker shares, GUI-free so they can be unit-tested.
//
// A favourite is stored as a filament *key*. For a system preset that is its alias: the name without
// the printer suffix (" @BBL X1C", " @U1 0.4 nozzle"), so one star marks "Bambu PLA Basic" on every
// printer and in every picker, whether the picker lists aliases (sidebar, AMS slot dialog) or full
// names (Filament settings tab). A user preset is keyed by its full name: users name their own presets
// freely, and "Bambu PLA Basic @BBL X1C(1)" must not be starred because the system one is.
//
// The set is one per-user list in the app config, local only (never synced). It survives a system
// preset being renamed: migrate() follows the renamed_from names a vendor ships with the new preset.
//
// Originally OrcaSlicer #13946 (AMS dialog only, keyed by filament_id); extended here to every picker.
//
// These rules are not filament-specific: favourite process (print) presets use the same keys, toggle,
// migration and grouping, each kind with its own stored set (FavoritePresets.hpp).

#include "FavoritePresets.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace FavoriteFilaments {

// The app config key ("app" section) holding the escaped list of filament keys (the other kinds'
// keys are in FavoritePresets::config_key).
inline const char *config_key() { return FavoritePresets::config_key(FavoritePresets::Kind::Filament); }

// The printer-independent part of a filament preset name: everything before the first " @".
// "Bambu PLA Basic @BBL X1C" -> "Bambu PLA Basic"; "My PLA" -> "My PLA". Trailing spaces go too.
inline std::string strip_printer_suffix(const std::string &name)
{
    std::string key = name;
    const size_t at = key.find(" @");
    if (at != std::string::npos)
        key.erase(at);
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
        key.pop_back();
    return key;
}

// The key of a filament preset: a system preset's alias (or, without one, its name less the printer
// suffix); any other preset (user, project, default) its full name. A "(modified)" mark on a shown
// name must be cleaned by the caller.
inline std::string key_for(const std::string &name, const std::string &alias, bool is_system)
{
    if (!is_system)
        return name;
    return alias.empty() ? strip_printer_suffix(name) : alias;
}

// Old key -> new key for every system filament preset that lists renamed_from names. Each pair is
// (current preset name or alias, one of its renamed_from names); both sides are reduced to keys, and a
// rename that keeps the key (only the printer suffix changed) is dropped.
inline std::map<std::string, std::string> rename_map(const std::vector<std::pair<std::string, std::string>> &current_and_old)
{
    std::map<std::string, std::string> renames;
    for (const auto &[current, old] : current_and_old) {
        const std::string new_key = strip_printer_suffix(current);
        const std::string old_key = strip_printer_suffix(old);
        if (!new_key.empty() && !old_key.empty() && new_key != old_key)
            renames.emplace(old_key, new_key); // the first claim wins, as find_preset_renamed does
    }
    return renames;
}

// Follows renames: a stored key that no listed filament carries any more, but that a vendor renamed,
// becomes the new key. Keys still in use stay (a rename can leave other printers on the old name), and
// keys nobody knows stay too (the preset may be on another printer, or come back after a reinstall).
// Returns true when anything changed, so the caller can save.
inline bool migrate(std::set<std::string> &keys, const std::set<std::string> &current_keys,
                    const std::map<std::string, std::string> &renames)
{
    bool changed = false;
    std::set<std::string> out;
    for (const std::string &key : keys) {
        auto it = renames.find(key);
        // Follow a chain of renames (A -> B -> C), bounded so a cycle in bad data cannot hang.
        std::string target = key;
        for (int hop = 0; it != renames.end() && hop < 8 && current_keys.count(target) == 0; ++hop) {
            target = it->second;
            it     = renames.find(target);
        }
        if (target != key && current_keys.count(key) == 0 && current_keys.count(target) > 0) {
            out.insert(target);
            changed = true;
        } else {
            out.insert(key);
        }
    }
    keys.swap(out);
    return changed;
}

// Adds the key when it is missing and removes it when present. Returns the new state.
inline bool toggle(std::set<std::string> &keys, const std::string &key)
{
    if (key.empty())
        return false;
    auto it = keys.find(key);
    if (it != keys.end()) {
        keys.erase(it);
        return false;
    }
    keys.insert(key);
    return true;
}

// One row of a filament list grouped with the favourites on top.
struct Row
{
    enum Kind { FavoritesHeader, AllHeader, Item };
    Kind   kind{Item};
    size_t index{0};      // for Item: the position in the input list
    bool   favorite{false}; // for Item: this filament is a favourite (wherever the row is)
    bool   in_favorites{false}; // for Item: the row is the copy in the favourites group
};

// Lays out a list whose items have the given keys (in the order they are to be shown): when any is a
// favourite, a favourites header and those items (same relative order) come first, then an "all"
// header and every item again. With no favourites the list is unchanged and has no headers, so a
// user who never stars anything sees exactly the old list. Duplicate keys (one filament listed for
// two printers) are each repeated, as the caller listed them.
inline std::vector<Row> group(const std::vector<std::string> &item_keys, const std::set<std::string> &favorites,
                              bool with_all_header = true)
{
    std::vector<Row> rows;
    std::vector<size_t> fav;
    for (size_t i = 0; i < item_keys.size(); ++i)
        if (favorites.count(item_keys[i]) > 0)
            fav.push_back(i);
    if (!fav.empty()) {
        rows.push_back(Row{Row::FavoritesHeader, 0, false, false});
        for (size_t i : fav)
            rows.push_back(Row{Row::Item, i, true, true});
        if (with_all_header)
            rows.push_back(Row{Row::AllHeader, 0, false, false});
    }
    for (size_t i = 0; i < item_keys.size(); ++i)
        rows.push_back(Row{Row::Item, i, favorites.count(item_keys[i]) > 0, false});
    return rows;
}

// The rows of a flat list (no headers possible, e.g. a native macOS combo box): the favourites first,
// each marked with a star prefix by the caller, then every item. Same as group() without headers.
inline std::vector<Row> flat(const std::vector<std::string> &item_keys, const std::set<std::string> &favorites)
{
    std::vector<Row> rows;
    for (const Row &r : group(item_keys, favorites, false))
        if (r.kind == Row::Item)
            rows.push_back(r);
    return rows;
}

// The star prefix a flat list shows on its favourites (U+2605 BLACK STAR and a space), and its removal from a shown name.
inline const std::string &star_prefix()
{
    static const std::string prefix = "\xE2\x98\x85 "; // U+2605 BLACK STAR + space, UTF-8
    return prefix;
}
inline std::string strip_star(const std::string &shown)
{
    const std::string &p = star_prefix();
    return shown.compare(0, p.size(), p) == 0 ? shown.substr(p.size()) : shown;
}

}}} // namespace Slic3r::GUI::FavoriteFilaments
