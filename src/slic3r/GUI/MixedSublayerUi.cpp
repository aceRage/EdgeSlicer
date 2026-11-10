#include "MixedSublayerUi.hpp"

#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ToolSlots.hpp"

#include <boost/format.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>

namespace Slic3r { namespace GUI { namespace MixedSublayerUi {

bool row_is_gradient(const MixedFilament &row)
{
    return row.gradient_enabled && row.component_a != row.component_b;
}

bool row_can_sublayer(const MixedFilament &row)
{
    // PrintObjectSlice.cpp local_z_eligible_mixed_row(): cycle patterns and same-layer pointillism keep their own order.
    return row.manual_pattern.empty() && row.distribution_mode != int(MixedFilament::SameLayerPointillisme);
}

MixedSublayerMethod effective_method(const MixedFilament &row, const ProcessDefaults &defaults)
{
    if (!row_can_sublayer(row))
        return MixedSublayerMethod::Cadence;
    const MixedSublayerMethod method = mixed_sublayer_effective_method(row.sublayer_method, defaults.enable_mixed_color_sublayer,
                                                                       defaults.local_z_mode, row_is_gradient(row));
    // The planner prints a Ratio row with fewer than two usable filaments as whole layers.
    if (method == MixedSublayerMethod::Ratio && mixed_ratio_components(row, 0).size() < 2)
        return MixedSublayerMethod::Cadence;
    return method;
}

Badge badge_for_row(const MixedFilament &row, const ProcessDefaults &defaults)
{
    switch (effective_method(row, defaults)) {
    case MixedSublayerMethod::Ratio:     return Badge::Ratio;
    case MixedSublayerMethod::Subdivide: return Badge::Subdivide;
    default: break;
    }
    return row.sublayer_method == int(MixedSublayerMethod::Cadence) ? Badge::Cadence : Badge::Hidden;
}

const char *badge_text(Badge badge)
{
    switch (badge) {
    case Badge::Ratio:     return "Ratio";
    case Badge::Subdivide: return "Subdivide";
    case Badge::Cadence:   return "Layers";
    default: break;
    }
    return "";
}

int method_to_choice(int sublayer_method)
{
    return std::clamp(sublayer_method, int(MixedSublayerMethod::Auto), int(MixedSublayerMethod::Ratio));
}

int choice_to_method(int choice)
{
    return std::clamp(choice, int(MixedSublayerMethod::Auto), int(MixedSublayerMethod::Ratio));
}

CostEstimate estimate_cost(const CostInput &in)
{
    CostEstimate out;
    std::vector<unsigned int> ids;
    for (unsigned int id : in.components)
        if (id >= 1 && (in.num_physical == 0 || id <= in.num_physical) && std::find(ids.begin(), ids.end(), id) == ids.end())
            ids.push_back(id);
    if (ids.size() < 2)
        return out;
    out.valid     = true;
    out.per_layer = mixed_ratio_toolchanges_per_layer(ids.size(), in.bs_order, in.wipe_tower);
    out.changes   = out.per_layer * in.layers;
    if (!in.single_nozzle)
        return out;

    double flush = 0.;
    size_t pairs = 0;
    const size_t n = in.num_physical;
    for (unsigned int a : ids)
        for (unsigned int b : ids) {
            if (a == b)
                continue;
            const size_t at = size_t(a - 1) * n + size_t(b - 1);
            if (at < in.flush_matrix.size()) {
                flush += in.flush_matrix[at];
                ++pairs;
            }
        }
    const double per_change = pairs > 0 ? flush / double(pairs) * in.flush_multiplier : 0.;
    double density = 0.;
    size_t known   = 0;
    for (unsigned int id : ids)
        if (size_t(id - 1) < in.density.size() && in.density[id - 1] > 0.) {
            density += in.density[id - 1];
            ++known;
        }
    density = known > 0 ? density / double(known) : 1.24; // PLA when no preset says
    // Without an object the line is per layer: the flush of one layer.
    const size_t counted = in.layers > 0 ? out.changes : out.per_layer;
    out.flush_mm3 = per_change * double(counted);
    out.flush_g   = out.flush_mm3 * density / 1000.;
    return out;
}

std::string format_grams(double grams)
{
    char buf[64];
    if (grams < 10.)
        std::snprintf(buf, sizeof(buf), "%.1f", std::max(0., grams));
    else
        std::snprintf(buf, sizeof(buf), "%.0f", grams);
    return buf;
}

std::string format_cost_line(const CostEstimate &cost, const CostLineTemplates &templates)
{
    if (!cost.valid)
        return {};
    const bool per_layer  = cost.changes == 0;
    const size_t count    = per_layer ? cost.per_layer : cost.changes;
    const bool with_flush = cost.flush_mm3 > 0.;
    const std::string &shape = per_layer ? (with_flush ? templates.per_layer_with_flush : templates.per_layer_no_flush)
                                         : (with_flush ? templates.total_with_flush : templates.total_no_flush);
    try {
        boost::format f(shape);
        f.exceptions(boost::io::all_error_bits ^ (boost::io::too_many_args_bit | boost::io::too_few_args_bit));
        f % count;
        if (with_flush)
            f % format_grams(cost.flush_g);
        return f.str();
    } catch (const std::exception &) {
        return {};
    }
}

MixedDiameterMismatch broken_ratio_row(const MixedFilament &row, const ProcessDefaults &defaults, size_t num_physical,
                                       const std::vector<double> &nozzle_by_filament)
{
    if (effective_method(row, defaults) != MixedSublayerMethod::Ratio)
        return {};
    std::vector<unsigned int> ids;
    for (const MixedRatioComponent &c : mixed_ratio_components(row, num_physical))
        ids.push_back(c.extruder);
    return mixed_ratio_diameter_mismatch(ids, nozzle_by_filament);
}

static void add_if_mixed(std::vector<unsigned int> &out, const MixedFilamentManager &mgr, size_t num_physical, int id)
{
    if (id > 0 && mgr.is_mixed(unsigned(id), num_physical))
        out.push_back(unsigned(id));
}

static void add_config_filaments(std::vector<unsigned int> &out, const MixedFilamentManager &mgr, size_t num_physical,
                                 const ModelConfig &config, bool with_extruder)
{
    static const char *const keys[] = { "wall_filament", "sparse_infill_filament", "solid_infill_filament" };
    if (with_extruder && config.has("extruder"))
        add_if_mixed(out, mgr, num_physical, config.option("extruder")->getInt());
    for (const char *key : keys)
        if (config.has(key))
            add_if_mixed(out, mgr, num_physical, config.option(key)->getInt());
}

std::vector<unsigned int> mixed_ids_used_by_object(const ModelObject &object, const MixedFilamentManager &mgr, size_t num_physical)
{
    std::vector<unsigned int> ids;
    add_config_filaments(ids, mgr, num_physical, object.config, true);
    for (const ModelVolume *mv : object.volumes) {
        if (mv == nullptr)
            continue;
        for (int id : mv->get_extruders())
            add_if_mixed(ids, mgr, num_physical, id);
        add_config_filaments(ids, mgr, num_physical, mv->config, false);
    }
    for (const auto &range : object.layer_config_ranges)
        add_config_filaments(ids, mgr, num_physical, range.second, true);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

bool object_uses_ratio_rows(const ModelObject &object, const MixedFilamentManager &mgr, size_t num_physical,
                            const ProcessDefaults &defaults)
{
    for (unsigned int id : mixed_ids_used_by_object(object, mgr, num_physical))
        if (const MixedFilament *mf = mgr.mixed_filament_from_id(id, num_physical);
            mf != nullptr && effective_method(*mf, defaults) == MixedSublayerMethod::Ratio)
            return true;
    return false;
}

bool any_ratio_row(const MixedFilamentManager &mgr, const ProcessDefaults &defaults)
{
    for (const MixedFilament &mf : mgr.mixed_filaments())
        if (!mf.deleted && mf.enabled && effective_method(mf, defaults) == MixedSublayerMethod::Ratio)
            return true;
    return false;
}

std::vector<LegendRow> legend_rows(const MixedFilamentManager &mgr, const std::vector<unsigned int> &used_ids, size_t num_physical,
                                   const ProcessDefaults &defaults)
{
    std::vector<LegendRow> rows;
    std::vector<unsigned int> ids = used_ids;
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    for (unsigned int id : ids) {
        const MixedFilament *mf = mgr.mixed_filament_from_id(id, num_physical);
        if (mf == nullptr)
            continue;
        LegendRow row;
        row.virtual_id = id;
        row.method     = effective_method(*mf, defaults);
        const std::vector<MixedRatioComponent> comps = mixed_ratio_components(*mf, num_physical);
        // Largest remainder, so the shown percents always add up to 100.
        int sum = 0;
        std::vector<std::pair<size_t, double>> remainders;
        for (size_t i = 0; i < comps.size(); ++i) {
            const double exact = comps[i].ratio * 100.;
            const int    whole = int(std::floor(exact + 1e-9));
            row.components.push_back({ comps[i].extruder, whole });
            remainders.emplace_back(i, exact - double(whole));
            sum += whole;
        }
        std::stable_sort(remainders.begin(), remainders.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
        for (size_t k = 0; sum < 100 && !remainders.empty(); ++k, ++sum)
            row.components[remainders[k % remainders.size()].first].percent += 1;
        rows.push_back(std::move(row));
    }
    return rows;
}

size_t count_mixed_definition_rows(const std::string &definitions)
{
    size_t count = 0;
    size_t start = 0;
    while (start <= definitions.size()) {
        size_t end = definitions.find(';', start);
        if (end == std::string::npos)
            end = definitions.size();
        const std::string row = definitions.substr(start, end - start);
        if (row.find_first_not_of(" \t\r\n") != std::string::npos)
            ++count;
        start = end + 1;
    }
    return count;
}

size_t estimate_layers(double height, double layer_height)
{
    if (!(height > 0.) || !(layer_height > 0.))
        return 0;
    return size_t(std::ceil(height / layer_height - 1e-9));
}

static bool bool_of(const DynamicPrintConfig &config, const char *key, bool fallback)
{
    if (const auto *opt = config.option<ConfigOptionBool>(key))
        return opt->value;
    if (const auto *opt = config.option<ConfigOptionInt>(key))
        return opt->value != 0;
    return fallback;
}

ProcessDefaults process_defaults(const PresetBundle &bundle)
{
    const DynamicPrintConfig &print = bundle.prints.get_edited_preset().config;
    ProcessDefaults out;
    out.enable_mixed_color_sublayer = bool_of(print, "enable_mixed_color_sublayer", false);
    out.local_z_mode = bool_of(bundle.project_config, "dithering_local_z_mode", bool_of(print, "dithering_local_z_mode", false));
    return out;
}

std::vector<double> nozzle_by_filament(const PresetBundle &bundle, size_t num_physical)
{
    DynamicPrintConfig config = bundle.printers.get_edited_preset().config;
    config.apply(bundle.project_config, true);
    std::vector<double> out(num_physical, 0.);
    for (size_t i = 0; i < num_physical; ++i)
        out[i] = nozzle_diameter_for_filament(static_cast<const ConfigBase &>(config), unsigned(i + 1));
    return out;
}

CostInput cost_input(const PresetBundle &bundle, const MixedFilament &row, size_t num_physical, size_t layers)
{
    const DynamicPrintConfig &printer = bundle.printers.get_edited_preset().config;
    const DynamicPrintConfig &print   = bundle.prints.get_edited_preset().config;
    CostInput in;
    for (const MixedRatioComponent &c : mixed_ratio_components(row, num_physical))
        in.components.push_back(c.extruder);
    in.bs_order     = row.sublayer_bs_order;
    in.num_physical = num_physical;
    in.layers       = layers;
    // Print::has_wipe_tower(): the tower is on, more than one filament, no spiral vase.
    in.wipe_tower = bool_of(print, "enable_prime_tower", false) && num_physical > 1 && !bool_of(print, "spiral_mode", false);
    const auto *nozzles = printer.option<ConfigOptionFloats>("nozzle_diameter");
    in.single_nozzle    = (nozzles != nullptr && nozzles->values.size() == 1) || bool_of(printer, "single_extruder_multi_material", false);
    if (const auto *matrix = bundle.project_config.option<ConfigOptionFloats>("flush_volumes_matrix"))
        in.flush_matrix = matrix->values;
    if (const auto *mult = bundle.project_config.option<ConfigOptionFloat>("flush_multiplier"))
        in.flush_multiplier = mult->value;
    in.density.assign(num_physical, 0.);
    for (size_t i = 0; i < num_physical && i < bundle.filament_presets.size(); ++i)
        if (const Preset *preset = bundle.filaments.find_preset(bundle.filament_presets[i]))
            if (const auto *density = preset->config.option<ConfigOptionFloats>("filament_density"); density != nullptr && !density->values.empty())
                in.density[i] = density->values.front();
    return in;
}

}}} // namespace Slic3r::GUI::MixedSublayerUi
