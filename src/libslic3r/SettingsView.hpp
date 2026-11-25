#ifndef slic3r_SettingsView_hpp_
#define slic3r_SettingsView_hpp_

// Settings views: which option lines the settings tabs show (tests/design_view_modes.md).
//
// The level (ConfigOptionMode, AppConfig "user_mode": simple / advanced / develop) stays what it always
// was. The "Bambu" view is an orthogonal key filter (AppConfig "settings_view" = "bambu"): it shows the
// options Bambu Studio shows with its Advanced switch on (Format/BambuUiKeys.cpp), on EdgeSlicer's own
// pages, plus any option outside that list whose value differs from the system preset it inherits
// from ("changed"), or that the user revealed from search. Developer mode wins over the view.
//
// This file is GUI-free: the GUI asks line_show() per option line and keeps the "changed" and
// "revealed" sets; the tests exercise every rule here.

#include "Config.hpp"

#include <set>
#include <string>
#include <vector>

namespace Slic3r {

class PresetCollection;
class Preset;

enum class SettingsView { Default, Bambu };

// AppConfig "settings_view": "bambu" -> Bambu, anything else (absent, empty, unknown) -> Default.
SettingsView settings_view_from_string(const std::string &value);
// Default -> "" (the key is removed / left absent), Bambu -> "bambu".
std::string  settings_view_to_string(SettingsView view);

struct ViewFilter
{
    ConfigOptionMode level       = comSimple;             // user_mode
    SettingsView     view        = SettingsView::Default; // settings_view
    bool             bbl_printer = false;                 // current printer is a Bambu Lab printer (design D15)
    // Base option keys (no "#index") shown in the Bambu view although Bambu Studio does not show them:
    const std::set<std::string> *changed  = nullptr; // differ from the system preset / overridden by the object
    const std::set<std::string> *revealed = nullptr; // revealed from search in this session

    // The Bambu filter is in force: Bambu view and not Developer mode.
    bool bambu() const { return view == SettingsView::Bambu && level != comDevelop; }
    // Same filter decision for every key (used to skip work when nothing changed).
    bool operator==(const ViewFilter &rhs) const
    {
        return level == rhs.level && view == rhs.view && bbl_printer == rhs.bbl_printer && changed == rhs.changed &&
               revealed == rhs.revealed;
    }
    bool operator!=(const ViewFilter &rhs) const { return !(*this == rhs); }
};

enum class LineShow {
    Hidden,
    Shown,
    // Shown in the Bambu view although Bambu Studio does not show it (changed or revealed): the line
    // gets the "Not in Bambu Studio" chip.
    ShownNotInBambu,
};

namespace SettingsViewKeys {
// "wall_loops#1" -> "wall_loops".
std::string base_key(const std::string &opt_id);
// Our key is on Bambu Studio's Simple + Advanced list, under its own name, a BambuKeyAliases rename or
// an inferred equivalent.
bool on_bambu_list(const std::string &our_key);
// Bambu Studio hides this line on every Bambu Lab printer (the seven *_jerk lines at 02.09.01.70).
bool hidden_by_bambu_on_bbl(const std::string &our_key);
// Number of our keys on the Bambu list (for tests and logs).
size_t bambu_list_size();
} // namespace SettingsViewKeys

// Visibility of one option line before the per-printer / per-value toggles (`toggle_visible`, which the
// caller ANDs in). option_ids: the line's option ids ("key" or "key#index"); line_mode: the line's mode
// (the mode of its first option, as today). A line with no option (a widget-only row) is decided by the
// group: see widget_line_show().
LineShow line_show(const ViewFilter &filter, const std::vector<std::string> &option_ids, ConfigOptionMode line_mode);
// A widget-only line: shown in every level as today; in the Bambu view only while its group shows at
// least one option line, so a widget does not survive alone in an emptied group.
LineShow widget_line_show(const ViewFilter &filter, bool group_has_visible_option_line);
inline bool shown(LineShow s) { return s != LineShow::Hidden; }

// ---------------------------------------------------------------------------------------------
// "Changed" (design rule A, fallback B): the keys of the edited preset whose value differs from the
// system preset it inherits from. A root user preset (no "inherits": its own base) and an external
// preset with no parent have no system preset: they are compared with the option defaults instead.
// ---------------------------------------------------------------------------------------------

// The preset the edited one is compared with, or nullptr when the option defaults are used.
const Preset *settings_view_reference(const PresetCollection &presets);
// Base keys of the edited preset that the Bambu view shows as changed.
std::set<std::string> settings_view_changed_options(const PresetCollection &presets, bool deep_compare = false);
// Base keys of `config` whose value differs from the option definition default (vectors compared
// element-wise against the default's first value, whatever their length).
std::set<std::string> options_differing_from_defaults(const ConfigBase &config);
// Object / part / layer-range settings: every key the item carries is an override by definition.
void add_override_keys(const ConfigBase &item_config, std::set<std::string> &out);

} // namespace Slic3r

#endif // slic3r_SettingsView_hpp_
