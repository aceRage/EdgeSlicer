#ifndef slic3r_Format_BambuMultiDiameter_hpp_
#define slic3r_Format_BambuMultiDiameter_hpp_

// AdaptiveLayerMerge step 5: the 3MF keys Bambu Studio 2.9.1 (tag v02.09.01.70) writes and reads for a job that prints
// with several nozzle diameters on one extruder's rack (the H3C), so Bambu Studio, Bambu Connect and the printer see the
// same metadata from an EdgeSlicer job.
//
// Bambu Studio has a project-only switch for this, multi_nozzle_diameter_mode (PresetBundle.cpp:78 / :430, defined at
// PrintConfig.cpp:5829), offered only on a single extruder that carries several nozzles (is_single_ext_multi_nozzle,
// PrintConfig.cpp:1199; NozzleInventory.cpp:137), and a per-filament nozzle choice, filament_diameter_map (coStrings,
// PrintConfig.cpp:5824: one diameter per filament, "" = not selected). Both are project keys (PresetBundle.cpp:77-78), so
// they land in Metadata/project_settings.config. slice_info.config carries the switch per plate
// (bbs_3mf.cpp:9127-9128) and, with the switch on, reports nozzle_diameters as the lowest-slot used filament's nozzle
// size repeated per extruder (bbs_3mf.cpp:9100-9106).
//
// EdgeSlicer has no such switch (owner decision): the band merge is gated on Print::uses_multiple_nozzle_diameters()
// alone. On export the switch is therefore derived - on for a job that uses several diameters on a machine Bambu would
// offer the mode on - and nothing is written otherwise, so a single-diameter job's files are unchanged. On import the
// two keys are consumed: they set up the project's nozzles (nozzle_diameter_override, the rack override and the
// per-filament rack pins) and are not kept as config keys.

#include <string>
#include <vector>

namespace Slic3r {

class ConfigBase;
class DynamicPrintConfig;
struct FilamentInfo;

namespace BambuMultiDiameter {

inline constexpr const char *MODE_KEY          = "multi_nozzle_diameter_mode";
inline constexpr const char *DIAMETER_MAP_KEY  = "filament_diameter_map";
// Bambu's per-slot nozzle diameters and flow types, flat in physical slot order (PrintConfig.cpp:5762 / :5745).
inline constexpr const char *SLOT_DIAMETER_KEY = "extruder_nozzle_diameter";
inline constexpr const char *SLOT_VOLUME_KEY   = "extruder_nozzle_volume_type";

// Bambu's is_single_ext_multi_nozzle in EdgeSlicer terms: one extruder whose rack holds more than one nozzle.
bool single_extruder_rack(const ConfigBase &config);
// The multi_nozzle_diameter_mode Bambu Studio would have for a job: the job prints with several nozzle diameters
// (Print::uses_multiple_nozzle_diameters()) on a single-extruder rack.
bool mode_for_job(bool uses_multiple_nozzle_diameters, const ConfigBase &config);

// filament_diameter_map as Bambu Studio completes it in multi-diameter mode (complete_filament_nozzle_diameter_selections):
// one entry per filament (filament_colour), the nozzle size the filament prints with, "0.4" style; "" when unknown.
std::vector<std::string> filament_diameter_map(const ConfigBase &config);
// Whether the project's filaments are set to print with more than one nozzle size (filament_diameter_map holds
// several distinct sizes). The project-wide answer for a save that has no sliced plate.
bool config_uses_multiple_diameters(const ConfigBase &config);
// Writes MODE_KEY = 1 and DIAMETER_MAP_KEY into `out` (the project config being saved), as Bambu Studio has them.
void add_project_keys(DynamicPrintConfig &out, const ConfigBase &config);

// slice_info.config's nozzle_diameters in multi-diameter mode (bbs_3mf.cpp:9100-9106 on the tag): the nozzle size of
// the used filament with the lowest slot, once per extruder. `fallback` when no used filament carries a size.
std::string slice_info_nozzle_diameters(const std::vector<FilamentInfo> &used_filaments, size_t extruder_count,
                                        const std::string &fallback);

// ---- Import ------------------------------------------------------------------------------------------------------

struct ImportedKeys
{
    bool                     has_mode{ false };
    bool                     mode{ false };
    bool                     has_map{ false };
    std::vector<std::string> diameter_map;      // filament_diameter_map, one entry per filament
    std::vector<double>      slot_diameters;    // extruder_nozzle_diameter
    std::vector<std::string> slot_volume_types; // extruder_nozzle_volume_type ("Standard", "High Flow", ...)

    bool any() const { return has_mode || has_map; }
};

// The keys out of a project_settings.config text. Nothing (any() == false) when the text has neither key or is not
// JSON.
ImportedKeys read_keys(const std::string &json_text);

// What apply_import() set up.
struct ImportResult
{
    bool rack_set{ false };     // extruder_rack_nozzle_override from extruder_nozzle_diameter
    bool override_set{ false }; // nozzle_diameter_override from the per-extruder sizes
    int  pins_set{ 0 };         // filaments pinned (filament_rack_nozzle_diameter) from filament_diameter_map
};

// Sets up the project's nozzles from a Bambu Studio multi-diameter project (keys.mode == true; nothing otherwise):
//  * the rack: extruder_rack_nozzle_override from extruder_nozzle_diameter (+ flow types), on a single extruder, when
//    the project carries no rack of its own;
//  * the extruder's size: in multi-diameter mode Bambu puts the printer on its canonical 0.2 preset and writes the
//    smallest installed nozzle into nozzle_diameter; nozzle_diameter goes back to the preset's size (printer_variant)
//    and the installed size becomes nozzle_diameter_override, unless the project already has an override;
//  * the filaments: every filament_diameter_map entry that differs from the size its extruder prints with becomes a
//    rack pin (filament_rack_nozzle_diameter), unless the project already pins a filament.
// A file EdgeSlicer wrote itself therefore loads back unchanged.
ImportResult apply_import(DynamicPrintConfig &config, const ImportedKeys &keys);

// Removes the keys apply_import() consumed from a load's unrecognized-key list, so they do not read as unknown keys.
void forget_consumed_keys(std::vector<std::string> &unrecognized_keys, const ImportedKeys &keys, const ImportResult &result);

} // namespace BambuMultiDiameter
} // namespace Slic3r

#endif // slic3r_Format_BambuMultiDiameter_hpp_
