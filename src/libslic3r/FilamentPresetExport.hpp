// The row model behind File > Export > Export Preset Bundle > "Filament presets (.zip)": one row
// per exportable user filament preset, with the printers it is compatible with, its vendor and
// material. The shared types, the selection -> zip entries mapping and the zip writer are in
// ProcessPresetExport.hpp.
//
// What can be exported is what the earlier filament export enumerated: every user filament
// preset (not a system or default one) that has a base preset. That export grouped them by
// filament name (the base preset's name without its "@printer" tail) and ticked a whole group;
// PresetExportRow::group keeps that name so a group can still be selected as before.
#pragma once

#include <string>
#include <vector>

#include "ProcessPresetExport.hpp"

namespace Slic3r {

// The name the earlier export grouped a filament preset by: its base preset's name without the
// " @printer" tail ("Generic PLA @U1" -> "Generic PLA"; a name without '@' is kept whole).
std::string filament_group_name(const std::string &base_preset_name);

// For each visible printer preset: select it on `bundle` (a scratch copy) and keep the user
// filament presets compatible with it (not system, not the default, not embedded in a project).
UserPresetsByPrinter collect_user_filament_presets(PresetBundle &bundle);

// The user filament presets the earlier export could list (see above). Pointers into
// bundle.filaments.
std::vector<const Preset *> collect_exportable_user_filaments(const PresetBundle &bundle);

// One row per preset of `exportable`; the printers come from `by_printer` as ExportPrinterIndex
// says (a filament preset compatible with no known printer is still a row, with no printers).
// `filaments` finds each preset's base for the group name.
PresetExportModel build_filament_export_model(const UserPresetsByPrinter &by_printer, const std::vector<const Preset *> &exportable,
                                              const PresetCollection &printers, const PresetCollection &filaments);

// Convenience for tests and tools: the collectors + build_filament_export_model().
PresetExportModel build_filament_export_model(PresetBundle &scratch_bundle);

} // namespace Slic3r
