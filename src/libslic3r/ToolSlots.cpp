// Per-nozzle process P4: the generic N-tool model - see ToolSlots.hpp.

#include "ToolSlots.hpp"

#include "MultiNozzleUtils.hpp"
#include "NozzleOverride.hpp"
#include "Preset.hpp"
#include "PrintConfig.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <iomanip>
#include <locale>
#include <set>
#include <sstream>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/log/trivial.hpp>

namespace Slic3r {

namespace {

template<class T> const T *opt_of(const ConfigBase &config, const char *key)
{
    return dynamic_cast<const T *>(config.option(key));
}

std::vector<double> floats_of(const ConfigBase &config, const char *key)
{
    if (const auto *opt = opt_of<ConfigOptionFloats>(config, key))
        return opt->values;
    if (const auto *opt = opt_of<ConfigOptionFloatsNullable>(config, key))
        return opt->values;
    return {};
}

std::vector<int> ints_of(const ConfigBase &config, const char *key)
{
    if (const auto *opt = opt_of<ConfigOptionInts>(config, key))
        return opt->values;
    return {};
}

bool bool_of(const ConfigBase &config, const char *key)
{
    const auto *opt = opt_of<ConfigOptionBool>(config, key);
    return opt != nullptr && opt->value;
}

size_t filament_count_of(const ConfigBase &config)
{
    if (const auto *opt = dynamic_cast<const ConfigOptionVectorBase *>(config.option("filament_diameter")))
        return opt->size();
    return 0;
}

std::string trimmed(std::string s)
{
    boost::algorithm::trim(s);
    return s;
}

// "standard" / "Standard" / "high_flow" / "High Flow" / "hf" -> the flow mode, -1 otherwise.
int parse_flow_word(std::string word)
{
    boost::algorithm::to_lower(word);
    std::replace(word.begin(), word.end(), ' ', '_');
    if (word == "standard" || word == "std")
        return int(fvtStandard);
    if (word == "high_flow" || word == "highflow" || word == "hf")
        return int(fvtHighFlow);
    return -1;
}

bool parse_positive_number(const std::string &text, double &out)
{
    if (text.empty())
        return false;
    char        *end   = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || !(value > 0.) || !std::isfinite(value))
        return false;
    out = value;
    return true;
}

NozzleVolumeType parse_nozzle_volume(std::string word, bool &ok)
{
    boost::algorithm::to_lower(word);
    std::replace(word.begin(), word.end(), ' ', '_');
    ok = true;
    if (word == "standard" || word == "std")
        return nvtStandard;
    if (word == "high_flow" || word == "hf")
        return nvtHighFlow;
    if (word == "tpu_high_flow")
        return nvtTPUHighFlow;
    if (word == "e3d_high_flow")
        return nvtE3DHighFlow;
    ok = false;
    return nvtStandard;
}

const char *nozzle_volume_word(NozzleVolumeType type)
{
    switch (type) {
    case nvtHighFlow: return "high_flow";
    case nvtHybrid: return "hybrid";
    case nvtTPUHighFlow: return "tpu_high_flow";
    case nvtE3DHighFlow: return "e3d_high_flow";
    default: return "standard";
    }
}

NozzleVolumeType extruder_volume_type(const ConfigBase &config, size_t extruder)
{
    if (const auto *opt = opt_of<ConfigOptionEnumsGeneric>(config, "nozzle_volume_type"))
        if (extruder < opt->values.size())
            return NozzleVolumeType(opt->values[extruder]);
    return nvtStandard;
}

// The physical extruder (0-based) of 1-based `filament_id`, from any config.
size_t physical_extruder_of(const ConfigBase &config, unsigned int filament_id)
{
    const std::vector<double> nozzles = floats_of(config, "nozzle_diameter");
    return physical_extruder_for_filament(nozzles.size(), filament_count_of(config), ints_of(config, "filament_map"), filament_id,
                                          ints_of(config, "filament_nozzle_assignment"));
}

} // namespace

bool same_nozzle_diameter(double a, double b) { return std::abs(a - b) < 0.001; }

// ---- 1. The per-tool slot model ------------------------------------------------------------------------------

std::vector<ToolSlot> process_tool_slots(const ConfigBase &config)
{
    const auto *ids      = opt_of<ConfigOptionInts>(config, "print_extruder_id");
    const auto *variants = opt_of<ConfigOptionStrings>(config, "print_extruder_variant");
    if (ids == nullptr || variants == nullptr || ids->values.empty() || ids->values.size() != variants->values.size())
        return {};
    const std::vector<double> diameters = floats_of(config, "print_extruder_nozzle_diameter");
    std::vector<ToolSlot>     slots(ids->values.size());
    for (size_t i = 0; i < slots.size(); ++i) {
        slots[i].tool      = std::max(0, ids->values[i]);
        slots[i].flow_mode = process_variant_flow_mode(variants->values[i]);
        if (diameters.size() == slots.size() && diameters[i] > 0.)
            slots[i].diameter = diameters[i];
    }
    return slots;
}

bool process_has_diameter_slots(const ConfigBase &config)
{
    // No allocation: this runs for every process value the slicer reads.
    const auto *diameters = opt_of<ConfigOptionFloats>(config, "print_extruder_nozzle_diameter");
    const auto *ids       = opt_of<ConfigOptionInts>(config, "print_extruder_id");
    if (diameters == nullptr || diameters->values.empty() || ids == nullptr || ids->values.size() != diameters->values.size())
        return false;
    return std::any_of(diameters->values.begin(), diameters->values.end(), [](double d) { return d > 0.; });
}

bool tool_slot_model_enabled(const ConfigBase &config) { return bool_of(config, "tool_slot_model"); }

bool tool_slot_model_active(const ConfigBase &config)
{
    if (process_has_diameter_slots(config))
        return true;
    if (!tool_slot_model_enabled(config))
        return false;
    const auto *ids      = opt_of<ConfigOptionInts>(config, "print_extruder_id");
    const auto *variants = opt_of<ConfigOptionStrings>(config, "print_extruder_variant");
    return ids != nullptr && variants != nullptr && !ids->values.empty() && ids->values.size() == variants->values.size();
}

