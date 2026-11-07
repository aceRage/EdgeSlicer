// The rows behind the Filament manager and the Process manager (resources/web/guide/preset_manager,
// src/slic3r/GUI/PresetManagerDialog). Design: tests/design_preset_manager_tables.md.
//
// Everything that does not need a window lives here so it can be unit tested:
//   * collect_preset_manager_items(): one PresetManagerItem per listed preset of the loaded bundle, with
//     the printers it is compatible with (evaluated once per open, conditions cached per printer);
//   * build_preset_manager_model(): the rows. System filaments are grouped one row per vendor bundle +
//     alias (the unit the [filaments] section and the favourite star use); user filaments and every
//     process preset are one row each. Project-embedded and default presets are not listed;
//   * preset_manager_rows_json(): what the page gets;
//   * the "Enabled" storage edits: [filaments] for system filaments, the hidden process set
//     (AppConfig::SECTION_HIDDEN_PROCESSES) for system process presets. User presets are always shown.
#pragma once

#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Preset.hpp"

namespace Slic3r {

class PresetBundle;

enum class PresetManagerKind { Filament, Process };

// One listed preset. Numbers are NaN when unknown.
struct PresetManagerItem
{
    std::string              name;            // full preset name
    std::string              alias;           // system presets: the alias (name without " @printer")
    std::string              vendor_id;       // system presets: the vendor bundle id; empty for user presets
    std::string              version;         // system presets: the vendor bundle version
    bool                     is_system = false;
    bool                     visible   = true; // Preset::is_visible
    bool                     in_use    = false; // a filament slot of the project / the plate's process
    std::vector<std::string> renamed_from;
    std::string              favorite_key;    // FavoriteFilaments::key_for(): alias for system, full name for user
    // filaments
    std::string              filament_vendor; // filament_vendor[0]
    std::string              type;            // filament_type[0]
    std::string              filament_id;
    double                   nozzle_t  = std::numeric_limits<double>::quiet_NaN();
    double                   nozzle_lo = std::numeric_limits<double>::quiet_NaN();
    double                   nozzle_hi = std::numeric_limits<double>::quiet_NaN();
    double                   bed_t     = std::numeric_limits<double>::quiet_NaN();
    // processes
    double                   layer_height = std::numeric_limits<double>::quiet_NaN();
    std::string              inherits;        // user presets: the parent preset name, empty when detached
    std::vector<std::string> nozzles;         // nozzle diameters of the compatible printers ("0.4"), sorted
    // both
    std::int64_t             mtime = 0;       // user presets: last write time of the file, Unix s; 0 unknown
    std::vector<std::string> printers;        // compatible listed printers, sorted
    bool                     all_printers = false; // compatible with every listed printer
};

struct PresetManagerRow
{
    std::string              key;             // unique: "s|<vendor>|<alias>" (system filament group), "s|<name>", "u|<name>"
    std::string              favorite_key;
    std::string              name;            // shown: the alias for system presets, the full name otherwise
    std::vector<std::string> members;         // full preset names the row stands for (a system filament group: several)
    std::vector<std::string> old_names;       // their renamed_from names (removed with them when unticked)
    std::string              vendor;          // filaments: filament_vendor; processes: empty
    std::string              type;
    std::string              filament_id;
    bool                     is_system = false;
    std::string              version;
    std::vector<std::string> printers;
    bool                     all_printers = false;
    std::vector<std::string> nozzles;
    double                   nozzle_t  = std::numeric_limits<double>::quiet_NaN();
    double                   nozzle_lo = std::numeric_limits<double>::quiet_NaN();
    double                   nozzle_hi = std::numeric_limits<double>::quiet_NaN();
    double                   bed_t     = std::numeric_limits<double>::quiet_NaN();
    double                   layer_height = std::numeric_limits<double>::quiet_NaN();
    std::string              tier;            // processes: "Standard", "Extra Fine"...; user presets: the parent, or "Detached"
    std::string              inherits;
    std::int64_t             mtime = 0;
    bool                     enabled = true;  // system: any member visible; user: always
    bool                     in_use  = false; // any member in use
    // Why the Enabled box cannot change: "user" (user presets are always shown), "in_use" (shown and in
    // use in this project), empty when it can.
    std::string              lock;
    // User presets (rename, phase 2): the name split as the rename field shows it (filaments: the part
    // before " @" and the " @printer" suffix kept as it is; processes: the whole name, no suffix), and
    // the user filament presets renamed with it by default (same filament_id and base name, itself
    // included; one entry when it has no family).
    std::string              base;
    std::string              suffix;
    std::vector<std::string> family;
};

struct PresetManagerModel
{
    PresetManagerKind             kind = PresetManagerKind::Filament;
    std::vector<PresetManagerRow> rows;     // system rows first (by vendor, name), then user rows (by name)
    std::vector<std::string>      printers; // every printer some row lists, sorted
    const PresetManagerRow *find(const std::string &key) const;
};

// The quality word of a system process alias: "0.20mm Standard" -> "Standard", "0.08mm Extra Fine" ->
// "Extra Fine", "0.20mm Strength" -> "Strength"; empty when the alias does not start with a layer height.
std::string process_quality_tier(const std::string &alias);

// The favourite key rule of FavoriteFilaments::key_for (GUI-free copy): a system preset's alias, or its
// name up to " @" without one; any other preset its full name.
std::string preset_manager_favorite_key(const std::string &name, const std::string &alias, bool is_system);

PresetManagerModel build_preset_manager_model(PresetManagerKind kind, const std::vector<PresetManagerItem> &items);

// The listed presets of the bundle: system ones (not of the "Template" vendor) and user ones; not the
// defaults, not project-embedded ones. Printers are the visible, non-default printer presets.
// bed_temp_key: the filament option holding the bed temperature of the current plate's bed type
// (get_bed_temp_key()), empty to leave the bed column unknown.
std::vector<PresetManagerItem> collect_preset_manager_items(const PresetBundle &bundle, PresetManagerKind kind, const std::string &bed_temp_key);

// What the page gets:
//   { kind, rows: [ { key, fav, name, members, vendor, type, source: "System"|"User", filament_id, version,
//                     printers, all, nozzles, nozzle_t, nozzle_lo, nozzle_hi, bed_t, lh, tier, inherits,
//                     mtime, enabled, lock, in_use, star, can_rename, can_delete, base, suffix, family } ],
//     printers, current_printer }
// Unknown numbers are null. star = favorite_key in `favorites`.
nlohmann::json preset_manager_rows_json(const PresetManagerModel &model, const std::set<std::string> &favorites, const std::string &current_printer);

// "My PLA @BBL X1C" -> "My PLA" + " @BBL X1C" (the first " @"); a name without one is all base.
void split_preset_suffix(const std::string &name, std::string &base, std::string &suffix);

// A rename the preset managers asked for: the names that change (old -> new, the row's first) and
// whether the new names pass validate_preset_name (PresetNameUse::Rename) plus the rename rules:
// the edited part of a filament " @printer" name may not contain "@" (the printer part is kept) and
// may not be empty or start / end with a space.
struct PresetRenamePlan
{
    PresetNameCheck                                  check;
    std::string                                      checked_name; // the new name the check is about (the one that failed)
    std::vector<std::pair<std::string, std::string>> renames;      // empty when nothing changes or the check failed
};
// `name`: the row's preset; `new_base`: what the user typed (the base for a filament with a suffix, the
// whole name otherwise); `family`: the presets renamed with it (each keeps its own suffix; `name` is
// added when missing). Process presets: pass an empty family.
PresetRenamePlan plan_preset_rename(const PresetCollection &presets, const std::string &name, const std::string &new_base,
                                    const std::vector<std::string> &family);

// [filaments] with the row's presets enabled (members added) or not (members and their old names removed).
std::map<std::string, std::string> filament_section_with(std::map<std::string, std::string> section, const PresetManagerRow &row, bool enabled);
// The hidden process presets with the row's presets hidden (enabled = false) or shown (removed, with old names).
std::map<std::string, std::string> hidden_process_section_with(std::map<std::string, std::string> section, const PresetManagerRow &row, bool enabled);

} // namespace Slic3r
