#include "BambuMultiDiameter.hpp"

#include "../Config.hpp"
#include "../LocalesUtils.hpp"
#include "../NozzleOverride.hpp"
#include "../PrintConfig.hpp"
#include "../ProjectTask.hpp"
#include "../ToolSlots.hpp"

#include <algorithm>
#include <cmath>

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

namespace Slic3r {
namespace BambuMultiDiameter {

namespace {

// Bambu's NozzleVolumeType names (extruder_nozzle_volume_type) -> the flow word of a rack text ("0.4:high_flow").
std::string rack_flow_word(const std::string &bambu_name)
{
    if (bambu_name == "High Flow")
        return "high_flow";
    if (bambu_name == "TPU High Flow")
        return "tpu_high_flow";
    if (bambu_name == "TPU285")
        return "tpu285";
    if (bambu_name == "E3D High Flow")
        return "e3d_high_flow";
    return std::string(); // Standard, Hybrid or unknown: the plain size
}

// One filament_diameter_map entry as a size; 0 when it is empty, a ";" list (Bambu's "not chosen yet") or no number.
double selected_diameter(const std::string &entry)
{
    if (entry.empty() || entry.find(';') != std::string::npos)
        return 0.;
    const double d = string_to_double_decimal_point(entry);
    return d > 0. ? d : 0.;
}

std::vector<double> floats(const ConfigBase &config, const char *key)
{
    if (const auto *opt = dynamic_cast<const ConfigOptionFloats *>(config.option(key)))
        return opt->values;
    return {};
}

std::string json_scalar_text(const nlohmann::json &value)
{
    if (value.is_string())
        return value.get<std::string>();
    if (value.is_boolean())
        return value.get<bool>() ? "1" : "0";
    if (value.is_number())
        return value.dump();
    return std::string();
}

std::vector<std::string> json_string_list(const nlohmann::json &value)
{
    std::vector<std::string> out;
    if (value.is_array()) {
        for (const nlohmann::json &v : value)
            out.push_back(json_scalar_text(v));
    } else if (value.is_string()) {
        // A config text ("0.4;0.8" style is a single entry for Bambu, so only a bare value is taken as one entry).
        out.push_back(value.get<std::string>());
    }
    return out;
}

} // namespace

bool single_extruder_rack(const ConfigBase &config)
{
    if (effective_nozzle_diameters(config).size() != 1)
        return false;
    const std::vector<std::vector<RackNozzle>> racks = extruder_rack_nozzles(config);
    return !racks.empty() && racks.front().size() > 1;
}

bool mode_for_job(bool uses_multiple_nozzle_diameters, const ConfigBase &config)
{
    return uses_multiple_nozzle_diameters && single_extruder_rack(config);
}

std::vector<std::string> filament_diameter_map(const ConfigBase &config)
{
    size_t count = 0;
    if (const auto *colours = dynamic_cast<const ConfigOptionStrings *>(config.option("filament_colour")))
        count = colours->values.size();
    std::vector<std::string> out;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const double d = nozzle_diameter_for_filament(config, unsigned(i + 1));
        out.push_back(d > 0. ? format_diameter_to_str(d) : std::string());
    }
    return out;
}

bool config_uses_multiple_diameters(const ConfigBase &config)
{
    double first = 0.;
    for (const std::string &entry : filament_diameter_map(config)) {
        const double d = selected_diameter(entry);
        if (d <= 0.)
            continue;
        if (first <= 0.)
            first = d;
        else if (!same_nozzle_diameter(d, first))
            return true;
    }
    return false;
}

void add_project_keys(DynamicPrintConfig &out, const ConfigBase &config)
{
    // Neither key has a definition here (no switch, by owner decision); the project writer serializes any option.
    out.set_key_value(MODE_KEY, new ConfigOptionBool(true));
    out.set_key_value(DIAMETER_MAP_KEY, new ConfigOptionStrings(filament_diameter_map(config)));
}

std::string slice_info_nozzle_diameters(const std::vector<FilamentInfo> &used_filaments, size_t extruder_count,
                                        const std::string &fallback)
{
    // Bambu's list is ascending by slot and takes its front; take the lowest slot whatever the order.
    const auto lowest = std::min_element(used_filaments.begin(), used_filaments.end(),
                                         [](const FilamentInfo &a, const FilamentInfo &b) { return a.id < b.id; });
    if (lowest == used_filaments.end() || !(lowest->nozzle_diameter > 0.))
        return fallback;
    return ConfigOptionFloats(std::vector<double>(std::max<size_t>(extruder_count, 1), lowest->nozzle_diameter)).serialize();
}

ImportedKeys read_keys(const std::string &json_text)
{
    ImportedKeys keys;
    // Most projects carry neither key; skip the second parse for them.
    if (json_text.find(MODE_KEY) == std::string::npos && json_text.find(DIAMETER_MAP_KEY) == std::string::npos)
        return keys;
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json_text);
    } catch (const std::exception &) {
        return keys;
    }
    if (!j.is_object())
        return keys;
    if (auto it = j.find(MODE_KEY); it != j.end()) {
        keys.has_mode         = true;
        const std::string txt = json_scalar_text(*it);
        keys.mode             = txt == "1" || txt == "true";
    }
    if (auto it = j.find(DIAMETER_MAP_KEY); it != j.end()) {
        keys.has_map      = true;
        keys.diameter_map = json_string_list(*it);
    }
    if (auto it = j.find(SLOT_DIAMETER_KEY); it != j.end())
        for (const std::string &entry : json_string_list(*it))
            keys.slot_diameters.push_back(selected_diameter(entry));
    if (auto it = j.find(SLOT_VOLUME_KEY); it != j.end())
        keys.slot_volume_types = json_string_list(*it);
    return keys;
}

