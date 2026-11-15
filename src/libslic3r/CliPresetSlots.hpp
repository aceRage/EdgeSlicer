#pragma once
#ifndef slic3r_CliPresetSlots_hpp_
#define slic3r_CliPresetSlots_hpp_

// CLI bookkeeping for the per-preset string vectors (different_settings_to_system,
// inherits_group): slot 0 is the process, slots 1..N the filaments, slot N+1 the printer.
// Kept out of Snapmaker_Orca.cpp so Catch2 can test it.

#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r {

class DynamicPrintConfig;

// Number of filament slots the CLI fills: the project's own count, grown to the number of
// --load-filaments / --filament-presets passed. A project with 2 filaments sliced with 4
// presets has 4 filaments; using 2 wrote past the end of the vectors above.
int cli_filament_slot_count(int project_filament_count, int loaded_filament_count);

// Resizes a process/filaments/printer vector to filament_count + 2. When it grows, the old
// last entry (the printer) moves to the new last slot and the new filament slots are empty.
void resize_cli_preset_slots(std::vector<std::string> &slots, int filament_count);

// Filament presets given to the CLI (--filament-presets, --load-filaments) written into a project's config, one value
// per filament as the application reads them.
//
// The application keeps every flow variant of a filament's flow-variant settings side by side
// (filament_flow_step_size entries per filament) and reads the one of the filament's flow type
// (filament_volume_type) through get_config_idx. The CLI writes one value per filament instead, so it first makes the
// project's own lists one per filament: each filament's flow-variant lists hold the entry it reads, its
// filament_flow_support entry the variant that is, and filament_flow_step_size is 1 for each. Read through
// get_config_idx, every filament reads what it read before.
void cli_flatten_filament_flow_variants(DynamicPrintConfig &config, size_t filament_count);

// Writes `key` of a filament preset into filament `filament_idx` (0-based) of `config`, made one value per filament by
// cli_flatten_filament_flow_variants(): for a flow-variant setting (and filament_flow_support) the preset's entry for
// the filament's flow type (filament_volume_type of `config`; the preset's first entry when it has none of that type,
// and for a nil there over a set first entry), for any other list its first entry. The list is cut to
// filament_count, dropping what a longer project list had past it.
void cli_set_filament_preset_value(DynamicPrintConfig &config, const std::string &key, const DynamicPrintConfig &filament,
                                   size_t filament_idx, size_t filament_count);

} // namespace Slic3r

#endif // slic3r_CliPresetSlots_hpp_
