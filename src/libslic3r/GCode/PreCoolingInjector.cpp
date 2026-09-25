#include "PreCoolingInjector.hpp"

#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <limits>
#include <regex>

namespace Slic3r {
namespace PreCooling {

namespace {

// Marker lines, as GCode.cpp writes them (without the leading ';').
const std::string MachineStartGCodeEndTag = MachineStartGCodeEndMarker;
const std::string MachineEndGCodeStartTag = MachineEndGCodeStartMarker;
const std::string NozzleChangeStartTag    = " NOZZLE_CHANGE_START";
const std::string NozzleChangeEndTag      = " NOZZLE_CHANGE_END";
const std::string ToolchangeWipeTag       = " CP_TOOLCHANGE_WIPE";
const std::string SkippableStartTag       = " SKIPPABLE_START";
const std::string SkippableEndTag         = " SKIPPABLE_END";

constexpr unsigned int NoLine = (unsigned int) -1;

std::string_view without_eol(const std::string &line)
{
    std::string_view v(line);
    while (!v.empty() && (v.back() == '\n' || v.back() == '\r'))
        v.remove_suffix(1);
    return v;
}

std::string_view skip_whitespace(std::string_view v)
{
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
        v.remove_prefix(1);
    return v;
}

bool starts_with(std::string_view v, std::string_view prefix) { return v.size() >= prefix.size() && v.compare(0, prefix.size(), prefix) == 0; }

// Parses the integer at the start of v; returns false when there is none.
bool parse_int(std::string_view &v, int &out)
{
    v = skip_whitespace(v);
    size_t i = 0;
    if (i < v.size() && (v[i] == '-' || v[i] == '+'))
        ++i;
    const size_t digits = i;
    while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i])))
        ++i;
    if (i == digits)
        return false;
    out = std::atoi(std::string(v.substr(0, i)).c_str());
    v.remove_prefix(i);
    return true;
}

// "<fid>[ ... H<nozzle>]" as BambuStudio reads the T / ;VT / M1020 S commands: the filament id, then
// the value of the first H parameter, -1 without one.
bool parse_filament_and_nozzle(std::string_view v, int &filament_id, int &nozzle_id)
{
    if (!parse_int(v, filament_id) || filament_id < 0 || filament_id >= 255)
        return false;
    nozzle_id = -1;
    for (v = skip_whitespace(v); !v.empty(); v = skip_whitespace(v)) {
        if (v.front() == ';')
            break;
        const char param = v.front();
        v.remove_prefix(1);
        if (param == 'H') {
            if (!parse_int(v, nozzle_id))
                nozzle_id = -1;
            break;
        }
        // skip the rest of this parameter
        while (!v.empty() && v.front() != ' ' && v.front() != '\t')
            v.remove_prefix(1);
    }
    return true;
}

bool parse_nozzle_change(std::string_view line, int &old_filament, int &new_filament, int &old_nozzle, int &new_nozzle)
{
    static const std::regex re(R"(OF(\d+)\s+NF(\d+)\s+ON(\d+)\s+NN(\d+))");
    std::match_results<std::string_view::const_iterator> m;
    if (!std::regex_search(line.begin(), line.end(), m, re))
        return false;
    old_filament = std::stoi(m[1].str());
    new_filament = std::stoi(m[2].str());
    old_nozzle   = std::stoi(m[3].str());
    new_nozzle   = std::stoi(m[4].str());
    return true;
}

std::string extruder_type_name(int type) { return type == int(ExtruderType::etBowden) ? "Bowden" : "Direct Drive"; }

