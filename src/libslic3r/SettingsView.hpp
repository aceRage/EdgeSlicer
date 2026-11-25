#ifndef slic3r_SettingsView_hpp_
#define slic3r_SettingsView_hpp_

// Settings views: which option lines the settings tabs show (tests/design_view_modes.md).
//
// The level (ConfigOptionMode) is what it always was: Simple for Basic, Advanced for Advanced, and
// Develop when Preferences > Developer mode is on (Advanced or Bambu view). The "Bambu" view is an
// orthogonal key filter (AppConfig "settings_view" = "bambu"): it shows the options Bambu Studio shows
// with its Advanced switch on (Format/BambuUiKeys.cpp), on EdgeSlicer's own pages, plus any option
// outside that list whose value differs from the system preset it inherits from ("changed"), or that
// the user revealed from search. The view applies in Developer mode too (owner, 2026-11-25): Developer
// mode only raises the level, so our Developer lines show in Advanced, and in Bambu only as exceptions.
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

// The selector in the sidebar and tab headers (Basic | Advanced | Bambu). Preferences > Developer mode
// (AppConfig "developer_mode") is a separate flag on top of the chosen view (owner, 2026-11-25): it
// never changes the view and never disables the selector.
enum class ViewChoice { Basic, Advanced, Bambu };
// What the selector shows for AppConfig "user_mode" / "settings_view" (either may be absent: "").
// settings_view bambu -> Bambu; user_mode advanced -> Advanced; the legacy user_mode "develop" (written
// by builds before this change) -> Advanced; else Basic (absent or unknown reads as simple).
ViewChoice   view_choice(const std::string &user_mode, const std::string &settings_view);
// What choosing a segment writes. settings_view empty = remove the key. Bambu is Bambu Studio's Advanced
// list, so it writes user_mode "advanced" (design D2): an older build without views shows Advanced.
struct ViewChoiceConfig
{
    std::string user_mode;
    std::string settings_view;
};
ViewChoiceConfig view_choice_config(ViewChoice choice);
// The level the settings show (GUI_App::get_mode()): Basic -> Simple; Advanced and Bambu -> Advanced,
// or Develop with Developer mode on (the Bambu filter then still applies, see line_show()).
ConfigOptionMode effective_level(ViewChoice choice, bool developer_mode);
// Migration of builds where Developer mode wrote user_mode "develop": becomes user_mode "advanced"
// with developer_mode on. Returns true when it changed something.
bool         migrate_legacy_developer_user_mode(std::string &user_mode, bool &developer_mode);

struct ViewFilter
{
    ConfigOptionMode level       = comSimple;             // effective level (user_mode + Developer mode)
    SettingsView     view        = SettingsView::Default; // settings_view
    bool             bbl_printer = false;                 // current printer is a Bambu Lab printer (design D15)
    // Base option keys (no "#index") shown in the Bambu view although Bambu Studio does not show them:
    const std::set<std::string> *changed  = nullptr; // differ from the system preset / overridden by the object
    const std::set<std::string> *revealed = nullptr; // revealed from search in this session

    // The Bambu filter is in force (in Developer mode too).
    bool bambu() const { return view == SettingsView::Bambu; }
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

// Whether an option group shows anything, for the pages that are not on screen (the page tree only asks
// this; ConfigOptionsGroup::is_visible). One entry per option line: its mode and its option ids. As
// today, an empty group is visible and the per-printer toggles are not consulted here.
bool group_visible(const ViewFilter &filter, const std::vector<ConfigOptionMode> &line_modes,
                   const std::vector<std::vector<std::string>> &line_ids);

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
