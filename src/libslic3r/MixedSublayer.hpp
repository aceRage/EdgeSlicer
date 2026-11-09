#pragma once
#ifndef slic3r_MixedSublayer_hpp_
#define slic3r_MixedSublayer_hpp_

// Mixed color sublayer, phase 1 (tests/design_mixed_color_sublayer.md).
//
// Bambu Studio 2.9.1 ("enable_mixed_color_sublayer", BS ToolOrdering.cpp resolve_mixed_filaments :2893,
// GCode.cpp :6024-6051) prints every layer of a mixed filament once per component, at sub-Z
// print_z - lh + sum(previous passes) + h_k with h_k = ratio_k x lh and the flow scaled by the same ratio.
// Edge does not port that second engine (D2): a "Ratio" row is planned by Edge's own Local-Z
// (Subdivide Mix Layer) planner with this pass policy instead. Everything here is pure so it can be tested
// without slicing:
//   * the per-row method resolution (D1),
//   * the Ratio pass plan of one layer, with the thin-pass floor and carry-over (D5) and the order (D4),
//   * the same-nozzle-diameter rule (D8) and the single-nozzle tool-change estimate (D7),
//   * the prime tower purge for a Ratio tool change (D10),
//   * the Bambu Studio 3MF import of mixed filament slots into Edge mixed rows (D3, D11).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {

class DynamicPrintConfig;
class MixedFilamentManager;
struct MixedFilament;

// MixedFilament::sublayer_method values.
enum class MixedSublayerMethod : int {
    Auto      = 0,
    Cadence   = 1,
    Subdivide = 2,
    Ratio     = 3,
};

// D1: a row's own method wins; Auto takes the process defaults. Subdivide Mix Layer wins over the mixed color
// sublayer when both are on (it is the superset). With both off, a gradient row keeps Edge's existing behaviour
// (gradient rows always use Local-Z) and every other row is whole-layer cadence. Never returns Auto.
MixedSublayerMethod mixed_sublayer_effective_method(int  row_method,
                                                    bool enable_mixed_color_sublayer,
                                                    bool local_z_mode,
                                                    bool row_is_gradient);

// True when nothing in the print uses the new mechanism: the process key is off and every row is Auto.
// The Local-Z planner then runs exactly as before this feature (the identity gate relies on it).
bool mixed_sublayer_legacy_mode(const std::vector<MixedFilament> &rows, bool enable_mixed_color_sublayer);

// One component of a Ratio row on one layer.
struct MixedRatioComponent
{
    unsigned int extruder   { 0 };  // 1-based physical filament id
    double       ratio      { 0. }; // share of the layer, the components of a row sum to 1
    double       min_height { 0. }; // D5 floor: max(min_layer_height of its nozzle, mixed lower bound)
};

struct MixedRatioPass
{
    unsigned int extruder { 0 };
    double       height   { 0. };
};

// Carry-over state of one Ratio row along an object, layer to layer.
struct MixedRatioRowState
{
    // Height owed to (positive) or borrowed by (negative) each component, aligned with the component list.
    std::vector<double> carry;
    // The component that printed last on the previous planned layer; the alternating order starts with it.
    unsigned int        last_extruder { 0 };
    void reset() { carry.clear(); last_extruder = 0; }
};

// The components a row splits into: 3+ gradient ids with their weights (equal when missing), else A / B with
// mix_b_percent. Ratios are normalized to sum to 1 and zero-share components are dropped. `gradient_ratio_a`
// in (0, 1) overrides A's share of a two-component row (a Z gradient row on this layer); pass a negative value
// otherwise. Returns an empty list when the row has fewer than two usable components.
std::vector<MixedRatioComponent> mixed_ratio_components(const MixedFilament &row, size_t num_physical, double gradient_ratio_a = -1.);

// The Ratio passes of one layer, bottom to top. The heights always sum to `layer_height`.
//   * Each component wants ratio x lh plus its carry. A component whose want is below its floor gets no pass;
//     its share carries to the next layer (D5, Bambu Studio has no floor). The others are scaled so the layer
//     is filled; what they over- or under-print is carried too, so the carries always sum to the starting sum.
//   * If no component reaches its floor the layer is printed whole by the component that is owed the most
//     (the layer "reverts to cadence").
//   * Order: ascending physical id (Bambu Studio, higher id on top) when `ascending_order`, else the component
//     that ended the previous layer starts this one and the rest follow in ascending cyclic order (D4).
std::vector<MixedRatioPass> plan_mixed_ratio_layer(const std::vector<MixedRatioComponent> &components,
                                                   double                                   layer_height,
                                                   bool                                     ascending_order,
                                                   MixedRatioRowState                      &state);