int match_tool_slot(const std::vector<ToolSlot> &slots, const ToolSlotKey &key)
{
    int best = -1, best_score = -1;
    for (size_t i = 0; i < slots.size(); ++i) {
        const ToolSlot &slot = slots[i];
        if (slot.tool != 0 && size_t(slot.tool) != key.tool + 1)
            continue;
        if (slot.diameter > 0. && !same_nozzle_diameter(slot.diameter, key.diameter))
            continue;
        int score = 0;
        if (slot.tool != 0)
            score += 8;
        if (slot.diameter > 0.)
            score += 4;
        if (slot.flow_mode == key.flow_mode)
            score += 2;
        else if (slot.flow_mode < 0)
            score += 1;
        if (score > best_score) {
            best       = int(i);
            best_score = score;
        }
    }
    return best;
}

int filament_flow_mode(const ConfigBase &config, unsigned int filament_idx)
{
    if (const auto *types = opt_of<ConfigOptionEnumsGeneric>(config, "filament_volume_type"))
        if (filament_idx < types->values.size())
            return types->values[filament_idx] == int(fvtHighFlow) ? int(fvtHighFlow) : int(fvtStandard);
    return int(fvtStandard);
}

int tool_slot_for_filament(const ConfigBase &config, size_t opt_size, unsigned int filament_idx)
{
    if (opt_size < 2 || filament_idx == (unsigned int) -1 || !tool_slot_model_active(config))
        return -1;
    const std::vector<ToolSlot> slots = process_tool_slots(config);
    if (slots.size() != opt_size)
        return -1;
    ToolSlotKey key;
    // The extruder P1's resolver picks: filament_map on Bambu's grouping machines, the assignment, else the own tool.
    const auto *nozzles = dynamic_cast<const ConfigOptionVectorBase *>(config.option("nozzle_diameter"));
    key.tool            = process_extruder_for_filament(config, filament_idx, nozzles != nullptr ? nozzles->size() : 0);
    key.flow_mode = filament_flow_mode(config, filament_idx);
    key.diameter  = nozzle_diameter_for_filament(config, filament_idx + 1);
    return match_tool_slot(slots, key);
}

void set_process_tool_slots(DynamicPrintConfig &config, const std::vector<int> &tools, const std::vector<std::string> &variants,
                            const std::vector<double> &diameters)
{
    config.set_key_value("print_extruder_id", new ConfigOptionInts(tools));
    config.set_key_value("print_extruder_variant", new ConfigOptionStrings(variants));
    config.set_key_value("print_extruder_nozzle_diameter", new ConfigOptionFloats(diameters));
}

// ---- 3. Rack nozzle diameters (H2C) -----------------------------------------------------------------------------

std::vector<RackNozzle> parse_rack_nozzles(const std::string &text)
{
    std::vector<RackNozzle> out;
    size_t                  start = 0;
    while (start <= text.size()) {
        size_t comma = text.find(',', start);
        if (comma == std::string::npos)
            comma = text.size();
        const std::string entry = trimmed(text.substr(start, comma - start));
        start                   = comma + 1;
        if (entry.empty())
            continue;
        RackNozzle        nozzle;
        const size_t      colon    = entry.find(':');
        const std::string diameter = trimmed(entry.substr(0, colon));
        if (!parse_positive_number(diameter, nozzle.diameter))
            continue;
        if (colon != std::string::npos) {
            bool ok            = false;
            nozzle.volume_type = parse_nozzle_volume(trimmed(entry.substr(colon + 1)), ok);
            nozzle.volume_given = ok;
        }
        out.push_back(nozzle);
    }
    return out;
}

std::vector<std::vector<RackNozzle>> extruder_rack_nozzles(const ConfigBase &config)
{
    const size_t                         extruders = floats_of(config, "nozzle_diameter").size();
    std::vector<std::vector<RackNozzle>> out(extruders);
    if (const auto *opt = opt_of<ConfigOptionStrings>(config, "extruder_rack_nozzle_diameter"))
        for (size_t e = 0; e < extruders && e < opt->values.size(); ++e) {
            out[e] = parse_rack_nozzles(opt->values[e]);
            for (RackNozzle &nozzle : out[e])
                if (!nozzle.volume_given)
                    nozzle.volume_type = extruder_volume_type(config, e) == nvtHybrid ? nvtStandard : extruder_volume_type(config, e);
        }
    return out;
}

std::string validate_rack_nozzle_text(const std::string &text, double extruder_diameter)
{
    size_t start = 0;
    bool   any   = false, has_own = false;
    while (start <= text.size()) {
        size_t comma = text.find(',', start);
        if (comma == std::string::npos)
            comma = text.size();
        const std::string entry = trimmed(text.substr(start, comma - start));
        start                   = comma + 1;
        if (entry.empty())
            continue;
        const size_t      colon = entry.find(':');
        const std::string word  = trimmed(entry.substr(0, colon));
        double            d     = 0.;
        if (!parse_positive_number(word, d) || d > 3.)
            return "\"" + entry + "\": not a nozzle diameter in mm";
        if (colon != std::string::npos) {
            bool ok = false;
            parse_nozzle_volume(trimmed(entry.substr(colon + 1)), ok);
            if (!ok)
                return "\"" + entry + "\": the flow type after \":\" must be standard, high_flow, tpu_high_flow or e3d_high_flow";
        }
        any = true;
        has_own |= extruder_diameter > 0. && same_nozzle_diameter(d, extruder_diameter);
    }
    if (any && extruder_diameter > 0. && !has_own)
        return "the extruder's own nozzle (" + format_nozzle_diameter_word(extruder_diameter) + " mm) is not in the rack";
    return std::string();
}