// A per-extruder-variant printer value (hotend_cooling_rate / hotend_heating_rate: one entry per
// printer_extruder_variant) for logical extruder e. Bambu Studio resolves these to one value per
// extruder when it loads the preset (update_values_to_printer_extruders); this tree keeps the
// variant list, so pick the entry of the extruder's variant here.
double extruder_variant_value(const PrintConfig &cfg, const std::vector<double> &values, size_t e, double fallback)
{
    if (values.empty())
        return fallback;
    const std::vector<int>         &ids      = cfg.printer_extruder_id.values;
    const std::vector<std::string> &variants = cfg.printer_extruder_variant.values;
    if (ids.size() == values.size() && variants.size() == values.size()) {
        const int         type = e < cfg.extruder_type.values.size() ? cfg.extruder_type.values[e] : int(ExtruderType::etDirectDrive);
        NozzleVolumeType  nvt  = e < cfg.nozzle_volume_type.values.size() ? NozzleVolumeType(cfg.nozzle_volume_type.values[e]) :
                                                                         NozzleVolumeType::nvtStandard;
        if (nvt == NozzleVolumeType::nvtHybrid)
            nvt = NozzleVolumeType::nvtStandard; // Bambu: hybrid is not a preset variant
        const std::string wanted = extruder_type_name(type) + " " + get_nozzle_volume_type_string(nvt);
        int               first  = -1;
        for (size_t i = 0; i < ids.size(); ++i) {
            if (ids[i] != int(e + 1))
                continue;
            if (first < 0)
                first = int(i);
            if (variants[i] == wanted)
                return values[i];
        }
        if (first >= 0)
            return values[size_t(first)];
    }
    return values[std::min(e, values.size() - 1)];
}

} // namespace

bool pre_cooling_active(const Print &print)
{
    const PrintConfig &cfg = print.config();
    if (!print.is_BBL_printer() || !cfg.enable_pre_heating.value || cfg.nozzle_diameter.size() < 2)
        return false;
    const auto group = print.get_layered_nozzle_group_result();
    if (!group)
        return false;
    // Cooling a hotend the grouping calls idle while the printer really prints from it would ruin
    // the print, so the grouping has to describe this plate: every printed filament has a nozzle, on
    // the extruder filament_map routes it to (a result left over from an earlier slice of the same
    // Print does not qualify).
    const std::vector<unsigned int> printed = print.extruders(true);
    if (printed.empty())
        return false;
    const bool dynamic = group->is_support_dynamic_nozzle_map();
    for (unsigned int f : printed) {
        const auto nozzle = group->get_nozzle_for_filament(int(f), -1);
        if (!nozzle || nozzle->extruder_id < 0 || size_t(nozzle->extruder_id) >= cfg.nozzle_diameter.size())
            return false;
        if (!dynamic) {
            if (f >= cfg.filament_map.values.size() || cfg.filament_map.values[f] != nozzle->extruder_id + 1)
                return false;
        }
    }
    return true;
}

bool make_plan(const Print &print, Plan &plan)
{
    if (!pre_cooling_active(print))
        return false;
    const PrintConfig        &cfg  = print.config();
    const DynamicPrintConfig &full = print.full_print_config();

    Plan out;
    out.group = print.get_layered_nozzle_group_result();

    const size_t filaments = cfg.filament_diameter.values.size();
    InjectorParams &p = out.params;
    p.filament_nozzle_temps.resize(filaments, 0);
    p.filament_pre_cooling_temps.resize(filaments, 0);
    p.filament_preheat_temperature_delta.resize(filaments, 0.);
    for (size_t f = 0; f < filaments; ++f) {
        p.filament_nozzle_temps[f] = get_value_at(full, cfg.nozzle_temperature, ConfigFlowDomain::Filament, (unsigned int) f);
        if (!cfg.filament_pre_cooling_temperature.values.empty())
            p.filament_pre_cooling_temps[f] = cfg.filament_pre_cooling_temperature.get_at(f);
        if (!cfg.filament_preheat_temperature_delta.values.empty())
            p.filament_preheat_temperature_delta[f] = cfg.filament_preheat_temperature_delta.get_at(f);
    }

    const size_t extruders = cfg.nozzle_diameter.values.size();
    for (size_t e = 0; e < extruders; ++e) {
        p.cooling_rate.push_back(extruder_variant_value(cfg, cfg.hotend_cooling_rate.values, e, 2.));
        p.heating_rate.push_back(extruder_variant_value(cfg, cfg.hotend_heating_rate.values, e, 2.));
        // A zero rate would divide by zero below; Bambu's default is 2 degrees per second.
        if (!(p.cooling_rate.back() > 0.))
            p.cooling_rate.back() = 2.;
        if (!(p.heating_rate.back() > 0.))
            p.heating_rate.back() = 2.;
        p.physical_extruder_map.push_back(e < cfg.physical_extruder_map.values.size() ? cfg.physical_extruder_map.values[e] : int(e));
        p.extruder_max_nozzle_count.push_back(e < cfg.extruder_max_nozzle_count.values.size() ? cfg.extruder_max_nozzle_count.values[e] : 1);
        p.extruder_types.push_back(e < cfg.extruder_type.values.size() ? cfg.extruder_type.values[e] : int(ExtruderType::etDirectDrive));
    }
    p.nozzle_diameter           = cfg.nozzle_diameter.values;
    p.has_filament_switcher     = cfg.has_filament_switcher.value;
    p.handle_hotend_as_extruder = false; // no such printer key in this tree
    p.inject_time_threshold     = 0.f;   // BambuStudio GCodeProcessor.cpp:2750
    p.extruder_change_time      = float(cfg.machine_switch_extruder_time.value);
    p.filament_load_time        = float(cfg.machine_load_filament_time.value);
    p.filament_unload_time      = float(cfg.machine_unload_filament_time.value);

    plan = std::move(out);
    return true;
}

