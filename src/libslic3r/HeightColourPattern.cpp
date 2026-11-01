#include "HeightColourPattern.hpp"

#include "libslic3r.h"
#include "Model.hpp"
#include "PrintConfig.hpp"

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

namespace Slic3r {

namespace {

// Z values are rounded to a micron so that "the end of one band" and "the start of the
// next" are the same double however they were computed (range keys must not drift).
double round_z(double z) { return std::round(z * 1e6) / 1e6; }

constexpr double Z_EPS = 1e-4;

// The layer boundary nearest to z; a tie goes up. Boundaries are 0 and every layer top.
double snap_to_grid(double z, const std::vector<double> &tops)
{
    if (tops.empty())
        return z;
    if (z <= 0.)
        return 0.;
    auto it = std::lower_bound(tops.begin(), tops.end(), z - 1e-9);
    const double above = it == tops.end() ? tops.back() : *it;
    const double below = it == tops.begin() ? 0. : *(it - 1);
    return (above - z) <= (z - below) + 1e-9 ? above : below;
}

} // namespace

int height_pattern_planned_bands(int repeat, int sequence_length, HeightPatternGapMode gap_mode)
{
    if (repeat <= 0 || sequence_length <= 0)
        return 0;
    (void)gap_mode; // gaps never use up a sequence entry: every repetition is `sequence_length` bands
    return int(std::min<long long>((long long)repeat * sequence_length, 1000000));
}

int height_pattern_repeat_from_legacy_bands(int bands, int sequence_length, HeightPatternGapMode gap_mode)
{
    if (bands <= 0)
        return 0;
    if (sequence_length <= 0)
        return 1;
    (void)gap_mode;
    return int(std::max<long long>(1, ((long long)bands + sequence_length - 1) / sequence_length));
}

std::vector<double> height_pattern_layer_tops(const std::vector<double> &object_layers)
{
    std::vector<double> tops;
    tops.reserve(object_layers.size() / 2);
    for (size_t i = 1; i < object_layers.size(); i += 2)
        tops.push_back(object_layers[i]);
    return tops;
}

HeightPatternExpansion expand_height_colour_pattern(const HeightColourPattern &p,
                                                    const std::vector<double> &layer_tops,
                                                    double                     layer_height,
                                                    double                     object_height,
                                                    int                        max_filament)
{
    HeightPatternExpansion out;
    if (p.sequence.empty()) {
        out.error = "The filament sequence is empty.";
        return out;
    }
    for (int f : p.sequence) {
        if (f < 1) {
            out.error = "Filament ids start at 1.";
            return out;
        }
        if (max_filament > 0 && f > max_filament) {
            out.error = "Filament " + std::to_string(f) + " is not in the project (" + std::to_string(max_filament) + " filaments).";
            return out;
        }
    }
    if (max_filament > 0 && p.gap_mode == int(HeightPatternGapMode::Fixed) && p.gap_filament > max_filament) {
        out.error = "Filament " + std::to_string(p.gap_filament) + " is not in the project (" + std::to_string(max_filament) + " filaments).";
        return out;
    }
    if (!(p.band_h > 0.)) {
        out.error = "The band height must be above zero.";
        return out;
    }
    if (p.gap_h < 0. || p.seq_gap_h < 0. || p.start_z < 0.) {
        out.error = "Start and gap cannot be negative.";
        return out;
    }
    if (p.repeat < 0) {
        out.error = "The repeat count cannot be negative.";
        return out;
    }
    const auto gap_mode = HeightPatternGapMode(p.gap_mode);
    if (gap_mode == HeightPatternGapMode::Fixed && p.gap_filament < 1) {
        out.error = "The gap filament must be set.";
        return out;
    }
    if (p.seq_gap_mode < 0 || p.seq_gap_mode > 2) {
        out.error = "Unknown sequence gap mode.";
        return out;
    }
    if (p.seq_gap_h > 0. && p.seq_gap_mode == int(HeightPatternGapMode::Fixed) && p.seq_gap_filament < 1) {
        out.error = "The sequence gap filament must be set.";
        return out;
    }
    if (max_filament > 0 && p.seq_gap_h > 0. && p.seq_gap_mode == int(HeightPatternGapMode::Fixed) && p.seq_gap_filament > max_filament) {
        out.error = "Filament " + std::to_string(p.seq_gap_filament) + " is not in the project (" + std::to_string(max_filament) + " filaments).";
        return out;
    }
    if (!(layer_height > 0.)) {
        out.error = "The layer height must be above zero.";
        return out;
    }

    const double top = layer_tops.empty() ? object_height : layer_tops.back();
    if (!(top > 0.)) {
        out.error = "The object has no height.";
        return out;
    }
    const std::vector<double> &grid = p.snap_to_layers ? layer_tops : std::vector<double>();
    auto snap = [&](double z) { return round_z(snap_to_grid(z, grid)); };

    const size_t n = p.sequence.size();
    const auto seq_mode = HeightPatternGapMode(p.seq_gap_mode);
    // Source index (the i of the i-th band attempted) of every emitted band, to place the gaps.
    std::vector<int> source;

    // Repeat counts whole sequences: every repetition uses every filament, in order.
    const int total_bands = p.repeat == 0 ? 0 : height_pattern_planned_bands(p.repeat, int(n), gap_mode);
    out.planned_bands   = total_bands;
    out.repeats         = p.repeat;
    out.sequence_length = int(n);
    bool stopped_early = false;

    // The gap after band i is the Sequence Gap when band i ends a pass over the sequence, else the
    // Filament Gap. Bands run at start_z + i * band_h + the gaps before them.
    auto gap_after = [&](int i) { return (size_t(i) + 1) % n == 0 ? p.seq_gap_h : p.gap_h; };
    double z = p.start_z;
    for (int i = 0; p.repeat == 0 || i < total_bands; ++i) {
        const double raw_lo = round_z(z);
        z = raw_lo + p.band_h + gap_after(i);
        if (raw_lo >= top - Z_EPS) {
            stopped_early = true;
            break;
        }
        const double raw_hi = round_z(raw_lo + p.band_h);
        const double lo = snap(raw_lo);
        const double hi = std::min(snap(raw_hi), round_z(top));
        const bool clipped = raw_hi > top + Z_EPS;
        if (hi - lo < Z_EPS) {
            // Snapping (or the object top) squashed the band to nothing.
            ++out.dropped;
            continue;
        }
        HeightPatternRange band;
        band.lo = lo;
        band.hi = hi;
        // Bands always run the whole sequence in order; gaps never take a sequence entry.
        band.extruder = p.sequence[size_t(i) % n];
        band.layer_height = layer_height;
        out.ranges.push_back(band);
        source.push_back(i);
        ++out.bands;
        out.clipped_at_top = clipped;
        out.last_filament = band.extruder;
        if (clipped) {
            stopped_early = i + 1 < total_bands;
            break;
        }
    }
    if (p.repeat > 0 && out.bands > 0) {
        out.cut_off = stopped_early && out.bands < total_bands;
        out.last_sequence_index = int(size_t(source.back()) % n) + 1;
    }

    // Gaps between consecutive emitted bands. The gap before the first band and after the
    // last one is never written: the pattern starts and ends with a band.
    const bool any_gap_mode = gap_mode != HeightPatternGapMode::ObjectFilament || seq_mode != HeightPatternGapMode::ObjectFilament;
    if (any_gap_mode && out.bands > 1) {
        // Cycle cursors, one per gap type. The Filament Gap cursor starts one entry before F1 (the last
        // entry) so that, with bands and gaps alternating, a gap is not simply the band below it.
        size_t cursor_fil = n - 1, cursor_seq = 0;
        auto cycle = [&](size_t &cursor, int below, int above) {
            // First entry from the cursor that differs from both neighbouring bands.
            for (size_t k = 0; k < n; ++k) {
                const size_t idx = (cursor + k) % n;
                const int f = p.sequence[idx];
                if (f != below && f != above) {
                    cursor = idx + 1;
                    return f;
                }
            }
            // Every entry equals a neighbour (1-2 filaments): least bad is an entry that differs from the
            // band below, so the gap at least starts with a change; it then merges with the band above.
            for (size_t k = 0; k < n; ++k) {
                const size_t idx = (cursor + k) % n;
                if (p.sequence[idx] != below) {
                    cursor = idx + 1;
                    return p.sequence[idx];
                }
            }
            const int f = p.sequence[cursor % n];
            ++cursor;
            return f;
        };
        std::vector<HeightPatternRange> merged;
        merged.reserve(out.ranges.size() * 2);
        for (size_t k = 0; k < out.ranges.size(); ++k) {
            if (k > 0) {
                const HeightPatternRange &prev = out.ranges[k - 1];
                const HeightPatternRange &cur  = out.ranges[k];
                const bool seq_boundary = (size_t(source[k - 1]) + 1) % n == 0;
                const HeightPatternGapMode m = seq_boundary ? seq_mode : gap_mode;
                if (m != HeightPatternGapMode::ObjectFilament && cur.lo - prev.hi > Z_EPS) {
                    HeightPatternRange gap;
                    gap.lo           = prev.hi;
                    gap.hi           = cur.lo;
                    gap.layer_height = layer_height;
                    gap.gap          = true;
                    gap.extruder     = m == HeightPatternGapMode::Fixed ?
                                           (seq_boundary ? p.seq_gap_filament : p.gap_filament) :
                                           cycle(seq_boundary ? cursor_seq : cursor_fil, prev.extruder, cur.extruder);
                    merged.push_back(gap);
                }
            }
            merged.push_back(out.ranges[k]);
        }
        out.ranges = std::move(merged);
    }
    return out;
}

// ---- applying to an object -------------------------------------------------

namespace {

constexpr double KEY_TOL = 1e-4;

t_layer_config_ranges::iterator find_range(t_layer_config_ranges &ranges, double lo, double hi)
{
    for (auto it = ranges.begin(); it != ranges.end(); ++it)
        if (std::abs(it->first.first - lo) < KEY_TOL && std::abs(it->first.second - hi) < KEY_TOL)
            return it;
    return ranges.end();
}

t_layer_config_ranges::const_iterator find_range(const t_layer_config_ranges &ranges, double lo, double hi)
{
    for (auto it = ranges.begin(); it != ranges.end(); ++it)
        if (std::abs(it->first.first - lo) < KEY_TOL && std::abs(it->first.second - hi) < KEY_TOL)
            return it;
    return ranges.end();
}

// Is the stored range exactly {layer_height, extruder} with the values the pattern wrote?
bool range_matches(const ModelConfig &cfg, const HeightPatternRange &r)
{
    const DynamicPrintConfig &c = cfg.get();
    if (c.keys().size() != 2 || !c.has("layer_height") || !c.has("extruder"))
        return false;
    return c.opt_int("extruder") == r.extruder && std::abs(c.opt_float("layer_height") - r.layer_height) < KEY_TOL;
}

} // namespace

const HeightColourPattern *find_height_colour_pattern(const ModelObject &object, int pattern_id)
{
    for (const HeightColourPattern &p : object.height_colour_patterns)
        if (p.id == pattern_id)
            return &p;
    return nullptr;
}

HeightPatternApplyResult apply_height_colour_pattern(ModelObject               &object,
                                                     HeightColourPattern        pattern,
                                                     const std::vector<double> &layer_tops,
                                                     double                     layer_height,
                                                     bool                       replace_overlaps,
                                                     double                     object_height,
                                                     int                        max_filament)
{
    HeightPatternApplyResult result;
    result.expansion = expand_height_colour_pattern(pattern, layer_tops, layer_height, object_height, max_filament);
    if (!result.expansion.ok()) {
        result.error = result.expansion.error;
        return result;
    }
    if (result.expansion.ranges.empty()) {
        result.error = "No band fits inside the object.";
        return result;
    }

    // The pattern being regenerated owns ranges that are about to go; they are not clashes.
    const HeightColourPattern *previous = pattern.id > 0 ? find_height_colour_pattern(object, pattern.id) : nullptr;
    auto is_owned_by_previous = [&](const t_layer_height_range &key) {
        if (previous == nullptr)
            return false;
        for (const HeightPatternRange &g : previous->generated)
            if (std::abs(g.lo - key.first) < KEY_TOL && std::abs(g.hi - key.second) < KEY_TOL)
                return true;
        return false;
    };

    std::vector<t_layer_config_ranges::iterator> clashing;
    for (auto it = object.layer_config_ranges.begin(); it != object.layer_config_ranges.end(); ++it) {
        if (is_owned_by_previous(it->first))
            continue;
        for (const HeightPatternRange &r : result.expansion.ranges)
            if (it->first.first < r.hi - Z_EPS && it->first.second > r.lo + Z_EPS) {
                clashing.push_back(it);
                result.clashes.emplace_back(it->first.first, it->first.second);
                break;
            }
    }
    if (!clashing.empty() && !replace_overlaps) {
        result.error = "The pattern overlaps existing height ranges.";
        return result;
    }

    // Nothing can fail from here on.
    if (previous != nullptr)
        for (const HeightPatternRange &g : previous->generated)
            if (auto it = find_range(object.layer_config_ranges, g.lo, g.hi); it != object.layer_config_ranges.end())
                object.layer_config_ranges.erase(it);
    for (const auto &c : result.clashes)
        if (auto it = find_range(object.layer_config_ranges, c.first, c.second); it != object.layer_config_ranges.end())
            object.layer_config_ranges.erase(it);

    for (const HeightPatternRange &r : result.expansion.ranges) {
        DynamicPrintConfig cfg;
        // layer_height is mandatory: Slicing.cpp dereferences it for every range.
        cfg.set_key_value("layer_height", new ConfigOptionFloat(r.layer_height));
        cfg.set_key_value("extruder", new ConfigOptionInt(r.extruder));
        object.layer_config_ranges[{ r.lo, r.hi }].assign_config(cfg);
    }

    pattern.generated = result.expansion.ranges;
    if (previous != nullptr) {
        for (HeightColourPattern &p : object.height_colour_patterns)
            if (p.id == pattern.id)
                p = std::move(pattern), result.pattern_id = p.id;
    } else {
        int next = 1;
        for (const HeightColourPattern &p : object.height_colour_patterns)
            next = std::max(next, p.id + 1);
        pattern.id        = next;
        result.pattern_id = next;
        object.height_colour_patterns.push_back(std::move(pattern));
    }
    result.ok = true;
    return result;
}

std::vector<int> height_colour_pattern_missing_filaments(const ModelObject &object, int filament_count)
{
    std::vector<int> out;
    for (const HeightColourPattern &p : object.height_colour_patterns)
        for (const HeightPatternRange &g : p.generated)
            if (g.extruder > filament_count)
                out.push_back(g.extruder);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

bool height_colour_pattern_layer_height_mismatch(const ModelObject &object, double layer_height)
{
    for (const HeightColourPattern &p : object.height_colour_patterns)
        for (const HeightPatternRange &g : p.generated)
            if (std::abs(g.layer_height - layer_height) > 1e-4)
                return true;
    return false;
}

bool remove_height_colour_pattern(ModelObject &object, int pattern_id)
{
    auto it = std::find_if(object.height_colour_patterns.begin(), object.height_colour_patterns.end(),
                           [&](const HeightColourPattern &p) { return p.id == pattern_id; });
    if (it == object.height_colour_patterns.end())
        return false;
    for (const HeightPatternRange &g : it->generated)
        if (auto r = find_range(object.layer_config_ranges, g.lo, g.hi); r != object.layer_config_ranges.end())
            object.layer_config_ranges.erase(r);
    object.height_colour_patterns.erase(it);
    return true;
}

bool detach_height_colour_pattern(ModelObject &object, int pattern_id)
{
    auto it = std::find_if(object.height_colour_patterns.begin(), object.height_colour_patterns.end(),
                           [&](const HeightColourPattern &p) { return p.id == pattern_id; });
    if (it == object.height_colour_patterns.end())
        return false;
    object.height_colour_patterns.erase(it);
    return true;
}

HeightPatternState height_colour_pattern_state(const ModelObject &object, const HeightColourPattern &pattern)
{
    HeightPatternState state;
    for (const HeightPatternRange &g : pattern.generated) {
        auto it = find_range(object.layer_config_ranges, g.lo, g.hi);
        if (it == object.layer_config_ranges.end() || !range_matches(it->second, g)) {
            state.modified = true;
            state.changed.emplace_back(g.lo, g.hi);
        }
    }
    return state;
}

// ---- sidecar ---------------------------------------------------------------

std::string height_patterns_to_json(const std::vector<const std::vector<HeightColourPattern> *> &objects)
{
    using nlohmann::json;
    json root;
    root["version"] = 1;
    json objs = json::array();
    for (size_t i = 0; i < objects.size(); ++i) {
        if (objects[i] == nullptr || objects[i]->empty())
            continue;
        json pats = json::array();
        for (const HeightColourPattern &p : *objects[i]) {
            json gen = json::array();
            for (const HeightPatternRange &g : p.generated) {
                json jr = { { "lo", g.lo }, { "hi", g.hi }, { "extruder", g.extruder }, { "layer_height", g.layer_height } };
                if (g.gap)
                    jr["gap"] = true;
                gen.push_back(std::move(jr));
            }
            json pat = { { "id", p.id }, { "start_z", p.start_z }, { "band_h", p.band_h }, { "gap_h", p.gap_h },
                             { "repeat", p.repeat }, { "sequence", p.sequence }, { "gap_mode", p.gap_mode },
                             { "gap_filament", p.gap_filament }, { "snap_to_layers", p.snap_to_layers }, { "generated", gen } };
            // Two gap types. Absent in older records: those get the Sequence Gap of the old single gap.
            pat["seq_gap_h"]        = p.seq_gap_h;
            pat["seq_gap_mode"]     = p.seq_gap_mode;
            pat["seq_gap_filament"] = p.seq_gap_filament;
            if (p.repeat_is_sequences)
                pat["repeat_seq"] = true;   // absent = a record from before: repeat counted bands
            if (p.axis != 0)
                pat["axis"] = p.axis;   // only written when set: Phase 1 files stay byte-identical
            pats.push_back(std::move(pat));
        }
        objs.push_back({ { "object", int(i) + 1 }, { "patterns", pats } });
    }
    root["objects"] = objs;
    return root.dump(1);
}

std::vector<std::pair<int, std::vector<HeightColourPattern>>> height_patterns_from_json(const std::string &text)
{
    using nlohmann::json;
    std::vector<std::pair<int, std::vector<HeightColourPattern>>> out;
    json root = json::parse(text, nullptr, false);
    if (!root.is_object() || root.value("version", 0) != 1 || !root.contains("objects") || !root["objects"].is_array())
        return out;
    try {
        for (const json &o : root["objects"]) {
            const int idx = o.value("object", 0);
            if (idx < 1 || !o.contains("patterns") || !o["patterns"].is_array())
                continue;
            std::vector<HeightColourPattern> pats;
            for (const json &j : o["patterns"]) {
                HeightColourPattern p;
                p.id             = j.value("id", 0);
                p.start_z        = j.value("start_z", 0.);
                p.band_h         = j.value("band_h", 0.);
                p.gap_h          = j.value("gap_h", 0.);
                p.repeat         = j.value("repeat", 0);
                p.gap_mode       = j.value("gap_mode", 0);
                p.gap_filament   = j.value("gap_filament", 0);
                p.snap_to_layers = j.value("snap_to_layers", true);
                p.axis           = j.value("axis", 0);
                p.repeat_is_sequences = true;
                if (j.contains("sequence") && j["sequence"].is_array())
                    for (const json &f : j["sequence"])
                        p.sequence.push_back(f.get<int>());
                if (j.contains("generated") && j["generated"].is_array())
                    for (const json &g : j["generated"]) {
                        HeightPatternRange r;
                        r.lo           = g.at("lo").get<double>();
                        r.hi           = g.at("hi").get<double>();
                        r.extruder     = g.at("extruder").get<int>();
                        r.layer_height = g.at("layer_height").get<double>();
                        r.gap          = g.value("gap", false);
                        p.generated.push_back(r);
                    }
                // Two gap types. A record without them had one gap between every pair of bands: the Filament
                // Gap keeps it and the Sequence Gap repeats it, which regenerates the same spacing. Its stored
                // gap ranges (every other range, when it wrote any) get the gap flag.
                if (j.contains("seq_gap_h")) {
                    p.seq_gap_h        = j.value("seq_gap_h", 0.);
                    p.seq_gap_mode     = j.value("seq_gap_mode", 0);
                    p.seq_gap_filament = j.value("seq_gap_filament", 0);
                } else {
                    p.seq_gap_h        = p.gap_h;
                    p.seq_gap_mode     = p.gap_mode;
                    p.seq_gap_filament = p.gap_filament;
                    if (p.gap_mode != int(HeightPatternGapMode::ObjectFilament) && p.generated.size() > 1)
                        for (size_t gi = 1; gi < p.generated.size(); gi += 2)
                            p.generated[gi].gap = true;
                }
                if (p.seq_gap_h < 0. || p.seq_gap_mode < 0 || p.seq_gap_mode > 2)
                    continue;
                // Old records counted single bands in `repeat`. The generated ranges are stored and stay
                // as they were; only the stored count is converted to whole sequences so that editing the
                // pattern shows a sensible number (it covers at least the bands the record had).
                if (!j.value("repeat_seq", false) && p.repeat > 0 && !p.sequence.empty())
                    p.repeat = height_pattern_repeat_from_legacy_bands(p.repeat, int(p.sequence.size()), HeightPatternGapMode(std::clamp(p.gap_mode, 0, 2)));
                // A record that cannot describe a pattern is dropped, not half-applied.
                if (p.id < 1 || p.sequence.empty() || p.generated.empty() || p.gap_mode < 0 || p.gap_mode > 2 || p.axis != 0)
                    continue;
                bool unique = true;
                for (const HeightColourPattern &q : pats)
                    unique = unique && q.id != p.id;
                if (unique)
                    pats.push_back(std::move(p));
            }
            if (!pats.empty())
                out.emplace_back(idx, std::move(pats));
        }
    } catch (const std::exception &) {
        return {};
    }
    return out;
}

} // namespace Slic3r
