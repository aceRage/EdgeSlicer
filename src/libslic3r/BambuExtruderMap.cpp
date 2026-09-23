#include "BambuExtruderMap.hpp"

namespace Slic3r { namespace BambuExtruderMap {

int logical_to_physical(const std::vector<int> &physical_extruder_map, int logical)
{
    if (logical >= 0 && logical < int(physical_extruder_map.size()))
        return physical_extruder_map[size_t(logical)];
    return logical;
}

int physical_to_logical(const std::vector<int> &physical_extruder_map, int physical)
{
    for (size_t logical = 0; logical < physical_extruder_map.size(); ++logical)
        if (physical_extruder_map[logical] == physical)
            return int(logical);
    return physical;
}

bool is_ams_ht_id(int ams_id) { return ams_id >= 128 && ams_id < 153; }

bool is_external_spool_ams_id(int ams_id) { return ams_id == 254 || ams_id == 255; }

std::string tray_name(int ams_id, int slot_id)
{
    if (is_external_spool_ams_id(ams_id))
        return "Ext";
    if (is_ams_ht_id(ams_id))
        return "HT-" + std::string(1, char('A' + (ams_id - 128)));
    if (ams_id >= 0 && ams_id < 26 && slot_id >= 0 && slot_id < 9)
        return std::string(1, char('A' + ams_id)) + std::string(1, char('1' + slot_id));
    return "A1";
}

std::vector<int> filaments_on_wrong_extruder(const std::vector<int>        &filament_map,
                                             const std::vector<int>        &physical_extruder_map,
                                             const std::vector<MappedTray> &mapped)
{
    std::vector<int> wrong;
    for (const MappedTray &m : mapped) {
        if (m.filament_id < 0 || m.filament_id >= int(filament_map.size()) || m.tray_physical_extruder < 0)
            continue;
        const int logical = filament_map[size_t(m.filament_id)] - 1;
        if (logical < 0)
            continue;
        if (logical_to_physical(physical_extruder_map, logical) != m.tray_physical_extruder)
            wrong.push_back(m.filament_id);
    }
    return wrong;
}

}} // namespace Slic3r::BambuExtruderMap