// ---------------------------------------------------------------------------------------------------
// UsageBlockBuilder: BambuStudio GCodeProcessor.cpp:945-1175

UsageBlockBuilder::UsageBlockBuilder(const MultiNozzleUtils::LayeredNozzleGroupResult &group, std::string layer_change_tag)
    : m_group(group), m_layer_change_tag(std::move(layer_change_tag))
{
    // The first use of an extruder writes no nozzle-change marker, so start with a dummy block.
    extruder_blocks.emplace_back();
}

void UsageBlockBuilder::handle_filament_change(int filament_id, unsigned int line_id, int nozzle_id)
{
    // Filament changes inside the machine start / end G-code do not count.
    if (machine_start_gcode_end_id == NoLine || (machine_end_gcode_start_id != NoLine && line_id > machine_end_gcode_start_id))
        return;
    if (!filament_blocks.empty())
        filament_blocks.back().upper_gcode_id = line_id;
    if (nozzle_id == -1)
        nozzle_id = m_group.get_nozzle_id(filament_id, m_layer_id);
    int extruder_id = 0;
    if (auto nozzle = m_group.get_nozzle_from_id(nozzle_id))
        extruder_id = nozzle->extruder_id;
    filament_blocks.emplace_back(filament_id, extruder_id, nozzle_id, line_id, NoLine);
}

void UsageBlockBuilder::on_line(const std::string &raw_line, unsigned int line_id)
{
    const std::string_view line = skip_whitespace(without_eol(raw_line));
    if (line.empty())
        return;

    if (line.front() == ';') {
        const std::string_view tag = line.substr(1);
        if (tag == MachineStartGCodeEndTag) {
            machine_start_gcode_end_id = line_id;
            return;
        }
        if (tag == MachineEndGCodeStartTag) {
            machine_end_gcode_start_id = line_id;
            return;
        }
        if (tag == SkippableStartTag) {
            skippable_blocks.emplace_back(line_id, 0);
            return;
        }
        if (tag == SkippableEndTag) {
            if (!skippable_blocks.empty())
                skippable_blocks.back().second = line_id;
            return;
        }
        if (starts_with(tag, "VT")) {
            int filament_id = -1, nozzle_id = -1;
            if (parse_filament_and_nozzle(tag.substr(2), filament_id, nozzle_id))
                handle_filament_change(filament_id, line_id, nozzle_id);
            return;
        }
        if (starts_with(tag, NozzleChangeStartTag)) {
            int of = -1, nf = -1, on = -1, nn = -1;
            parse_nozzle_change(tag, of, nf, on, nn);
            if (!extruder_blocks.empty())
                extruder_blocks.back().initialize_step_2(line_id);
            return;
        }
        if (starts_with(tag, NozzleChangeEndTag)) {
            int of = -1, nf = -1, on = -1, nn = -1;
            parse_nozzle_change(tag, of, nf, on, nn);
            int extruder_id = -1;
            if (auto nozzle = m_group.get_nozzle_from_id(nn))
                extruder_id = nozzle->extruder_id;
            if (!extruder_blocks.empty())
                extruder_blocks.back().initialize_step_3(line_id, of, line_id, on);
            m_temp_block.initialize_step_1(extruder_id, line_id, nf, nn);
            // Bambu Studio reads this from the tower's "; CP_TOOLCHANGE_WIPE CT<contact> FL<first layer>"
            // right after the nozzle change: no pre-heat delta on the first layer (and on a tower
            // interface layer, which this tree's wipe tower does not have).
            m_temp_block.ignore_cooling_before_tower = m_layer_id <= 1;
            extruder_blocks.emplace_back(m_temp_block);
            m_temp_block.reset();
            return;
        }
        if (starts_with(tag, ToolchangeWipeTag)) {
            static const std::regex re(R"(CT(\d)(?:\s+FL(\d))?)");
            std::match_results<std::string_view::const_iterator> m;
            if (std::regex_search(tag.begin(), tag.end(), m, re) && !extruder_blocks.empty())
                extruder_blocks.back().ignore_cooling_before_tower = m[1].str() != "0" || (m[2].matched && m[2].str() != "0");
            return;
        }
        if (!m_layer_change_tag.empty() && starts_with(tag, m_layer_change_tag))
            ++m_layer_id;
        return;
    }

    if (line.front() == 'T') {
        int filament_id = -1, nozzle_id = -1;
        if (parse_filament_and_nozzle(line.substr(1), filament_id, nozzle_id))
            handle_filament_change(filament_id, line_id, nozzle_id);
        return;
    }
    if (starts_with(line, "M1020") && (line.size() == 5 || line[5] == ' ' || line[5] == '\t')) {
        const size_t s = line.find('S');
        int          filament_id = -1, nozzle_id = -1;
        if (s != std::string_view::npos && parse_filament_and_nozzle(line.substr(s + 1), filament_id, nozzle_id))
            handle_filament_change(filament_id, line_id, nozzle_id);
        return;
    }
}