std::vector<double> filament_rack_pin_choices(const ConfigBase &config, unsigned int filament_id)
{
    const std::vector<std::vector<RackNozzle>> racks = extruder_rack_nozzles(config);
    const size_t                               e     = physical_extruder_of(config, filament_id);
    std::vector<double>                        out;
    if (e >= racks.size())
        return out;
    for (const RackNozzle &nozzle : racks[e])
        if (std::none_of(out.begin(), out.end(), [&nozzle](double d) { return same_nozzle_diameter(d, nozzle.diameter); }))
            out.push_back(nozzle.diameter);
    std::sort(out.begin(), out.end());
    if (out.size() < 2)
        out.clear(); // one size: nothing to choose
    return out;
}

std::vector<double> set_filament_rack_pin(std::vector<double> pins, size_t filament, size_t filaments, double diameter)
{
    if (pins.size() < filaments)
        pins.resize(filaments, 0.);
    if (filament < pins.size())
        pins[filament] = diameter > 0. ? diameter : 0.;
    // An all-0 list is stored as the key's "unused" value, { 0 }, so a project that pins nothing reads as before.
    if (std::all_of(pins.begin(), pins.end(), [](double d) { return !(d > 0.); }))
        return { 0. };
    return pins;
}

bool has_mixed_rack_diameters(const ConfigBase &config)
{
    for (const std::vector<RackNozzle> &rack : extruder_rack_nozzles(config))
        for (const RackNozzle &nozzle : rack)
            if (!same_nozzle_diameter(nozzle.diameter, rack.front().diameter))
                return true;
    return false;
}

double filament_rack_pin(const ConfigBase &config, unsigned int filament_id)
{
    const std::vector<double> pins = floats_of(config, "filament_rack_nozzle_diameter");
    if (filament_id == 0 || filament_id > pins.size())
        return 0.;
    return pins[filament_id - 1] > 0. ? pins[filament_id - 1] : 0.;
}

double nozzle_diameter_for_filament(const ConfigBase &config, unsigned int filament_id)
{
    // Per-nozzle process P7: the size the extruder carries in the project (a full config before Print::apply, as the
    // preset bundle builds it, still has the preset's nozzle_diameter beside nozzle_diameter_override).
    const std::vector<double> nozzles = effective_nozzle_diameters(config);
    if (nozzles.empty())
        return 0.;
    const size_t extruder = std::min(physical_extruder_of(config, filament_id), nozzles.size() - 1);
    const double own      = nozzles[extruder];
    const auto  *racks    = opt_of<ConfigOptionStrings>(config, "extruder_rack_nozzle_diameter");
    if (racks == nullptr || extruder >= racks->values.size() || racks->values[extruder].empty())
        return own;
    const double pin = filament_rack_pin(config, filament_id);
    if (pin <= 0.)
        return own;
    for (const RackNozzle &nozzle : parse_rack_nozzles(racks->values[extruder]))
        if (same_nozzle_diameter(nozzle.diameter, pin))
            return nozzle.diameter;
    return own;
}

double nozzle_diameter_for_filament(const PrintConfig &config, unsigned int filament_id)
{
    const double own = config.nozzle_diameter.get_at(physical_extruder_for_filament(config, filament_id));
    if (config.extruder_rack_nozzle_diameter.values.empty())
        return own;
    return nozzle_diameter_for_filament(static_cast<const ConfigBase &>(config), filament_id);
}

double min_physical_nozzle_diameter(const PrintConfig &config)
{
    const std::vector<double> &nozzles = config.nozzle_diameter.values;
    double                     out     = nozzles.empty() ? 0. : *std::min_element(nozzles.begin(), nozzles.end());
    if (!config.extruder_rack_nozzle_diameter.values.empty())
        for (const std::vector<RackNozzle> &rack : extruder_rack_nozzles(config))
            for (const RackNozzle &nozzle : rack)
                out = std::min(out, nozzle.diameter);
    return out;
}

double nozzle_diameter_for_filament_on_layer(const PrintConfig &config, const MultiNozzleUtils::LayeredNozzleGroupResult *group,
                                             unsigned int filament_id, int layer_id)
{
    if (group != nullptr && filament_id > 0)
        if (const auto nozzle = group->get_nozzle_for_filament(int(filament_id - 1), layer_id); nozzle && !nozzle->diameter.empty()) {
            double d = 0.;
            if (parse_positive_number(nozzle->diameter, d))
                return d;
        }
    return nozzle_diameter_for_filament(config, filament_id);
}

bool rack_nozzle_groups(const ConfigBase &config, size_t extruder, std::vector<std::pair<std::string, std::pair<NozzleVolumeType, int>>> &groups)
{
    const std::vector<std::vector<RackNozzle>> racks = extruder_rack_nozzles(config);
    if (extruder >= racks.size() || racks[extruder].empty())
        return false;
    std::map<std::pair<std::string, int>, int> counts;
    std::vector<std::pair<std::string, int>>   order;
    for (const RackNozzle &nozzle : racks[extruder]) {
        const auto kind = std::make_pair(format_diameter_to_str(nozzle.diameter), int(nozzle.volume_type));
        if (counts[kind]++ == 0)
            order.push_back(kind);
    }
    groups.clear();
    for (const auto &kind : order)
        groups.emplace_back(kind.first, std::make_pair(NozzleVolumeType(kind.second), counts[kind]));
    return true;
}

