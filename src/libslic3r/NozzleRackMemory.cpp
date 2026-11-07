#include "NozzleRackMemory.hpp"

#include "Config.hpp"
#include "NozzleOverride.hpp"
#include "ToolSlots.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace Slic3r {

namespace {

bool rack_has_type(const std::vector<NozzleVolumeType> &types, NozzleVolumeType t)
{
    return std::find(types.begin(), types.end(), t) != types.end();
}

void add_type(std::vector<NozzleVolumeType> &types, NozzleVolumeType t)
{
    if (!rack_has_type(types, t))
        types.push_back(t);
}

bool has_size(const std::vector<double> &sizes, double d)
{
    return std::any_of(sizes.begin(), sizes.end(), [d](double s) { return same_nozzle_diameter(s, d); });
}

std::vector<int> capacity_of(const ConfigBase &printer, size_t extruders)
{
    std::vector<int>    out(extruders, 1);
    const ConfigOption *opt = printer.option("extruder_max_nozzle_count");
    if (const auto *counts = dynamic_cast<const ConfigOptionIntsNullable *>(opt)) {
        for (size_t e = 0; e < extruders && e < counts->values.size(); ++e)
            out[e] = counts->is_nil(e) ? 1 : std::max(1, counts->values[e]);
    } else if (const auto *counts = dynamic_cast<const ConfigOptionInts *>(opt)) {
        for (size_t e = 0; e < extruders && e < counts->values.size(); ++e)
            out[e] = std::max(1, counts->values[e]);
    }
    return out;
}

// The types one printer config names, per extruder of `capacity`.
std::vector<std::vector<NozzleVolumeType>> types_of(const ConfigBase &printer, const std::vector<int> &capacity)
{
    const size_t                               extruders = capacity.size();
    std::vector<std::vector<NozzleVolumeType>> out(extruders);
    for (auto &t : out)
        t.push_back(nvtStandard);
    auto add_all = [&out](NozzleVolumeType t) {
        for (auto &types : out)
            add_type(types, t);
    };
    // extruder_variant_list: one text per extruder, "Direct Drive Standard,Direct Drive High Flow".
    if (const auto *list = dynamic_cast<const ConfigOptionStrings *>(printer.option("extruder_variant_list"))) {
        for (size_t i = 0; i < list->values.size(); ++i) {
            const std::string &text  = list->values[i];
            size_t             start = 0;
            while (start <= text.size()) {
                size_t comma = text.find(',', start);
                if (comma == std::string::npos)
                    comma = text.size();
                NozzleVolumeType t = nvtStandard;
                if (nozzle_type_of_extruder_variant(text.substr(start, comma - start), t)) {
                    if (list->values.size() == extruders)
                        add_type(out[i], t);
                    else
                        add_all(t);
                }
                start = comma + 1;
            }
        }
    }
    // default_nozzle_volume_type: per extruder, or (Bambu Studio 2.9.1, H3C) one per rack slot of every extruder.
    size_t total = 0;
    for (int c : capacity)
        total += size_t(std::max(1, c));
    for (const char *key : { "default_nozzle_volume_type", "nozzle_volume_type" })
        if (const auto *opt = dynamic_cast<const ConfigOptionEnumsGeneric *>(printer.option(key))) {
            const std::vector<int> &values = opt->values;
            if (values.size() == extruders) {
                for (size_t e = 0; e < extruders; ++e)
                    add_type(out[e], NozzleVolumeType(values[e]));
            } else if (values.size() == total && total > extruders) {
                size_t first = 0;
                for (size_t e = 0; e < extruders; ++e) {
                    for (int k = 0; k < std::max(1, capacity[e]); ++k)
                        add_type(out[e], NozzleVolumeType(values[first + size_t(k)]));
                    first += size_t(std::max(1, capacity[e]));
                }
            } else {
                for (int v : values)
                    add_all(NozzleVolumeType(v));
            }
        }
    return out;
}

std::vector<double> sizes_of(const ConfigBase &printer)
{
    std::vector<std::string> variants;
    if (const auto *variant = dynamic_cast<const ConfigOptionString *>(printer.option("printer_variant")); variant != nullptr && !variant->value.empty())
        variants.push_back(variant->value);
    std::vector<double> current;
    if (const auto *nd = dynamic_cast<const ConfigOptionFloats *>(printer.option("nozzle_diameter")))
        current = nd->values;
    return nozzle_size_choices(current, variants);
}

} // namespace

std::string serialize_nozzle_rack_memory(const NozzleRackMemory &memory)
{
    if (memory.empty())
        return std::string();
    nlohmann::json j;
    j["diameters"] = memory.diameters;
    j["types"]     = memory.volume_types;
    j["racks"]     = memory.rack_texts;
    return j.dump();
}

bool parse_nozzle_rack_memory(const std::string &text, NozzleRackMemory &out)
{
    out = NozzleRackMemory();
    if (text.empty())
        return false;
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return false;
    NozzleRackMemory m;
    auto             diameters = j.find("diameters");
    if (diameters == j.end() || !diameters->is_array() || diameters->empty())
        return false;
    for (const auto &v : *diameters) {
        if (!v.is_number())
            return false;
        const double d = v.get<double>();
        if (!(d > 0.) || !std::isfinite(d))
            return false;
        m.diameters.push_back(d);
    }
    if (auto types = j.find("types"); types != j.end()) {
        if (!types->is_array())
            return false;
        for (const auto &v : *types) {
            if (!v.is_number_integer())
                return false;
            const int t = v.get<int>();
            if (t < 0 || t > int(nvtMaxNozzleVolumeType))
                return false;
            m.volume_types.push_back(t);
        }
    }
    if (auto racks = j.find("racks"); racks != j.end()) {
        if (!racks->is_array())
            return false;
        for (const auto &v : *racks) {
            if (!v.is_string())
                return false;
            m.rack_texts.push_back(v.get<std::string>());
        }
    }
    out = std::move(m);
    return true;
}

