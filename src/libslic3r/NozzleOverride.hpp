#ifndef slic3r_NozzleOverride_hpp_
#define slic3r_NozzleOverride_hpp_

// Per-nozzle process P7: every tool's nozzle size set independently (owner requirement 2026-10-31: "set all 4
// nozzles on the U1 to different sizes or to whichever size we want independently, same for Bambu and other brands").
//
// The printer preset stays what it is ("Snapmaker U1 (0.4 nozzle)", "Bambu Lab H2D 0.4 nozzle"). The size each tool
// really carries is a PROJECT key, nozzle_diameter_override: one value per physical extruder, 0 = the printer preset's
// nozzle_diameter for that extruder. It is stored in the 3MF's project settings and remembered per printer model in
// the app config, like the other per-nozzle project keys (nozzle_process_presets, filament_nozzle_assignment).
//
// Print::apply() writes the effective sizes into nozzle_diameter (apply_nozzle_diameter_override) before anything
// reads it, so everything P2 / P4 / P5 / P6 resolve per nozzle follows: process selection by size, per-nozzle filament
// values by size, P6 line widths (mm widths scaled from the reference nozzle, also on a printer with one size), the
// smallest-nozzle layer guard, the mixed-width prime towers, flow and volumetric limits, the G-code's config block and
// the 3MF's slice_info. Without an override nothing changes: the G-code is byte-identical.

#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r {

class ConfigBase;
class DynamicPrintConfig;
namespace DualNozzleSync { struct PrinterState; }

// The project key.
inline constexpr const char *NOZZLE_DIAMETER_OVERRIDE_KEY = "nozzle_diameter_override";

// The stored override values (0 = the preset's size), or an empty list when the key is missing.
std::vector<double> nozzle_diameter_override_values(const ConfigBase &config);
// True when any extruder's size is overridden (a value > 0).
bool has_nozzle_diameter_override(const ConfigBase &config);
// `preset` (one size per extruder) with every override entry > 0 put over it. Entries past the preset's extruder
// count are ignored; extruders without an entry keep the preset's size.
std::vector<double> nozzle_diameters_with_override(std::vector<double> preset, const std::vector<double> &override_values);
// The sizes a config's extruders print with: its nozzle_diameter with its nozzle_diameter_override applied.
std::vector<double> effective_nozzle_diameters(const ConfigBase &config);
// Print::apply: nozzle_diameter becomes the effective sizes. max_layer_height (and min_layer_height where it would
// exceed the new maximum) of an extruder whose size changed is scaled by new / old size, the ratio every printer
// preset family keeps between its nozzle variants. Returns true when nozzle_diameter changed.
bool apply_nozzle_diameter_override(DynamicPrintConfig &config);

// The stored form of an override: { 0 } (unused) when every entry is 0 or equals the preset's size, else the values
// as they are (one per extruder, zeros where the preset's size is kept).
std::vector<double> nozzle_diameter_override_stored(std::vector<double> values, const std::vector<double> &preset);
// The stored override after setting 0-based `tool` to `diameter` (0 = back to the preset's size).
std::vector<double> set_tool_nozzle_diameter(std::vector<double> override_values, const std::vector<double> &preset, size_t tool,
                                             double diameter);

// The sizes a tool picker offers: the standard 0.2 / 0.4 / 0.6 / 0.8, every size the printer's presets name in their
// printer_variant ("0.25", "0.4+0.6"), and the sizes in `current` (the preset's and the override's). Sorted, unique.
std::vector<double> nozzle_size_choices(const std::vector<double> &current, const std::vector<std::string> &printer_variants);
// The numbers in a printer_variant ("0.4+0.6" -> 0.4, 0.6); empty when it names none.
std::vector<double> printer_variant_sizes(const std::string &printer_variant);

// App config text: "0.2,0.4,0.6,0.8" (0 kept for extruders on the preset's size); empty for an unused override.
std::string serialize_nozzle_diameter_override(const std::vector<double> &values);
// The values of serialize_nozzle_diameter_override's text; empty when the text is empty or not a list of sizes.
std::vector<double> parse_nozzle_diameter_override(const std::string &text);

// "Sync from printer": the override that makes the effective sizes what the printer reported. `reported` holds one
// size per extruder in extruder order, 0 where the printer did not report one (that extruder keeps its current
// effective size). Stored form (nozzle_diameter_override_stored) against `preset`.
std::vector<double> nozzle_diameter_override_from_reported(const std::vector<double> &preset, const std::vector<double> &current_override,
                                                           const std::vector<double> &reported);

// A tool whose sliced size differs from the size the printer reports.
struct NozzleSizeMismatch
{
    size_t tool{ 0 };       // 0-based extruder
    double sliced{ 0. };    // the size the job was sliced for
    double reported{ 0. };  // the size the printer reports
};
// The tools of `used_tools` (0-based; empty = every tool) whose `sliced` size differs from `reported`. A reported
// size of 0 (unknown) or a tool the printer did not report never mismatches.
std::vector<NozzleSizeMismatch> nozzle_size_mismatches(const std::vector<double> &sliced, const std::vector<double> &reported,
                                                       const std::vector<size_t> &used_tools = {});
// "T2 0.6 mm (printer: 0.4 mm), T4 ..." with 1-based tools.
std::string describe_nozzle_size_mismatches(const std::vector<NozzleSizeMismatch> &mismatches);

// ---- What a Bambu printer reports (DualNozzleSync::PrinterState, the H2D / H2C / X2D push) ----
// The size of the nozzle mounted on each logical extruder, 0 where it is not reported.
std::vector<double> reported_extruder_nozzle_sizes(const DualNozzleSync::PrinterState &state, size_t extruders);
// Every size each logical extruder can print with: its mounted nozzle plus the usable nozzles of its rack (H2C).
std::vector<std::vector<double>> reported_available_nozzle_sizes(const DualNozzleSync::PrinterState &state, size_t extruders);
// Rack printers (H2C): the used tools whose sliced size is none of the sizes the printer has for that extruder (an
// extruder that reported nothing never mismatches). The printer swaps rack nozzles itself, so the mounted nozzle being
// another size is no mismatch.
std::vector<NozzleSizeMismatch> rack_nozzle_size_mismatches(const std::vector<double> &sliced,
                                                            const std::vector<std::vector<double>> &available,
                                                            const std::vector<size_t> &used_tools = {});

} // namespace Slic3r

#endif // slic3r_NozzleOverride_hpp_