int pin_rack_nozzle_diameters(const PrintConfig &config, const std::vector<MultiNozzleUtils::NozzleInfo> &nozzle_list,
                              const std::vector<unsigned int> &used_filaments, std::vector<int> &filament_nozzle_map)
{
    if (!has_mixed_rack_diameters(config))
        return 0;
    int moved = 0;
    for (unsigned int f : used_filaments) {
        if (f >= filament_nozzle_map.size())
            continue;
        const int current = filament_nozzle_map[f];
        if (current < 0 || size_t(current) >= nozzle_list.size())
            continue;
        const MultiNozzleUtils::NozzleInfo &now  = nozzle_list[current];
        const double                        want = nozzle_diameter_for_filament(config, f + 1);
        double                              have = 0.;
        if (parse_positive_number(now.diameter, have) && same_nozzle_diameter(have, want))
            continue;
        int same_type = -1, any = -1;
        for (size_t n = 0; n < nozzle_list.size(); ++n) {
            double d = 0.;
            if (nozzle_list[n].extruder_id != now.extruder_id || !parse_positive_number(nozzle_list[n].diameter, d) || !same_nozzle_diameter(d, want))
                continue;
            if (any < 0)
                any = int(n);
            if (same_type < 0 && nozzle_list[n].volume_type == now.volume_type)
                same_type = int(n);
        }
        const int target = same_type >= 0 ? same_type : any;
        if (target >= 0) {
            filament_nozzle_map[f] = target;
            ++moved;
        }
    }
    if (moved > 0)
        BOOST_LOG_TRIVIAL(info) << "rack nozzle sizes: moved " << moved << " filament(s) to rack nozzles of their declared size";
    return moved;
}

std::string check_rack_nozzle_diameters(const PrintConfig &config, const MultiNozzleUtils::LayeredNozzleGroupResult &group)
{
    if (!has_mixed_rack_diameters(config))
        return {};
    const int layers = std::max(1, int(group.get_layer_count()));
    for (int layer = 0; layer < layers; ++layer) {
        const int layer_id = group.get_layer_count() == 0 ? -1 : layer;
        for (unsigned int f : group.get_used_filaments()) {
            const auto nozzle = group.get_nozzle_for_filament(int(f), layer_id);
            double     have   = 0.;
            if (!nozzle || !parse_positive_number(nozzle->diameter, have))
                continue;
            const double want = nozzle_diameter_for_filament(config, f + 1);
            if (!same_nozzle_diameter(have, want))
                return "Filament " + std::to_string(f + 1) + " was sliced for a " + format_diameter_to_str(want) +
                       " mm nozzle, but the nozzle grouping prints it with a " + format_diameter_to_str(have) + " mm rack nozzle" +
                       (layer_id >= 0 ? " on layer " + std::to_string(layer_id + 1) : std::string()) + ".";
        }
    }
    return {};
}

std::string validate_rack_nozzle_diameters(const PrintConfig &config, const std::vector<unsigned int> &used_filaments)
{
    const std::vector<std::vector<RackNozzle>> racks = extruder_rack_nozzles(config);
    for (size_t e = 0; e < racks.size(); ++e) {
        if (racks[e].empty())
            continue;
        const double own   = config.nozzle_diameter.get_at(e);
        const bool   found = std::any_of(racks[e].begin(), racks[e].end(), [own](const RackNozzle &n) { return same_nozzle_diameter(n.diameter, own); });
        if (!found)
            return "Extruder " + std::to_string(e + 1) + ": its nozzle diameter " + format_diameter_to_str(own) +
                   " mm is not one of its rack nozzle diameters.";
    }
    for (unsigned int f : used_filaments) {
        const double pin = filament_rack_pin(config, f + 1);
        if (pin <= 0.)
            continue;
        const size_t e = physical_extruder_for_filament(config, f + 1);
        if (e >= racks.size() || racks[e].empty())
            continue;
        const bool found = std::any_of(racks[e].begin(), racks[e].end(), [pin](const RackNozzle &n) { return same_nozzle_diameter(n.diameter, pin); });
        if (!found)
            return "Filament " + std::to_string(f + 1) + " asks for a " + format_diameter_to_str(pin) + " mm rack nozzle, but extruder " +
                   std::to_string(e + 1) + "'s rack has none.";
    }
    return {};
}

// ---- 4. Per-nozzle filament values ------------------------------------------------------------------------------

std::vector<FilamentNozzleVariant> parse_filament_nozzle_variants(const std::string &text, std::vector<std::string> *errors)
{
    std::vector<FilamentNozzleVariant> out;
    size_t                             start = 0;
    while (start <= text.size()) {
        size_t bar = text.find('|', start);
        if (bar == std::string::npos)
            bar = text.size();
        const std::string entry = trimmed(text.substr(start, bar - start));
        start                   = bar + 1;
        if (entry.empty())
            continue;
        const size_t colon = entry.find(':');
        if (colon == std::string::npos) {
            if (errors)
                errors->push_back("\"" + entry + "\": no \"<nozzle>:\" before the values");
            continue;
        }
        FilamentNozzleVariant variant;
        bool                  selector_ok = false, selector_bad = false;
        {
            // The selector: words separated by spaces. A number is a diameter, T<n> a tool, the rest a flow type
            // ("High Flow" is two words).
            std::string              selector = trimmed(entry.substr(0, colon));
            std::vector<std::string> words;
            size_t                   s = 0;
            while (s < selector.size()) {
                size_t e = selector.find(' ', s);
                if (e == std::string::npos)
                    e = selector.size();
                if (e > s)
                    words.push_back(selector.substr(s, e - s));
                s = e + 1;
            }
            std::string flow_words;
            for (const std::string &word : words) {
                double d = 0.;
                if (parse_positive_number(word, d)) {
                    variant.diameter = d;
                    selector_ok      = true;
                } else if ((word[0] == 'T' || word[0] == 't') && word.size() > 1 &&
                           std::all_of(word.begin() + 1, word.end(), [](char c) { return c >= '0' && c <= '9'; })) {
                    variant.tool = std::atoi(word.c_str() + 1);
                    selector_ok  = variant.tool > 0;
                    selector_bad |= variant.tool <= 0;
                } else
                    flow_words += (flow_words.empty() ? "" : " ") + word;
            }
            if (!flow_words.empty()) {
                variant.flow_mode = parse_flow_word(flow_words);
                if (variant.flow_mode < 0)
                    selector_bad = true;
                else
                    selector_ok = true;
            }
        }
        if (!selector_ok || selector_bad) {
            if (errors)
                errors->push_back("\"" + entry.substr(0, colon) + "\": not a nozzle diameter, flow type or tool");
            continue;
        }
        const std::string pairs = entry.substr(colon + 1);
        size_t            p     = 0;
        while (p <= pairs.size()) {
            size_t comma = pairs.find(',', p);
            if (comma == std::string::npos)
                comma = pairs.size();
            const std::string pair = trimmed(pairs.substr(p, comma - p));
            p                      = comma + 1;
            if (pair.empty())
                continue;
            const size_t eq = pair.find('=');
            if (eq == std::string::npos || trimmed(pair.substr(0, eq)).empty()) {
                if (errors)
                    errors->push_back("\"" + pair + "\": not key=value");
                continue;
            }
            variant.values.emplace_back(trimmed(pair.substr(0, eq)), trimmed(pair.substr(eq + 1)));
        }
        if (!variant.values.empty())
            out.push_back(std::move(variant));
    }
    return out;
}

