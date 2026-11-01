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

#include "PrintConfig.hpp"

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

// Per-nozzle process P2: whether filaments can be put on a nozzle of the user's choosing
// (filament_nozzle_assignment): a toolchanger (supports_nozzle_processes()) that is not one of Bambu's
// grouping machines (H2D / H2C / X2D route filaments with filament_map and their own grouping dialog).
bool supports_filament_nozzle_assignment(const DynamicPrintConfig &printer_config);
// The stored assignment for `filaments` filaments, 0 = the filament's own nozzle; an all-0 list is stored empty.
std::vector<int> filament_nozzle_assignment_stored(std::vector<int> assignment);

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

// ---- Per-nozzle process P2: line widths per nozzle (design D8) ----
//
// FOrca 0b1f80cd22's technique (AGPL-3.0): resolve the width each nozzle's process gives a role, then write it
// into the region / object scalar the engine already reads, so no reader changes and a job without
// per-nozzle processes is untouched. The values travel per FILAMENT in the nozzle_*_line_width arrays of the
// region and object configs (index filament - 1).

// (array key, the role width key it stands for), the support one last.
const std::vector<std::pair<std::string, std::string>> &nozzle_line_width_keys();
// The 0-based nozzle 1-based `filament_id` prints on (physical_extruder_for_filament on a full config,
// filament_nozzle_assignment included).
size_t nozzle_for_filament(const ConfigBase &config, unsigned int filament_id);
// Writes the nozzle_*_line_width arrays of a full config: for each filament whose nozzle has a source process,
// that process's role width (its line_width when the role's is 0); every other filament the plate's width. All
// arrays are left empty when no filament's nozzle has a source. Called by fill_nozzle_process_slots(). Returns whether any entry was set.
bool fill_nozzle_line_widths(DynamicPrintConfig &config, const std::vector<const DynamicPrintConfig *> &sources);
// Layer-height guard (design D9, FOrca 0b1f80cd22's Tab rule, AGPL-3.0): one global layer height, held to the
// [min_layer_height, max_layer_height] of the SMALLEST nozzle that prints (`used_nozzles`, 0-based), not to the
// loosest limits of all nozzles. With uniform nozzles or no used nozzle known: the old rule, the smallest
// minimum and the largest maximum of all extruders.
void layer_height_limits_for_nozzles(const ConfigBase &printer_config, const std::vector<size_t> &used_nozzles, double &min_layer_height,
                                     double &max_layer_height);
// Stamps the arrays into the role widths, each through the filament that prints the role (PrintRegion::extruder).
void stamp_nozzle_line_widths(PrintRegionConfig &config);
void stamp_nozzle_line_widths(PrintObjectConfig &config);

// The values a filament on extruder `extruder` reads for `key` in a filled config: what the G-code of
// that extruder will use. Test and UI helper.
std::string nozzle_process_value(const DynamicPrintConfig &config, const std::string &key, size_t extruder, bool high_flow);

// ---- One process with separate values per nozzle (the Speed page's Left / Right editing) ----

// Whether `process`'s nozzles can be edited separately on a printer with `extruders` extruders: two or more
// (P2). A process whose slot layout does not cover exactly those extruders gets one when it is split.
bool process_nozzles_splittable(const ConfigBase &process, size_t extruders);

// The Speed page's "Edit nozzles separately" / "Link nozzles" button (P2: testable enable rule).
enum class NozzleSplitControl {
    Hidden,     // fewer than two nozzles
    Split,      // enabled: "Edit nozzles separately"
    Link,       // enabled: "Link nozzles" (the process is split)
    OwnProcess, // disabled: the shown nozzle takes its speeds from its own process ("Speeds from")
};
NozzleSplitControl nozzle_split_control(size_t extruders, bool nozzle_has_own_process, bool split);
// Whether the slicer reads `process`'s extruder slots (process_extruder_values).
bool process_nozzles_split(const ConfigBase &process);
// The slot of `key` holding extruder `extruder`'s value for flow mode `mode` in a split process, or -1 when
// the process is not split or `key` is not a per-nozzle key of the layout's length.
int process_nozzle_slot(const ConfigBase &process, const std::string &key, size_t extruder, FilamentVolumeType mode);
// "Edit nozzles separately": spread every per-nozzle key over the layout (a vector a user preset cut short
// included), give every nozzle's Standard / High Flow slots the values it prints with now (the flow-mode
// column) and set process_extruder_values. Returns false (process untouched) when not splittable.
bool split_process_per_nozzle(DynamicPrintConfig &process, size_t extruders);
// "Apply to all nozzles": copy extruder `from`'s Standard / High Flow slots to every other extruder.
// Returns true when a value changed.
bool copy_nozzle_slots_to_all(DynamicPrintConfig &process, size_t from);
// "Link nozzles": every nozzle takes extruder `from`'s values and process_extruder_values is cleared, so
// the process is edited and sliced with one set of values again. A process split and linked back with
// unchanged values is the process it was (stock / older presets slice exactly as before).
// `reference` (the saved preset) gives back the shape of a key that splitting spread over the layout (a
// one- or two-value vector) when its linked values still fit that shape, so an unchanged process is
// exactly the saved one again. Returns false when the process was not split.
bool link_process_nozzles(DynamicPrintConfig &process, size_t from, const ConfigBase *reference = nullptr);

// ---- Per-nozzle process P5: one process with separate values per nozzle SIZE (the Speed page's size switch) ----
//
// The P4 slot model's diameter dimension (print_extruder_nozzle_diameter): one slot per (size, flow mode) for any
// tool, so a filament on a 0.6 nozzle reads the 0.6 values whichever tool it is on. Splitting per size and per
// nozzle (process_extruder_values) exclude each other.