bool nozzle_type_of_extruder_variant(const std::string &variant, NozzleVolumeType &out)
{
    std::string text = variant;
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.pop_back();
    size_t best = 0;
    for (int t = 0; t <= int(nvtMaxNozzleVolumeType); ++t) {
        const std::string name = get_nozzle_volume_type_string(NozzleVolumeType(t));
        if (name.size() > text.size() || name.size() <= best)
            continue;
        if (text.compare(text.size() - name.size(), name.size(), name) != 0)
            continue;
        // A whole word: "Direct Drive High Flow" names High Flow, "TPU High Flow" is the longer match.
        if (text.size() > name.size() && !std::isspace(static_cast<unsigned char>(text[text.size() - name.size() - 1])))
            continue;
        best = name.size();
        out  = NozzleVolumeType(t);
    }
    return best > 0;
}

NozzleRackLimits nozzle_rack_limits(const ConfigBase &printer)
{
    NozzleRackLimits limits;
    size_t           extruders = 0;
    if (const auto *nd = dynamic_cast<const ConfigOptionFloats *>(printer.option("nozzle_diameter")))
        extruders = nd->values.size();
    limits.capacity = capacity_of(printer, extruders);
    limits.types    = types_of(printer, limits.capacity);
    limits.sizes    = sizes_of(printer);
    return limits;
}

void merge_nozzle_rack_limits(NozzleRackLimits &into, const ConfigBase &variant)
{
    const NozzleRackLimits other = nozzle_rack_limits(variant);
    if (other.capacity.size() != into.capacity.size())
        return;
    for (size_t e = 0; e < into.capacity.size(); ++e) {
        into.capacity[e] = std::max(into.capacity[e], other.capacity[e]);
        for (NozzleVolumeType t : other.types[e])
            add_type(into.types[e], t);
    }
    for (double d : other.sizes)
        if (!has_size(into.sizes, d))
            into.sizes.push_back(d);
    std::sort(into.sizes.begin(), into.sizes.end());
}

std::vector<NozzleVolumeType> nozzle_rack_types(const NozzleRackLimits &limits, size_t extruder)
{
    std::vector<NozzleVolumeType> out{ nvtStandard };
    if (extruder >= limits.types.size())
        return out;
    for (int t = 0; t <= int(nvtMaxNozzleVolumeType); ++t)
        if (NozzleVolumeType(t) != nvtStandard && rack_has_type(limits.types[extruder], NozzleVolumeType(t)))
            out.push_back(NozzleVolumeType(t));
    return out;
}

std::string validate_nozzle_rack_memory(const NozzleRackMemory &memory, const NozzleRackLimits &limits)
{
    const size_t extruders = limits.capacity.size();
    if (extruders == 0 || memory.diameters.size() != extruders)
        return "remembered for " + std::to_string(memory.diameters.size()) + " extruders, the printer has " + std::to_string(extruders);
    auto type_fits = [&limits](size_t e, NozzleVolumeType t) {
        return t == nvtStandard || (e < limits.types.size() && rack_has_type(limits.types[e], t));
    };
    for (size_t e = 0; e < extruders; ++e)
        if (!limits.sizes.empty() && !has_size(limits.sizes, memory.diameters[e]))
            return "extruder " + std::to_string(e + 1) + ": no " + format_nozzle_diameter_word(memory.diameters[e]) + " mm nozzle for this printer";
    if (!memory.volume_types.empty()) {
        if (memory.volume_types.size() != extruders)
            return "flow types for " + std::to_string(memory.volume_types.size()) + " extruders";
        for (size_t e = 0; e < extruders; ++e)
            if (!type_fits(e, NozzleVolumeType(memory.volume_types[e])))
                return "extruder " + std::to_string(e + 1) + ": no " + get_nozzle_volume_type_string(NozzleVolumeType(memory.volume_types[e])) +
                       " nozzle for this printer";
    }
    if (!memory.rack_texts.empty()) {
        if (memory.rack_texts.size() != extruders)
            return "racks for " + std::to_string(memory.rack_texts.size()) + " extruders";
        for (size_t e = 0; e < extruders; ++e) {
            const std::string &text = memory.rack_texts[e];
            if (text.empty())
                continue;
            const std::string where = "extruder " + std::to_string(e + 1) + " rack: ";
            if (limits.capacity[e] < 2)
                return where + "the extruder has no rack";
            if (std::string error = validate_rack_nozzle_text(text, memory.diameters[e]); !error.empty())
                return where + error;
            const std::vector<RackNozzle> rack = parse_rack_nozzles(text);
            if (rack.empty())
                return where + "no nozzle";
            if (rack.size() > size_t(limits.capacity[e]))
                return where + std::to_string(rack.size()) + " nozzles, " + std::to_string(limits.capacity[e]) + " slots";
            for (const RackNozzle &n : rack) {
                if (!limits.sizes.empty() && !has_size(limits.sizes, n.diameter))
                    return where + "no " + format_nozzle_diameter_word(n.diameter) + " mm nozzle for this printer";
                if (n.volume_given && !type_fits(e, n.volume_type))
                    return where + "no " + get_nozzle_volume_type_string(n.volume_type) + " nozzle for this printer";
            }
        }
    }
    return std::string();
}

} // namespace Slic3r
