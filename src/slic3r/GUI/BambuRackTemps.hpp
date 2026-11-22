#ifndef slic3r_GUI_BambuRackTemps_hpp_
#define slic3r_GUI_BambuRackTemps_hpp_

// The temperature of each nozzle in a Bambu nozzle rack (the H3C's rack of up to nine), read-only.
// Kept free of wx and DeviceManager so it can be unit tested on its own
// (tests/slic3rutils/bambu_rack_temps_tests.cpp); phase 4 of tests/design_device_multi_tool_temps.md.
//
// Wire format, from Bambu Studio's parser (DevNozzleSystemParser, DevNozzleSystem.cpp; we have no
// captured H3C report with these fields yet): each device.nozzle.info[] entry may carry
//   "temp": low 16 bits the current temperature, high 16 bits the target (as device.extruder.info[]);
//   "stat": bits 3-4 the heating state 0 idle / 1 heating / 2 keeping / 3 cooling - Bambu Studio
//           reads these only on a direct-connected rack (the H3C), so do we.
// Both are optional. A missing or odd value leaves the nozzle without a reading; nothing here throws.
//
// Rack positions: an entry with id 0x10 + n sits in the rack; its position is addr_id (0-based) or,
// when the report has none, id & 0xF. The nozzle on the extruder is the entry with id 0 / 1 whose
// addr_id names the rack position it came from (Bambu Studio PageHotend IsNozzleOnExtruder): that
// position is "mounted", and its reading is the extruder's own row in heaters[].
//
// Nothing here can set a temperature (owner decision D4): every rack entry is settable = false
// until an H3C hand test has proven which command heats which nozzle (phase 5).

#include "DeviceControls.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace BambuRackTemps {

// One device.nozzle.info[] entry, as far as this needs it (DeviceManager's struct Nozzle).
struct NozzleSample
{
    int  id { 0 };          // report id: 0 / 1 extruder nozzle, 0x10 + n rack position n
    int  addr_id { -1 };    // rack position, -1 = not reported
    bool exist { true };    // its bit in device.nozzle.exist
    bool has_temp { false };
    int  temp { 0 }, target { 0 };
    int  temp_state { -1 }; // stat bits 3-4, -1 = no "stat"
};

inline bool on_rack(int report_id) { return ((report_id >> 4) & 0xF) == 1; }

// The temperature fields of one device.nozzle.info[] entry. Never throws; leaves the outputs as
// they are for anything that is not there or not a whole number.
inline void parse_nozzle_temp(const nlohmann::json& entry, int& temp, int& target, bool& has_temp, int& temp_state)
{
    try {
        if (!entry.is_object()) return;
        const auto t = entry.find("temp");
        if (t != entry.end() && t->is_number_integer()) {
            const long long v = t->get<long long>();
            if (v >= 0) {
                temp     = int(v & 0xFFFF);
                target   = int((v >> 16) & 0xFFFF);
                has_temp = true;
            }
        }
        const auto s = entry.find("stat");
        if (s != entry.end() && s->is_number_integer()) temp_state = int((s->get<long long>() >> 3) & 0x3);
    } catch (...) {}
}

inline std::string state_name(int temp_state)
{
    switch (temp_state) {
    case 0: return "idle";
    case 1: return "heating";
    case 2: return "holding"; // Bambu Studio's "Keeping"
    case 3: return "cooling";
    default: return std::string();
    }
}

// The rack as controls.rack[] entries, one per position the report names, in position order.
// Empty unless at least one rack entry reported a temperature: a rack that sends none (an H2C, as
// far as we know) and every printer without a rack keep the controls JSON they had.
// `direct_rack`: the H3C's kind of rack (MachineObject::has_rack_ams), the only one whose state bits
// Bambu Studio reads. `nozzle_max`: the printer's nozzle limit, <= 0 for the default.
inline std::vector<DeviceControls::Heater> rack_heaters(const std::vector<NozzleSample>& nozzles, bool direct_rack, int nozzle_max)
{
    using DeviceControls::Heater;
    std::vector<Heater> out;
    bool any_temp = false;
    for (const NozzleSample& n : nozzles)
        if (on_rack(n.id) && n.has_temp) any_temp = true;
    if (!any_temp) return out;

    const int max = nozzle_max > 0 ? nozzle_max : DeviceControls::DEFAULT_NOZZLE_MAX;
    std::map<int, Heater> by_pos;
    for (const NozzleSample& n : nozzles) {
        if (!on_rack(n.id)) continue;
        const int pos = n.addr_id >= 0 ? n.addr_id : (n.id & 0xF);
        if (pos < 0 || pos > 15 || by_pos.count(pos)) continue;
        Heater h;
        h.id       = DeviceControls::rack_id(pos);
        h.label    = "Rack " + std::to_string(pos + 1);
        h.group    = "rack";
        h.present  = n.exist;
        h.has_temp = n.exist && n.has_temp;
        h.temp     = h.has_temp ? n.temp : 0;
        h.target   = h.has_temp ? n.target : 0;
        h.max      = max;
        h.settable = false; // D4: read-only until the H3C hand test (phase 5)
        if (direct_rack && h.has_temp) h.state = state_name(n.temp_state);
        by_pos[pos] = h;
    }
    // The nozzle on the extruder: its position is taken, and its reading is the extruder's row.
    for (const NozzleSample& n : nozzles) {
        if (on_rack(n.id) || n.addr_id < 0 || n.addr_id > 15) continue;
        Heater h;
        h.id       = DeviceControls::rack_id(n.addr_id);
        h.label    = "Rack " + std::to_string(n.addr_id + 1);
        h.group    = "rack";
        h.present  = true;
        h.mounted  = true;
        h.has_temp = false;
        h.max      = max;
        h.settable = false;
        by_pos[n.addr_id] = h;
    }
    for (auto& kv : by_pos) out.push_back(kv.second);
    return out;
}

}}} // namespace Slic3r::GUI::BambuRackTemps

#endif // slic3r_GUI_BambuRackTemps_hpp_