ImportResult apply_import(DynamicPrintConfig &config, const ImportedKeys &keys)
{
    ImportResult result;
    if (!keys.mode)
        return result;

    // 1. The rack: Bambu's per-slot nozzles of the (single) extruder, when the project has no rack text of its own.
    if (keys.slot_diameters.size() > 1 && floats(config, "nozzle_diameter").size() == 1 &&
        std::all_of(keys.slot_diameters.begin(), keys.slot_diameters.end(), [](double d) { return d > 0.; })) {
        const auto *rack      = dynamic_cast<const ConfigOptionStrings *>(config.option("extruder_rack_nozzle_override"));
        const bool  own_rack  = rack != nullptr &&
                               std::any_of(rack->values.begin(), rack->values.end(), [](const std::string &t) { return !t.empty(); });
        if (!own_rack) {
            const bool  with_flow = keys.slot_volume_types.size() == keys.slot_diameters.size();
            std::string text;
            for (size_t i = 0; i < keys.slot_diameters.size(); ++i) {
                if (i > 0)
                    text += ",";
                text += format_diameter_to_str(keys.slot_diameters[i]);
                if (with_flow)
                    if (const std::string flow = rack_flow_word(keys.slot_volume_types[i]); !flow.empty())
                        text += ":" + flow;
            }
            config.set_key_value("extruder_rack_nozzle_override", new ConfigOptionStrings({ text }));
            result.rack_set = true;
        }
    }

    // 2. The extruders' sizes: back onto the preset's size (printer_variant) with the installed size as the override.
    if (!has_nozzle_diameter_override(config)) {
        std::vector<double>       nozzles  = floats(config, "nozzle_diameter");
        double                    variant  = 0.;
        if (const auto *opt = dynamic_cast<const ConfigOptionString *>(config.option("printer_variant"))) {
            const std::vector<double> sizes = printer_variant_sizes(opt->value);
            if (sizes.size() == 1)
                variant = sizes.front();
        }
        if (variant > 0. && !nozzles.empty()) {
            std::vector<double> override_values(nozzles.size(), 0.);
            bool                any = false;
            for (size_t e = 0; e < nozzles.size(); ++e)
                if (nozzles[e] > 0. && !same_nozzle_diameter(nozzles[e], variant)) {
                    override_values[e] = nozzles[e];
                    nozzles[e]         = variant;
                    any                = true;
                }
            if (any) {
                config.set_key_value("nozzle_diameter", new ConfigOptionFloats(nozzles));
                config.set_key_value(NOZZLE_DIAMETER_OVERRIDE_KEY,
                                     new ConfigOptionFloats(nozzle_diameter_override_stored(override_values, nozzles)));
                result.override_set = true;
            }
        }
    }

    // 3. The filaments: a selected size other than the one the filament's extruder prints with becomes a rack pin.
    const std::vector<double> pins_now = floats(config, "filament_rack_nozzle_diameter");
    const bool                pinned   = std::any_of(pins_now.begin(), pins_now.end(), [](double d) { return d > 0.; }) ||
                                has_filament_rack_slot_pins(config);
    if (keys.has_map && !pinned) {
        size_t filaments = keys.diameter_map.size();
        if (const auto *colours = dynamic_cast<const ConfigOptionStrings *>(config.option("filament_colour")))
            filaments = std::max(filaments, colours->values.size());
        std::vector<double> pins;
        for (size_t f = 0; f < keys.diameter_map.size(); ++f) {
            const double want = selected_diameter(keys.diameter_map[f]);
            if (want <= 0.)
                continue;
            const double own = nozzle_diameter_for_filament(static_cast<const ConfigBase &>(config), unsigned(f + 1));
            if (own > 0. && same_nozzle_diameter(want, own))
                continue;
            pins = set_filament_rack_pin(std::move(pins), f, filaments, want);
            ++result.pins_set;
        }
        if (result.pins_set > 0)
            config.set_key_value("filament_rack_nozzle_diameter", new ConfigOptionFloats(pins));
    }

    BOOST_LOG_TRIVIAL(info) << "Bambu multi-diameter project: rack " << (result.rack_set ? "set" : "kept") << ", nozzle size override "
                            << (result.override_set ? "set" : "kept") << ", " << result.pins_set << " filament pin(s)";
    return result;
}

void forget_consumed_keys(std::vector<std::string> &unrecognized_keys, const ImportedKeys &keys, const ImportResult &result)
{
    auto forget = [&unrecognized_keys](const char *key) {
        unrecognized_keys.erase(std::remove(unrecognized_keys.begin(), unrecognized_keys.end(), std::string(key)), unrecognized_keys.end());
    };
    if (keys.has_mode)
        forget(MODE_KEY);
    if (keys.has_map)
        forget(DIAMETER_MAP_KEY);
    if (result.rack_set)
        forget(SLOT_DIAMETER_KEY);
}

} // namespace BambuMultiDiameter
} // namespace Slic3r