bool is_filament_nozzle_variant_key(const std::string &key)
{
    static const std::set<std::string> identity { "filament_nozzle_variants", "filament_settings_id", "filament_vendor", "filament_type",
                                                  "filament_colour", "default_filament_colour", "filament_ids", "inherits",
                                                  "compatible_printers", "compatible_prints", "filament_extruder_variant",
                                                  "filament_self_index", "filament_diameter", "filament_flow_support" };
    if (identity.count(key) > 0)
        return false;
    const auto &options = Preset::filament_options();
    if (std::find(options.begin(), options.end(), key) == options.end())
        return false;
    const ConfigOptionDef *def = print_config_def.get(key);
    return def != nullptr && (def->type & coVectorType) != 0;
}

namespace {

// Whether `value` is one value of an option of `type` (deserialize() of the numeric vectors takes anything).
bool value_text_valid(ConfigOptionType type, const std::string &value)
{
    switch (type) {
    case coFloats:
    case coPercents:
    case coFloatsOrPercents:
    case coInts: {
        std::string v = value;
        if (type != coInts && !v.empty() && v.back() == '%')
            v.pop_back();
        if (v.empty())
            return false;
        char *end = nullptr;
        if (type == coInts)
            (void) std::strtol(v.c_str(), &end, 10);
        else
            (void) std::strtod(v.c_str(), &end);
        return end != v.c_str() && *end == '\0';
    }
    case coBools: return value == "0" || value == "1" || value == "true" || value == "false";
    default: return !value.empty();
    }
}

int variant_specificity(const FilamentNozzleVariant &v)
{
    return (v.tool > 0 ? 8 : 0) + (v.diameter > 0. ? 4 : 0) + (v.flow_mode >= 0 ? 2 : 0);
}

} // namespace

int resolve_filament_nozzle_variants(DynamicPrintConfig &config, std::vector<std::string> *errors)
{
    const auto *variants_opt = config.option<ConfigOptionStrings>("filament_nozzle_variants");
    if (variants_opt == nullptr ||
        std::all_of(variants_opt->values.begin(), variants_opt->values.end(), [](const std::string &s) { return trimmed(s).empty(); }))
        return 0;
    const std::vector<std::string> texts     = variants_opt->values;
    const size_t                   filaments = filament_count_of(config);

    // Packed flow-variant segments (filament_flow_step_size), as get_config_idx(Filament) reads them.
    std::vector<size_t> seg_start(filaments, 0), seg_size(filaments, 1);
    size_t              packed_total = filaments;
    if (const auto *steps = config.option<ConfigOptionInts>("filament_flow_step_size"); steps != nullptr && !steps->values.empty()) {
        size_t at = 0;
        for (size_t f = 0; f < filaments; ++f) {
            seg_start[f] = at;
            seg_size[f]  = f < steps->values.size() ? size_t(std::max(1, steps->values[f])) : 1;
            at += seg_size[f];
        }
        packed_total = at;
    }
    const auto *flow_support = config.option<ConfigOptionStrings>("filament_flow_support");

    int written = 0;
    for (size_t f = 0; f < filaments && f < texts.size(); ++f) {
        std::vector<FilamentNozzleVariant> variants = parse_filament_nozzle_variants(texts[f], errors);
        if (variants.empty())
            continue;
        const size_t tool     = physical_extruder_of(config, unsigned(f + 1));
        const double diameter = nozzle_diameter_for_filament(config, unsigned(f + 1));
        const int    flow     = filament_flow_mode(config, unsigned(f));
        std::stable_sort(variants.begin(), variants.end(),
                         [](const FilamentNozzleVariant &a, const FilamentNozzleVariant &b) { return variant_specificity(a) < variant_specificity(b); });
        for (const FilamentNozzleVariant &v : variants) {
            if (v.tool > 0 && size_t(v.tool) != tool + 1)
                continue;
            if (v.diameter > 0. && !same_nozzle_diameter(v.diameter, diameter))
                continue;
            // A flow-specific entry sets the slot of its flow type in a key packed per flow type, and a plain
            // per-filament key only when the filament slices with that flow type.
            const bool flow_matches = v.flow_mode < 0 || v.flow_mode == flow;
            for (const auto &[key, value] : v.values) {
                auto *opt = dynamic_cast<ConfigOptionVectorBase *>(config.option(key));
                if (!is_filament_nozzle_variant_key(key) || opt == nullptr) {
                    if (errors)
                        errors->push_back("filament " + std::to_string(f + 1) + ": \"" + key + "\" is not a filament value");
                    continue;
                }
                const ConfigOptionDef        *def = print_config_def.get(key);
                std::unique_ptr<ConfigOption> one(def->create_default_option()); // not create_empty_option(): it throws for nullable enums
                if (!(value_text_valid(def->type, value) || (def->nullable && value == "nil")) || !one->deserialize(value) || !one->is_vector() ||
                    static_cast<ConfigOptionVectorBase *>(one.get())->size() != 1) {
                    if (errors)
                        errors->push_back("filament " + std::to_string(f + 1) + ": \"" + value + "\" is not a value of " + key);
                    continue;
                }
                std::vector<size_t> positions;
                const bool          packed = is_filament_flow_variant_option(key) && opt->size() == packed_total && packed_total != filaments;
                if (!packed && !flow_matches)
                    continue;
                if (packed) {
                    for (size_t pos = 0; pos < seg_size[f]; ++pos) {
                        const size_t at = seg_start[f] + pos;
                        if (v.flow_mode >= 0 && flow_support != nullptr && at < flow_support->values.size() &&
                            flow_support->values[at] != to_string(FilamentVolumeType(v.flow_mode)))
                            continue;
                        positions.push_back(at);
                    }
                } else if (opt->size() == filaments)
                    positions.push_back(f);
                else if (opt->size() == packed_total)
                    for (size_t pos = 0; pos < seg_size[f]; ++pos)
                        positions.push_back(seg_start[f] + pos);
                for (size_t at : positions) {
                    opt->set_at(one.get(), at, 0);
                    ++written;
                }
            }
        }
    }
    if (written > 0)
        BOOST_LOG_TRIVIAL(info) << "per-nozzle filament values: " << written << " value(s) taken from filament_nozzle_variants";
    return written;
}

