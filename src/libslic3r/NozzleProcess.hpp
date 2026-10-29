#ifndef slic3r_NozzleProcess_hpp_
#define slic3r_NozzleProcess_hpp_

// Per-nozzle process P1 (tests/design_per_nozzle_process.md, owner decisions 2026-10-29).
//
// A printer with several physical extruders (U1 T1-T4, H2D / H2C / X2D left and right) can take the
// speeds, accelerations and jerk of each extruder from its own process preset. The plate's process
// stays the process: layer height, walls, infill, line widths, supports and every other key come from
// it. A chosen per-nozzle process only fills that extruder's slots of per_nozzle_process_options()
// in Bambu's per-extruder slot layout (print_extruder_id / print_extruder_variant, design D1), which
// the slicer reads through process_config_idx().

#include <string>
#include <vector>

namespace Slic3r {

class ConfigBase;
class DynamicPrintConfig;

// The non-empty entries of nozzle_process_presets, or an empty list when it names none.
std::vector<std::string> nozzle_process_preset_names(const ConfigBase &config);
bool has_nozzle_process_presets(const ConfigBase &config);

// Whether the printer can take a process per nozzle: two or more physical extruders and not a single
// nozzle fed by several filaments (single_extruder_multi_material).
bool supports_nozzle_processes(const ConfigBase &printer_config);

// Copy each extruder's chosen process into that extruder's slots of per_nozzle_process_options() in
// `config` (a full print config). sources[e] is extruder e's process, nullptr for the plate's process.
//
// - The layout: the config's own print_extruder_id / print_extruder_variant when it covers exactly the
//   printer's extruders (the Bambu dual processes), else a new one with one slot per extruder and
//   process_flow_support mode ("Direct Drive Standard", "Direct Drive High Flow"; the U1).
// - A slot is filled from its source's value for the slot's flow mode (process_variant_fill_mode, D6):
//   the source's own slot of that extruder when the source has the same kind of layout, else its
//   process_flow_support column of that mode, else its first value.
// - Extruders without a chosen process keep printing what they printed before: the plate's flow-mode
//   column, unless the plate process was split per nozzle (process_extruder_values), whose slots are
//   kept as they are. Extruder 1's slots are kept either way, so filling twice changes nothing.
// - Sets process_extruder_values when the result really differs per extruder (so the slicer reads the
//   slots), leaves it as the plate had it otherwise.
// Returns false (config untouched) when no extruder has a source or the printer has fewer than two
// extruders.
bool fill_nozzle_process_slots(DynamicPrintConfig &config, const std::vector<const DynamicPrintConfig *> &sources);

// The values a filament on extruder `extruder` reads for `key` in a filled config: what the G-code of
// that extruder will use. Test and UI helper.
std::string nozzle_process_value(const DynamicPrintConfig &config, const std::string &key, size_t extruder, bool high_flow);

} // namespace Slic3r

#endif // slic3r_NozzleProcess_hpp_
