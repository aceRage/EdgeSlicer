#ifndef slic3r_ToolSlots_hpp_
#define slic3r_ToolSlots_hpp_

// Per-nozzle process P4: the generic N-tool model (tests/design_per_nozzle_process.md "P4: Generic N-tool",
// spec docs/superpowers/specs/2026-09-07-mixed-nozzle-sizes.md "Phase 4" / "Phase 5").
//
// 1. A per-tool slot model. Bambu's process layout (print_extruder_id / print_extruder_variant) names one slot per
//    (extruder, variant). P4 adds a diameter dimension (print_extruder_nozzle_diameter) and lets a slot stand for
//    any tool (id 0), so one process can carry values per (tool, flow type, nozzle diameter). A filament reads the
//    slot of the nozzle it prints on. It applies when the process has diameter slots, or on a printer that opts in
//    (tool_slot_model), where it replaces process_flow_support for processes with a layout.
// 3. Rack nozzle diameters (H2C): extruder_rack_nozzle_diameter declares the rack's nozzles, and
//    filament_rack_nozzle_diameter the size each filament prints with. Flow takes that size; the nozzle grouping is
//    made to honour it on every layer (pin_rack_nozzle_diameters) and checked per layer afterwards.
// 4. Per-nozzle filament values: filament_nozzle_variants, resolved into the filament's own values when the print
//    is applied (resolve_filament_nozzle_variants).
// 5. physical_nozzle_properties: the fitted nozzles declared in the G-code.
// 7. The G-code's tool numbers follow filament_nozzle_assignment (filament_output_tools).
//
// Every rule here is a no-op without the new keys, so stock jobs slice exactly as before.

#include "PrintConfig.hpp"

#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

class ConfigBase;
class DynamicPrintConfig;

namespace MultiNozzleUtils {
struct NozzleInfo;
class LayeredNozzleGroupResult;
} // namespace MultiNozzleUtils

// Two nozzle diameters are the same nozzle size (0.001 mm tolerance).
bool same_nozzle_diameter(double a, double b);

// ---- 1. The per-tool slot model ------------------------------------------------------------------------------

struct ToolSlot
{
    int    tool{0};       // 1-based tool, 0 = any tool
    int    flow_mode{-1}; // fvtStandard / fvtHighFlow, -1 = a variant without a flow mode (E3D, TPU High Flow)
    double diameter{0.};  // the nozzle diameter the slot is for, 0 = any
};

struct ToolSlotKey
{
    size_t tool{0};               // 0-based tool
    int    flow_mode{fvtStandard}; // the filament's flow type
    double diameter{0.};          // the nozzle's diameter
};

// The slots of a process: print_extruder_id / print_extruder_variant (same length) and, when it has that length,
// print_extruder_nozzle_diameter. Empty when there is no such layout.
std::vector<ToolSlot> process_tool_slots(const ConfigBase &config);
// Whether a process carries the diameter dimension: print_extruder_nozzle_diameter of the layout's length with at
// least one entry above 0.
bool process_has_diameter_slots(const ConfigBase &config);
// The printer's opt-in to the slot model (tool_slot_model).
bool tool_slot_model_enabled(const ConfigBase &config);
// Whether process values are read through the slot model: diameter slots, or a layout on an opted-in printer.
bool tool_slot_model_active(const ConfigBase &config);
// The best slot for `key`, -1 when none fits. A slot fits when its tool is the key's (or any) and its diameter is
// the key's (or any); among those the more specific wins: tool, then diameter, then flow mode (a slot of another
// flow mode is the last resort, one of the right mode or without a mode comes first). Ties go to the first slot.
int match_tool_slot(const std::vector<ToolSlot> &slots, const ToolSlotKey &key);
// The flow mode filament `filament_idx` (0-based) slices with (filament_volume_type).
int filament_flow_mode(const ConfigBase &config, unsigned int filament_idx);
// The slot of an option of `opt_size` values that filament `filament_idx` (0-based) reads under the slot model, or
// -1 when the slot model does not apply (the caller then takes the P1 / flow-mode rule).
int tool_slot_for_filament(const ConfigBase &config, size_t opt_size, unsigned int filament_idx);
// Builds the slot layout of a process from (tool, variant, diameter) triples; test and profile helper.
void set_process_tool_slots(DynamicPrintConfig &config, const std::vector<int> &tools, const std::vector<std::string> &variants,
                            const std::vector<double> &diameters);

// ---- 3. Rack nozzle diameters (H2C) -----------------------------------------------------------------------------

struct RackNozzle
{
    double           diameter{0.};
    NozzleVolumeType volume_type{nvtStandard};
    bool             volume_given{false}; // ":<flow>" was written
};

// "0.4,0.4,0.2:high_flow" -> its nozzles; entries that are not a positive number are skipped.
std::vector<RackNozzle> parse_rack_nozzles(const std::string &text);
// The rack nozzles of each extruder (an empty list for an extruder without declared rack diameters).
std::vector<std::vector<RackNozzle>> extruder_rack_nozzles(const ConfigBase &config);
// Whether some extruder's rack holds nozzles of more than one diameter.
bool has_mixed_rack_diameters(const ConfigBase &config);
// The rack nozzle size filament `filament_id` (1-based) is pinned to, 0 when none.
double filament_rack_pin(const ConfigBase &config, unsigned int filament_id);
// The nozzle diameter 1-based `filament_id` prints with, on any config: its physical extruder's
// (physical_extruder_for_filament), or the rack nozzle size it is pinned to when its extruder's rack has one.
// Equals nozzle_diameter[physical extruder] whenever no rack diameters are declared. (The PrintConfig overload is
// declared in PrintConfig.hpp.)
double nozzle_diameter_for_filament(const ConfigBase &config, unsigned int filament_id);
// The same on a given layer of a grouped print: the diameter of the nozzle the grouping put the filament on there,
// or the static answer when the grouping has no entry for it.
double nozzle_diameter_for_filament_on_layer(const PrintConfig &config, const MultiNozzleUtils::LayeredNozzleGroupResult *group,
                                             unsigned int filament_id, int layer_id);