// ---- 4b. Editing per-nozzle filament values (P5) ------------------------------------------------------------------

std::string format_nozzle_diameter_word(double diameter)
{
    std::ostringstream ss;
    ss.imbue(std::locale::classic());
    ss << std::setprecision(6) << diameter;
    return ss.str();
}

std::string format_filament_nozzle_selector(int tool, double diameter, int flow_mode)
{
    std::string out;
    auto add = [&out](const std::string &word) {
        if (!out.empty())
            out += ' ';
        out += word;
    };
    if (tool > 0)
        add("T" + std::to_string(tool));
    if (diameter > 0.)
        add(format_nozzle_diameter_word(diameter));
    if (flow_mode >= 0)
        add(to_string(FilamentVolumeType(flow_mode)));
    return out;
}

std::string format_filament_nozzle_variants(const std::vector<FilamentNozzleVariant> &variants)
{
    std::string out;
    for (const FilamentNozzleVariant &v : variants) {
        if (v.values.empty())
            continue;
        const std::string selector = format_filament_nozzle_selector(v.tool, v.diameter, v.flow_mode);
        if (selector.empty())
            continue;
        if (!out.empty())
            out += " | ";
        out += selector + ":";
        for (size_t i = 0; i < v.values.size(); ++i)
            out += (i == 0 ? " " : ", ") + v.values[i].first + "=" + v.values[i].second;
    }
    return out;
}

bool is_filament_nozzle_size_key(const std::string &key)
{
    if (!is_filament_nozzle_variant_key(key))
        return false;
    // The spool's and the bed's values, not the nozzle's.
    static const std::set<std::string> not_per_nozzle { "filament_cost", "filament_density", "filament_soluble", "filament_is_support",
                                                        "filament_shrink", "filament_shrinkage_compensation_z", "required_nozzle_HRC",
                                                        "temperature_vitrification", "chamber_temperature", "filament_flow_step_size",
                                                        "pellet_flow_coefficient", "filament_change_length", "idle_temperature" };
    if (not_per_nozzle.count(key) > 0)
        return false;
    if (boost::algorithm::ends_with(key, "_plate_temp") || boost::algorithm::ends_with(key, "_plate_temp_initial_layer"))
        return false;
    const ConfigOptionDef *def = print_config_def.get(key);
    switch (def->type) {
    case coFloats:
    case coInts:
    case coPercents:
    case coFloatsOrPercents:
    case coBools:
    case coEnums: return true;
    default: return false;
    }
}

std::string validate_filament_nozzle_value(const std::string &key, const std::string &value)
{
    if (!is_filament_nozzle_variant_key(key))
        return "\"" + key + "\" is not a filament value that can differ per nozzle";
    if (value.empty())
        return "no value";
    if (value.find_first_of(",|:\n\r") != std::string::npos)
        return "a value cannot contain , | or :";
    const ConfigOptionDef *def = print_config_def.get(key);
    if (def->nullable && value == "nil")
        return std::string();
    std::unique_ptr<ConfigOption> one(def->create_default_option()); // not create_empty_option(): it throws for nullable enums
    bool                          ok = value_text_valid(def->type, value);
    if (ok) {
        try {
            ok = one->deserialize(value) && one->is_vector() && static_cast<ConfigOptionVectorBase *>(one.get())->size() == 1;
        } catch (...) {
            ok = false;
        }
    }
    if (!ok)
        return "\"" + value + "\" is not a value of " + key;
    // The option's own bounds, where it has them.
    if (def->type == coFloats || def->type == coInts) {
        const double v = std::strtod(value.c_str(), nullptr);
        if (v < def->min || v > def->max)
            return "\"" + value + "\" is outside " + format_nozzle_diameter_word(def->min) + " .. " + format_nozzle_diameter_word(def->max);
    }
    return std::string();
}

std::vector<std::string> validate_filament_nozzle_variants(const std::string &text)
{
    std::vector<std::string>           errors;
    std::vector<FilamentNozzleVariant> variants = parse_filament_nozzle_variants(text, &errors);
    for (const FilamentNozzleVariant &v : variants)
        for (const auto &[key, value] : v.values)
            if (std::string error = validate_filament_nozzle_value(key, value); !error.empty())
                errors.push_back(format_filament_nozzle_selector(v.tool, v.diameter, v.flow_mode) + ": " + key + ": " + error);
    return errors;
}

namespace {

bool same_selector(const FilamentNozzleVariant &v, int tool, double diameter, int flow_mode)
{
    return std::max(0, v.tool) == std::max(0, tool) && v.flow_mode == flow_mode &&
           ((v.diameter <= 0. && diameter <= 0.) || (v.diameter > 0. && diameter > 0. && same_nozzle_diameter(v.diameter, diameter)));
}

} // namespace

bool find_filament_nozzle_value(const std::string &text, int tool, double diameter, int flow_mode, const std::string &key, std::string *value)
{
    for (const FilamentNozzleVariant &v : parse_filament_nozzle_variants(text))
        if (same_selector(v, tool, diameter, flow_mode))
            for (const auto &[k, val] : v.values)
                if (k == key) {
                    if (value != nullptr)
                        *value = val;
                    return true;
                }
    return false;
}

