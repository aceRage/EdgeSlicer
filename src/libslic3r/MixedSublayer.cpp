#include "MixedSublayer.hpp"

#include "MixedFilament.hpp"
#include "MixedFilamentConfigRemap.hpp"
#include "PrintConfig.hpp"
#include "Preset.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <numeric>
#include <set>
#include <sstream>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/log/trivial.hpp>

#include "nlohmann/json.hpp"

namespace Slic3r {

static constexpr double k_mixsub_eps = 1e-6;

MixedSublayerMethod mixed_sublayer_effective_method(int row_method, bool enable_mixed_color_sublayer, bool local_z_mode, bool row_is_gradient)
{
    switch (row_method) {
    case int(MixedSublayerMethod::Cadence): return MixedSublayerMethod::Cadence;
    case int(MixedSublayerMethod::Subdivide): return MixedSublayerMethod::Subdivide;
    case int(MixedSublayerMethod::Ratio): return MixedSublayerMethod::Ratio;
    default: break;
    }
    if (local_z_mode)
        return MixedSublayerMethod::Subdivide;
    if (enable_mixed_color_sublayer)
        return MixedSublayerMethod::Ratio;
    return row_is_gradient ? MixedSublayerMethod::Subdivide : MixedSublayerMethod::Cadence;
}

bool mixed_sublayer_legacy_mode(const std::vector<MixedFilament> &rows, bool enable_mixed_color_sublayer)
{
    if (enable_mixed_color_sublayer)
        return false;
    return std::all_of(rows.begin(), rows.end(), [](const MixedFilament &mf) { return mf.sublayer_method == int(MixedSublayerMethod::Auto); });
}

std::vector<MixedRatioComponent> mixed_ratio_components(const MixedFilament &row, size_t num_physical, double gradient_ratio_a)
{
    std::vector<MixedRatioComponent> out;
    const std::vector<unsigned int> ids = MixedFilamentManager::decode_gradient_component_ids(row.gradient_component_ids, num_physical);
    if (ids.size() >= 3) {
        std::vector<double> weights;
        {
            std::string token;
            for (const char c : row.gradient_component_weights + "/") {
                if (c >= '0' && c <= '9') {
                    token.push_back(c);
                } else if (!token.empty()) {
                    weights.push_back(double(std::max(0, std::atoi(token.c_str()))));
                    token.clear();
                }
            }
        }
        if (weights.size() != ids.size() || std::accumulate(weights.begin(), weights.end(), 0.) <= 0.)
            weights.assign(ids.size(), 1.);
        for (size_t i = 0; i < ids.size(); ++i)
            out.push_back({ ids[i], weights[i], 0. });
    } else {
        double share_b = double(std::clamp(row.mix_b_percent, 0, 100)) / 100.;
        if (gradient_ratio_a > 0. && gradient_ratio_a < 1.)
            share_b = 1. - gradient_ratio_a;
        out.push_back({ row.component_a, 1. - share_b, 0. });
        out.push_back({ row.component_b, share_b, 0. });
    }

    // Drop invalid, duplicate and zero-share components, then normalize.
    std::vector<MixedRatioComponent> clean;
    for (const MixedRatioComponent &c : out) {
        if (c.extruder == 0 || (num_physical > 0 && c.extruder > num_physical) || c.ratio <= k_mixsub_eps)
            continue;
        auto it = std::find_if(clean.begin(), clean.end(), [&c](const MixedRatioComponent &o) { return o.extruder == c.extruder; });
        if (it != clean.end())
            it->ratio += c.ratio;
        else
            clean.push_back(c);
    }
    const double sum = std::accumulate(clean.begin(), clean.end(), 0., [](double s, const MixedRatioComponent &c) { return s + c.ratio; });
    if (clean.size() < 2 || sum <= k_mixsub_eps)
        return {};
    for (MixedRatioComponent &c : clean)
        c.ratio /= sum;
    return clean;
}

std::vector<MixedRatioPass> plan_mixed_ratio_layer(const std::vector<MixedRatioComponent> &components,
                                                   double                                   layer_height,
                                                   bool                                     ascending_order,
                                                   MixedRatioRowState                      &state)
{
    std::vector<MixedRatioPass> passes;
    const size_t n = components.size();
    if (n == 0 || layer_height <= k_mixsub_eps)
        return passes;
    if (state.carry.size() != n)
        state.carry.assign(n, 0.);

    std::vector<double> want(n);
    for (size_t i = 0; i < n; ++i)
        want[i] = components[i].ratio * layer_height + state.carry[i];

    // Components that reach their floor. Scaling the eligible wants up to the layer height can only grow them,
    // but carries can make the sum exceed the layer height, so re-check after scaling until it settles.
    std::vector<uint8_t> eligible(n, 0);
    for (size_t i = 0; i < n; ++i)
        eligible[i] = want[i] + k_mixsub_eps >= std::max(components[i].min_height, k_mixsub_eps) ? 1 : 0;

    std::vector<double> height(n, 0.);
    for (size_t guard = 0; guard <= n; ++guard) {
        double sum = 0.;
        for (size_t i = 0; i < n; ++i)
            if (eligible[i])
                sum += want[i];
        if (sum <= k_mixsub_eps)
            break;
        bool changed = false;
        std::fill(height.begin(), height.end(), 0.);
        for (size_t i = 0; i < n; ++i)
            if (eligible[i])
                height[i] = layer_height * want[i] / sum;
        // Drop the smallest pass that a down-scale pushed under its floor, then rescale.
        size_t worst = n;
        for (size_t i = 0; i < n; ++i)
            if (eligible[i] && height[i] + k_mixsub_eps < components[i].min_height &&
                (worst == n || height[i] < height[worst]))
                worst = i;
        if (worst != n) {
            eligible[worst] = 0;
            changed         = true;
        }
        if (!changed)
            break;
    }

    size_t eligible_count = size_t(std::count(eligible.begin(), eligible.end(), uint8_t(1)));
    if (eligible_count == 0) {
        // Revert to cadence: the whole layer goes to the component owed the most.
        size_t best = 0;
        for (size_t i = 1; i < n; ++i)
            if (want[i] > want[best] + k_mixsub_eps)
                best = i;
        std::fill(height.begin(), height.end(), 0.);
        height[best] = layer_height;
    }

    for (size_t i = 0; i < n; ++i)
        state.carry[i] = want[i] - height[i];

    // Order.
    std::vector<size_t> order;
    for (size_t i = 0; i < n; ++i)
        if (height[i] > k_mixsub_eps)
            order.push_back(i);
    std::sort(order.begin(), order.end(), [&components](size_t l, size_t r) { return components[l].extruder < components[r].extruder; });
    if (!ascending_order && state.last_extruder != 0) {
        // Rotate so the component that ended the previous layer starts this one; with it absent, start at the
        // first component above it in cyclic ascending order.
        auto it = std::find_if(order.begin(), order.end(), [&](size_t i) { return components[i].extruder >= state.last_extruder; });
        if (it != order.end())
            std::rotate(order.begin(), it, order.end());
    }

    double z_sum = 0.;
    for (size_t k = 0; k < order.size(); ++k) {
        const size_t i = order[k];
        const double h = (k + 1 == order.size()) ? layer_height - z_sum : height[i];
        passes.push_back({ components[i].extruder, h });
        z_sum += h;
    }
    if (!passes.empty())
        state.last_extruder = passes.back().extruder;
    return passes;
}

MixedRatioBandPlan plan_mixed_ratio_band(const std::vector<MixedRatioComponent> &components,
                                         double                                   band_height,
                                         double                                   swallowed_height,
                                         bool                                     ascending_order,
                                         MixedRatioRowState                      &state)
{
    MixedRatioBandPlan out;
    const MixedRatioRowState saved = state;
    out.passes = plan_mixed_ratio_layer(components, band_height, ascending_order, state);
    if (out.passes.empty())
        return out;
    if (swallowed_height <= k_mixsub_eps || out.passes.front().height + k_mixsub_eps >= swallowed_height) {
        out.split = out.passes.size() > 1;
        return out;
    }
    // Start with the first pass, in the planned (cyclic) order, that reaches above the swallowed layers.
    for (size_t k = 1; k < out.passes.size(); ++k)
        if (out.passes[k].height + k_mixsub_eps >= swallowed_height) {
            std::rotate(out.passes.begin(), out.passes.begin() + std::ptrdiff_t(k), out.passes.end());
            state.last_extruder = out.passes.back().extruder;
            out.split           = true;
            return out;
        }
    // No pass is tall enough: the band prints whole, by the component owed the most.
    state = saved;
    const size_t n = components.size();
    if (state.carry.size() != n)
        state.carry.assign(n, 0.);
    size_t              best = 0;
    std::vector<double> want(n);
    for (size_t i = 0; i < n; ++i) {
        want[i] = components[i].ratio * band_height + state.carry[i];
        if (want[i] > want[best] + k_mixsub_eps)
            best = i;
    }
    for (size_t i = 0; i < n; ++i)
        state.carry[i] = want[i] - (i == best ? band_height : 0.);
    state.last_extruder = components[best].extruder;
    out.passes          = { MixedRatioPass{ components[best].extruder, band_height } };
    out.split           = false;
    return out;
}

MixedDiameterMismatch mixed_ratio_diameter_mismatch(const std::vector<unsigned int> &components, const std::vector<double> &nozzle_by_filament)
{
    MixedDiameterMismatch out;
    auto nozzle = [&nozzle_by_filament](unsigned int id) -> double {
        return (id >= 1 && id <= nozzle_by_filament.size()) ? nozzle_by_filament[id - 1] : 0.;
    };
    for (size_t i = 0; i < components.size(); ++i)
        for (size_t j = i + 1; j < components.size(); ++j) {
            const double da = nozzle(components[i]);
            const double db = nozzle(components[j]);
            if (da > 0. && db > 0. && std::abs(da - db) > 1e-4) {
                out.filament_a = components[i];
                out.filament_b = components[j];
                out.diameter_a = da;
                out.diameter_b = db;
                return out;
            }
        }
    return out;
}

size_t mixed_ratio_toolchanges_per_layer(size_t component_count, bool ascending_order, bool with_wipe_tower)
{
    if (component_count < 2)
        return 0;
    const size_t changes = ascending_order ? component_count : component_count - 1;
    return with_wipe_tower ? 2 * changes : changes;
}

float mixed_ratio_toolchange_purge_volume(bool same_nozzle, float flush_volume, float flush_multiplier, float prime_volume)
{
    if (same_nozzle)
        return std::max(prime_volume, flush_volume * flush_multiplier);
    return prime_volume;
}

// ---- Bambu Studio 3MF import --------------------------------------------------------------------------------

bool BambuMixedSlots::any_mixed() const
{
    return std::any_of(is_mixed.begin(), is_mixed.end(), [](bool b) { return b; });
}

static std::vector<std::string> mixsub_split_trimmed(const std::string &s, char sep)
{
    std::vector<std::string> out;
    std::stringstream        ss(s);
    std::string              tok;
    while (std::getline(ss, tok, sep)) {
        size_t lo = 0, hi = tok.size();
        while (lo < hi && std::isspace(static_cast<unsigned char>(tok[lo])))
            ++lo;
        while (hi > lo && std::isspace(static_cast<unsigned char>(tok[hi - 1])))
            --hi;
        out.push_back(tok.substr(lo, hi - lo));
    }
    return out;
}

static bool mixsub_parse_double(const std::string &s, double &out)
{
    if (s.empty())
        return false;
    try {
        size_t used = 0;
        out = std::stod(s, &used);
        return used == s.size() && std::isfinite(out);
    } catch (...) {
        return false;
    }
}

static bool mixsub_json_bool(const nlohmann::json &v)
{
    if (v.is_boolean())
        return v.get<bool>();
    if (v.is_number())
        return v.get<double>() != 0.;
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        return s == "1" || s == "true" || s == "True";
    }
    return false;
}