void UsageBlockBuilder::finish()
{
    if (!filament_blocks.empty())
        filament_blocks.back().upper_gcode_id = machine_end_gcode_start_id;
    if (extruder_blocks.empty())
        return;
    int first_filament = 0;
    int last_filament  = 0;
    if (!filament_blocks.empty()) {
        first_filament = filament_blocks.front().filament_id;
        last_filament  = filament_blocks.back().filament_id;
    }
    {
        auto nozzle = m_group.get_first_nozzle_for_filament(first_filament);
        extruder_blocks.front().initialize_step_1(nozzle ? nozzle->extruder_id : -1, machine_start_gcode_end_id, first_filament,
                                                  nozzle ? nozzle->group_id : -1);
    }
    extruder_blocks.back().initialize_step_2(machine_end_gcode_start_id);
    const int last_nozzle_id = filament_blocks.empty() ? -1 : filament_blocks.back().nozzle_id;
    extruder_blocks.back().initialize_step_3(machine_end_gcode_start_id, last_filament, machine_end_gcode_start_id, last_nozzle_id);
}

// ---------------------------------------------------------------------------------------------------
// PreCoolingInjector: BambuStudio GCodeProcessor.cpp:6490-6858

void PreCoolingInjector::process_pre_cooling_and_heating(InsertedLines &inserted_lines) const
{
    const InjectorParams &p = m_params;
    auto get_nozzle_temp = [&p](int filament_id, bool from_or_to, bool consider_preheat_temperature_delta) -> int {
        if (filament_id < 0 || size_t(filament_id) >= p.filament_nozzle_temps.size())
            return from_or_to ? 140 : 0; // default temp
        const double temp = p.filament_nozzle_temps[size_t(filament_id)];
        if (consider_preheat_temperature_delta && size_t(filament_id) < p.filament_preheat_temperature_delta.size())
            return int(temp - p.filament_preheat_temperature_delta[size_t(filament_id)]);
        return int(temp);
    };

    // Bambu's X2D workaround: with mixed extruder types (e.g. direct drive + bowden) keep the pre-heat
    // target below the print temperature so the slower extruder does not overshoot.
    const bool  has_mixed_extruder_types = p.extruder_types.size() > 1 &&
                                          std::adjacent_find(p.extruder_types.begin(), p.extruder_types.end(), std::not_equal_to<>()) !=
                                              p.extruder_types.end();
    const float first_nozzle_dia     = p.nozzle_diameter.empty() ? 0.4f : float(p.nozzle_diameter.front());
    const float switcher_temp_offset = (first_nozzle_dia >= 0.6f - EPSILON) ? 40.f : 20.f;

    std::map<int, std::vector<ExtruderFreeBlock>> per_extruder_free_blocks;
    for (const ExtruderFreeBlock &block : m_free_blocks)
        per_extruder_free_blocks[block.extruder_id].emplace_back(block);

    for (const auto &[extruder_id, blocks] : per_extruder_free_blocks) {
        for (auto it = blocks.begin(); it != blocks.end(); ++it) {
            const bool is_end            = std::next(it) == blocks.end();
            const bool apply_pre_cooling = true;
            const bool apply_pre_heating = !is_end;
            const float curr_temp        = float(get_nozzle_temp(it->last_filament_id, true, false));
            float       target_temp      = float(get_nozzle_temp(it->next_filament_id, false, !it->ignore_cooling_before_tower));
            if (p.has_filament_switcher && has_mixed_extruder_types && apply_pre_heating) {
                const float print_temp = float(get_nozzle_temp(it->next_filament_id, false, false));
                target_temp            = std::min(target_temp, print_temp - switcher_temp_offset);
            }
            inject_cooling_heating_command(inserted_lines, *it, curr_temp, target_temp, apply_pre_cooling, apply_pre_heating);
        }
    }
}

