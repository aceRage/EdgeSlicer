#include "FineDetail.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r {
namespace FineDetail {

Floors floors(FineDetailLevel level, double raw_line_percent, double raw_feature_percent)
{
    switch (level) {
    case FineDetailLevel::Balanced: return { balanced_line_percent, balanced_feature_percent };
    case FineDetailLevel::FineText: return { fine_text_line_percent, fine_text_feature_percent };
    case FineDetailLevel::Custom:   return { std::max(0., raw_line_percent), std::max(0., raw_feature_percent) };
    case FineDetailLevel::Off:
    default:                        return {};
    }
}

Floors floors(const PrintRegionConfig &config)
{
    return floors(config.fine_detail.value, config.thin_feature_line_width.value, config.thin_feature_min_size.value);
}

bool enabled(const PrintRegionConfig &config) { return config.fine_detail.value != FineDetailLevel::Off; }

bool same_walls(const PrintRegionConfig &a, const PrintRegionConfig &b)
{
    if (!enabled(a) && !enabled(b))
        return true;
    const Floors fa = floors(a), fb = floors(b);
    return a.fine_detail.value == b.fine_detail.value && fa.line_percent == fb.line_percent &&
           fa.feature_percent == fb.feature_percent && a.fine_detail_speed.value == b.fine_detail_speed.value;
}

bool classic_thin_walls(const PrintRegionConfig &config) { return config.detect_thin_wall.value || enabled(config); }

double min_spacing_for_layer(double layer_height) { return std::max(0., layer_height) * 0.25 * PI; }

double effective_thin_bead_width(double floor_mm, double min_bead_width_mm, double layer_height)
{
    if (floor_mm <= 0.)
        return min_bead_width_mm;
    return std::min(std::max(floor_mm, min_spacing_for_layer(layer_height)), min_bead_width_mm);
}

double effective_thin_feature_size(double floor_mm, double min_feature_size_mm)
{
    if (floor_mm <= 0.)
        return min_feature_size_mm;
    return std::min(floor_mm, min_feature_size_mm);
}

double line_width(double spacing, double layer_height) { return spacing + std::max(0., layer_height) * (1. - 0.25 * PI); }

Millimetres millimetres(const DynamicPrintConfig &print, double nozzle)
{
    auto percent = [&print](const char *key) {
        if (const auto *opt = print.option<ConfigOptionPercent>(key))
            return opt->value;
        return print_config_def.get(key)->get_default_value<ConfigOptionPercent>()->value;
    };
    Millimetres mm;
    mm.nozzle       = nozzle;
    mm.layer_height = print.has("layer_height") ? print.opt_float("layer_height") : 0.;
    mm.skip_thinner_than         = percent("min_feature_size") * 0.01 * nozzle;
    mm.thinnest_line             = percent("min_bead_width") * 0.01 * nozzle;
    mm.thinnest_line_first_layer = percent("initial_layer_min_bead_width") * 0.01 * nozzle;
    const double odd  = percent("min_odd_wall_line_width") * 0.01 * nozzle;
    const double even = percent("min_even_wall_line_width") * 0.01 * nozzle;
    mm.odd_minimum  = odd > 0. ? odd : mm.thinnest_line;
    mm.even_minimum = even > 0. ? even : mm.thinnest_line;

    FineDetailLevel level = FineDetailLevel::Off;
    if (const auto *opt = print.option<ConfigOptionEnum<FineDetailLevel>>("fine_detail"))
        level = opt->value;
    const Floors f = floors(level, percent("thin_feature_line_width"), percent("thin_feature_min_size"));
    if (level != FineDetailLevel::Off) {
        mm.fine_detail_line = effective_thin_bead_width(f.line_percent * 0.01 * nozzle, mm.thinnest_line, mm.layer_height);
        mm.fine_detail_skip = effective_thin_feature_size(f.feature_percent * 0.01 * nozzle, mm.skip_thinner_than);
    }
    return mm;
}

const std::vector<std::string> &raw_keys()
{
    static const std::vector<std::string> keys{ "thin_feature_line_width", "thin_feature_min_size" };
    return keys;
}

const std::vector<std::string> &all_keys()
{
    static const std::vector<std::string> keys{ "fine_detail", "thin_feature_line_width", "thin_feature_min_size", "fine_detail_speed" };
    return keys;
}

static bool fine_detail_is_named(FineDetailLevel level) { return level == FineDetailLevel::Balanced || level == FineDetailLevel::FineText; }

DynamicPrintConfig changes_for_edit(const DynamicPrintConfig &config, const std::string &changed_key)
{
    DynamicPrintConfig out;
    const auto *level_opt = config.option<ConfigOptionEnum<FineDetailLevel>>("fine_detail");
    if (level_opt == nullptr)
        return out;
    const FineDetailLevel level = level_opt->value;
    if (!fine_detail_is_named(level))
        return out;
    const Floors named = floors(level, 0., 0.);

    if (changed_key == "fine_detail") {
        // A named level was chosen: show its values in the raw keys.
        const auto *line    = config.option<ConfigOptionPercent>("thin_feature_line_width");
        const auto *feature = config.option<ConfigOptionPercent>("thin_feature_min_size");
        if (line == nullptr || line->value != named.line_percent)
            out.set_key_value("thin_feature_line_width", new ConfigOptionPercent(named.line_percent));
        if (feature == nullptr || feature->value != named.feature_percent)
            out.set_key_value("thin_feature_min_size", new ConfigOptionPercent(named.feature_percent));
        return out;
    }

    if (std::find(raw_keys().begin(), raw_keys().end(), changed_key) != raw_keys().end()) {
        // A raw value was edited under a named level: if it no longer is that level, the level is Custom.
        const auto  *opt   = config.option<ConfigOptionPercent>(changed_key);
        const double named_value = changed_key == "thin_feature_line_width" ? named.line_percent : named.feature_percent;
        if (opt != nullptr && opt->value != named_value)
            out.set_key_value("fine_detail", new ConfigOptionEnum<FineDetailLevel>(FineDetailLevel::Custom));
    }
    return out;
}

} // namespace FineDetail
} // namespace Slic3r