// The distinct nozzle sizes of a printer: its heads and the nozzles of its racks, sorted.
std::vector<double> printer_nozzle_sizes(const ConfigBase &printer_config);
// The sizes a process is split into (its diameter slots), sorted; empty when it is not split by size.
std::vector<double> process_slot_diameters(const ConfigBase &process);
// The slot of `key` holding size `diameter`'s value for flow mode `mode` in a process split by size, else -1.
int process_diameter_slot(const ConfigBase &process, const std::string &key, double diameter, FilamentVolumeType mode);
// "Edit by nozzle size": one slot per size of `diameters` (two or more) and flow mode (process_flow_support), each
// starting from the value it prints with now. Returns false (process untouched) when the process is split per
// nozzle or already by size, or fewer than two sizes are given.
bool split_process_per_diameter(DynamicPrintConfig &process, std::vector<double> diameters);
// "Link nozzle sizes": back to one set of values, size `from`'s, in the shape the keys have in `reference` (the saved
// preset) when it is not split by size itself, else one value per flow mode. Returns false when not split by size.
bool link_process_diameters(DynamicPrintConfig &process, double from, const ConfigBase *reference = nullptr);

// ---- Per-nozzle process P6: every line width correct per nozzle on mixed-nozzle printers ----
//
// The width a role gets on a nozzle, in order of precedence:
//   1. The nozzle's own process ("Speeds from", nozzle_process_presets): that process's widths, as it has them (P2),
//      plus that process's own table entries (2.) for the nozzle. The nozzle's process is its process, for widths as
//      for speeds: the plate process's values for that nozzle, split per nozzle or per size, are not used (the Speed
//      page locks them for such a nozzle).
//   2. An explicit per-nozzle value of the plate process: process_nozzle_line_widths, the "Line widths per nozzle"
//      table. Entries use the filament_nozzle_variants selectors ("0.6: outer_wall_line_width=0.62 | T2: ..."); the
//      most specific one wins (tool and size, then tool, then size).
//   3. The plate's value: a percentage (or 0 = auto) resolves against the nozzle that prints the role, as always.
//   4. An absolute (mm) value on a printer with nozzles of two or more sizes is scaled by
//      nozzle diameter / line_width_reference_diameter(), the size the process's widths were made for.
//      A 0 role width stands for line_width and follows it. initial_layer_line_width 0 ("the role widths") and
//      bridge_line_width 0 ("auto") keep their meaning.
// On a printer with one nozzle size and no table entries nothing is resolved, so the G-code is unchanged.

// The process width keys resolved per nozzle: the Line width group, plus the locked-zag skin / skeleton widths.
const std::vector<std::string> &nozzle_width_option_keys();
// The nozzle diameter a process's absolute widths were made for: line_width_nozzle_diameter when set (> 0), else the
// first number of the printer's printer_variant ("0.4", "0.4+0.6": the nozzle the printer preset, and so the
// processes made for it, is for), else the first extruder's nozzle_diameter. 0 when none is known.
double line_width_reference_diameter(const ConfigBase &config);
// The value process_nozzle_line_widths gives `key` on 1-based tool `tool` (0 = unknown) with a `diameter` nozzle;
// false when no entry matches.
bool process_nozzle_width_override(const ConfigBase &process, const std::string &key, int tool, double diameter, FloatOrPercent &out);
// The text with the entry of exactly this selector (1-based tool or 0, diameter or 0) setting `key` to `value`; an empty
// `value` removes it.
std::string set_process_nozzle_width(const std::string &text, int tool, double diameter, const std::string &key, const std::string &value);
// Every problem of a process_nozzle_line_widths text (unknown keys, values that are not widths, flow-type selectors);
// empty when it is valid.
std::vector<std::string> validate_process_nozzle_line_widths(const std::string &text);
// Why `value` is not a width for the table ("0.62", "105%", >= 0), or empty when it is one.
std::string validate_nozzle_width_value(const std::string &value);

enum class NozzleWidthSource {
    Plate,    // the plate process's value as it is (a percentage, auto, or the reference size's own value)
    Scaled,   // the plate's absolute value scaled to the nozzle's size
    Override, // an entry of process_nozzle_line_widths
};
struct NozzleWidth
{
    FloatOrPercent    value{ 0., false };
    NozzleWidthSource source{ NozzleWidthSource::Plate };
};
// The width `key` takes on 1-based tool `tool` (0 = any) with a `diameter` nozzle under the plate process of `config`
// (process and printer keys): rules 2-4 above. The Tab shows it as the effective value of a nozzle.
NozzleWidth nozzle_width_for(const ConfigBase &config, const std::string &key, int tool, double diameter);
// Print::apply: rules 1-4 per filament into the nozzle_*_line_width arrays of a full print config (rule 1 keeps the
// entries fill_nozzle_line_widths wrote for a filament whose nozzle has its own process). When no filament's width
// changes the arrays are left as they are and false is returned.
bool resolve_nozzle_line_widths(DynamicPrintConfig &config);
// The first-layer width of 1-based `filament`: its entry of nozzle_initial_layer_line_width, else the plate's.
ConfigOptionFloatOrPercent initial_layer_line_width_for_filament(const PrintConfig &config, unsigned int filament);
// The default width of 1-based `filament`: its entry of nozzle_line_width, else `object_width` (the object's line_width).
ConfigOptionFloatOrPercent line_width_for_filament(const PrintConfig &config, const ConfigOptionFloatOrPercent &object_width, unsigned int filament);

} // namespace Slic3r

#endif // slic3r_NozzleProcess_hpp_