std::string set_filament_nozzle_value(const std::string &text, int tool, double diameter, int flow_mode, const std::string &key,
                                      const std::string &value)
{
    std::vector<FilamentNozzleVariant> variants = parse_filament_nozzle_variants(text);
    bool                               done     = false;
    for (FilamentNozzleVariant &v : variants) {
        if (!same_selector(v, tool, diameter, flow_mode))
            continue;
        auto it = std::find_if(v.values.begin(), v.values.end(), [&key](const auto &kv) { return kv.first == key; });
        if (value.empty()) {
            if (it != v.values.end())
                v.values.erase(it);
        } else if (!done) {
            if (it != v.values.end())
                it->second = value;
            else
                v.values.emplace_back(key, value);
            done = true;
        } else if (it != v.values.end())
            v.values.erase(it); // a second entry of the same selector: the first one holds the value
    }
    if (!value.empty() && !done) {
        FilamentNozzleVariant v;
        v.tool      = std::max(0, tool);
        v.diameter  = diameter > 0. ? diameter : 0.;
        v.flow_mode = flow_mode;
        v.values.emplace_back(key, value);
        variants.push_back(std::move(v));
    }
    return format_filament_nozzle_variants(variants);
}

std::vector<double> filament_nozzle_variant_diameters(const std::string &text)
{
    std::vector<double> out;
    for (const FilamentNozzleVariant &v : parse_filament_nozzle_variants(text))
        if (v.diameter > 0. &&
            std::none_of(out.begin(), out.end(), [&v](double d) { return same_nozzle_diameter(d, v.diameter); }))
            out.push_back(v.diameter);
    std::sort(out.begin(), out.end());
    return out;
}

int apply_filament_nozzle_size_view(DynamicPrintConfig &filament, double diameter, int flow_mode)
{
    if (!(diameter > 0.))
        return 0;
    const auto *text_opt = filament.option<ConfigOptionStrings>("filament_nozzle_variants");
    if (text_opt == nullptr || text_opt->values.empty())
        return 0;
    std::vector<FilamentNozzleVariant> variants = parse_filament_nozzle_variants(text_opt->values.front());
    variants.erase(std::remove_if(variants.begin(), variants.end(),
                                  [diameter](const FilamentNozzleVariant &v) {
                                      return v.tool > 0 || (v.diameter > 0. && !same_nozzle_diameter(v.diameter, diameter));
                                  }),
                   variants.end());
    std::stable_sort(variants.begin(), variants.end(),
                     [](const FilamentNozzleVariant &a, const FilamentNozzleVariant &b) { return variant_specificity(a) < variant_specificity(b); });
    std::vector<std::string> modes;
    if (const auto *support = filament.option<ConfigOptionStrings>("filament_flow_support"))
        modes = support->values;
    int written = 0;
    for (const FilamentNozzleVariant &v : variants)
        for (const auto &[key, value] : v.values) {
            auto *opt = dynamic_cast<ConfigOptionVectorBase *>(filament.option(key));
            if (opt == nullptr || opt->size() == 0 || !validate_filament_nozzle_value(key, value).empty())
                continue;
            const ConfigOptionDef        *def = print_config_def.get(key);
            std::unique_ptr<ConfigOption> one(def->create_default_option()); // not create_empty_option(): it throws for nullable enums
            one->deserialize(value);
            const bool per_mode = is_filament_flow_variant_option(key) && opt->size() > 1 && opt->size() == modes.size();
            for (size_t at = 0; at < opt->size(); ++at) {
                if (v.flow_mode >= 0) {
                    if (per_mode) {
                        if (modes[at] != to_string(FilamentVolumeType(v.flow_mode)))
                            continue;
                    } else if (flow_mode != v.flow_mode)
                        continue;
                }
                opt->set_at(one.get(), at, 0);
                ++written;
            }
        }
    return written;
}

// ---- 5. physical_nozzle_properties ------------------------------------------------------------------------------

std::string physical_nozzle_properties(const ConfigBase &config)
{
    const std::vector<double>                  nozzles = floats_of(config, "nozzle_diameter");
    const std::vector<std::vector<RackNozzle>> racks   = extruder_rack_nozzles(config);
    std::string                                out;
    auto add = [&out](double diameter, NozzleVolumeType type) {
        if (!out.empty())
            out += ',';
        out += format_diameter_to_str(diameter);
        out += '-';
        out += nozzle_volume_word(type);
    };
    for (size_t e = 0; e < nozzles.size(); ++e) {
        if (e < racks.size() && !racks[e].empty())
            for (const RackNozzle &nozzle : racks[e])
                add(nozzle.diameter, nozzle.volume_type);
        else
            add(nozzles[e], extruder_volume_type(config, e));
    }
    return out;
}

bool emit_physical_nozzle_properties(const ConfigBase &config)
{
    if (bool_of(config, "support_filament_nozzle_mapping"))
        return true;
    if (has_mixed_rack_diameters(config))
        return true;
    const std::vector<double> nozzles = floats_of(config, "nozzle_diameter");
    if (nozzles.size() < 2)
        return false;
    if (const auto *semm = opt_of<ConfigOptionBool>(config, "single_extruder_multi_material"); semm != nullptr && semm->value)
        return false;
    return std::any_of(nozzles.begin(), nozzles.end(), [&nozzles](double d) { return !same_nozzle_diameter(d, nozzles.front()); });
}

// ---- 7. The G-code's tool numbers -------------------------------------------------------------------------------