// Nozzle groups of an extruder with declared rack diameters: one (diameter, volume type) group per kind of nozzle.
// Returns false (groups untouched) when the extruder declares none.
bool rack_nozzle_groups(const ConfigBase &config, size_t extruder, std::vector<std::pair<std::string, std::pair<NozzleVolumeType, int>>> &groups);
// Makes a filament -> nozzle map (0-based nozzle ids into `nozzle_list`) honour the rack sizes: a used filament on a
// nozzle of another diameter than nozzle_diameter_for_filament() moves to a nozzle of its extruder with that
// diameter (the same volume type when there is one). Returns the number of filaments moved.
int pin_rack_nozzle_diameters(const PrintConfig &config, const std::vector<MultiNozzleUtils::NozzleInfo> &nozzle_list,
                              const std::vector<unsigned int> &used_filaments, std::vector<int> &filament_nozzle_map);
// Per layer, whether every filament prints on a nozzle of the diameter it was sliced for. Empty when it does,
// else a message naming the first filament, layer and diameters that do not match.
std::string check_rack_nozzle_diameters(const PrintConfig &config, const MultiNozzleUtils::LayeredNozzleGroupResult &group);
// Configuration errors of the rack keys (an extruder's nozzle_diameter missing from its rack, a pin no rack nozzle
// has), or empty.
std::string validate_rack_nozzle_diameters(const PrintConfig &config, const std::vector<unsigned int> &used_filaments);

// ---- 4. Per-nozzle filament values ------------------------------------------------------------------------------

struct FilamentNozzleVariant
{
    int                                              tool{0};       // 1-based, 0 = any
    int                                              flow_mode{-1}; // -1 = any
    double                                           diameter{0.};  // 0 = any
    std::vector<std::pair<std::string, std::string>> values;
};

// Parses one filament's filament_nozzle_variants. Problems (an entry without a selector, a value without "=") go
// to `errors` and the entry or pair is skipped.
std::vector<FilamentNozzleVariant> parse_filament_nozzle_variants(const std::string &text, std::vector<std::string> *errors = nullptr);
// The keys an entry may set: the filament preset's vector options other than identity ones (the preset's names,
// colour, type, the variants key itself).
bool is_filament_nozzle_variant_key(const std::string &key);
// Writes each filament's matching entries over its own values in a full print config, the less specific first
// (any < flow < diameter < diameter + flow < tool). Flow-variant keys packed per filament
// (filament_flow_step_size) get the value in each of the filament's flow slots, or only in the slot of the entry's
// flow type. Returns the number of values written; `errors` gets the keys that could not be set.
int resolve_filament_nozzle_variants(DynamicPrintConfig &config, std::vector<std::string> *errors = nullptr);

// ---- 5. physical_nozzle_properties ------------------------------------------------------------------------------

// "0.4-standard,0.6-high_flow,..." : every physical nozzle (an extruder's rack nozzles one by one) with its
// diameter and flow type.
std::string physical_nozzle_properties(const ConfigBase &config);
// Whether the G-code declares them: the printer asks for it (support_filament_nozzle_mapping), or it has two or more
// nozzles of different diameters (mixed heads or a mixed rack).
bool emit_physical_nozzle_properties(const ConfigBase &config);

// ---- 7. The G-code's tool numbers -------------------------------------------------------------------------------

// The tool (0-based) the G-code names for each filament (index filament - 1) when filament_nozzle_assignment moves
// filaments to other tools of a toolchanger: each filament's assigned tool, the filaments the print does not use
// spread over the tools left free so the map is one-to-one. Empty - the G-code names each filament's own tool, as
// before - when nothing is assigned, the result would be the identity, the printer is not a toolchanger with one
// tool per filament, or two used filaments share a tool (`shared`, when given, is then set).
std::vector<int> filament_output_tools(const PrintConfig &config, const std::vector<unsigned int> &used_filaments, bool *shared = nullptr);
// The inverse: for each tool the filament (0-based) it prints, from a filament -> tool map.
std::vector<int> output_tool_filaments(const std::vector<int> &filament_tools);
// Writes into `out` the per-filament values of `source` in tool space: entry t of each per-filament vector is the value
// of the filament printed on tool t (a tool no filament prints takes filament 1's). The filament options, the
// project's per-filament keys (colour, flow type...) and the G-code's temperature aliases are rewritten; a value
// packed per flow type (filament_flow_step_size) is first resolved to the filament's own flow type, and the packing
// keys follow. Values of `out` win over `source` as the source of a key (the placeholder parser's own overrides).
// filament_nozzle_assignment becomes empty: in tool space every filament is on its own tool.
void filament_values_to_tool_space(DynamicConfig &out, const ConfigBase &source, const std::vector<int> &filament_tools);

} // namespace Slic3r

#endif // slic3r_ToolSlots_hpp_
