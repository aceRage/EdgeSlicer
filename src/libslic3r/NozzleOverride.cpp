// Per-nozzle process P7 - see NozzleOverride.hpp.

#include "NozzleOverride.hpp"

#include "PrintConfig.hpp"
#include "ToolSlots.hpp"
#include "BambuDualNozzleSync.hpp"
#include "NozzleProcess.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace Slic3r {

namespace {

// A nozzle size we accept from the app config, a picker or a printer report.
bool plausible_size(double d) { return d > 0.05 && d < 3.; }

} // namespace

std::vector<double> nozzle_diameter_override_values(const ConfigBase &config)
{
    if (const auto *opt = dynamic_cast<const ConfigOptionFloats *>(config.option(NOZZLE_DIAMETER_OVERRIDE_KEY)))
        return opt->values;
    return {};
}

bool has_nozzle_diameter_override(const ConfigBase &config)
{
    const std::vector<double> values = nozzle_diameter_override_values(config);
    return std::any_of(values.begin(), values.end(), [](double v) { return v > 0.; });
}

std::vector<double> nozzle_diameters_with_override(std::vector<double> preset, const std::vector<double> &override_values)
{
    for (size_t e = 0; e < std::min(preset.size(), override_values.size()); ++e)
        if (override_values[e] > 0.)
            preset[e] = override_values[e];
    return preset;
}

std::vector<double> effective_nozzle_diameters(const ConfigBase &config)
{
    std::vector<double> preset;
    if (const auto *nozzles = dynamic_cast<const ConfigOptionFloats *>(config.option("nozzle_diameter")))
        preset = nozzles->values;
    return nozzle_diameters_with_override(std::move(preset), nozzle_diameter_override_values(config));
}

bool apply_nozzle_diameter_override(DynamicPrintConfig &config)
{
    if (!has_nozzle_diameter_override(config))
        return false;
    auto *nozzles = config.option<ConfigOptionFloats>("nozzle_diameter");
    if (nozzles == nullptr || nozzles->values.empty())
        return false;
    const std::vector<double> before = nozzles->values;
    const std::vector<double> after  = nozzle_diameters_with_override(before, nozzle_diameter_override_values(config));
    bool changed = false;
    for (size_t e = 0; e < before.size(); ++e)
        changed |= !same_nozzle_diameter(before[e], after[e]);
    if (!changed)
        return false;
    // The size the process's mm widths were made for (P6) is the preset's. A printer preset that names no size in its
    // printer_variant (a custom printer) gave it by its first extruder's nozzle, which the override is about to change:
    // keep it on the process.
    if (auto *reference = config.option<ConfigOptionFloat>("line_width_nozzle_diameter"); reference != nullptr && !(reference->value > 0.)) {
        const auto *variant = config.option<ConfigOptionString>("printer_variant");
        if (variant == nullptr || printer_variant_sizes(variant->value).empty())
            reference->value = line_width_reference_diameter(config);
    }
    nozzles->values = after;

    // The layer-height limits of an extruder come with its nozzle size: the preset's limits are for the preset's size.
    auto *max_lh = config.option<ConfigOptionFloats>("max_layer_height");
    auto *min_lh = config.option<ConfigOptionFloats>("min_layer_height");
    for (size_t e = 0; e < before.size(); ++e) {
        if (same_nozzle_diameter(before[e], after[e]) || !(before[e] > 0.))
            continue;
        const double ratio = after[e] / before[e];
        if (max_lh != nullptr && max_lh->values.size() == before.size() && max_lh->values[e] > 0.) {
            max_lh->values[e] = std::round(max_lh->values[e] * ratio * 1000.) / 1000.;
            if (min_lh != nullptr && min_lh->values.size() == before.size() && min_lh->values[e] > max_lh->values[e])
                min_lh->values[e] = max_lh->values[e];
        }
    }
    return true;
}