void PreCoolingInjector::build_extruder_free_blocks(const std::vector<FilamentUsageBlock> &filament_blocks,
                                                    const std::vector<ExtruderUsageBlock> &extruder_blocks)
{
    if (extruder_blocks.size() <= 1)
        build_by_filament_blocks(filament_blocks);
    else
        build_by_extruder_blocks(extruder_blocks);
}

void PreCoolingInjector::inject_cooling_heating_command(
    InsertedLines &inserted_lines, const ExtruderFreeBlock &block, float curr_temp, float target_temp, bool pre_cooling, bool pre_heating) const
{
    const InjectorParams &p = m_params;
    using MoveIt            = std::vector<TimedMove>::const_iterator;

    auto get_valid_extruder_id = [this](int last_nozzle_id) {
        auto nozzle = m_group.get_nozzle_from_id(last_nozzle_id);
        return nozzle ? nozzle->extruder_id : 0;
    };
    auto is_pre_cooling_valid = [&p](int idx) -> bool {
        if (idx < 0 || size_t(idx) >= p.filament_pre_cooling_temps.size() || size_t(idx) >= p.filament_nozzle_temps.size())
            return false;
        return p.filament_pre_cooling_temps[size_t(idx)] > 0 && p.filament_pre_cooling_temps[size_t(idx)] < p.filament_nozzle_temps[size_t(idx)];
    };
    auto get_partial_free_cooling_thres = [&p](int idx) -> float {
        if (idx < 0 || size_t(idx) >= p.filament_pre_cooling_temps.size() || size_t(idx) >= p.filament_nozzle_temps.size())
            return 30.f;
        return float(p.filament_nozzle_temps[size_t(idx)]) - float(p.filament_pre_cooling_temps[size_t(idx)]);
    };
    auto gcode_move_comp = [](const TimedMove &a, unsigned int gcode_id) { return a.gcode_id < gcode_id; };

    // Pre-heat lines never go inside a skippable block (the time-lapse sequence): move them in front of it.
    auto find_skip_block_start = [this](unsigned int gcode_id) -> unsigned int {
        auto it = std::upper_bound(m_skippable_blocks.begin(), m_skippable_blocks.end(), gcode_id,
                                   [](unsigned int id, const std::pair<unsigned int, unsigned int> &b) { return id < b.first; });
        if (it != m_skippable_blocks.begin()) {
            auto candidate = std::prev(it);
            if (gcode_id >= candidate->first && gcode_id <= candidate->second)
                return candidate->first;
        }
        return 0;
    };
    auto adjust_iter_backward = [&](MoveIt iter, MoveIt begin) -> MoveIt {
        while (iter != begin) {
            const unsigned int skip_block_start = find_skip_block_start(iter->gcode_id);
            if (skip_block_start == 0)
                break;
            MoveIt new_iter = std::lower_bound(begin, iter, skip_block_start, gcode_move_comp);
            if (new_iter == begin)
                break;
            iter = std::prev(new_iter);
        }
        return iter;
    };

    if (!pre_cooling && !pre_heating && block.free_upper_gcode_id <= block.free_lower_gcode_id)
        return;

    MoveIt move_iter_lower = std::lower_bound(m_moves.begin(), m_moves.end(), block.free_lower_gcode_id, gcode_move_comp);
    MoveIt move_iter_upper = std::lower_bound(m_moves.begin(), m_moves.end(), block.free_upper_gcode_id, gcode_move_comp); // closed below
    if (move_iter_lower == m_moves.end() || move_iter_upper == m_moves.begin())
        return;
    --move_iter_upper;
    const float complete_free_time_gap = move_iter_lower == m_moves.begin() ?
                                             move_iter_upper->time :
                                             move_iter_upper->time - std::prev(move_iter_lower)->time;

    MoveIt partial_free_move_lower = std::lower_bound(m_moves.begin(), m_moves.end(), block.partial_free_lower_id, gcode_move_comp);
    MoveIt partial_free_move_upper = std::lower_bound(m_moves.begin(), m_moves.end(), block.partial_free_upper_id, gcode_move_comp);
    if (partial_free_move_lower == m_moves.end() || partial_free_move_upper == m_moves.begin())
        return;
    --partial_free_move_upper;
    const float partial_free_time_gap = partial_free_move_lower == m_moves.begin() ?
                                            partial_free_move_upper->time :
                                            partial_free_move_upper->time - std::prev(partial_free_move_lower)->time;

    if (move_iter_lower >= move_iter_upper)
        return;

    const bool apply_cooling_when_partial_free = is_pre_cooling_valid(block.last_filament_id) && pre_cooling;
    if (apply_cooling_when_partial_free && partial_free_time_gap + complete_free_time_gap < p.inject_time_threshold)
        return;
    if (!apply_cooling_when_partial_free && complete_free_time_gap < p.inject_time_threshold)
        return;

    const int   extruder_id      = get_valid_extruder_id(block.last_nozzle_id);
    const float ext_heating_rate = float(extruder_id >= 0 && size_t(extruder_id) < p.heating_rate.size() ? p.heating_rate[size_t(extruder_id)] : 2.);
    const float ext_cooling_rate = float(extruder_id >= 0 && size_t(extruder_id) < p.cooling_rate.size() ? p.cooling_rate[size_t(extruder_id)] : 2.);

    auto add_M104_lines = [&](unsigned int gcode_id, int target_extruder, int temp, int target_filament, bool skippable, int next_filament_idx,
                              int next_nozzle_id, bool is_heating, const std::string &comment) {
        std::vector<std::string> &buffer = inserted_lines[gcode_id];
        if (skippable) {
            std::string m632_line = "M632 S" + std::to_string(next_filament_idx);
            if (m_group.is_support_dynamic_nozzle_map())
                m632_line += " H" + std::to_string(next_nozzle_id);
            if (target_extruder >= 0 && size_t(target_extruder) < p.extruder_max_nozzle_count.size() &&
                p.extruder_max_nozzle_count[size_t(target_extruder)] > 1)
                m632_line += " N R";
            m632_line += " W\n";
            buffer.emplace_back(std::move(m632_line));
        }
        // Only wait before cooling: G1 is non-blocking, so an M104 cool-down must not start mid-travel.
        // Heating can start early and needs no M400.
        if (!is_heating)
            buffer.emplace_back("M400\n");
        std::string m104_line = "M104";
        if (p.handle_hotend_as_extruder)
            m104_line += " I" + std::to_string(target_filament == -1 ? next_filament_idx : target_filament);
        else if (target_extruder != -1) {
            const int physical = size_t(target_extruder) < p.physical_extruder_map.size() ? p.physical_extruder_map[size_t(target_extruder)] :
                                                                                            target_extruder;
            m104_line += " T" + std::to_string(physical);
        }
        m104_line += " S" + std::to_string(temp);
        m104_line += " N0"; // N0: written by the slicer
        if (!comment.empty())
            m104_line += " ;" + comment;
        m104_line += '\n';
        buffer.emplace_back(std::move(m104_line));
        if (skippable)
            buffer.emplace_back("M633\n");
    };

    constexpr float room_temperature = 25.f;

    if (apply_cooling_when_partial_free) {
        const float max_cooling_temp = std::min(curr_temp, std::min(get_partial_free_cooling_thres(block.last_filament_id),
                                                                    partial_free_time_gap * ext_cooling_rate));
        curr_temp = std::max(room_temperature, curr_temp - max_cooling_temp); // the temperature after cooling during post extrusion
        add_M104_lines(block.partial_free_lower_id, extruder_id, int(curr_temp), block.last_filament_id, false, block.next_filament_id,
                       block.next_nozzle_id, false, "Multi extruder pre cooling in post extrusion");
    }

    if (pre_cooling && !pre_heating) {
        // only cool down
        if (target_temp >= curr_temp)
            return;
        const int clamped_target = std::max(int(room_temperature), int(target_temp));
        add_M104_lines(block.free_lower_gcode_id, extruder_id, clamped_target, block.last_filament_id, false, block.next_filament_id,
                       block.next_nozzle_id, false, "Multi extruder pre cooling");
        return;
    }

    auto first_move_after = [&](float time) {
        return std::upper_bound(move_iter_lower, move_iter_upper + 1, time, [](float t, const TimedMove &a) { return t < a.time; });
    };

    if (!pre_cooling && pre_heating) {
        // only heat up
        if (target_temp <= curr_temp)
            return;
        const float heating_start_time = move_iter_upper->time - (target_temp - curr_temp) / ext_heating_rate;
        MoveIt      heating_move_iter  = first_move_after(heating_start_time);
        if (heating_move_iter == move_iter_lower) {
            add_M104_lines(block.free_lower_gcode_id, extruder_id, int(target_temp), block.next_filament_id, true, block.next_filament_id,
                           block.next_nozzle_id, true, "Multi extruder pre heating");
        } else {
            --heating_move_iter;
            heating_move_iter = adjust_iter_backward(heating_move_iter, move_iter_lower);
            add_M104_lines(heating_move_iter->gcode_id, extruder_id, int(target_temp), block.next_filament_id, true, block.next_filament_id,
                           block.next_nozzle_id, true, "Multi extruder pre heating");
        }
        return;
    }

    // Cool down first, then heat up in time for the next use.
    const float mid_temp = std::max(room_temperature, (curr_temp * ext_heating_rate + target_temp * ext_cooling_rate -
                                                       complete_free_time_gap * ext_cooling_rate * ext_heating_rate) /
                                                          (ext_cooling_rate + ext_heating_rate));
    const float heating_temp       = target_temp - mid_temp;
    const float heating_start_time = move_iter_upper->time - heating_temp / ext_heating_rate;
    MoveIt      heating_move_iter  = first_move_after(heating_start_time);
    if (heating_move_iter == move_iter_lower)
        return;
    --heating_move_iter;
    heating_move_iter = adjust_iter_backward(heating_move_iter, move_iter_lower);

    // Where the heat command really goes decides how long the nozzle cools.
    const float real_cooling_time = heating_move_iter->time - move_iter_lower->time;
    const int   real_delta_temp   = std::min(int(real_cooling_time * ext_cooling_rate), int(curr_temp));
    if (real_delta_temp == 0)
        return;
    const int cooling_temp = std::max(int(room_temperature), int(curr_temp) - real_delta_temp);
    add_M104_lines(block.free_lower_gcode_id, extruder_id, cooling_temp, block.last_filament_id, false, block.next_filament_id,
                   block.next_nozzle_id, false, "Multi extruder pre cooling");
    add_M104_lines(heating_move_iter->gcode_id, extruder_id, int(target_temp), block.next_filament_id, true, block.next_filament_id,
                   block.next_nozzle_id, true, "Multi extruder pre heating");
}

