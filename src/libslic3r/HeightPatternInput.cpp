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
        return "The gap cannot be negative.";
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
    } else {
        p.start_z = in.start;
        p.band_h  = in.band;
        p.gap_h   = in.gap;
    }
    p.repeat         = in.to_top ? 0 : in.repeat;
    p.sequence       = in.sequence;
    p.gap_mode       = in.gap_mode;
    p.gap_filament   = in.gap_filament;
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
    const int n = int(p.generated.size());
    // Gaps are only written between two bands and only when they have a filament of their own.
    if (p.gap_mode == int(HeightPatternGapMode::ObjectFilament) || n < 2)
        return n;
    return (n + 1) / 2;
}

std::string height_pattern_summary(const HeightPatternExpansion &e, double object_top)
{
    if (!e.ok())
        return {};
    if (e.bands == 0)
        return "No band fits inside the object.";
    // Bands only: with gap ranges mixed in, every other range is a gap.
    std::vector<const HeightPatternRange *> bands;
    const bool has_gap_ranges = int(e.ranges.size()) > e.bands;
    for (size_t i = 0; i < e.ranges.size(); i += has_gap_ranges ? 2 : 1)
        bands.push_back(&e.ranges[i]);

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
    if (e.clipped_at_top)
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
    return "Height colour pattern (" + std::to_string(distinct.size()) + (distinct.size() == 1 ? " filament x " : " filaments x ") +
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

} // namespace Slic3r
