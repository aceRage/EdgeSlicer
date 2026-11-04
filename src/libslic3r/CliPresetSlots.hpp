#pragma once
#ifndef slic3r_CliPresetSlots_hpp_
#define slic3r_CliPresetSlots_hpp_

// CLI bookkeeping for the per-preset string vectors (different_settings_to_system,
// inherits_group): slot 0 is the process, slots 1..N the filaments, slot N+1 the printer.
// Kept out of Snapmaker_Orca.cpp so Catch2 can test it.

#include <string>
#include <vector>

namespace Slic3r {

// Number of filament slots the CLI fills: the project's own count, grown to the number of
// --load-filaments / --filament-presets passed. A project with 2 filaments sliced with 4
// presets has 4 filaments; using 2 wrote past the end of the vectors above.
int cli_filament_slot_count(int project_filament_count, int loaded_filament_count);

// Resizes a process/filaments/printer vector to filament_count + 2. When it grows, the old
// last entry (the printer) moves to the new last slot and the new filament slots are empty.
void resize_cli_preset_slots(std::vector<std::string> &slots, int filament_count);

} // namespace Slic3r

#endif // slic3r_CliPresetSlots_hpp_