void PreCoolingInjector::build_by_filament_blocks(const std::vector<FilamentUsageBlock> &filament_usage_blocks)
{
    m_free_blocks.clear();

    std::map<int, std::vector<FilamentUsageBlock>> per_extruder_usage_blocks;
    for (const FilamentUsageBlock &block : filament_usage_blocks)
        per_extruder_usage_blocks[block.extruder_id].emplace_back(block);

    const FilamentUsageBlock start_filament_block(-1, -1, -1, 0, m_machine_start_gcode_end_id);
    const FilamentUsageBlock end_filament_block(-1, -1, -1, m_machine_end_gcode_start_id, std::numeric_limits<unsigned int>::max());
    for (auto &[extruder_id, blocks] : per_extruder_usage_blocks) {
        blocks.insert(blocks.begin(), start_filament_block);
        blocks.emplace_back(end_filament_block);
    }

    for (const auto &[extruder_id, blocks] : per_extruder_usage_blocks) {
        for (auto it = blocks.begin(); it < blocks.end(); ++it) {
            auto nit = std::next(it);
            if (nit == blocks.end())
                break;
            ExtruderFreeBlock block;
            block.free_lower_gcode_id = it->upper_gcode_id;
            block.last_filament_id    = it->filament_id;
            block.last_nozzle_id      = it->nozzle_id;
            block.free_upper_gcode_id = nit->lower_gcode_id;
            block.next_filament_id    = nit->filament_id;
            block.next_nozzle_id      = nit->nozzle_id;
            if (block.last_nozzle_id == -1)
                block.last_nozzle_id = block.next_nozzle_id;
            block.extruder_id           = extruder_id;
            block.partial_free_lower_id = block.free_lower_gcode_id;
            block.partial_free_upper_id = block.free_lower_gcode_id;
            m_free_blocks.emplace_back(block);
        }
    }
    for (ExtruderFreeBlock &block : m_free_blocks)
        block.ignore_cooling_before_tower = true;
    std::sort(m_free_blocks.begin(), m_free_blocks.end(), [](const auto &a, const auto &b) {
        return a.free_lower_gcode_id < b.free_lower_gcode_id ||
               (a.free_lower_gcode_id == b.free_lower_gcode_id && a.free_upper_gcode_id < b.free_upper_gcode_id);
    });
}