bool parse_bambu_mixed_slots_json(const std::string &json_text, BambuMixedSlots &out)
{
    out = BambuMixedSlots();
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json_text);
    } catch (...) {
        return false;
    }
    if (!j.is_object())
        return false;
    auto strings = [&j](const char *key, std::vector<std::string> &dst) {
        auto it = j.find(key);
        if (it == j.end() || !it->is_array())
            return;
        for (const auto &v : *it)
            dst.push_back(v.is_string() ? v.get<std::string>() : v.dump());
    };
    auto bools = [&j](const char *key, std::vector<bool> &dst) {
        auto it = j.find(key);
        if (it == j.end() || !it->is_array())
            return;
        for (const auto &v : *it)
            dst.push_back(mixsub_json_bool(v));
    };
    bools("filament_is_mixed", out.is_mixed);
    strings("filament_mixed_components", out.components);
    strings("filament_mixed_sublayer_ratios", out.ratios);
    bools("filament_mixed_gradient", out.gradient);
    strings("filament_mixed_gradient_range", out.gradient_range);
    strings("filament_mixed_gradient_curve", out.gradient_curve);
    bools("filament_mixed_gradient_per_part", out.gradient_per_part);
    return true;
}

// Per-filament keys of a project config: filament preset options and anything named filament_*. Compatibility
// conditions are per preset, not per slot.
static bool mixsub_is_per_filament_key(const std::string &key)
{
    if (boost::starts_with(key, "compatible_"))
        return false;
    if (boost::starts_with(key, "filament_") || key == "default_filament_colour")
        return true;
    static const std::set<std::string> filament_keys = [] {
        const std::vector<std::string> &opts = Preset::filament_options();
        return std::set<std::string>(opts.begin(), opts.end());
    }();
    return filament_keys.count(key) != 0;
}