// P3, D9: the Ratio passes of one AdaptiveLayerMerge band. A band of height `band_height` is printed on its top
// layer and swallows the layers below it, so the passes start at the band bottom, below the layer the G-code is
// on. `swallowed_height` (band height - the top layer's own height) is how far below that layer's bottom they
// reach: the first pass is chosen so its top is not under it (the nozzle never drops below the material the
// previous layer printed next to the mixed region). Plans as plan_mixed_ratio_layer() over the band height, then
// starts with the first component in that order whose pass is tall enough. When none is (three or more even
// components on a deep band), the band prints whole with the component owed the most (cadence): `split` is false.
struct MixedRatioBandPlan
{
    std::vector<MixedRatioPass> passes;
    bool                        split { false };
};
MixedRatioBandPlan plan_mixed_ratio_band(const std::vector<MixedRatioComponent> &components,
                                         double                                   band_height,
                                         double                                   swallowed_height,
                                         bool                                     ascending_order,
                                         MixedRatioRowState                      &state);

// D8: the components of a Ratio row must print through one nozzle diameter, on every printer. `nozzle_for` gives
// the nozzle diameter of a 1-based physical filament id. Returns the 1-based ids of two components that differ
// (first pair found), or {0, 0} when they all match.
struct MixedDiameterMismatch
{
    unsigned int filament_a { 0 };
    unsigned int filament_b { 0 };
    double       diameter_a { 0. };
    double       diameter_b { 0. };
    bool         mismatch() const { return filament_a != 0; }
};
MixedDiameterMismatch mixed_ratio_diameter_mismatch(const std::vector<unsigned int> &components,
                                                    const std::vector<double>       &nozzle_by_filament);

// D7: a rough count of the tool changes a Ratio row adds on a single-nozzle machine. Alternating order changes
// n - 1 times per layer (the next layer starts where this one ended); ascending order changes n times.
// With a prime tower the count doubles: ToolOrdering does not know the passes (P1 limit), so the G-code switches
// back to the tower plan's filament after them and the planned tower change follows (gate: 1.98 per layer).
size_t mixed_ratio_toolchanges_per_layer(size_t component_count, bool ascending_order, bool with_wipe_tower = false);

// D10: the purge volume of a tool change into a Ratio pass. Two filaments sharing one nozzle (an AMS on one
// extruder, single_extruder_multi_material) get the full flush of the flush matrix, as an ordinary filament
// change does: a mini-wipe would bleed the previous colour into the mix. Different heads (a toolchanger,
// H2D left / right) only prime: `prime_volume`.
float mixed_ratio_toolchange_purge_volume(bool same_nozzle, float flush_volume, float flush_multiplier, float prime_volume);

// ---- Bambu Studio 3MF import (D3, D11) -------------------------------------------------------------------
//
// In Bambu Studio 2.9.1 a mixed filament is a real filament slot: filament_is_mixed[i] plus
// filament_mixed_components[i] = "1,3" (1-based slots) and filament_mixed_sublayer_ratios[i] = "0.7,0.3".
// Edge keeps mixed filaments as virtual rows above the physical ids (MixedFilamentManager), so the slot is
// removed from every per-filament array, a mixed row is appended to mixed_filament_definitions, and every id
// in the model is remapped: physical ids above a removed slot shift down, the mixed slot becomes the row's
// virtual id. Edge never stores filament_is_mixed arrays (D3).
struct BambuMixedSlots
{
    std::vector<bool>        is_mixed;          // filament_is_mixed
    std::vector<std::string> components;        // filament_mixed_components
    std::vector<std::string> ratios;            // filament_mixed_sublayer_ratios
    std::vector<bool>        gradient;          // filament_mixed_gradient
    std::vector<std::string> gradient_range;    // filament_mixed_gradient_range
    std::vector<std::string> gradient_curve;    // filament_mixed_gradient_curve (dropped, D11)
    std::vector<bool>        gradient_per_part; // filament_mixed_gradient_per_part (dropped, D11)

    bool any_mixed() const;
};

struct BambuMixedImportResult
{
    bool                      converted { false };
    size_t                    old_slot_count { 0 };
    size_t                    new_physical_count { 0 };
    // Indexed by the old 1-based slot id: the new 1-based id (physical or virtual), 0 when it has none.
    std::vector<unsigned int> id_remap;
    // 0-based old slots that were mixed and are gone from the filament arrays.
    std::vector<size_t>       removed_slots;
    // Rows that lost a custom gradient curve or a per-part gradient (D11), for the load notification.
    std::vector<std::string>  notices;
};

// Read the Bambu Studio mixed keys from a project_settings.config JSON text. Edge does not define these keys,
// so the ordinary config loader drops them; this reads them from the raw file. Returns false on a parse error.
bool parse_bambu_mixed_slots_json(const std::string &json_text, BambuMixedSlots &out);

// Convert `config` (an Edge project config just loaded from a Bambu Studio project) in place. The per-filament
// arrays must still have one entry per Bambu slot. Does nothing (converted = false) without mixed slots, with
// fewer than two physical slots, or when the arrays do not match the slot count.
BambuMixedImportResult convert_bambu_mixed_slots(DynamicPrintConfig &config, const BambuMixedSlots &slots);

// Apply a BambuMixedImportResult::id_remap to a 1-based id (0 and out-of-range ids map to 0).
unsigned int bambu_mixed_remap_id(const BambuMixedImportResult &result, int old_id);

} // namespace Slic3r

#endif // slic3r_MixedSublayer_hpp_