std::vector<int> filament_output_tools(const PrintConfig &config, const std::vector<unsigned int> &used_filaments, bool *shared)
{
    if (shared)
        *shared = false;
    const std::vector<int> &assignment = config.filament_nozzle_assignment.values;
    if (std::none_of(assignment.begin(), assignment.end(), [](int t) { return t > 0; }))
        return {};
    const size_t tools     = config.nozzle_diameter.values.size();
    const size_t filaments = config.filament_diameter.values.size();
    if (tools < 2 || filaments == 0 || filaments > tools || config.single_extruder_multi_material.value)
        return {};
    // Bambu's grouping machines route filaments with filament_map; their G-code tool is the filament (AMS slot).
    if (is_bambu_grouping_machine(config, tools))
        return {};
    std::vector<int>  out(filaments, -1);
    std::vector<bool> taken(tools, false);
    for (unsigned int f : used_filaments) {
        if (f >= filaments)
            continue;
        const size_t tool = physical_extruder_for_filament(config, f + 1);
        if (tool >= tools)
            return {};
        if (taken[tool]) {
            if (shared)
                *shared = true;
            return {};
        }
        taken[tool] = true;
        out[f]      = int(tool);
    }
    // The unused filaments: their own tool when it is free, else the first free one.
    for (size_t f = 0; f < filaments; ++f) {
        if (out[f] >= 0)
            continue;
        if (f < tools && !taken[f]) {
            out[f]   = int(f);
            taken[f] = true;
            continue;
        }
        for (size_t t = 0; t < tools; ++t)
            if (!taken[t]) {
                out[f]   = int(t);
                taken[t] = true;
                break;
            }
    }
    bool identity = true;
    for (size_t f = 0; f < filaments; ++f)
        identity &= out[f] == int(f);
    if (identity)
        return {};
    return out;
}

void filament_values_to_tool_space(DynamicConfig &out, const ConfigBase &source, const std::vector<int> &filament_tools)
{
    const size_t filaments = filament_tools.size();
    if (filaments == 0)
        return;
    const std::vector<int> tool_filaments = output_tool_filaments(filament_tools);
    const size_t           tools          = std::max(tool_filaments.size(), filaments);
    auto value_of = [&out, &source](const std::string &key) -> const ConfigOption * {
        if (const ConfigOption *opt = out.option(key))
            return opt;
        return source.option(key);
    };
    // Packed flow-type segments, as get_config_idx(Filament) reads them.
    size_t packed_total = filaments;
    if (const auto *steps = dynamic_cast<const ConfigOptionInts *>(source.option("filament_flow_step_size")); steps != nullptr && !steps->values.empty()) {
        packed_total = 0;
        for (size_t f = 0; f < filaments; ++f)
            packed_total += f < steps->values.size() ? size_t(std::max(1, steps->values[f])) : 1;
    }

    std::set<std::string> keys(Preset::filament_options().begin(), Preset::filament_options().end());
    for (const char *key : { "filament_colour", "extruder_colour", "filament_volume_type", "filament_multi_colors", "filament_colour_type",
                             "filament_rack_nozzle_diameter", "filament_flow_support", "temperature", "first_layer_temperature",
                             "filament_multitool_ramming" })
        keys.insert(key);
    keys.erase("filament_nozzle_assignment");
    keys.erase("filament_flow_step_size");

    for (const std::string &key : keys) {
        const auto *src = dynamic_cast<const ConfigOptionVectorBase *>(value_of(key));
        if (src == nullptr || src->size() == 0)
            continue;
        // One value per filament: resolve a packed key to each filament's own flow type first.
        std::unique_ptr<ConfigOptionVectorBase> per_filament(static_cast<ConfigOptionVectorBase *>(src->clone()));
        if (src->size() == packed_total && packed_total != filaments) {
            per_filament->resize(filaments);
            for (size_t f = 0; f < filaments; ++f)
                per_filament->set_at(src, f, get_config_idx(source, ConfigFlowDomain::Filament, unsigned(f)));
        } else if (src->size() != filaments)
            continue;
        std::unique_ptr<ConfigOptionVectorBase> per_tool(static_cast<ConfigOptionVectorBase *>(per_filament->clone()));
        per_tool->resize(tools);
        for (size_t t = 0; t < tools; ++t) {
            const int f = t < tool_filaments.size() && tool_filaments[t] >= 0 ? tool_filaments[t] : 0;
            per_tool->set_at(per_filament.get(), t, size_t(f));
        }
        out.set_key_value(key, per_tool.release());
    }
    // P5: the purge matrices (one filaments x filaments block per extruder, from -> to) in tool space too: entry
    // (from tool i, to tool j) is the purge between the filaments those tools print.
    if (const auto *matrix = dynamic_cast<const ConfigOptionFloats *>(value_of("flush_volumes_matrix")); matrix != nullptr) {
        const size_t block = filaments * filaments;
        if (!matrix->values.empty() && matrix->values.size() % block == 0) {
            const size_t        blocks = matrix->values.size() / block;
            std::vector<double> values;
            values.reserve(blocks * tools * tools);
            auto filament_of = [&tool_filaments](size_t t) {
                return t < tool_filaments.size() && tool_filaments[t] >= 0 ? size_t(tool_filaments[t]) : size_t(0);
            };
            for (size_t b = 0; b < blocks; ++b)
                for (size_t from = 0; from < tools; ++from)
                    for (size_t to = 0; to < tools; ++to)
                        values.push_back(matrix->values[b * block + filament_of(from) * filaments + filament_of(to)]);
            out.set_key_value("flush_volumes_matrix", new ConfigOptionFloats(std::move(values)));
        }
    }
    if (source.option("filament_flow_step_size") != nullptr)
        out.set_key_value("filament_flow_step_size", new ConfigOptionInts(tools, 1));
    if (source.option("filament_nozzle_assignment") != nullptr)
        out.set_key_value("filament_nozzle_assignment", new ConfigOptionInts());
}

std::vector<int> output_tool_filaments(const std::vector<int> &filament_tools)
{
    int max_tool = -1;
    for (int t : filament_tools)
        max_tool = std::max(max_tool, t);
    std::vector<int> out(size_t(max_tool + 1), -1);
    for (size_t f = 0; f < filament_tools.size(); ++f)
        if (filament_tools[f] >= 0)
            out[size_t(filament_tools[f])] = int(f);
    return out;
}

} // namespace Slic3r
