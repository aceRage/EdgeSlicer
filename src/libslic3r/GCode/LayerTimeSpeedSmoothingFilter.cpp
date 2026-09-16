#include "LayerTimeSpeedSmoothingFilter.hpp"

#include "../Circle.hpp"
#include "../ExtrusionEntity.hpp"
#include "../GCodeReader.hpp"
#include "../LayerTimeSpeedSmoothing.hpp"
#include "../libslic3r.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include <boost/algorithm/string/predicate.hpp>

namespace Slic3r {

namespace {

constexpr double LTSS_FACTOR_EPS = 1e-4;

struct ParsedLine
{
    std::string   raw;
    bool          is_motion     = false;
    bool          has_f         = false;
    bool          extruding     = false;
    bool          wipe          = false;
    float         length        = 0.f;
    float         feedrate_mm_min = 0.f;
    float         mm3_per_mm    = 0.f;
    unsigned      extruder      = 0;
    ExtrusionRole role          = erNone;
    float         time          = 0.f;
};

static std::string trim_copy(std::string s)
{
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
        ++i;
    return s.substr(i);
}

static ExtrusionRole role_from_raw(const std::string &raw)
{
    auto parse_after = [&](const char *tag) -> ExtrusionRole {
        const size_t pos = raw.find(tag);
        if (pos == std::string::npos)
            return erNone;
        const size_t comment = raw.find(';');
        if (comment != std::string::npos && pos < comment)
            return erNone;
        return ExtrusionEntity::string_to_role(trim_copy(raw.substr(pos + std::strlen(tag))));
    };
    ExtrusionRole role = parse_after("FEATURE: ");
    if (role != erNone)
        return role;
    return parse_after("TYPE:");
}

static bool raw_has_tag(const std::string &raw, const char *tag)
{
    const size_t pos = raw.find(tag);
    if (pos == std::string::npos)
        return false;
    const size_t comment = raw.find(';');
    return comment != std::string::npos && pos >= comment;
}

static float g4_seconds(const std::string &raw)
{
    const size_t comment = raw.find(';');
    const size_t end     = comment == std::string::npos ? raw.size() : comment;
    auto         value_at = [&](char key) -> float {
        for (size_t i = 0; i + 1 < end; ++i) {
            if (raw[i] != key)
                continue;
            if (i > 0 && raw[i - 1] != ' ' && raw[i - 1] != '\t' && raw[i - 1] != '4')
                continue;
            return float(std::atof(raw.c_str() + i + 1));
        }
        return -1.f;
    };
    const float s = value_at('S');
    if (s >= 0.f)
        return s;
    const float p = value_at('P');
    return p >= 0.f ? p * 0.001f : 0.f;
}

static bool parse_toolchange(const std::string &raw, unsigned &extruder)
{
    const char *c = raw.c_str();
    while (*c == ' ' || *c == '\t')
        ++c;
    if (*c == 'T' && c[1] >= '0' && c[1] <= '9') {
        extruder = unsigned(std::atoi(c + 1));
        return true;
    }
    if (boost::starts_with(c, "M135") || boost::starts_with(c, "M108")) {
        const char *t = std::strchr(c, 'T');
        if (t && t[1] >= '0' && t[1] <= '9') {
            extruder = unsigned(std::atoi(t + 1));
            return true;
        }
    }
    return false;
}

static std::string rewrite_f(const std::string &raw, int f_mm_min)
{
    const size_t comment    = raw.find(';');
    const size_t search_end = comment == std::string::npos ? raw.size() : comment;
    size_t       fpos       = std::string::npos;
    for (size_t i = 0; i + 1 < search_end; ++i) {
        if (raw[i] == 'F' && (i == 0 || raw[i - 1] == ' ' || raw[i - 1] == '\t')) {
            fpos = i;
            break;
        }
    }
    const std::string fword = std::to_string(f_mm_min);
    if (fpos != std::string::npos) {
        size_t vend = fpos + 1;
        while (vend < search_end && raw[vend] != ' ' && raw[vend] != '\t')
            ++vend;
        return raw.substr(0, fpos + 1) + fword + raw.substr(vend);
    }
    const std::string insert = " F" + fword;
    if (comment == std::string::npos)
        return raw + insert;
    std::string out = raw.substr(0, comment);
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t'))
        out.pop_back();
    out += insert;
    if (comment < raw.size() && (out.empty() || out.back() != ' '))
        out += ' ';
    out += raw.substr(comment);
    return out;
}

// Modes A/B never speed these up (concept Must-PASS #9): overhang wall, bridge, internal
// bridge, ironing, top solid, and support (including interface and transition). Mode C is
// unchanged. Every role is named here rather than borrowed from is_bridge()/is_top_surface():
// those helpers answer other questions (flow, fan, seam placement) and this fork edits them
// (erOverSupportPerimeter is deliberately outside is_bridge()), so the speed-up guarantee
// must not move when they do.
static bool is_support_role(ExtrusionRole role)
{
    return role == erSupportMaterial || role == erSupportMaterialInterface || role == erSupportTransition;
}

static bool is_speedup_protected(ExtrusionRole role)
{
    return role == erOverhangPerimeter
        || role == erBridgeInfill
        || role == erInternalBridgeInfill
        || role == erIroning
        || role == erTopSolidInfill
        || is_support_role(role);
}

// The speed floor CoolingBuffer itself observes when it stretches a layer: slow_down_min_speed
// of the line's filament, in mm/min. 0 = no floor.
static float min_print_feedrate_mm_min(const ParsedLine &line, const PrintConfig &config)
{
    const double min_mm_s = config.slow_down_min_speed.get_at(line.extruder);
    return min_mm_s > 0.0 ? float(min_mm_s * 60.0) : 0.f;
}

// A line CoolingBuffer (or the profile) already put at or below slow_down_min_speed is left
// alone in every mode: not sped up (it is there for a reason) and not slowed further.
static bool already_at_min_print_speed(const ParsedLine &line, const PrintConfig &config)
{
    const double min_mm_s = config.slow_down_min_speed.get_at(line.extruder);
    if (min_mm_s <= 0.0)
        return false;
    return (line.feedrate_mm_min / 60.f) <= float(min_mm_s) + 1e-3f;
}

static bool line_eligible(LayerTimeSpeedSmoothMode mode, LayerTimeSlowdownScope scope, const ParsedLine &line, const PrintConfig &config)
{
    if (!line.is_motion || !line.extruding || line.wipe)
        return false;
    if (already_at_min_print_speed(line, config))
        return false;
    if (is_layer_time_speed_up(mode)) {
        if (is_speedup_protected(line.role))
            return false;
        if (mode == ltssmSpeedUpExcludeOuter && is_external_perimeter(line.role))
            return false;
        return true;
    }
    if (is_layer_time_slowdown(mode)) {
        if (scope == ltssExcludeOuterWalls && is_external_perimeter(line.role))
            return false;
        return true;
    }
    return false;
}

// F (mm/min) of an eligible line after the layer factor, clamped the same way wherever the
// line is retimed (apply_factor_to_lines and time_after_factor must agree):
//  - the filament's max volumetric speed caps it (F_new = min(F_old * f, 60 * max_vol / mm3_per_mm));
//  - a slowdown (factor < 1) is floored at slow_down_min_speed. CoolingBuffer never writes a
//    speed below that floor, and neither does this stage.
static float scaled_feedrate(const ParsedLine &line, double factor, const PrintConfig &config)
{
    float f = float(line.feedrate_mm_min * factor);
    const double max_vol = config.filament_max_volumetric_speed.get_at(line.extruder);
    if (max_vol > 0.0 && line.mm3_per_mm > 0.f) {
        const float cap = float(60.0 * max_vol / line.mm3_per_mm);
        if (cap > 0.f)
            f = std::min(f, cap);
    }
    if (factor < 1.0) {
        const float floor_f = min_print_feedrate_mm_min(line, config);
        if (floor_f > 0.f)
            f = std::max(f, floor_f);
    }
    return std::max(f, 1.f);
}

struct ParseState
{
    GCodeReader   reader;
    unsigned      extruder = 0;
    ExtrusionRole role     = erNone;
    bool          wipe     = false;
    bool          relative_e = false;
};

static std::vector<ParsedLine> parse_layer_lines(ParseState &state, const std::string &gcode, const PrintConfig &config)
{
    std::vector<ParsedLine> lines;
    if (gcode.empty())
        return lines;

    const float filament_area0 = float((M_PI / 4.0) * std::pow(config.filament_diameter.get_at(0), 2));

    state.reader.parse_buffer(gcode, [&](GCodeReader &reader, const GCodeReader::GCodeLine &gline) {
        ParsedLine line;
        line.raw     = gline.raw();
        line.role    = state.role;
        line.wipe    = state.wipe;
        line.extruder = state.extruder;
        line.has_f   = gline.has_f();

        const ExtrusionRole tagged = role_from_raw(line.raw);
        if (tagged != erNone) {
            state.role = tagged;
            line.role  = tagged;
        }
        if (raw_has_tag(line.raw, "WIPE_START"))
            state.wipe = true;
        if (raw_has_tag(line.raw, "WIPE_END"))
            state.wipe = false;
        line.wipe = state.wipe;

        unsigned tool = state.extruder;
        if (parse_toolchange(line.raw, tool)) {
            state.extruder = tool;
            line.extruder  = tool;
        }

        if (gline.cmd_is("G4")) {
            line.time = g4_seconds(line.raw);
            lines.emplace_back(std::move(line));
            return;
        }

        const bool motion = gline.cmd_is("G0") || gline.cmd_is("G1") || gline.cmd_is("G2") || gline.cmd_is("G3");
        if (!motion) {
            lines.emplace_back(std::move(line));
            return;
        }

        line.is_motion = true;
        float dx = gline.dist_X(reader);
        float dy = gline.dist_Y(reader);
        float dz = gline.dist_Z(reader);
        if (gline.cmd_is("G2") || gline.cmd_is("G3")) {
            Vec3f start(reader.x(), reader.y(), 0.f);
            Vec3f end(gline.new_X(reader), gline.new_Y(reader), 0.f);
            Vec3f center(reader.x() + (gline.has_i() ? gline.i() : 0.f), reader.y() + (gline.has_j() ? gline.j() : 0.f), 0.f);
            const float dxy = ArcSegment::calc_arc_length(start, end, center, gline.cmd_is("G3"));
            line.length     = std::sqrt(dxy * dxy + dz * dz);
        } else {
            line.length = std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        float e_mm = 0.f;
        if (gline.has_e())
            e_mm = state.relative_e ? gline.e() : gline.dist_E(reader);
        line.extruding = e_mm > 0.f;

        line.feedrate_mm_min = gline.has_f() ? gline.f() : reader.f();
        if (line.feedrate_mm_min <= 0.f)
            line.feedrate_mm_min = float(config.travel_speed.value) * 60.f;
        const float feedrate_mm_s = line.feedrate_mm_min / 60.f;
        if (line.length > 0.f && feedrate_mm_s > 0.f)
            line.time = line.length / feedrate_mm_s;
        else if (std::abs(e_mm) > 0.f && feedrate_mm_s > 0.f)
            line.time = std::abs(e_mm) / feedrate_mm_s;

        if (line.extruding && line.length > 0.f) {
            const float area = float((M_PI / 4.0) * std::pow(config.filament_diameter.get_at(line.extruder), 2));
            line.mm3_per_mm  = (e_mm * (area > 0.f ? area : filament_area0)) / line.length;
        }

        lines.emplace_back(std::move(line));
    });
    return lines;
}

static double cooling_floor_s(const PrintConfig &config)
{
    double floor_s = 0.0;
    const size_t n = std::max(config.slow_down_layer_time.size(), config.slow_down_for_layer_cooling.size());
    for (size_t i = 0; i < n; ++i) {
        if (config.slow_down_for_layer_cooling.get_at(i))
            floor_s = std::max(floor_s, config.slow_down_layer_time.get_at(i));
    }
    return floor_s;
}

// Layer-level guard against fighting CoolingBuffer, in both directions.
//  - Speed-up: the layer's output time never drops below slow_down_layer_time (floor_s). t_raw
//    is the post-CoolingBuffer time, so a layer CoolingBuffer stretched to the floor stays there.
//  - Slowdown: a layer CoolingBuffer already stretched (cooling_slowed_down) keeps factor 1.
//    The solver was handed the same layer as frozen, so normally this changes nothing; it is
//    the guarantee that no other source of a factor slows such a layer a second time.
// The per-line floor at slow_down_min_speed lives in scaled_feedrate().
static double apply_cooling_floor(double factor, double t_raw, double floor_s, bool speed_up, bool cooling_slowed_down)
{
    if (!speed_up)
        return cooling_slowed_down ? 1.0 : factor;
    if (factor <= 1.0 + LTSS_FACTOR_EPS || t_raw <= 0.0 || floor_s <= 0.0)
        return factor;
    const double t_out = t_raw / factor;
    if (t_out + LTSS_FACTOR_EPS >= floor_s)
        return factor;
    if (t_raw <= floor_s)
        return 1.0;
    return t_raw / floor_s;
}

static std::string apply_factor_to_lines(const std::vector<ParsedLine> &lines,
                                         double                         factor,
                                         LayerTimeSpeedSmoothMode       mode,
                                         LayerTimeSlowdownScope         scope,
                                         const PrintConfig             &config,
                                         float                         &emitted_f)
{
    std::string out;
    out.reserve(lines.empty() ? 0 : lines.size() * 32);
    const bool rewrite = std::abs(factor - 1.0) > LTSS_FACTOR_EPS;

    for (const ParsedLine &line : lines) {
        std::string raw = line.raw;
        if (rewrite && line.is_motion) {
            const float original_f = line.feedrate_mm_min > 0.f ? line.feedrate_mm_min : emitted_f;
            const bool  eligible   = line_eligible(mode, scope, line, config);
            const float new_f      = eligible ? scaled_feedrate(line, factor, config) : original_f;
            const int new_i    = std::max(1, int(std::lround(new_f)));
            const int emit_i   = std::max(1, int(std::lround(emitted_f)));
            const int orig_i   = std::max(1, int(std::lround(original_f)));
            if (eligible && new_i != orig_i) {
                raw       = rewrite_f(raw, new_i);
                emitted_f = float(new_i);
            } else if (eligible && new_i != emit_i) {
                raw       = rewrite_f(raw, new_i);
                emitted_f = float(new_i);
            } else if (!eligible && orig_i != emit_i) {
                // Restore the unscaled modal F so a following ineligible move does not inherit it.
                raw       = rewrite_f(raw, orig_i);
                emitted_f = float(orig_i);
            } else if (line.has_f)
                emitted_f = original_f;
        } else if (line.has_f && line.feedrate_mm_min > 0.f)
            emitted_f = line.feedrate_mm_min;

        out += raw;
        if (raw.empty() || raw.back() != '\n')
            out += '\n';
    }
    return out;
}

static double time_after_factor(const std::vector<ParsedLine> &lines,
                                double                         factor,
                                LayerTimeSpeedSmoothMode       mode,
                                LayerTimeSlowdownScope         scope,
                                const PrintConfig             &config)
{
    if (std::abs(factor - 1.0) <= LTSS_FACTOR_EPS) {
        double t = 0.0;
        for (const ParsedLine &line : lines)
            t += line.time;
        return t;
    }
    double t = 0.0;
    for (const ParsedLine &line : lines) {
        if (!line.is_motion || line.length <= 0.f || line.feedrate_mm_min <= 0.f) {
            t += line.time;
            continue;
        }
        const float f = line_eligible(mode, scope, line, config) ? scaled_feedrate(line, factor, config) : line.feedrate_mm_min;
        t += line.length / (f / 60.f);
    }
    return t;
}

} // namespace

LayerTimeSpeedSmoothingFilter::LayerTimeSpeedSmoothingFilter(const PrintConfig &config)
    : m_config(config)
    , m_mode(config.layer_time_speed_smoothing.value)
    , m_slowdown_scope(config.layer_time_speed_slowdown_scope.value)
    , m_spiral_mode(config.spiral_mode.value)
    , m_relative_e(config.use_relative_e_distances.value)
    , m_slow_down_layers(config.slow_down_layers.value)
{}

void LayerTimeSpeedSmoothingFilter::reset() { m_layers.clear(); }

const char *LayerTimeSpeedSmoothingFilter::mode_key(LayerTimeSpeedSmoothMode mode)
{
    switch (mode) {
    case ltssmOff:                 return "off";
    case ltssmSpeedUpExcludeOuter: return "speed_up_exclude_outer";
    case ltssmSpeedUpAll:          return "speed_up_all";
    case ltssmSlowDown:            return "slow_down";
    }
    return "off";
}

std::string LayerTimeSpeedSmoothingFilter::format_comment(LayerTimeSpeedSmoothMode mode, double factor, double t_raw, double t_out)
{
    char buf[192];
    std::snprintf(buf, sizeof(buf), "; LAYER_TIME_SPEED_SMOOTH mode=%s factor=%.3f t_raw=%.2f t_out=%.2f\n", mode_key(mode), factor, t_raw,
                  t_out);
    return std::string(buf);
}

std::string LayerTimeSpeedSmoothingFilter::process_layer(std::string &&gcode)
{
    return process_layer(std::move(gcode), 0, true);
}

std::string LayerTimeSpeedSmoothingFilter::process_layer(std::string &&gcode, size_t layer_id, bool last_layer, bool cooling_slowed_down)
{
    if (m_mode == ltssmOff || m_spiral_mode)
        return std::move(gcode);

    if (!gcode.empty())
        m_layers.push_back(BufferedLayer{std::move(gcode), layer_id, cooling_slowed_down});

    if (!last_layer)
        return {};

    return flush();
}

std::string LayerTimeSpeedSmoothingFilter::flush()
{
    if (m_layers.empty())
        return {};

    ParseState state;
    state.reader.f()     = float(m_config.travel_speed.value) * 60.f;
    state.relative_e     = m_relative_e;

    struct LayerParse
    {
        std::vector<ParsedLine> lines;
        double                  time = 0.0;
        size_t                  layer_id = 0;
        bool                    cooling_slowed_down = false;
    };
    std::vector<LayerParse> parsed;
    parsed.reserve(m_layers.size());

    for (const BufferedLayer &layer : m_layers) {
        LayerParse rec;
        rec.layer_id            = layer.layer_id;
        rec.cooling_slowed_down = layer.cooling_slowed_down;
        rec.lines               = parse_layer_lines(state, layer.gcode, m_config);
        for (const ParsedLine &line : rec.lines)
            rec.time += line.time;
        parsed.emplace_back(std::move(rec));
    }

    std::vector<double> times;
    std::vector<bool>   frozen;
    times.reserve(parsed.size());
    frozen.reserve(parsed.size());
    for (const LayerParse &rec : parsed) {
        times.push_back(rec.time);
        frozen.push_back(rec.cooling_slowed_down);
    }

    const size_t skip_until = std::max<size_t>(1, size_t(std::max(0, m_slow_down_layers)));
    size_t       first_layer = parsed.size();
    for (size_t i = 0; i < parsed.size(); ++i) {
        if (parsed[i].layer_id >= skip_until) {
            first_layer = i;
            break;
        }
    }

    LayerTimeSpeedSolveResult solved;
    solved.times = times;
    solved.speed_factors.assign(times.size(), 1.0);
    solved.effective_max_variation = m_config.layer_time_speed_max_variation.value / 100.0;

    if (is_layer_time_speed_up(m_mode)) {
        LayerTimeSpeedUpParams params;
        params.max_variation = m_config.layer_time_speed_max_variation.value / 100.0;
        params.max_speedup   = m_config.layer_time_speed_max_speedup.value / 100.0;
        solved               = solve_layer_time_speed_up(times, params, first_layer);
    } else if (is_layer_time_slowdown(m_mode)) {
        LayerTimeSlowdownParams params;
        params.max_variation     = m_config.layer_time_speed_max_variation.value / 100.0;
        params.max_slowdown      = m_config.layer_time_speed_max_slowdown.value / 100.0;
        params.max_time_increase = m_config.layer_time_speed_max_time_increase.value / 100.0;
        // Layers CoolingBuffer already stretched keep their time and only bound their neighbours.
        solved                   = solve_layer_time_slowdown(times, params, first_layer, frozen);
    }

    const double floor_s  = cooling_floor_s(m_config);
    const bool   speed_up = is_layer_time_speed_up(m_mode);

    std::string out;
    float       emitted_f = float(m_config.travel_speed.value) * 60.f;
    for (size_t i = 0; i < parsed.size(); ++i) {
        double factor = (i < solved.speed_factors.size()) ? solved.speed_factors[i] : 1.0;
        if (i < first_layer)
            factor = 1.0;
        factor = apply_cooling_floor(factor, parsed[i].time, floor_s, speed_up, parsed[i].cooling_slowed_down);

        const std::string body = apply_factor_to_lines(parsed[i].lines, factor, m_mode, m_slowdown_scope, m_config, emitted_f);
        const double      t_out = time_after_factor(parsed[i].lines, factor, m_mode, m_slowdown_scope, m_config);
        out += format_comment(m_mode, factor, parsed[i].time, t_out);
        out += body;
    }

    m_layers.clear();
    return out;
}

} // namespace Slic3r