std::vector<double> nozzle_diameter_override_stored(std::vector<double> values, const std::vector<double> &preset)
{
    bool any = false;
    for (size_t e = 0; e < values.size(); ++e) {
        if (!(values[e] > 0.)) {
            values[e] = 0.;
            continue;
        }
        if (e >= preset.size())
            values[e] = 0.;
        else if (!same_nozzle_diameter(values[e], preset[e]))
            any = true;
    }
    if (!any)
        return { 0. };
    if (values.size() > preset.size())
        values.resize(preset.size());
    return values;
}

std::vector<double> set_tool_nozzle_diameter(std::vector<double> override_values, const std::vector<double> &preset, size_t tool, double diameter)
{
    if (tool >= preset.size())
        return nozzle_diameter_override_stored(std::move(override_values), preset);
    override_values.resize(preset.size(), 0.);
    // Keep the other tools' sizes as they are now, whether the preset's or overridden.
    override_values[tool] = diameter > 0. ? diameter : 0.;
    return nozzle_diameter_override_stored(std::move(override_values), preset);
}

std::vector<double> printer_variant_sizes(const std::string &printer_variant)
{
    std::vector<double> out;
    size_t pos = 0;
    while (pos < printer_variant.size()) {
        const size_t start = printer_variant.find_first_of("0123456789.", pos);
        if (start == std::string::npos)
            break;
        size_t end = printer_variant.find_first_not_of("0123456789.", start);
        if (end == std::string::npos)
            end = printer_variant.size();
        try {
            const double d = std::stod(printer_variant.substr(start, end - start));
            if (plausible_size(d))
                out.push_back(d);
        } catch (...) {
        }
        pos = end;
    }
    return out;
}

std::vector<double> nozzle_size_choices(const std::vector<double> &current, const std::vector<std::string> &printer_variants)
{
    std::vector<double> out;
    auto add = [&out](double d) {
        if (plausible_size(d) && std::none_of(out.begin(), out.end(), [d](double s) { return same_nozzle_diameter(s, d); }))
            out.push_back(std::round(d * 1000.) / 1000.);
    };
    for (double d : { 0.2, 0.4, 0.6, 0.8 })
        add(d);
    for (const std::string &variant : printer_variants)
        for (double d : printer_variant_sizes(variant))
            add(d);
    for (double d : current)
        add(d);
    std::sort(out.begin(), out.end());
    return out;
}

std::string serialize_nozzle_diameter_override(const std::vector<double> &values)
{
    if (std::none_of(values.begin(), values.end(), [](double v) { return v > 0.; }))
        return std::string();
    std::string out;
    for (size_t e = 0; e < values.size(); ++e) {
        if (e > 0)
            out += ",";
        out += values[e] > 0. ? format_diameter_to_str(values[e]) : std::string("0");
    }
    return out;
}

std::vector<double> parse_nozzle_diameter_override(const std::string &text)
{
    std::vector<double> out;
    std::stringstream   ss(text);
    std::string         item;
    while (std::getline(ss, item, ',')) {
        const size_t a = item.find_first_not_of(" \t");
        const size_t b = item.find_last_not_of(" \t");
        if (a == std::string::npos)
            return {};
        item = item.substr(a, b - a + 1);
        try {
            size_t      used = 0;
            const double d   = std::stod(item, &used);
            if (used != item.size() || !(d == 0. || plausible_size(d)))
                return {};
            out.push_back(d);
        } catch (...) {
            return {};
        }
    }
    if (std::none_of(out.begin(), out.end(), [](double v) { return v > 0.; }))
        return {};
    return out;
}

std::vector<double> nozzle_diameter_override_from_reported(const std::vector<double> &preset, const std::vector<double> &current_override,
                                                           const std::vector<double> &reported)
{
    std::vector<double> values = nozzle_diameters_with_override(preset, current_override);
    for (size_t e = 0; e < std::min(values.size(), reported.size()); ++e)
        if (plausible_size(reported[e]))
            values[e] = reported[e];
    return nozzle_diameter_override_stored(std::move(values), preset);
}

