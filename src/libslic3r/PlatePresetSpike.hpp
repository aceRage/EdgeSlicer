#ifndef slic3r_PlatePresetSpike_hpp_
#define slic3r_PlatePresetSpike_hpp_

#include <string>

#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

class AppConfig;

// Throwaway Option A spike (SPIKE.md). All GUI apply-site changes sit behind
// per_plate_presets (default off) or EDGE_PLATE_PRESETS_SPIKE=1. CLI opts in
// by supplying EDGE_PLATE_PRESET_CONFIG_<n>=path for plate n (0-based).

bool per_plate_presets_spike_env_enabled();
bool per_plate_presets_spike_enabled(const AppConfig *cfg);

// Same rule the CLI already uses at Snapmaker_Orca.cpp ~L5960: printer_model
// (then printer_settings_id) starting with "Bambu Lab".
bool is_bbl_printer_from_config(const DynamicPrintConfig &cfg);

// Load EDGE_PLATE_PRESET_CONFIG_<plate_index> if that env var is set.
// Accepts .json (profile style) or .ini / .config (ConfigBase::save).
bool load_spike_plate_config_file(int plate_index, DynamicPrintConfig &out);

// S9: if a per-plate file is supplied, it becomes the base; otherwise project_base.
DynamicPrintConfig cli_base_config_for_plate(const DynamicPrintConfig &project_base, int plate_index);

std::string spike_plate_config_env_key(int plate_index);

} // namespace Slic3r

#endif