void PreCoolingInjector::build_by_extruder_blocks(const std::vector<ExtruderUsageBlock> &extruder_usage_blocks)
{
    m_free_blocks.clear();
    std::map<int, std::vector<ExtruderUsageBlock>> per_extruder_usage_blocks;
    for (const ExtruderUsageBlock &block : extruder_usage_blocks)
        per_extruder_usage_blocks[block.extruder_id].emplace_back(block);

    for (auto &[extruder_id, blocks] : per_extruder_usage_blocks) {
        ExtruderUsageBlock start_filament_block;
        start_filament_block.initialize_step_1(extruder_id, 0, -1, -1);
        start_filament_block.initialize_step_2(m_machine_start_gcode_end_id);
        start_filament_block.initialize_step_3(m_machine_start_gcode_end_id, -1, m_machine_start_gcode_end_id, -1);

        ExtruderUsageBlock end_filament_block;
        end_filament_block.initialize_step_1(extruder_id, m_machine_end_gcode_start_id, -1, -1);
        end_filament_block.initialize_step_2((unsigned int) std::numeric_limits<int>::max());
        end_filament_block.initialize_step_3((unsigned int) std::numeric_limits<int>::max(), -1, (unsigned int) std::numeric_limits<int>::max(), -1);

        blocks.insert(blocks.begin(), start_filament_block);
        blocks.emplace_back(end_filament_block);
    }

    for (const auto &[extruder_id, blocks] : per_extruder_usage_blocks) {
        for (auto it = blocks.begin(); it != blocks.end(); ++it) {
            auto nit = std::next(it);
            if (nit == blocks.end())
                break;
            ExtruderFreeBlock block;
            block.free_lower_gcode_id = it->end_id;
            block.last_filament_id    = it->end_filament;
            block.last_nozzle_id      = it->end_nozzle_id;
            block.free_upper_gcode_id = nit->start_id;
            block.next_filament_id    = nit->start_filament;
            block.next_nozzle_id      = nit->start_nozzle_id;
            if (block.last_nozzle_id == -1)
                block.last_nozzle_id = block.next_nozzle_id;
            block.extruder_id                 = extruder_id;
            block.partial_free_lower_id       = it->post_extrusion_start_id;
            block.partial_free_upper_id       = it->post_extrusion_end_id;
            block.ignore_cooling_before_tower = nit->ignore_cooling_before_tower;
            m_free_blocks.emplace_back(block);
        }
    }

    std::sort(m_free_blocks.begin(), m_free_blocks.end(), [](const auto &a, const auto &b) {
        return a.free_lower_gcode_id < b.free_lower_gcode_id ||
               (a.free_lower_gcode_id == b.free_lower_gcode_id && a.free_upper_gcode_id < b.free_upper_gcode_id);
    });
}

} // namespace PreCooling
} // namespace Slic3r
