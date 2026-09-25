#include "BambuDualNozzleSync.hpp"
#include "BambuExtruderMap.hpp"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <tuple>

namespace Slic3r { namespace DualNozzleSync {

namespace {

// "#rrggbbaa" / "RRGGBB" -> "RRGGBB" (upper case, alpha dropped); "" when unparseable.
std::string norm_color(const std::string &in)
{
    std::string s;
    for (char c : in)
        if (c != '#' && !std::isspace((unsigned char) c))
            s.push_back((char) std::toupper((unsigned char) c));
    if (s.size() < 6)
        return "";
    s.resize(6);
    for (char c : s)
        if (!std::isxdigit((unsigned char) c))
            return "";
    return s;
}

bool rgb_of(const std::string &in, int &r, int &g, int &b)
{
    const std::string s = norm_color(in);
    if (s.empty())
        return false;
    r = (int) std::strtol(s.substr(0, 2).c_str(), nullptr, 16);
    g = (int) std::strtol(s.substr(2, 2).c_str(), nullptr, 16);
    b = (int) std::strtol(s.substr(4, 2).c_str(), nullptr, 16);
    return true;
}

std::string upper(std::string s)
{
    for (char &c : s) c = (char) std::toupper((unsigned char) c);
    return s;
}

bool same_diameter(const std::string &reported, double preset)
{
    if (reported.empty() || preset <= 0.)
        return true; // nothing to compare against
    char *end = nullptr;
    const double d = std::strtod(reported.c_str(), &end);
    if (end == reported.c_str())
        return true;
    return std::abs(d - preset) < 0.01;
}

const PrinterState::TrayOnSide *find_tray(const std::vector<PrinterState::TrayOnSide> &trays, const TrayRef &ref)
{
    for (const auto &t : trays)
        if (t.tray.ams_id == ref.ams_id && t.tray.slot_id == ref.slot_id)
            return &t;
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------------------------

int PrinterState::logical_extruder_of(const AmsUnit &unit) const
{
    if (unit.physical_extruder < 0)
        return -1;
    return BambuExtruderMap::physical_to_logical(physical_extruder_map, unit.physical_extruder);
}

int PrinterState::logical_extruder_of(const PrinterNozzle &nozzle) const
{
    const int physical = nozzle.on_rack() ? 0 : nozzle.pos;
    if (physical < 0)
        return -1;
    return BambuExtruderMap::physical_to_logical(physical_extruder_map, physical);
}

std::vector<PrinterState::TrayOnSide> PrinterState::all_trays() const
{
    std::vector<TrayOnSide> out;
    for (const AmsUnit &unit : ams) {
        if (BambuExtruderMap::is_external_spool_ams_id(unit.ams_id))
            continue;
        const int side = logical_extruder_of(unit);
        for (const Tray &t : unit.trays)
            out.push_back({ t, side, unit.slot_count });
    }
    return out;
}

// ---------------------------------------------------------------------------------------------

std::vector<std::string> save_extruder_ams_count_to_string(const std::vector<std::map<int, int>> &extruder_ams_count)
{
    std::vector<std::string> out;
    for (const auto &item : extruder_ams_count) {
        std::ostringstream oss;
        for (auto it = item.begin(); it != item.end(); ++it) {
            oss << it->first << "#" << it->second;
            if (std::next(it) != item.end())
                oss << "|";
        }
        out.push_back(oss.str());
    }
    return out;
}

std::vector<std::string> save_extruder_nozzle_stats_to_string(const std::vector<std::map<NozzleVolumeType, int>> &stats)
{
    std::vector<std::string> out;
    for (const auto &item : stats) {
        std::ostringstream oss;
        for (auto it = item.begin(); it != item.end(); ++it) {
            oss << get_nozzle_volume_type_string(it->first) << "#" << it->second;
            if (std::next(it) != item.end())
                oss << "|";
        }
        out.push_back(oss.str());
    }
    return out;
}

std::vector<std::map<int, int>> extruder_ams_counts(const PrinterState &state, size_t extruder_count)
{
    std::vector<std::map<int, int>> counts(extruder_count, std::map<int, int>{ { 1, 0 }, { 4, 0 } });
    for (const AmsUnit &unit : state.ams) {
        if (BambuExtruderMap::is_external_spool_ams_id(unit.ams_id))
            continue;
        const int side = state.logical_extruder_of(unit);
        if (side < 0 || side >= (int) extruder_count)
            continue;
        counts[size_t(side)][unit.slot_count == 1 ? 1 : 4] += 1;
    }
    return counts;
}

std::vector<std::string> extruder_ams_count_strings(const PrinterState &state, size_t extruder_count)
{
    return save_extruder_ams_count_to_string(extruder_ams_counts(state, extruder_count));
}

std::vector<std::map<NozzleVolumeType, int>> extruder_nozzle_stats(const PrinterState &state, size_t extruder_count,
                                                                   const std::vector<double> &preset_diameters)
{
    std::vector<std::map<NozzleVolumeType, int>> stats(extruder_count);
    for (const PrinterNozzle &n : state.nozzles) {
        if (!n.normal)
            continue;
        const int side = state.logical_extruder_of(n);
        if (side < 0 || side >= (int) extruder_count)
            continue;
        const double preset = size_t(side) < preset_diameters.size() ? preset_diameters[size_t(side)] : 0.;
        if (!same_diameter(n.diameter, preset))
            continue;
        stats[size_t(side)][n.volume] += 1;
    }
    return stats;
}

std::vector<std::string> extruder_nozzle_stats_strings(const PrinterState &state, size_t extruder_count,
                                                       const std::vector<double> &preset_diameters)
{
    return save_extruder_nozzle_stats_to_string(extruder_nozzle_stats(state, extruder_count, preset_diameters));
}

std::string state_fingerprint(const PrinterState &state)
{
    if (!state.has_report)
        return "";
    std::vector<std::string> ams_parts;
    for (const AmsUnit &unit : state.ams) {
        if (BambuExtruderMap::is_external_spool_ams_id(unit.ams_id))
            continue;
        std::vector<std::string> trays;
        for (const Tray &t : unit.trays)
            trays.push_back(std::to_string(t.slot_id) + ":" + norm_color(t.color) + ":" + upper(t.type));
        std::sort(trays.begin(), trays.end());
        std::string s = "a" + std::to_string(unit.ams_id) + "@" + std::to_string(state.logical_extruder_of(unit)) + "x" +
                        std::to_string(unit.slot_count) + "[";
        for (const auto &t : trays) s += t + ",";
        ams_parts.push_back(s + "]");
    }
    std::sort(ams_parts.begin(), ams_parts.end());
    // Nozzles by the extruder they serve, not by where they sit: the H2C swaps nozzles between its
    // hotend and the rack slots during a print, which changes nothing a slice depends on (the
    // G-code reads extruder_nozzle_stats, the per-extruder count of each diameter and flow type).
    std::vector<std::string> noz_parts;
    for (const PrinterNozzle &n : state.nozzles)
        noz_parts.push_back("n" + std::to_string(state.logical_extruder_of(n)) + ":" + n.diameter + ":" +
                            get_nozzle_volume_type_string(n.volume) + (n.normal ? "" : ":bad"));
    std::sort(noz_parts.begin(), noz_parts.end());
    std::string fp = "dev=" + state.dev_id + ";";
    for (const auto &s : ams_parts) fp += s + ";";
    for (const auto &s : noz_parts) fp += s + ";";
    return fp;
}

// ---------------------------------------------------------------------------------------------

double color_distance(const std::string &a, const std::string &b)
{
    int r1, g1, b1, r2, g2, b2;
    if (!rgb_of(a, r1, g1, b1) || !rgb_of(b, r2, g2, b2))
        return 1000.;
    const double rmean = (r1 + r2) / 2.0;
    const double dr = r1 - r2, dg = g1 - g2, db = b1 - b2;
    return std::sqrt((2.0 + rmean / 256.0) * dr * dr + 4.0 * dg * dg + (2.0 + (255.0 - rmean) / 256.0) * db * db);
}

std::vector<Tray> trays_for_extruder(const PrinterState &state, int logical_extruder)
{
    std::vector<Tray> out;
    for (const auto &t : state.all_trays())
        if (t.logical_extruder == logical_extruder)
            out.push_back(t.tray);
    return out;
}

Arrangement propose_arrangement(const PrinterState &state, const std::vector<ProjectFilament> &used,
                                size_t filament_count, const std::vector<int> &prior_map)
{
    Arrangement arr;
    arr.filament_map.assign(filament_count, 1);
    for (size_t i = 0; i < filament_count && i < prior_map.size(); ++i)
        arr.filament_map[i] = (prior_map[i] == 2) ? 2 : 1;
    if (!state.has_report)
        return arr;

    const auto trays = state.all_trays();
    if (trays.empty())
        return arr;

    // Cost of putting filament f on tray t: colour distance, plus a penalty when both name a
    // filament type and the types differ (the printer would refuse a PETG spool for a PLA part).
    struct Cand { double cost; size_t f; size_t t; };
    std::vector<Cand> cands;
    for (size_t f = 0; f < used.size(); ++f)
        for (size_t t = 0; t < trays.size(); ++t) {
            if (trays[t].logical_extruder < 0)
                continue;
            double cost = color_distance(used[f].color, trays[t].tray.color);
            if (!used[f].type.empty() && !trays[t].tray.type.empty() && upper(used[f].type) != upper(trays[t].tray.type))
                cost += 500.;
            cands.push_back({ cost, f, t });
        }
    std::stable_sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) {
        return std::tie(a.cost, a.f, a.t) < std::tie(b.cost, b.f, b.t);
    });

