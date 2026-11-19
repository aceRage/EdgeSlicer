#pragma once

// "Use Bambu defaults" on the process Multimaterial page's Prime tower group: which process keys the button
// sets, where their values come from and which of them would change. No GUI and no user-visible strings here,
// so the rules can be unit tested (tests/libslic3r/test_prime_tower_defaults.cpp).
// Design: tests/design_prime_tower_menu_rework.md section 5 (owner, 2026-11-16: only the button is built).

#include "PrintConfig.hpp"

#include <string>
#include <vector>

namespace Slic3r {
namespace PrimeTowerDefaults {

// The process keys the button sets, in the order the Prime tower group shows them (the confirm dialog lists the
// changes in this order). Process keys only: no filament, printer or project key is ever in this list.
const std::vector<std::string>& bambu_default_tower_keys();

// Tower process keys the button deliberately leaves alone:
// enable_prime_tower (the button never switches a tower on or off), the Develop-mode brim chamfer pair (Edge-only
// shape options), wipe_tower_filament (the "Filament for Features" group) and local_z_wipe_tower_purge_lines
// (no row anywhere). Every process key that names the tower is in exactly one of the two lists (unit test).
const std::vector<std::string>& excluded_tower_keys();

// Whether the row with the button is shown: on a Bambu Lab printer, while the prime tower is on.
bool offered(bool is_bbl_printer, bool prime_tower_enabled);

// The values the button applies, for the keys of bambu_default_tower_keys() only:
// the system process preset's value (`system_process`), or the code default for a key that preset lacks,
// then Bambu Studio's one code default that Edge's system presets do not reproduce: the rib wall
// (Bambu prime_tower_rib_wall = true, Edge wipe_tower_wall_type = Rib).
DynamicPrintConfig bambu_tower_defaults(const DynamicPrintConfig& system_process);

// The keys of bambu_default_tower_keys() whose value in `current` differs from `defaults`, in list order.
// Keys missing from either config are skipped. Empty when the tower already uses the defaults.
std::vector<std::string> changed_keys(const DynamicPrintConfig& current, const DynamicPrintConfig& defaults);

// Only the changed keys of `defaults`: what Tab::load_config() is given, so nothing outside the list is touched.
DynamicPrintConfig changes_to_apply(const DynamicPrintConfig& current, const DynamicPrintConfig& defaults);

} // namespace PrimeTowerDefaults
} // namespace Slic3r
