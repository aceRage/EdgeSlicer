#include "HeightPatternInput.hpp"

#include "libslic3r.h"
#include "Model.hpp"
#include "PrintConfig.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace Slic3r {

namespace {

constexpr double TOL = 1e-4;

std::string fmt(double v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

// Mirror of the ownership test in HeightColourPattern.cpp: the range carries exactly the two keys
// the pattern wrote, with the values it wrote.
bool config_matches(const ModelConfig &cfg, const HeightPatternRange &r)
{
    const DynamicPrintConfig &c = cfg.get();
    if (c.keys().size() != 2 || !c.has("layer_height") || !c.has("extruder"))
        return false;
    return c.opt_int("extruder") == r.extruder && std::abs(c.opt_float("layer_height") - r.layer_height) < TOL;
}

} // namespace

std::string validate_height_pattern_input(const HeightPatternInput &in, int max_filament)
{
    if (in.sequence.empty())
        return "Pick at least one filament for the sequence.";
    for (int f : in.sequence) {
        if (f < 1)
            return "Filament numbers start at 1.";
        if (max_filament > 0 && f > max_filament)
            return "Filament " + std::to_string(f) + " is not in the project (" + std::to_string(max_filament) + " filaments).";
    }
    if (!(in.band > 0.))
        return "The band height must be above zero.";
    if (in.gap < 0.)
        return "The filament gap cannot be negative.";
    if (in.seq_gap < 0.)
        return "The sequence gap cannot be negative.";
    if (in.start < 0.)
        return "The start cannot be negative.";
    if (!in.to_top && in.repeat < 1)
        return "The repeat count must be at least 1.";
    if (in.gap_mode == int(HeightPatternGapMode::Fixed)) {
        if (in.gap_filament < 1)
            return "Pick the filament the gaps use.";
        if (max_filament > 0 && in.gap_filament > max_filament)
            return "Filament " + std::to_string(in.gap_filament) + " is not in the project (" + std::to_string(max_filament) + " filaments).";
    }
    if (in.seq_gap > 0. && in.seq_gap_mode == int(HeightPatternGapMode::Fixed)) {
        if (in.seq_gap_filament < 1)
            return "Pick the filament the sequence gap uses.";
        if (max_filament > 0 && in.seq_gap_filament > max_filament)
            return "Filament " + std::to_string(in.seq_gap_filament) + " is not in the project (" + std::to_string(max_filament) + " filaments).";
    }
    return {};
}

HeightColourPattern height_pattern_from_input(const HeightPatternInput &in, double layer_height,
                                              const std::vector<double> &layer_tops)
{
    HeightColourPattern p;
    if (in.unit == HeightPatternUnit::Layers) {
        const long long skipped = std::max<long long>(0, std::llround(in.start));
        if (skipped == 0)
            p.start_z = 0.;
        else if (layer_tops.empty())
            p.start_z = double(skipped) * layer_height;
        else
            p.start_z = layer_tops[std::min<size_t>(size_t(skipped), layer_tops.size()) - 1];
        p.band_h = in.band * layer_height;
        p.gap_h  = in.gap * layer_height;
        p.seq_gap_h = in.seq_gap * layer_height;
    } else {
        p.start_z = in.start;
        p.band_h  = in.band;
        p.gap_h   = in.gap;
        p.seq_gap_h = in.seq_gap;
    }
    p.repeat         = in.to_top ? 0 : in.repeat;
    p.sequence       = in.sequence;
    p.gap_mode       = in.gap_mode;
    p.gap_filament   = in.gap_filament;
    p.seq_gap_mode   = in.seq_gap_mode;
    p.seq_gap_filament = in.seq_gap_filament;
    p.snap_to_layers = in.snap_to_layers;
    return p;
}

HeightPatternInput height_pattern_input_from_pattern(const HeightColourPattern &p)
{
    HeightPatternInput in;
    in.unit           = HeightPatternUnit::Mm;
    in.start          = p.start_z;
    in.band           = p.band_h;
    in.gap            = p.gap_h;
    in.to_top         = p.repeat == 0;
    in.repeat         = p.repeat == 0 ? 3 : p.repeat;
    in.sequence       = p.sequence;
    in.gap_mode       = p.gap_mode;
    in.gap_filament   = p.gap_filament;
    in.seq_gap        = p.seq_gap_h;
    in.seq_gap_mode   = p.seq_gap_mode;
    in.seq_gap_filament = p.seq_gap_filament;
    in.snap_to_layers = p.snap_to_layers;
    return in;
}

std::vector<std::pair<double, double>> find_height_pattern_clashes(const ModelObject                   &object,
                                                                   const std::vector<HeightPatternRange> &ranges,
                                                                   int                                    own_pattern_id)
{
    std::vector<std::pair<double, double>> out;
    const HeightColourPattern *own = own_pattern_id > 0 ? find_height_colour_pattern(object, own_pattern_id) : nullptr;
    for (const auto &kv : object.layer_config_ranges) {
        bool owned = false;
        if (own != nullptr)
            for (const HeightPatternRange &g : own->generated)
                if (std::abs(g.lo - kv.first.first) < TOL && std::abs(g.hi - kv.first.second) < TOL) {
                    owned = true;
                    break;
                }
        if (owned)
            continue;
        for (const HeightPatternRange &r : ranges)
            if (kv.first.first < r.hi - TOL && kv.first.second > r.lo + TOL) {
                out.emplace_back(kv.first.first, kv.first.second);
                break;
            }
    }
    return out;
}

bool height_pattern_owns_range(const ModelObject &object, const t_layer_height_range &range)
{
    if (object.height_colour_patterns.empty())
        return false;
    auto it = object.layer_config_ranges.find(range);
    if (it == object.layer_config_ranges.end())
        return false;
    for (const HeightColourPattern &p : object.height_colour_patterns)
        for (const HeightPatternRange &g : p.generated)
            if (std::abs(g.lo - range.first) < TOL && std::abs(g.hi - range.second) < TOL)
                return config_matches(it->second, g);
    return false;
}

int height_pattern_band_count(const HeightColourPattern &p)
{
    int n = 0;
    for (const HeightPatternRange &r : p.generated)
        if (!r.gap)
            ++n;
    return n;
}

std::vector<HeightPatternRange> height_pattern_band_ranges(const HeightPatternExpansion &e)
{
    std::vector<HeightPatternRange> bands;
    for (const HeightPatternRange &r : e.ranges)
        if (!r.gap)
            bands.push_back(r);
    return bands;
}

std::string height_pattern_short_summary(const HeightPatternExpansion &e)
{
    if (!e.ok())
        return {};
    if (e.bands == 0)
        return "No band fits inside the object.";
    std::string s = std::to_string(e.bands) + (e.bands == 1 ? " band" : " bands");
    if (e.repeats > 0 && e.sequence_length > 0)
        s += " (" + std::to_string(e.repeats) + (e.repeats == 1 ? " repeat" : " repeats") + " of " + std::to_string(e.sequence_length) +
             (e.sequence_length == 1 ? " filament)" : " filaments)");
    if (e.cut_off)
        s += ", last repeat cut off at the object top after filament " + std::to_string(e.last_filament) + " (of " +
             std::to_string(e.planned_bands) + " planned bands)";
    else if (e.clipped_at_top)
        s += ", last band cut short by the top of the object";
    if (e.dropped > 0)
        s += ", " + std::to_string(e.dropped) + (e.dropped == 1 ? " band" : " bands") + " too thin for the layer grid dropped";
    return s;
}

std::string height_pattern_summary(const HeightPatternExpansion &e, double object_top)
{
    if (!e.ok())
        return {};
    if (e.bands == 0)
        return "No band fits inside the object.";
    // Bands only: with gap ranges mixed in, every other range is a gap.
    std::vector<const HeightPatternRange *> bands;
    for (const HeightPatternRange &r : e.ranges)
        if (!r.gap)
            bands.push_back(&r);

    std::string s = std::to_string(e.bands) + (e.bands == 1 ? " band: " : " bands: ");
    const size_t shown = std::min<size_t>(bands.size(), 6);
    for (size_t i = 0; i < shown; ++i) {
        if (i > 0)
            s += ", ";
        s += fmt(bands[i]->lo) + "-" + fmt(bands[i]->hi);
    }
    if (bands.size() > shown)
        s += ", ...";
    s += " mm";
    if (!bands.empty()) {
        s += " (last band ends at " + fmt(bands.back()->hi);
        if (object_top > 0.)
            s += " of " + fmt(object_top);
        s += " mm)";
    }
    if (e.cut_off)
        s += ". The last repeat is cut off at the object top after filament " + std::to_string(e.last_filament) + ": " +
             std::to_string(e.bands) + " of " + std::to_string(e.planned_bands) + " planned bands fit.";
    else if (e.clipped_at_top)
        s += ". The last band is cut short by the top of the object.";
    if (e.dropped > 0)
        s += ". " + std::to_string(e.dropped) + (e.dropped == 1 ? " band" : " bands") + " too thin for the layer grid " + (e.dropped == 1 ? "was" : "were") + " dropped.";
    return s;
}

std::string height_pattern_clash_text(const std::vector<std::pair<double, double>> &clashes)
{
    if (clashes.empty())
        return {};
    std::string s = "Overlaps " + std::to_string(clashes.size()) + (clashes.size() == 1 ? " existing height range: " : " existing height ranges: ");
    const size_t shown = std::min<size_t>(clashes.size(), 4);
    for (size_t i = 0; i < shown; ++i) {
        if (i > 0)
            s += ", ";
        s += fmt(clashes[i].first) + "-" + fmt(clashes[i].second);
    }
    if (clashes.size() > shown)
        s += ", ...";
    return s + " mm";
}

std::string height_pattern_row_label(const HeightColourPattern &p)
{
    const std::set<int> distinct(p.sequence.begin(), p.sequence.end());
    const int           bands = height_pattern_band_count(p);
    return "Color Pattern (" + std::to_string(distinct.size()) + (distinct.size() == 1 ? " filament x " : " filaments x ") +
           std::to_string(bands) + (bands == 1 ? " band)" : " bands)");
}

HeightPatternPreview preview_height_pattern(const ModelObject          *object,
                                            const HeightPatternInput   &in,
                                            int                         own_pattern_id,
                                            const std::vector<double>  &layer_tops,
                                            double                      layer_height,
                                            double                      object_height,
                                            int                         max_filament)
{
    HeightPatternPreview out;
    out.error = validate_height_pattern_input(in, max_filament);
    if (!out.ok())
        return out;
    const HeightColourPattern p = height_pattern_from_input(in, layer_height, layer_tops);
    out.expansion = expand_height_colour_pattern(p, layer_tops, layer_height, object_height, max_filament);
    if (!out.expansion.ok()) {
        out.error = out.expansion.error;
        return out;
    }
    if (out.expansion.ranges.empty()) {
        out.error = "No band fits inside the object.";
        return out;
    }
    const double top = layer_tops.empty() ? object_height : layer_tops.back();
    out.summary      = height_pattern_summary(out.expansion, top);
    if (object != nullptr)
        out.clashes = find_height_pattern_clashes(*object, out.expansion.ranges, own_pattern_id);
    return out;
}

HeightPatternHealth height_pattern_health(const ModelObject &object, const HeightColourPattern &p,
                                          int filament_count, double layer_height)
{
    HeightPatternHealth h;
    h.modified = height_colour_pattern_state(object, p).modified;
    std::set<int> missing;
    for (const HeightPatternRange &g : p.generated) {
        if (filament_count > 0 && g.extruder > filament_count)
            missing.insert(g.extruder);
        if (layer_height > 0. && std::abs(g.layer_height - layer_height) > TOL)
            h.layer_height_stale = true;
    }
    h.missing_filaments.assign(missing.begin(), missing.end());
    return h;
}

// ---- filament changes the pattern adds -----------------------------------------------------------------

HeightPatternChangeEstimate estimate_height_pattern_changes(const std::vector<HeightPatternRange> &ranges,
                                                            double                                  object_top,
                                                            const HeightPatternChangeInput         &in)
{
    HeightPatternChangeEstimate out;
    out.purge_known = true;
    if (ranges.empty())
        return out;

    auto change = [&](int from, int to) {
        if (from == to)
            return;
        ++out.changes;
        if (in.filament_count > 0 && from >= 1 && to >= 1 && from <= in.filament_count && to <= in.filament_count &&
            in.flush_matrix.size() >= size_t(in.filament_count) * size_t(in.filament_count)) {
            const double mm3 = double(in.flush_matrix[size_t(from - 1) * size_t(in.filament_count) + size_t(to - 1)]) * in.flush_multiplier;
            out.purge_mm3 += mm3;
            if (size_t(to) <= in.density.size() && in.density[size_t(to) - 1] > 0.)
                out.purge_g += mm3 / 1000. * in.density[size_t(to) - 1];
        } else
            out.purge_known = false;
    };

    constexpr double eps = 1e-4;
    int    cur = (ranges.front().lo < eps) ? ranges.front().extruder : in.base_filament;   // filament at Z 0
    double z   = 0.;
    for (const HeightPatternRange &r : ranges) {
        if (r.lo - z > eps) {           // a stretch with no range: the object's own filament
            change(cur, in.base_filament);
            cur = in.base_filament;
        }
        change(cur, r.extruder);
        cur = r.extruder;
        z   = r.hi;
    }
    if (object_top - z > eps)           // above the last band
        change(cur, in.base_filament);
    return out;
}

// ---- bands the user edited by hand -----------------------------------------------------------------------

namespace {

bool hpi_is_band_index(const HeightColourPattern &p, size_t i)
{
    return i < p.generated.size() && !p.generated[i].gap;
}

int hpi_band_number(const HeightColourPattern &p, size_t i)
{
    if (!hpi_is_band_index(p, i))
        return 0;
    int k = 0;
    for (size_t j = 0; j <= i; ++j)
        if (!p.generated[j].gap)
            ++k;
    return k;
}

const ModelConfig *hpi_find(const ModelObject &o, double lo, double hi)
{
    for (const auto &kv : o.layer_config_ranges)
        if (std::abs(kv.first.first - lo) < TOL && std::abs(kv.first.second - hi) < TOL)
            return &kv.second;
    return nullptr;
}

} // namespace

std::vector<HeightPatternBandDiff> height_pattern_modified_ranges(const ModelObject &object, const HeightColourPattern &p)
{
    std::vector<HeightPatternBandDiff> out;
    for (size_t i = 0; i < p.generated.size(); ++i) {
        const HeightPatternRange &g = p.generated[i];
        const ModelConfig *cfg = hpi_find(object, g.lo, g.hi);
        HeightPatternBandDiff d;
        d.band_number       = hpi_band_number(p, i);
        d.lo                = g.lo;
        d.hi                = g.hi;
        d.expected_extruder = g.extruder;
        if (cfg == nullptr) {
            d.status = HeightPatternBandStatus::Removed;
        } else if (config_matches(*cfg, g)) {
            continue;
        } else {
            d.actual_extruder = cfg->get().has("extruder") ? cfg->get().opt_int("extruder") : 0;
            d.status = d.actual_extruder != g.extruder ? HeightPatternBandStatus::FilamentChanged :
                                                         HeightPatternBandStatus::LayerHeightChanged;
        }
        out.push_back(d);
    }
    return out;
}

int reset_height_pattern_modified_ranges(ModelObject &object, int pattern_id)
{
    const HeightColourPattern *p = find_height_colour_pattern(object, pattern_id);
    if (p == nullptr)
        return -1;

    // The ranges that are still exactly what the pattern wrote stay as they are.
    std::vector<const HeightPatternRange *> untouched, to_restore;
    for (const HeightPatternRange &g : p->generated) {
        const ModelConfig *cfg = hpi_find(object, g.lo, g.hi);
        (cfg != nullptr && config_matches(*cfg, g) ? untouched : to_restore).push_back(&g);
    }

    int restored = 0;
    for (const HeightPatternRange *g : to_restore) {
        bool blocked = false;   // overlaps an untouched band: cannot happen for a pattern's own output, but never overwrite one
        for (const HeightPatternRange *u : untouched)
            if (u->lo < g->hi - TOL && u->hi > g->lo + TOL)
                blocked = true;
        if (blocked)
            continue;
        for (auto it = object.layer_config_ranges.begin(); it != object.layer_config_ranges.end();) {
            if (it->first.first < g->hi - TOL && it->first.second > g->lo + TOL)
                it = object.layer_config_ranges.erase(it);
            else
                ++it;
        }
        DynamicPrintConfig cfg;
        cfg.set_key_value("layer_height", new ConfigOptionFloat(g->layer_height));
        cfg.set_key_value("extruder", new ConfigOptionInt(g->extruder));
        object.layer_config_ranges[{ g->lo, g->hi }].assign_config(cfg);
        ++restored;
    }
    return restored;
}

} // namespace Slic3r