    std::vector<int>  tray_of(used.size(), -1);
    std::vector<bool> tray_used(trays.size(), false);
    for (const Cand &c : cands)
        if (tray_of[c.f] < 0 && !tray_used[c.t]) {
            tray_of[c.f]    = int(c.t);
            tray_used[c.t] = true;
        }
    // More filaments than trays: the rest share their closest tray, which still picks a side.
    for (const Cand &c : cands)
        if (tray_of[c.f] < 0)
            tray_of[c.f] = int(c.t);

    for (size_t f = 0; f < used.size(); ++f) {
        const int idx = used[f].index;
        if (idx < 0 || tray_of[f] < 0)
            continue;
        const auto &t = trays[size_t(tray_of[f])];
        if (size_t(idx) < arr.filament_map.size())
            arr.filament_map[size_t(idx)] = t.logical_extruder + 1;
        arr.trays[idx] = TrayRef{ t.tray.ams_id, t.tray.slot_id };
    }
    return arr;
}

bool all_on_one_side_with_trays_on_both(const Arrangement &arr, const PrinterState &state,
                                        const std::vector<ProjectFilament> &used)
{
    if (used.size() < 2 || !state.has_report)
        return false;
    std::set<int> tray_sides;
    for (const auto &t : state.all_trays())
        if (t.logical_extruder >= 0)
            tray_sides.insert(t.logical_extruder);
    if (tray_sides.size() < 2)
        return false;
    std::set<int> used_sides;
    for (const auto &f : used)
        if (f.index >= 0 && size_t(f.index) < arr.filament_map.size())
            used_sides.insert(arr.filament_map[size_t(f.index)]);
    return used_sides.size() == 1;
}

std::vector<Issue> validate_arrangement(const Arrangement &arr, const PrinterState &state,
                                        const std::vector<ProjectFilament> &used, size_t extruder_count,
                                        const std::vector<double> &preset_diameters)
{
    std::vector<Issue> issues;
    const auto trays = state.all_trays();
    std::set<int> sides_used;
    for (const auto &f : used) {
        if (f.index < 0 || size_t(f.index) >= arr.filament_map.size())
            continue;
        const int side = arr.filament_map[size_t(f.index)] - 1;
        sides_used.insert(side);
        auto it = arr.trays.find(f.index);
        if (it == arr.trays.end() || !it->second.valid())
            continue;
        const auto *t = find_tray(trays, it->second);
        if (!t) {
            if (state.has_report)
                issues.push_back({ Issue::Kind::TrayMissing, f.index, it->second, side });
            continue;
        }
        if (t->logical_extruder >= 0 && t->logical_extruder != side)
            issues.push_back({ Issue::Kind::TrayOnOtherExtruder, f.index, it->second, side });
    }
    if (state.has_report && !state.nozzles.empty()) {
        const auto stats = extruder_nozzle_stats(state, extruder_count, preset_diameters);
        for (int side : sides_used) {
            if (side < 0 || side >= (int) stats.size())
                continue;
            int total = 0;
            for (const auto &kv : stats[size_t(side)]) total += kv.second;
            if (total == 0)
                issues.push_back({ Issue::Kind::NoNozzleOnExtruder, -1, TrayRef{}, side });
        }
    }
    return issues;
}

// ---------------------------------------------------------------------------------------------

std::string Confirmation::serialize() const
{
    nlohmann::json j;
    j["v"]      = 1;
    j["dev"]    = dev_id;
    j["state"]  = state_fp;
    j["fils"]   = filaments_fp;
    j["map"]    = filament_map;
    j["synced"] = synced;
    nlohmann::json trays_j = nlohmann::json::object();
    for (const auto &kv : trays)
        if (kv.second.valid())
            trays_j[std::to_string(kv.first)] = { kv.second.ams_id, kv.second.slot_id };
    j["trays"] = trays_j;
    return j.dump();
}

Confirmation Confirmation::deserialize(const std::string &s)
{
    Confirmation c;
    if (s.empty())
        return c;
    try {
        const auto j = nlohmann::json::parse(s);
        if (!j.is_object() || j.value("v", 0) != 1)
            return c;
        c.dev_id       = j.value("dev", std::string());
        c.state_fp     = j.value("state", std::string());
        c.filaments_fp = j.value("fils", std::string());
        c.synced       = j.value("synced", false);
        if (j.contains("map") && j["map"].is_array())
            for (const auto &v : j["map"])
                c.filament_map.push_back(v.is_number_integer() ? v.get<int>() : 1);
        if (j.contains("trays") && j["trays"].is_object())
            for (auto it = j["trays"].begin(); it != j["trays"].end(); ++it)
                if (it.value().is_array() && it.value().size() == 2)
                    c.trays[std::atoi(it.key().c_str())] = TrayRef{ it.value()[0].get<int>(), it.value()[1].get<int>() };
    } catch (...) {
        return Confirmation();
    }
    return c;
}

std::string filaments_fingerprint(const std::vector<ProjectFilament> &used)
{
    std::vector<ProjectFilament> sorted = used;
    std::sort(sorted.begin(), sorted.end(), [](const ProjectFilament &a, const ProjectFilament &b) { return a.index < b.index; });
    std::string fp;
    for (const auto &f : sorted)
        fp += std::to_string(f.index) + ":" + norm_color(f.color) + ":" + upper(f.type) + ";";
    return fp;
}

ConfirmReason needs_confirmation(const Confirmation &stored, const PrinterState &state,
                                 const std::vector<ProjectFilament> &used, const std::vector<int> &current_map)
{
    if (stored.empty())
        return ConfirmReason::NeverConfirmed;
    if (current_map.empty())
        return ConfirmReason::MapChanged;
    for (const auto &f : used) {
        if (f.index < 0)
            continue;
        const size_t i = size_t(f.index);
        const int cur = i < current_map.size() ? current_map[i] : 1;
        const int was = i < stored.filament_map.size() ? stored.filament_map[i] : 1;
        if (cur != was)
            return ConfirmReason::MapChanged;
    }
    if (filaments_fingerprint(used) != stored.filaments_fp)
        return ConfirmReason::FilamentsChanged;
    if (!state.has_report)
        return ConfirmReason::None; // offline: the stored confirmation stands
    if (!stored.synced)
        return ConfirmReason::NowSynced;
    if (stored.dev_id != state.dev_id)
        return ConfirmReason::PrinterChanged;
    if (stored.state_fp != state_fingerprint(state))
        return ConfirmReason::PrinterStateChanged;
    return ConfirmReason::None;
}

SliceGate slice_gate(ConfirmReason reason, SliceTrigger trigger)
{
    if (reason == ConfirmReason::None)
        return SliceGate::Proceed;
    switch (trigger) {
    case SliceTrigger::User: return SliceGate::ShowDialog;
    case SliceTrigger::Background: return SliceGate::Defer;
    case SliceTrigger::Remote: return SliceGate::Proceed;
    }
    return SliceGate::ShowDialog;
}

bool slice_invalidated_by(const std::string &sliced_dev, const std::string &sliced_fp, const PrinterState &state)
{
    if (!state.has_report)
        return false;
    if (sliced_dev.empty() || sliced_dev != state.dev_id)
        return true;
    return sliced_fp != state_fingerprint(state);
}

}} // namespace Slic3r::DualNozzleSync
