#pragma once
#ifndef slic3r_GUI_MixedSublayerUi_hpp_
#define slic3r_GUI_MixedSublayerUi_hpp_

// Mixed color sublayer, phase 2 (GUI; tests/design_mixed_color_sublayer.md 3g). The parts of the GUI that are
// plain decisions and arithmetic live here, without wxWidgets, so slic3rutils_tests can check them:
//   * which sub-layer method a mixed row prints with and which badge the sidebar shows for it,
//   * the dialog's cost line ("about N tool changes, M g flush"),
//   * the "broken" row: a Ratio row whose filaments print through different nozzle diameters (D8),
//   * which mixed rows an object uses (variable layer height note, preview legend),
//   * the Bambu Studio import notice (how many rows a project's mixed_filament_definitions holds).
// The widgets themselves (MixedFilamentDialog, the sidebar rows, the legend) are hand-tested.

#include "libslic3r/MixedSublayer.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

class ModelObject;
class MixedFilamentManager;
class PresetBundle;
struct MixedFilament;

namespace GUI { namespace MixedSublayerUi {

// The two process keys a row whose method is Auto follows (D1).
struct ProcessDefaults
{
    bool enable_mixed_color_sublayer { false }; // "Mixed color sublayer (Bambu ratio)"
    bool local_z_mode                { false }; // "Subdivide Mix Layer" (dithering_local_z_mode)
};

// A Z gradient row (two different components, gradient on): Edge sub-layers it by itself (Local-Z), so turning a
// gradient on never needs the process toggle (Bambu Studio's dialog turns its toggle on instead).
bool row_is_gradient(const MixedFilament &row);
// False for rows the sub-layer planner never splits: cycle patterns and same-layer pointillism keep their own order.
bool row_can_sublayer(const MixedFilament &row);

// The method the row prints with, as the slicer resolves it: its own, or the process default for Auto; whole layers
// (Cadence) for a row that cannot be split, and for a Ratio row with fewer than two usable filaments. Never Auto.
MixedSublayerMethod effective_method(const MixedFilament &row, const ProcessDefaults &defaults);

// The small tag after a sidebar row: Ratio or Subdivide when the row is printed in sub-layers (a Z gradient row
// always is), Cadence only when the row was set to whole layers explicitly. Hidden for an Auto row that prints whole
// layers, so a project that uses no sub-layers looks as before.
enum class Badge { Hidden, Cadence, Subdivide, Ratio };
Badge badge_for_row(const MixedFilament &row, const ProcessDefaults &defaults);
// The badge's untranslated text (the GUI passes it through _L): "Ratio", "Subdivide", "Layers"; empty for Hidden.
const char *badge_text(Badge badge);

// The dialog's method choice: index 0 Auto, 1 Cadence, 2 Subdivide, 3 Ratio (= MixedFilament::sublayer_method).
int method_to_choice(int sublayer_method);
int choice_to_method(int choice);

// ---- Cost line (D7) -------------------------------------------------------------------------------------
struct CostInput
{
    std::vector<unsigned int> components;          // 1-based physical ids of the row
    bool                      bs_order { false };  // MixedFilament::sublayer_bs_order
    bool                      wipe_tower { false };
    // One nozzle for every filament (an AMS on one extruder, single_extruder_multi_material): every tool change
    // is a filament change with a flush. Toolchangers and two-extruder printers only count the changes.
    bool                      single_nozzle { false };
    size_t                    layers { 0 };        // layers of the tallest object; 0 = no object yet
    size_t                    num_physical { 0 };
    std::vector<double>       flush_matrix;        // flush_volumes_matrix, num_physical x num_physical, mm3
    double                    flush_multiplier { 1. };
    std::vector<double>       density;             // filament_density per physical filament, g/cm3
};

struct CostEstimate
{
    bool   valid { false };  // the row has two or more usable components
    size_t per_layer { 0 };  // tool changes per layer
    size_t changes { 0 };    // per_layer x layers
    double flush_mm3 { 0. }; // over the whole print (single nozzle only)
    double flush_g { 0. };
};

// Same counting as Print::validate's single-nozzle warning: mixed_ratio_toolchanges_per_layer() (doubled with a
// prime tower), the flush per change averaged over the ordered component pairs of the flush matrix times the
// multiplier, grams from the components' mean density.
CostEstimate estimate_cost(const CostInput &in);

// The translated sentence shapes the cost line uses, %1% / %2% placeholders (boost::format style).
struct CostLineTemplates
{
    std::string total_with_flush;     // "About %1% tool changes, %2% g flush"
    std::string total_no_flush;       // "About %1% tool changes"
    std::string per_layer_with_flush; // "About %1% tool changes and %2% g flush per layer"
    std::string per_layer_no_flush;   // "About %1% tool changes per layer"
};
// Without an object (layers == 0) the line is per layer and the flush is the flush of one layer. Grams print with
// one decimal under 10 g and as a whole number from 10 g. Empty when the estimate is not valid.
std::string format_cost_line(const CostEstimate &cost, const CostLineTemplates &templates);
// The flush text of format_cost_line, exposed for the tests: "0.4", "9.9", "12", "1234".
std::string format_grams(double grams);

// ---- Broken row (D8) -----------------------------------------------------------------------------------
// The components of a Ratio row that print through different nozzle diameters (`nozzle_by_filament`, 0-based by
// physical filament). Rows that do not print as Ratio are never broken by this rule.
MixedDiameterMismatch broken_ratio_row(const MixedFilament &row, const ProcessDefaults &defaults, size_t num_physical,
                                       const std::vector<double> &nozzle_by_filament);

// ---- Which rows an object uses -------------------------------------------------------------------------
// The 1-based virtual ids of the mixed rows an object uses: painted, per part or object, per height range, or as a
// wall / sparse / solid infill filament of the object or a part. Sorted, no duplicates.
std::vector<unsigned int> mixed_ids_used_by_object(const ModelObject &object, const MixedFilamentManager &mgr, size_t num_physical);
// True when one of those rows prints as Ratio.
bool object_uses_ratio_rows(const ModelObject &object, const MixedFilamentManager &mgr, size_t num_physical,
                            const ProcessDefaults &defaults);
// True when any row that is not deleted prints as Ratio (the plate tab's sequence controls, BS Tab.cpp:4227).
bool any_ratio_row(const MixedFilamentManager &mgr, const ProcessDefaults &defaults);

// ---- Preview legend ------------------------------------------------------------------------------------
struct LegendComponent
{
    unsigned int filament { 0 }; // 1-based physical id
    int          percent { 0 };  // whole percent; the row's components sum to 100
};
struct LegendRow
{
    unsigned int                 virtual_id { 0 };
    MixedSublayerMethod          method { MixedSublayerMethod::Cadence };
    std::vector<LegendComponent> components;
};
// One row per used virtual id that the manager knows, with its components (mixed_ratio_components: A / B and
// mix_b_percent, or 3+ gradient ids and weights) rounded to whole percents by largest remainder.
std::vector<LegendRow> legend_rows(const MixedFilamentManager &mgr, const std::vector<unsigned int> &used_ids, size_t num_physical,
                                   const ProcessDefaults &defaults);

// ---- Bambu Studio import notice ------------------------------------------------------------------------
// The number of rows in a mixed_filament_definitions value (rows are ';' separated; blank rows do not count).
size_t count_mixed_definition_rows(const std::string &definitions);

// ---- Reading the edited presets ------------------------------------------------------------------------
// The layers of an object `height` mm tall at `layer_height` (ceil); 0 for no object or a bad layer height.
size_t estimate_layers(double height, double layer_height);
// enable_mixed_color_sublayer from the edited process preset (a process key, D3); dithering_local_z_mode from the
// project config, which overrides the preset for the mixed keys (PresetBundle s_project_options), else the preset.
ProcessDefaults process_defaults(const PresetBundle &bundle);
// The nozzle diameter each physical filament prints through (0-based), from the edited printer preset and the
// project config (filament_map, nozzle overrides, rack pins): nozzle_diameter_for_filament().
std::vector<double> nozzle_by_filament(const PresetBundle &bundle, size_t num_physical);
// The cost input of a row on the edited presets: prime tower, single nozzle, flush matrix and multiplier from the
// project config, densities from the selected filament presets. `layers` as estimate_layers().
CostInput cost_input(const PresetBundle &bundle, const MixedFilament &row, size_t num_physical, size_t layers);

}}} // namespace Slic3r::GUI::MixedSublayerUi

#endif // slic3r_GUI_MixedSublayerUi_hpp_