// Keep the entries of `opt` whose index is in `keep`, in order.
static void mixsub_keep_vector_entries(ConfigOptionVectorBase &opt, const std::vector<size_t> &keep)
{
    std::unique_ptr<ConfigOption> src(opt.clone());
    for (size_t dst = 0; dst < keep.size(); ++dst)
        opt.set_at(src.get(), dst, keep[dst]);
    opt.resize(keep.size());
}

BambuMixedImportResult convert_bambu_mixed_slots(DynamicPrintConfig &config, const BambuMixedSlots &slots)
{
    BambuMixedImportResult result;
    if (!slots.any_mixed())
        return result;

    const size_t n = slots.is_mixed.size();
    const auto  *colours = config.option<ConfigOptionStrings>("filament_colour");
    if (colours == nullptr || colours->values.size() != n) {
        BOOST_LOG_TRIVIAL(warning) << "Bambu mixed filament import skipped: filament_is_mixed has " << n
                                   << " slots, filament_colour has " << (colours ? colours->values.size() : 0);
        return result;
    }

    std::vector<size_t> physical_slots;
    for (size_t i = 0; i < n; ++i)
        if (!slots.is_mixed[i])
            physical_slots.push_back(i);
    if (physical_slots.size() < 2) {
        BOOST_LOG_TRIVIAL(warning) << "Bambu mixed filament import skipped: fewer than two physical filaments";
        return result;
    }

    result.old_slot_count     = n;
    result.new_physical_count = physical_slots.size();
    result.id_remap.assign(n + 1, 0u);
    for (size_t k = 0; k < physical_slots.size(); ++k)
        result.id_remap[physical_slots[k] + 1] = unsigned(k + 1);
    for (size_t i = 0; i < n; ++i)
        if (slots.is_mixed[i])
            result.removed_slots.push_back(i);

    // 1. The new physical colours, then the mixed manager as a load of this project will rebuild it.
    std::vector<std::string> new_colours;
    for (size_t slot : physical_slots)
        new_colours.push_back(colours->values[slot]);
    MixedFilamentManager mgr;
    mgr.auto_generate(new_colours);
    std::string existing;
    if (const auto *defs = config.option<ConfigOptionString>("mixed_filament_definitions"))
        existing = defs->value;
    if (!existing.empty())
        mgr.load_custom_entries(existing, new_colours);

    // 2. One Edge row per Bambu mixed slot.
    std::vector<std::pair<size_t, size_t>> slot_to_row; // (old 0-based slot, row index)
    for (size_t i = 0; i < n; ++i) {
        if (!slots.is_mixed[i])
            continue;
        const std::string comp_str = i < slots.components.size() ? slots.components[i] : std::string();
        std::vector<unsigned int> comps;
        bool                      bad = false;
        for (const std::string &tok : mixsub_split_trimmed(comp_str, ',')) {
            double v = 0.;
            if (!mixsub_parse_double(tok, v) || v < 1. || v > double(n) || v != std::floor(v)) {
                bad = true;
                break;
            }
            const unsigned int mapped = result.id_remap[size_t(v)];
            if (mapped == 0) { // a component that is itself a mixed slot
                bad = true;
                break;
            }
            if (std::find(comps.begin(), comps.end(), mapped) == comps.end())
                comps.push_back(mapped);
        }
        if (bad || comps.size() < 2) {
            BOOST_LOG_TRIVIAL(warning) << "Bambu mixed filament import: slot " << (i + 1) << " has unusable components \"" << comp_str
                                       << "\", dropped";
            continue;
        }

        std::vector<double> ratios;
        for (const std::string &tok : mixsub_split_trimmed(i < slots.ratios.size() ? slots.ratios[i] : std::string(), ',')) {
            double v = 0.;
            if (!mixsub_parse_double(tok, v) || v < 0.) {
                ratios.clear();
                break;
            }
            ratios.push_back(v);
        }
        if (ratios.size() != comps.size() || std::accumulate(ratios.begin(), ratios.end(), 0.) <= k_mixsub_eps)
            ratios.assign(comps.size(), 1.);
        const double ratio_sum = std::accumulate(ratios.begin(), ratios.end(), 0.);

        const size_t before = mgr.mixed_filaments().size();
        const int    mix_b  = comps.size() == 2 ? int(std::lround(100. * ratios[1] / ratio_sum)) : 50;
        mgr.add_custom_filament(comps[0], comps[1], mix_b, new_colours);
        if (mgr.mixed_filaments().size() == before) {
            BOOST_LOG_TRIVIAL(warning) << "Bambu mixed filament import: no room for slot " << (i + 1) << ", dropped";
            continue;
        }
        MixedFilament &row = mgr.mixed_filaments().back();
        row.component_a     = comps[0];
        row.component_b     = comps[1];
        row.sublayer_method = int(MixedSublayerMethod::Auto);
        if (comps.size() >= 3) {
            row.gradient_component_ids = MixedFilamentManager::encode_gradient_component_ids(comps);
            std::string weights;
            for (size_t k = 0; k < ratios.size(); ++k) {
                if (k)
                    weights += '/';
                weights += std::to_string(int(std::lround(100. * ratios[k] / ratio_sum)));
            }
            row.gradient_component_weights = weights;
        }
        const bool gradient = i < slots.gradient.size() && slots.gradient[i] && comps.size() == 2;
        if (gradient) {
            const std::vector<std::string> range = mixsub_split_trimmed(i < slots.gradient_range.size() ? slots.gradient_range[i] : std::string(), ',');
            double start = MixedFilament::k_default_gradient_dominant, end = MixedFilament::k_default_gradient_minority;
            if (range.size() == 2) {
                double s = 0., e = 0.;
                if (mixsub_parse_double(range[0], s) && mixsub_parse_double(range[1], e)) {
                    start = s;
                    end   = e;
                }
            }
            start = std::clamp(start, 0.01, 0.99);
            end   = std::clamp(end, 0.01, 0.99);
            if (std::abs(start - end) >= double(MixedFilament::k_min_gradient_difference)) {
                row.gradient_enabled = true;
                row.gradient_start   = float(start);
                row.gradient_end     = float(end);
            }
        }
        // D11: the custom curve and the per-part gradient are dropped; the linear range is kept.
        const bool had_curve    = i < slots.gradient_curve.size() && !slots.gradient_curve[i].empty();
        const bool had_per_part = i < slots.gradient_per_part.size() && slots.gradient_per_part[i];
        if (had_curve || had_per_part) {
            std::string notice = "Bambu Studio mixed filament " + std::to_string(i + 1) + ": ";
            if (had_curve)
                notice += "custom gradient curve";
            if (had_curve && had_per_part)
                notice += " and ";
            if (had_per_part)
                notice += "per-part gradient";
            notice += " not supported, imported as a linear gradient over the whole object";
            result.notices.push_back(notice);
        }
        slot_to_row.emplace_back(i, mgr.mixed_filaments().size() - 1);
    }
    mgr.refresh_display_colors(new_colours);

    for (const auto &[slot, row_idx] : slot_to_row)
        result.id_remap[slot + 1] = mgr.filament_id_from_mixed_index(row_idx, new_colours.size());

    // 3. Remove the mixed slots from every per-filament array.
    std::vector<size_t> keep = physical_slots;
    for (const std::string &key : config.keys()) {
        ConfigOption *raw = config.optptr(key);
        auto         *vec = dynamic_cast<ConfigOptionVectorBase *>(raw);
        if (vec == nullptr)
            continue;
        const size_t size = vec->size();
        if (key == "flush_volumes_matrix") {
            // One n x n block per extruder: drop the row and the column of each mixed slot.
            if (size == 0 || size % (n * n) != 0)
                continue;
            std::vector<size_t> keep_matrix;
            for (size_t block = 0; block < size / (n * n); ++block)
                for (size_t r : physical_slots)
                    for (size_t c : physical_slots)
                        keep_matrix.push_back(block * n * n + r * n + c);
            mixsub_keep_vector_entries(*vec, keep_matrix);
        } else if (key == "flush_volumes_vector") {
            if (size != 2 * n)
                continue;
            std::vector<size_t> keep_pairs;
            for (size_t s : physical_slots) {
                keep_pairs.push_back(2 * s);
                keep_pairs.push_back(2 * s + 1);
            }
            mixsub_keep_vector_entries(*vec, keep_pairs);
        } else if (key == "different_settings_to_system" || key == "inherits_group") {
            // [process, filament 1..n, printer]
            if (size != n + 2)
                continue;
            std::vector<size_t> keep_presets { 0 };
            for (size_t s : physical_slots)
                keep_presets.push_back(s + 1);
            keep_presets.push_back(n + 1);
            mixsub_keep_vector_entries(*vec, keep_presets);
        } else if (size == n && mixsub_is_per_filament_key(key)) {
            mixsub_keep_vector_entries(*vec, keep);
        }
    }

    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(mgr.serialize_custom_entries()));
    // Process-level feature filaments (wall_filament, support_filament, ...) follow the same remap.
    size_t total = result.new_physical_count;
    for (unsigned int mapped : result.id_remap)
        total = std::max(total, size_t(mapped));
    remap_dynamic_config_feature_filament_ids(config, result.id_remap, total);
    result.converted = true;
    BOOST_LOG_TRIVIAL(info) << "Bambu mixed filament import: " << n << " slots -> " << result.new_physical_count << " physical + "
                            << slot_to_row.size() << " mixed rows";
    return result;
}

unsigned int bambu_mixed_remap_id(const BambuMixedImportResult &result, int old_id)
{
    if (old_id <= 0 || size_t(old_id) >= result.id_remap.size())
        return 0;
    return result.id_remap[size_t(old_id)];
}

} // namespace Slic3r
