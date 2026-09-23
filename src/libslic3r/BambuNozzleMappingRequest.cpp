#include "BambuNozzleMappingRequest.hpp"
#include "BambuExtruderMap.hpp"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace Slic3r { namespace BambuNozzleMapping {

using nlohmann::json;

std::string diameter_str(double d)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", (float) d);
    return buf;
}

std::string diameter_str(const std::string &d)
{
    if (d.empty())
        return d;
    char *end = nullptr;
    const double v = std::strtod(d.c_str(), &end);
    if (end == d.c_str())
        return d;
    return diameter_str(v);
}

// DevNozzle::ToNozzleFlowString
static std::string flow_string(NozzleVolumeType v)
{
    switch (v) {
    case nvtStandard:    return "Standard";
    case nvtHighFlow:    return "High Flow";
    case nvtTPUHighFlow: return "TPU High Flow";
    case nvtE3DHighFlow: return "E3D High Flow";
    default:             return "Standard";
    }
}

std::string build_v0_request(const RequestInput &in)
{
    json command_jj;
    command_jj["print"]["command"] = "get_auto_nozzle_mapping";
    // The network agent overwrites this with its own MQTT sequence id before publishing.
    command_jj["print"]["sequence_id"]              = "0";
    command_jj["print"]["calibration"]              = in.calibration;
    command_jj["print"]["extrude_cali_manual_mode"] = in.extrude_cali_manual_mode;

    // filament_seq. Bambu indexes a null json with an integer (filament_seq_jj[fila_id + 1] = idx),
    // which makes it an ARRAY indexed by the 1-based filament id, -1 for unused ids; an object
    // keyed by strings is a different payload.
    json filament_seq_jj = json::array();
    {
        int           max_fila_id = 0;
        std::set<int> seen;
        std::vector<int> first(1, -1);
        for (int idx = 0; idx < (int) in.filament_change_sequence.size(); ++idx) {
            const int fila_id = in.filament_change_sequence[size_t(idx)];
            if (fila_id < 0 || !seen.insert(fila_id).second)
                continue;
            if ((int) first.size() <= fila_id + 1)
                first.resize(size_t(fila_id) + 2, -1);
            first[size_t(fila_id) + 1] = idx;
            max_fila_id = std::max(max_fila_id, fila_id + 1);
        }
        first.resize(size_t(max_fila_id) + 1, -1);
        for (int v : first)
            filament_seq_jj.push_back(v);
    }
    command_jj["print"]["filament_seq"] = filament_seq_jj;

    std::vector<int> ams_mapping_vec(33, 0xFFFF);
    for (const auto &item : in.mapped) {
        if (item.id + 1 < 0 || item.id + 1 >= (int) ams_mapping_vec.size())
            continue;
        try {
            const int ams_id  = std::stoi(item.ams_id);
            const int slot_id = item.slot_id.empty() ? 0 : std::stoi(item.slot_id);
            ams_mapping_vec[size_t(item.id + 1)] = (ams_id << 8) | slot_id;
        } catch (...) {
            ams_mapping_vec[size_t(item.id + 1)] = item.tray_id;
        }
    }
    command_jj["print"]["ams_mapping"] = ams_mapping_vec;

    json filament_info_jj = json::array();
    for (const auto &fila : in.mapped) {
        for (const auto &nz : in.filament_nozzles) {
            if (nz.filament != fila.id)
                continue;
            json item;
            item["id"]        = fila.id + 1;
            item["direction"] = nz.logical_extruder == 0 ? 1 : 2;
            item["group"]     = nz.group_id;
            std::string d     = nz.diameter;
            if (d.empty() && size_t(nz.logical_extruder) < in.preset_diameters.size())
                d = diameter_str(in.preset_diameters[size_t(nz.logical_extruder)]);
            item["nozzle_d"] = diameter_str(d);
            // Bambu writes only these two names here (DevMappingNozzle.cpp:128).
            item["nozzle_v"] = nz.volume == nvtHighFlow ? "High Flow" : "Standard";
            item["cate"]     = fila.filament_id;
            item["color"]    = fila.color;
            filament_info_jj.push_back(item);
        }
    }
    command_jj["print"]["fila_info"] = filament_info_jj;

    json nozzle_info_jj = json::array();
    if (!in.printer_nozzles.empty()) {
        std::vector<DualNozzleSync::PrinterNozzle> ext, rack;
        for (const auto &n : in.printer_nozzles)
            (n.on_rack() ? rack : ext).push_back(n);
        auto by_pos = [](const DualNozzleSync::PrinterNozzle &a, const DualNozzleSync::PrinterNozzle &b) { return a.pos < b.pos; };
        std::sort(ext.begin(), ext.end(), by_pos);
        std::sort(rack.begin(), rack.end(), by_pos);
        for (const auto *list : { &ext, &rack })
            for (const auto &n : *list) {
                if (!n.normal)
                    continue;
                json item;
                item["pos"]      = n.pos;
                item["nozzle_d"] = diameter_str(n.diameter);
                item["nozzle_v"] = flow_string(n.volume);
                item["wear"]     = (float) std::max(0, n.wear);
                item["cate"]     = n.fila_id;
                item["color"]    = n.color;
                nozzle_info_jj.push_back(item);
            }
    } else {
        // No nozzle report: the preset's two extruder nozzles, pos = physical extruder id.
        for (size_t i = 0; i < in.preset_diameters.size(); ++i) {
            json item;
            item["pos"]      = BambuExtruderMap::logical_to_physical(in.physical_extruder_map, int(i));
            item["nozzle_d"] = diameter_str(in.preset_diameters[i]);
            item["nozzle_v"] = flow_string(i < in.preset_volumes.size() ? in.preset_volumes[i] : nvtStandard);
            nozzle_info_jj.push_back(item);
        }
    }
    command_jj["print"]["nozzle_info"] = nozzle_info_jj;
    return command_jj.dump();
}

}} // namespace Slic3r::BambuNozzleMapping