std::vector<NozzleSizeMismatch> nozzle_size_mismatches(const std::vector<double> &sliced, const std::vector<double> &reported,
                                                       const std::vector<size_t> &used_tools)
{
    std::vector<NozzleSizeMismatch> out;
    auto check = [&](size_t tool) {
        if (tool >= sliced.size() || tool >= reported.size())
            return;
        if (!plausible_size(reported[tool]) || !(sliced[tool] > 0.))
            return;
        if (!same_nozzle_diameter(sliced[tool], reported[tool]))
            out.push_back({ tool, sliced[tool], reported[tool] });
    };
    if (used_tools.empty()) {
        for (size_t t = 0; t < sliced.size(); ++t)
            check(t);
    } else {
        std::vector<size_t> tools = used_tools;
        std::sort(tools.begin(), tools.end());
        tools.erase(std::unique(tools.begin(), tools.end()), tools.end());
        for (size_t t : tools)
            check(t);
    }
    return out;
}

std::string describe_nozzle_size_mismatches(const std::vector<NozzleSizeMismatch> &mismatches)
{
    std::string out;
    for (const NozzleSizeMismatch &m : mismatches) {
        if (!out.empty())
            out += ", ";
        out += "T" + std::to_string(m.tool + 1) + " " + format_nozzle_diameter_word(m.sliced) + " mm (printer: " +
               format_nozzle_diameter_word(m.reported) + " mm)";
    }
    return out;
}

std::vector<double> reported_extruder_nozzle_sizes(const DualNozzleSync::PrinterState &state, size_t extruders)
{
    std::vector<double> out(extruders, 0.);
    for (const DualNozzleSync::PrinterNozzle &nozzle : state.nozzles) {
        if (nozzle.on_rack())
            continue;
        const int logical = state.logical_extruder_of(nozzle);
        if (logical < 0 || size_t(logical) >= extruders)
            continue;
        try {
            const double d = std::stod(nozzle.diameter);
            if (plausible_size(d))
                out[size_t(logical)] = d;
        } catch (...) {
        }
    }
    return out;
}

std::vector<std::vector<double>> reported_available_nozzle_sizes(const DualNozzleSync::PrinterState &state, size_t extruders)
{
    std::vector<std::vector<double>> out(extruders);
    for (const DualNozzleSync::PrinterNozzle &nozzle : state.nozzles) {
        if (nozzle.on_rack() && !nozzle.normal)
            continue;
        const int logical = state.logical_extruder_of(nozzle);
        if (logical < 0 || size_t(logical) >= extruders)
            continue;
        try {
            const double d = std::stod(nozzle.diameter);
            if (plausible_size(d))
                out[size_t(logical)].push_back(d);
        } catch (...) {
        }
    }
    return out;
}

std::vector<NozzleSizeMismatch> rack_nozzle_size_mismatches(const std::vector<double> &sliced, const std::vector<std::vector<double>> &available,
                                                            const std::vector<size_t> &used_tools)
{
    std::vector<NozzleSizeMismatch> out;
    std::vector<size_t>             tools = used_tools;
    if (tools.empty())
        for (size_t t = 0; t < sliced.size(); ++t)
            tools.push_back(t);
    std::sort(tools.begin(), tools.end());
    tools.erase(std::unique(tools.begin(), tools.end()), tools.end());
    for (size_t t : tools) {
        if (t >= sliced.size() || t >= available.size() || available[t].empty() || !(sliced[t] > 0.))
            continue;
        if (std::none_of(available[t].begin(), available[t].end(), [&](double d) { return same_nozzle_diameter(d, sliced[t]); }))
            out.push_back({ t, sliced[t], available[t].front() });
    }
    return out;
}

} // namespace Slic3r
