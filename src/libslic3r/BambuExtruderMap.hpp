#pragma once

#include <string>
#include <vector>

namespace Slic3r { namespace BambuExtruderMap {

// Bambu's two-extruder printers (H2D, H2D Pro, H2C) number their extruders twice.
//
// The slicer counts LOGICAL extruders: 0 = left, 1 = right. filament_map (1-based: 1 = left,
// 2 = right), extruder_ams_count, and the per-extruder machine filament lists the grouping
// reads are all in that order.
//
// The printer's MQTT report counts PHYSICAL extruders: 0 is the main extruder, which on these
// machines is the RIGHT one. The AMS "info" bits (Ams::nozzle in DeviceManager) carry the
// physical id of the extruder each AMS unit feeds.
//
// The printer profile's physical_extruder_map says physical = map[logical]; it is [1, 0] on
// every current Bambu two-extruder machine. A missing or short map means identity.

// Physical id of a logical extruder.
int logical_to_physical(const std::vector<int> &physical_extruder_map, int logical);
// Logical index of a physical extruder id.
int physical_to_logical(const std::vector<int> &physical_extruder_map, int physical);

// MQTT AMS ids: 0..3 are four-slot AMS / AMS 2 Pro units, 128..152 are one-slot AMS HT units,
// 254 and 255 are the external spool holders ("virtual trays").
bool is_ams_ht_id(int ams_id);
bool is_external_spool_ams_id(int ams_id);

// The tray name the grouping's machine-filament builder expects, as BambuStudio's
// Sidebar::build_filament_ams_list writes it: "A1".."D4" for four-slot units, "HT-A".. for
// AMS HT units and "Ext" for the external spool. Only "Ext" marks an external spool, which
// the grouping treats differently; an AMS HT is an ordinary AMS slot.
std::string tray_name(int ams_id, int slot_id);

struct MappedTray
{
    int filament_id{ -1 };            // 0-based filament index
    int tray_physical_extruder{ -1 }; // physical extruder the tray's AMS feeds; -1 unknown
};

// filament_map: 1-based logical extruder per filament as sliced (1 = left, 2 = right).
// Returns, in input order, the 0-based filament ids whose mapped tray feeds a different
// extruder than the one the sliced G-code prints them with. Such a job cannot print as
// sliced: the printer either refuses it or re-arranges the filaments on its own. Filaments
// without a known tray extruder or sliced extruder are not reported.
std::vector<int> filaments_on_wrong_extruder(const std::vector<int>        &filament_map,
                                             const std::vector<int>        &physical_extruder_map,
                                             const std::vector<MappedTray> &mapped);

}} // namespace Slic3r::BambuExtruderMap
