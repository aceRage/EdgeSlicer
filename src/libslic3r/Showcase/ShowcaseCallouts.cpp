#include "ShowcaseCallouts.hpp"
#include "ShowcaseScene.hpp"   // parse_hex_color

#include "../CustomGCode.hpp"
#include "../FilamentColorLibrary.hpp"
#include "../I18N.hpp"
#include "../Model.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <set>

#include <boost/log/trivial.hpp>

namespace Slic3r {
namespace Showcase {

// ------------------------------------------------------------------------------------------------
// Enum names (the JSON spellings; never translate them)
// ------------------------------------------------------------------------------------------------

namespace {

template<class E, size_t N> const char *callout_enum_name(E v, const char *const (&names)[N])
{
    const int i = int(v);
    return (i >= 0 && size_t(i) < N) ? names[i] : names[0];
}

template<class E, size_t N> bool callout_enum_parse(const std::string &s, E &v, const char *const (&names)[N])
{
    for (size_t i = 0; i < N; ++i)
        if (s == names[i]) {
            v = E(int(i));
            return true;
        }
    return false;
}

const char *const SOURCE_NAMES[] = { "object", "part", "paint", "height_range", "tool_change", "color_change" };
const char *const SHAPE_NAMES[]  = { "minimal", "card", "pill", "technical" };
const char *const LEADER_NAMES[] = { "straight", "elbow" };
const char *const LAYOUT_NAMES[] = { "auto", "columns", "legend" };

std::string co_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string co_trim(const std::string &s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char) s[b]))
        ++b;
    while (e > b && std::isspace((unsigned char) s[e - 1]))
        --e;
    return s.substr(b, e - b);
}

std::string co_upper_hex(const std::string &hex)
{
    std::string out = co_trim(hex);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    if (!out.empty() && out[0] != '#')
        out = "#" + out;
    // "#RRGGBBAA" -> "#RRGGBB": names and the library ignore alpha.
    if (out.size() == 9)
        out.resize(7);
    return out;
}

bool co_hex_rgb(const std::string &hex, std::array<int, 3> &rgb)
{
    std::array<float, 4> c;
    if (!parse_hex_color(co_upper_hex(hex), c))
        return false;
    for (int i = 0; i < 3; ++i)
        rgb[i] = int(std::lround(c[i] * 255.f));
    return true;
}

} // namespace

const char *to_string(CalloutSource v) { return callout_enum_name(v, SOURCE_NAMES); }
const char *to_string(CalloutShape v) { return callout_enum_name(v, SHAPE_NAMES); }
const char *to_string(LeaderStyle v) { return callout_enum_name(v, LEADER_NAMES); }
const char *to_string(CalloutLayout v) { return callout_enum_name(v, LAYOUT_NAMES); }
bool from_string(const std::string &s, CalloutSource &v) { return callout_enum_parse(s, v, SOURCE_NAMES); }
bool from_string(const std::string &s, CalloutShape &v) { return callout_enum_parse(s, v, SHAPE_NAMES); }
bool from_string(const std::string &s, LeaderStyle &v) { return callout_enum_parse(s, v, LEADER_NAMES); }
bool from_string(const std::string &s, CalloutLayout &v) { return callout_enum_parse(s, v, LAYOUT_NAMES); }

// ------------------------------------------------------------------------------------------------
// The stored callouts
// ------------------------------------------------------------------------------------------------

bool Callout::operator==(const Callout &o) const
{
    return key == o.key && filament == o.filament && source == o.source && z_from == o.z_from && color == o.color && anchors == o.anchors &&
           user_anchor == o.user_anchor && pinned == o.pinned && label_pos == o.label_pos && title == o.title &&
           color_name == o.color_name && note == o.note && hidden == o.hidden;
}

bool CalloutFields::operator==(const CalloutFields &o) const
{
    return brand == o.brand && product == o.product && color_name == o.color_name && hex == o.hex && type == o.type && weight == o.weight &&
           price == o.price;
}

bool CalloutStyle::operator==(const CalloutStyle &o) const
{
    return shape == o.shape && font_face == o.font_face && bold == o.bold && size_pct == o.size_pct && auto_colors == o.auto_colors &&
           text_color == o.text_color && line_color == o.line_color && background == o.background &&
           background_opacity == o.background_opacity && leader == o.leader;
}

bool Callouts::operator==(const Callouts &o) const
{
    return enabled == o.enabled && layout == o.layout && max_per_column == o.max_per_column && fields == o.fields && style == o.style &&
           items == o.items;
}

const Callout *Callouts::find(const std::string &key) const
{
    for (const Callout &c : items)
        if (c.key == key)
            return &c;
    return nullptr;
}

Callout *Callouts::find(const std::string &key)
{
    for (Callout &c : items)
        if (c.key == key)
            return &c;
    return nullptr;
}

std::vector<CalloutStylePreset> callout_style_presets()
{
    return { CalloutStylePreset::CardLight, CalloutStylePreset::CardDark, CalloutStylePreset::Minimal, CalloutStylePreset::Pill,
             CalloutStylePreset::Technical };
}

void apply_callout_style_preset(CalloutStyle &s, CalloutStylePreset preset)
{
    // The font stays the user's; everything else is the preset's.
    const std::string font = s.font_face;
    const bool        bold = s.bold;
    s           = CalloutStyle();
    s.font_face = font;
    s.bold      = bold;
    switch (preset) {
    case CalloutStylePreset::Minimal:
        s.shape  = CalloutShape::Minimal;
        s.leader = LeaderStyle::Straight;
        break;
    default:
    case CalloutStylePreset::CardLight:
        s.shape              = CalloutShape::Card;
        s.background         = "#FFFFFF";
        s.background_opacity = 0.92f;
        s.text_color         = "#1E1E1E";
        s.line_color         = "#1E1E1E";
        break;
    case CalloutStylePreset::CardDark:
        s.shape              = CalloutShape::Card;
        s.background         = "#1E1F22";
        s.background_opacity = 0.88f;
        s.text_color         = "#F2F2F2";
        s.line_color         = "#F2F2F2";
        break;
    case CalloutStylePreset::Pill:
        s.shape              = CalloutShape::Pill;
        s.leader             = LeaderStyle::Straight;
        s.background         = "#FFFFFF";
        s.background_opacity = 0.95f;
        break;
    case CalloutStylePreset::Technical:
        s.shape    = CalloutShape::Technical;
        s.size_pct = 2.f;
        // AutoCAD's multileader: a landing under the text.
        s.leader   = LeaderStyle::Elbow;
        break;
    }
}

std::string make_callout_key(int filament, double z_from, const std::string &color)
{
    std::string key = "f" + std::to_string(std::max(1, filament));
    if (z_from >= 0.) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "@%.2f", z_from);
        key += buf;
        if (!color.empty())
            key += co_upper_hex(color);
    }
    return key;
}

int prune_callouts(Callouts &callouts, size_t object_count, int filament_count)
{
    int dropped = 0;
    for (auto it = callouts.items.begin(); it != callouts.items.end();) {
        if (it->filament < 1 || (filament_count > 0 && it->filament > filament_count)) {
            BOOST_LOG_TRIVIAL(info) << "Showcase: callout " << it->key << " dropped, the project has no filament " << it->filament;
            it = callouts.items.erase(it);
            ++dropped;
            continue;
        }
        const size_t before = it->anchors.size();
        it->anchors.erase(std::remove_if(it->anchors.begin(), it->anchors.end(),
                                         [object_count](const CalloutAnchor &a) { return a.object_idx < 0 || size_t(a.object_idx) >= object_count; }),
                          it->anchors.end());
        if (it->anchors.size() != before) {
            BOOST_LOG_TRIVIAL(info) << "Showcase: callout " << it->key << " lost " << (before - it->anchors.size())
                                    << " anchor(s) on objects that are gone";
            dropped += int(before - it->anchors.size());
            if (it->anchors.empty())
                it->user_anchor = false;
        }
        ++it;
    }
    return dropped;
}

// ------------------------------------------------------------------------------------------------
// Which callouts a plate gets
// ------------------------------------------------------------------------------------------------

namespace {

constexpr double CO_Z_EPS = 1e-6;

void co_add_base(std::vector<BaseRegion> &bases, const BaseRegion &b)
{
    for (const BaseRegion &e : bases)
        if (e.filament == b.filament && e.object_default == b.object_default)
            return;
    bases.push_back(b);
}

} // namespace

CalloutRegions::CalloutRegions(const Model &model, const std::vector<int> &objects, const CustomGCode::Info *changes)
{
    std::set<int> seen;
    for (int idx : objects) {
        if (idx < 0 || size_t(idx) >= model.objects.size() || !seen.insert(idx).second)
            continue;
        const ModelObject *mo = model.objects[size_t(idx)];
        Object o;
        o.idx = idx;
        int object_filament = mo->config.has("extruder") ? mo->config.opt_int("extruder") : 0;
        if (object_filament <= 0)
            object_filament = 1;
        o.height = 0.;
        for (size_t i = 0; i < mo->instances.size(); ++i)
            o.height = std::max(o.height, mo->instance_bounding_box(i).max.z());
        if (mo->instances.empty())
            o.height = mo->raw_bounding_box().max.z();
        for (const ModelVolume *mv : mo->volumes) {
            if (mv == nullptr || !mv->is_model_part())
                continue;
            const ConfigOption *opt = mv->config.option("extruder");
            const int           own = opt != nullptr ? opt->getInt() : 0;
            BaseRegion          b;
            b.filament       = own > 0 ? own : object_filament;
            b.object_default = own <= 0;
            b.source         = own > 0 ? CalloutSource::Part : CalloutSource::Object;
            if (!mv->is_mm_painted()) {
                co_add_base(o.bases, b);
                continue;
            }
            // Multi-material painting: state s is filament s; state 0 is the volume's own filament
            // where nothing was painted (absent when every facet is painted).
            std::vector<indexed_triangle_set> per_state;
            mv->mmu_segmentation_facets.get_facets(*mv, per_state);
            for (size_t s = 0; s < per_state.size(); ++s) {
                if (per_state[s].indices.empty())
                    continue;
                if (s == 0)
                    co_add_base(o.bases, b);
                else
                    co_add_base(o.bases, BaseRegion{ int(s), false, CalloutSource::Paint });
            }
        }
        for (const auto &range : mo->layer_config_ranges) {
            const ConfigOption *opt = range.second.option("extruder");
            const int           f   = opt != nullptr ? opt->getInt() : 0;
            if (f > 0 && range.first.second > range.first.first)
                o.ranges.push_back({ range.first.first, range.first.second, f });
        }
        if (!o.bases.empty() && o.height > CO_Z_EPS)
            m_objects.push_back(std::move(o));
    }
    if (changes != nullptr)
        for (const CustomGCode::Item &item : changes->gcodes) {
            if (item.type == CustomGCode::ToolChange && item.extruder > 0)
                m_changes.push_back({ item.print_z, true, item.extruder, std::string() });
            else if (item.type == CustomGCode::ColorChange)
                m_changes.push_back({ item.print_z, false, item.extruder, co_upper_hex(item.color) });
        }
    std::stable_sort(m_changes.begin(), m_changes.end(), [](const Change &a, const Change &b) { return a.z < b.z; });
}

const CalloutRegions::Object *CalloutRegions::object(int object_idx) const
{
    for (const Object &o : m_objects)
        if (o.idx == object_idx)
            return &o;
    return nullptr;
}

const std::vector<BaseRegion> &CalloutRegions::base_regions(int object_idx) const
{
    static const std::vector<BaseRegion> none;
    const Object *o = object(object_idx);
    return o != nullptr ? o->bases : none;
}

CalloutTarget CalloutRegions::classify(int object_idx, const BaseRegion &base, double z) const
{
    CalloutTarget t;
    t.filament = std::max(1, base.filament);
    t.source   = base.source;
    // A height range modifier gives the object's own filament another one over its band.
    if (const Object *o = object(object_idx); o != nullptr && base.object_default)
        for (const Range &r : o->ranges)
            if (z >= r.lo - CO_Z_EPS && z < r.hi - CO_Z_EPS) {
                t.filament = r.filament;
                t.source   = CalloutSource::HeightRange;
            }
    // A tool change switches everything above it to its filament (multi-extruder as single).
    double tool_z = -1.;
    for (const Change &c : m_changes) {
        if (c.z > z + CO_Z_EPS)
            break;
        if (c.tool) {
            t.filament = c.extruder;
            t.source   = CalloutSource::ToolChange;
            t.z_from   = c.z;
            tool_z     = c.z;
            t.color.clear();
        }
    }
    // A colour change (M600) recolours its filament above it, until the next tool change.
    for (const Change &c : m_changes) {
        if (c.z > z + CO_Z_EPS)
            break;
        if (!c.tool && c.z >= tool_z && (c.extruder <= 0 || c.extruder == t.filament)) {
            t.source = CalloutSource::ColorChange;
            t.z_from = c.z;
            t.color  = c.color;
        }
    }
    t.key = make_callout_key(t.filament, t.z_from, t.color);
    t.objects.push_back(object_idx);
    return t;
}

std::vector<CalloutTarget> CalloutRegions::targets() const
{
    std::vector<CalloutTarget> out;
    for (const Object &o : m_objects) {
        // Each band between two heights where anything changes shows one filament per base region.
        std::vector<double> cuts{ 0., o.height };
        for (const Range &r : o.ranges)
            for (double z : { r.lo, r.hi })
                if (z > 0. && z < o.height)
                    cuts.push_back(z);
        for (const Change &c : m_changes)
            if (c.z > 0. && c.z < o.height)
                cuts.push_back(c.z);
        std::sort(cuts.begin(), cuts.end());
        cuts.erase(std::unique(cuts.begin(), cuts.end(), [](double a, double b) { return std::abs(a - b) < 1e-4; }), cuts.end());
        for (size_t i = 0; i + 1 < cuts.size(); ++i) {
            if (cuts[i + 1] - cuts[i] < 1e-4)
                continue;
            const double z = 0.5 * (cuts[i] + cuts[i + 1]);
            for (const BaseRegion &b : o.bases) {
                CalloutTarget t = classify(o.idx, b, z);
                auto it = std::find_if(out.begin(), out.end(), [&t](const CalloutTarget &e) { return e.key == t.key; });
                if (it == out.end())
                    out.push_back(std::move(t));
                else if (std::find(it->objects.begin(), it->objects.end(), o.idx) == it->objects.end())
                    it->objects.push_back(o.idx);
            }
        }
    }
    for (CalloutTarget &t : out)
        std::sort(t.objects.begin(), t.objects.end());
    std::stable_sort(out.begin(), out.end(), [](const CalloutTarget &a, const CalloutTarget &b) {
        if (a.filament != b.filament)
            return a.filament < b.filament;
        if (a.z_from != b.z_from)
            return a.z_from < b.z_from;
        return a.color < b.color;
    });
    return out;
}

std::vector<CalloutTarget> plan_callout_targets(const Model &model, const std::vector<int> &objects, const CustomGCode::Info *changes)
{
    return CalloutRegions(model, objects, changes).targets();
}

// ------------------------------------------------------------------------------------------------
// Anchors: the pole of inaccessibility of each visible region
// ------------------------------------------------------------------------------------------------

namespace {

// Felzenszwalb & Huttenlocher's exact squared distance transform of a sampled function, 1D.
void co_edt_1d(const std::vector<double> &f, int n, std::vector<double> &d, std::vector<int> &v, std::vector<double> &z)
{
    const double INF = 1e20;
    int k = 0;
    v[0] = 0;
    z[0] = -INF;
    z[1] = INF;
    for (int q = 1; q < n; ++q) {
        double s = ((f[q] + double(q) * q) - (f[v[k]] + double(v[k]) * v[k])) / (2. * q - 2. * v[k]);
        while (s <= z[k]) {
            --k;
            s = ((f[q] + double(q) * q) - (f[v[k]] + double(v[k]) * v[k])) / (2. * q - 2. * v[k]);
        }
        ++k;
        v[k]     = q;
        z[k]     = s;
        z[k + 1] = INF;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[k + 1] < q)
            ++k;
        d[q] = double(q - v[k]) * double(q - v[k]) + f[v[k]];
    }
}

struct CoComponent
{
    int              cls  = -1;
    int              area = 0;
    int              x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // bounding box, inclusive
    std::vector<int> pixels;
};

} // namespace

std::vector<std::vector<AnchorPixel>> find_anchor_pixels(const std::vector<int> &class_map, int w, int h, int num_classes, int max_regions,
                                                         int min_area)
{
    std::vector<std::vector<AnchorPixel>> out(size_t(std::max(0, num_classes)));
    if (w <= 0 || h <= 0 || class_map.size() < size_t(w) * size_t(h) || num_classes <= 0)
        return out;

    // Connected regions, 4-neighbour.
    std::vector<int>       comp_of(size_t(w) * size_t(h), -1);
    std::vector<CoComponent> comps;
    std::vector<int>       stack;
    for (int start = 0; start < w * h; ++start) {
        const int cls = class_map[size_t(start)];
        if (cls < 0 || cls >= num_classes || comp_of[size_t(start)] >= 0)
            continue;
        CoComponent c;
        c.cls = cls;
        c.x0 = c.x1 = start % w;
        c.y0 = c.y1 = start / w;
        const int id = int(comps.size());
        stack.assign(1, start);
        comp_of[size_t(start)] = id;
        while (!stack.empty()) {
            const int p = stack.back();
            stack.pop_back();
            c.pixels.push_back(p);
            const int x = p % w, y = p / w;
            c.x0 = std::min(c.x0, x);
            c.x1 = std::max(c.x1, x);
            c.y0 = std::min(c.y0, y);
            c.y1 = std::max(c.y1, y);
            const int nb[4][2] = { { x - 1, y }, { x + 1, y }, { x, y - 1 }, { x, y + 1 } };
            for (const auto &n : nb) {
                if (n[0] < 0 || n[0] >= w || n[1] < 0 || n[1] >= h)
                    continue;
                const int q = n[1] * w + n[0];
                if (comp_of[size_t(q)] < 0 && class_map[size_t(q)] == cls) {
                    comp_of[size_t(q)] = id;
                    stack.push_back(q);
                }
            }
        }
        c.area = int(c.pixels.size());
        comps.push_back(std::move(c));
    }

    // Per class: the largest regions.
    std::vector<std::vector<int>> by_class(static_cast<size_t>(num_classes));
    for (size_t i = 0; i < comps.size(); ++i)
        by_class[size_t(comps[i].cls)].push_back(int(i));
    for (int cls = 0; cls < num_classes; ++cls) {
        std::vector<int> &list = by_class[size_t(cls)];
        std::stable_sort(list.begin(), list.end(), [&comps](int a, int b) { return comps[size_t(a)].area > comps[size_t(b)].area; });
        if (list.size() > 1) {
            // Small specks (a sliver seen past another object) get no leader of their own.
            const size_t keep = std::max<size_t>(1, size_t(std::count_if(list.begin(), list.end(), [&comps, min_area](int i) {
                                                     return comps[size_t(i)].area >= min_area;
                                                 })));
            list.resize(std::min(keep, list.size()));
        }
        if (int(list.size()) > max_regions)
            list.resize(size_t(std::max(0, max_regions)));

        for (int ci : list) {
            const CoComponent &c = comps[size_t(ci)];
            // The distance transform of the region's box with a one-pixel border outside it: the
            // frame's edge counts as the region's edge too.
            const int bw = c.x1 - c.x0 + 3, bh = c.y1 - c.y0 + 3;
            const double INF = 1e20;
            std::vector<double> grid(size_t(bw) * size_t(bh), 0.);
            for (int p : c.pixels)
                grid[size_t((p / w - c.y0 + 1) * bw + (p % w - c.x0 + 1))] = INF;
            const int           n = std::max(bw, bh);
            std::vector<double> f(size_t(n) + 1), d(size_t(n) + 1), z(size_t(n) + 2);
            std::vector<int>    v(size_t(n) + 1);
            for (int x = 0; x < bw; ++x) {
                for (int y = 0; y < bh; ++y)
                    f[size_t(y)] = grid[size_t(y * bw + x)];
                co_edt_1d(f, bh, d, v, z);
                for (int y = 0; y < bh; ++y)
                    grid[size_t(y * bw + x)] = d[size_t(y)];
            }
            for (int y = 0; y < bh; ++y) {
                for (int x = 0; x < bw; ++x)
                    f[size_t(x)] = grid[size_t(y * bw + x)];
                co_edt_1d(f, bw, d, v, z);
                for (int x = 0; x < bw; ++x)
                    grid[size_t(y * bw + x)] = d[size_t(x)];
            }
            double cx = 0., cy = 0.;
            for (int p : c.pixels) {
                cx += p % w;
                cy += p / w;
            }
            cx /= double(c.area);
            cy /= double(c.area);
            AnchorPixel best;
            double      best_d = -1., best_c = 0.;
            for (int p : c.pixels) {
                const int    x  = p % w, y = p / w;
                const double dd = grid[size_t((y - c.y0 + 1) * bw + (x - c.x0 + 1))];
                const double dc = (x - cx) * (x - cx) + (y - cy) * (y - cy);
                // The deepest pixel; of equally deep ones, the one nearest the region's middle.
                if (dd > best_d + 1e-9 || (std::abs(dd - best_d) <= 1e-9 && dc < best_c)) {
                    best_d = dd;
                    best_c = dc;
                    best.x = x;
                    best.y = y;
                }
            }
            best.area      = c.area;
            best.clearance = std::sqrt(std::max(0., best_d));
            out[size_t(cls)].push_back(best);
        }
    }
    return out;
}

// ------------------------------------------------------------------------------------------------
// Colour names (D8)
// ------------------------------------------------------------------------------------------------

const std::vector<NamedColor> &named_colors()
{
    // Everyday colour names, the kind filament makers use. L() marks them for translation.
    static const std::vector<NamedColor> colors = {
        // Neutrals
        { L("Black"), "#000000" }, { L("Jet Black"), "#1B1B1B" }, { L("Charcoal"), "#36454F" }, { L("Graphite"), "#383838" },
        { L("Gunmetal"), "#2A3439" }, { L("Dark Grey"), "#555555" }, { L("Slate Grey"), "#708090" }, { L("Grey"), "#808080" },
        { L("Cool Grey"), "#8C92AC" }, { L("Warm Grey"), "#8B8680" }, { L("Titanium Grey"), "#878681" }, { L("Ash Grey"), "#B2BEB5" },
        { L("Silver"), "#C0C0C0" }, { L("Light Grey"), "#D3D3D3" }, { L("Platinum"), "#E5E4E2" }, { L("Off-White"), "#F5F5F0" },
        { L("White"), "#FFFFFF" }, { L("Ivory"), "#FFFFF0" }, { L("Cream"), "#FFFDD0" }, { L("Beige"), "#F5F5DC" },
        { L("Bone"), "#E3DAC9" }, { L("Champagne"), "#F7E7CE" }, { L("Wheat"), "#F5DEB3" }, { L("Sand"), "#C2B280" },
        { L("Khaki"), "#C3B091" }, { L("Tan"), "#D2B48C" }, { L("Taupe"), "#483C32" },
        // Browns and metals
        { L("Brown"), "#964B00" }, { L("Dark Brown"), "#5C4033" }, { L("Chocolate"), "#7B3F00" }, { L("Coffee"), "#6F4E37" },
        { L("Mocha"), "#967969" }, { L("Chestnut"), "#954535" }, { L("Walnut"), "#773F1A" }, { L("Caramel"), "#AF6E4D" },
        { L("Sienna"), "#A0522D" }, { L("Umber"), "#635147" }, { L("Rust"), "#B7410E" }, { L("Terracotta"), "#E2725B" },
        { L("Copper"), "#B87333" }, { L("Bronze"), "#CD7F32" }, { L("Brass"), "#B5A642" }, { L("Gold"), "#FFD700" },
        { L("Rose Gold"), "#B76E79" }, { L("Pewter"), "#96A8A1" }, { L("Steel Grey"), "#71797E" },
        // Reds
        { L("Red"), "#FF0000" }, { L("Fire Red"), "#CE2029" }, { L("Crimson"), "#DC143C" }, { L("Scarlet"), "#FF2400" },
        { L("Cherry Red"), "#D2042D" }, { L("Cardinal Red"), "#C41E3A" }, { L("Ruby"), "#E0115F" }, { L("Raspberry"), "#E30B5C" },
        { L("Dark Red"), "#8B0000" }, { L("Maroon"), "#800000" }, { L("Burgundy"), "#800020" }, { L("Wine"), "#722F37" },
        { L("Brick Red"), "#CB4154" }, { L("Tomato"), "#FF6347" }, { L("Coral"), "#FF7F50" }, { L("Salmon"), "#FA8072" },
        { L("Light Coral"), "#F08080" },
        // Oranges
        { L("Orange"), "#FFA500" }, { L("Dark Orange"), "#FF8C00" }, { L("Tangerine"), "#F28500" }, { L("Pumpkin"), "#FF7518" },
        { L("Burnt Orange"), "#CC5500" }, { L("Mango"), "#FF8243" }, { L("Apricot"), "#FBCEB1" }, { L("Peach"), "#FFE5B4" },
        { L("Amber"), "#FFBF00" }, { L("Saffron"), "#F4C430" },
        // Yellows
        { L("Yellow"), "#FFFF00" }, { L("Lemon Yellow"), "#FFF44F" }, { L("Canary Yellow"), "#FFEF00" }, { L("Golden Yellow"), "#FFDF00" },
        { L("Sunflower"), "#FFC512" }, { L("Mustard"), "#FFDB58" }, { L("Ochre"), "#CC7722" }, { L("Pale Yellow"), "#FFFF99" },
        { L("Light Yellow"), "#FFFFE0" },
        // Greens
        { L("Green"), "#008000" }, { L("Lime"), "#00FF00" }, { L("Lime Green"), "#32CD32" }, { L("Chartreuse"), "#7FFF00" },
        { L("Neon Green"), "#39FF14" }, { L("Grass Green"), "#7CFC00" }, { L("Kelly Green"), "#4CBB17" }, { L("Forest Green"), "#228B22" },
        { L("Dark Green"), "#006400" }, { L("Bottle Green"), "#006A4E" }, { L("Hunter Green"), "#355E3B" }, { L("Emerald"), "#50C878" },
        { L("Jade"), "#00A86B" }, { L("Sea Green"), "#2E8B57" }, { L("Pine Green"), "#01796F" }, { L("Olive"), "#808000" },
        { L("Olive Drab"), "#6B8E23" }, { L("Army Green"), "#4B5320" }, { L("Moss Green"), "#8A9A5B" }, { L("Fern Green"), "#4F7942" },
        { L("Avocado"), "#568203" }, { L("Sage"), "#B2AC88" }, { L("Pistachio"), "#93C572" }, { L("Mint"), "#98FF98" },
        { L("Mint Green"), "#3EB489" }, { L("Light Green"), "#90EE90" }, { L("Spring Green"), "#00FF7F" },
        // Cyans and teals
        { L("Teal"), "#008080" }, { L("Dark Teal"), "#014D4E" }, { L("Teal Blue"), "#367588" }, { L("Petrol"), "#005F6A" },
        { L("Turquoise"), "#40E0D0" }, { L("Cyan"), "#00FFFF" }, { L("Aquamarine"), "#7FFFD4" }, { L("Light Cyan"), "#E0FFFF" },
        { L("Cerulean"), "#007BA7" },
        // Blues
        { L("Blue"), "#0000FF" }, { L("Royal Blue"), "#4169E1" }, { L("Cobalt Blue"), "#0047AB" }, { L("Sapphire"), "#0F52BA" },
        { L("Azure"), "#007FFF" }, { L("Dodger Blue"), "#1E90FF" }, { L("Cornflower Blue"), "#6495ED" }, { L("Steel Blue"), "#4682B4" },
        { L("Denim"), "#1560BD" }, { L("Navy"), "#000080" }, { L("Dark Blue"), "#00008B" }, { L("Midnight Blue"), "#191970" },
        { L("Prussian Blue"), "#003153" }, { L("Ultramarine"), "#3F00FF" }, { L("Electric Blue"), "#7DF9FF" }, { L("Sky Blue"), "#87CEEB" },
        { L("Light Blue"), "#ADD8E6" }, { L("Baby Blue"), "#89CFF0" }, { L("Powder Blue"), "#B0E0E6" }, { L("Periwinkle"), "#CCCCFF" },
        { L("Slate Blue"), "#6A5ACD" }, { L("Indigo"), "#4B0082" },
        // Purples
        { L("Purple"), "#800080" }, { L("Violet"), "#8F00FF" }, { L("Dark Purple"), "#301934" }, { L("Grape"), "#6F2DA8" },
        { L("Amethyst"), "#9966CC" }, { L("Plum"), "#8E4585" }, { L("Eggplant"), "#614051" }, { L("Orchid"), "#DA70D6" },
        { L("Lilac"), "#C8A2C8" }, { L("Mauve"), "#E0B0FF" }, { L("Lavender"), "#E6E6FA" }, { L("Magenta"), "#FF00FF" },
        // Pinks
        { L("Pink"), "#FFC0CB" }, { L("Light Pink"), "#FFB6C1" }, { L("Baby Pink"), "#F4C2C2" }, { L("Pastel Pink"), "#F8C8DC" },
        { L("Bubblegum Pink"), "#FFC1CC" }, { L("Flamingo Pink"), "#FC8EAC" }, { L("Hot Pink"), "#FF69B4" }, { L("Deep Pink"), "#FF1493" },
        { L("Neon Pink"), "#FF6EC7" }, { L("Rose"), "#FF007F" }, { L("Blush"), "#DE5D83" }, { L("Old Rose"), "#C08081" },
    };
    return colors;
}

namespace {

std::array<double, 3> co_srgb_to_lab(const std::array<int, 3> &rgb)
{
    auto lin = [](int v) {
        const double c = double(v) / 255.;
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    const double r = lin(rgb[0]), g = lin(rgb[1]), b = lin(rgb[2]);
    const double X = (0.4124564 * r + 0.3575761 * g + 0.1804375 * b) / 0.95047;
    const double Y = (0.2126729 * r + 0.7151522 * g + 0.0721750 * b) / 1.;
    const double Z = (0.0193339 * r + 0.1191920 * g + 0.9503041 * b) / 1.08883;
    auto f = [](double t) {
        const double d = 6. / 29.;
        return t > d * d * d ? std::cbrt(t) : t / (3. * d * d) + 4. / 29.;
    };
    const double fx = f(X), fy = f(Y), fz = f(Z);
    return { 116. * fy - 16., 500. * (fx - fy), 200. * (fy - fz) };
}

} // namespace

double delta_e2000(const std::array<int, 3> &rgb1, const std::array<int, 3> &rgb2)
{
    static constexpr double PI_D = 3.14159265358979323846;
    const auto   lab1 = co_srgb_to_lab(rgb1), lab2 = co_srgb_to_lab(rgb2);
    const double L1 = lab1[0], a1 = lab1[1], b1 = lab1[2];
    const double L2 = lab2[0], a2 = lab2[1], b2 = lab2[2];
    const double C1 = std::hypot(a1, b1), C2 = std::hypot(a2, b2);
    const double Cm = 0.5 * (C1 + C2);
    const double G  = 0.5 * (1. - std::sqrt(std::pow(Cm, 7) / (std::pow(Cm, 7) + std::pow(25., 7))));
    const double a1p = (1. + G) * a1, a2p = (1. + G) * a2;
    const double C1p = std::hypot(a1p, b1), C2p = std::hypot(a2p, b2);
    auto hue = [](double b, double ap) {
        if (b == 0. && ap == 0.)
            return 0.;
        double h = std::atan2(b, ap) * 180. / PI_D;
        return h < 0. ? h + 360. : h;
    };
    const double h1p = hue(b1, a1p), h2p = hue(b2, a2p);
    const double dLp = L2 - L1;
    const double dCp = C2p - C1p;
    double       dhp = 0.;
    if (C1p * C2p != 0.) {
        dhp = h2p - h1p;
        if (dhp > 180.)
            dhp -= 360.;
        else if (dhp < -180.)
            dhp += 360.;
    }
    const double dHp = 2. * std::sqrt(C1p * C2p) * std::sin(dhp * PI_D / 360.);
    const double Lpm = 0.5 * (L1 + L2);
    const double Cpm = 0.5 * (C1p + C2p);
    double       hpm = h1p + h2p;
    if (C1p * C2p != 0.) {
        if (std::abs(h1p - h2p) <= 180.)
            hpm = 0.5 * (h1p + h2p);
        else
            hpm = (h1p + h2p < 360.) ? 0.5 * (h1p + h2p + 360.) : 0.5 * (h1p + h2p - 360.);
    }
    const double T = 1. - 0.17 * std::cos((hpm - 30.) * PI_D / 180.) + 0.24 * std::cos(2. * hpm * PI_D / 180.) +
                     0.32 * std::cos((3. * hpm + 6.) * PI_D / 180.) - 0.20 * std::cos((4. * hpm - 63.) * PI_D / 180.);
    const double dTheta = 30. * std::exp(-((hpm - 275.) / 25.) * ((hpm - 275.) / 25.));
    const double Rc     = 2. * std::sqrt(std::pow(Cpm, 7) / (std::pow(Cpm, 7) + std::pow(25., 7)));
    const double Sl     = 1. + (0.015 * (Lpm - 50.) * (Lpm - 50.)) / std::sqrt(20. + (Lpm - 50.) * (Lpm - 50.));
    const double Sc     = 1. + 0.045 * Cpm;
    const double Sh     = 1. + 0.015 * Cpm * T;
    const double Rt     = -std::sin(2. * dTheta * PI_D / 180.) * Rc;
    const double tl = dLp / Sl, tc = dCp / Sc, th = dHp / Sh;
    return std::sqrt(tl * tl + tc * tc + th * th + Rt * tc * th);
}

ColorNameMatch nearest_color_name(const std::string &hex)
{
    ColorNameMatch     best;
    std::array<int, 3> rgb;
    if (!co_hex_rgb(hex, rgb))
        return best;
    best.delta_e = std::numeric_limits<double>::max();
    for (const NamedColor &nc : named_colors()) {
        std::array<int, 3> c;
        if (!co_hex_rgb(nc.hex, c))
            continue;
        if (c == rgb) {
            best.name    = nc.name;
            best.exact   = true;
            best.delta_e = 0.;
            return best;
        }
        const double d = delta_e2000(rgb, c);
        if (d < best.delta_e) {
            best.delta_e = d;
            best.name    = nc.name;
        }
    }
    return best;
}

std::string library_color_name(const FilamentColorInfo *family, const std::vector<FilamentColorInfo> &library,
                               const std::vector<std::string> &colors, const std::string &lang)
{
    std::vector<std::string> want;
    for (const std::string &c : colors)
        if (!c.empty())
            want.push_back(co_upper_hex(c));
    if (want.empty())
        return {};
    auto name_of = [&lang](const FilamentColorItem &item) -> std::string {
        if (item.colorNames.empty())
            return {};
        if (auto it = item.colorNames.find(lang); it != item.colorNames.end() && !it->second.empty())
            return it->second;
        // "de_DE" also takes "de"; then English; then whatever there is.
        const std::string short_lang = lang.substr(0, std::min<size_t>(2, lang.size()));
        if (!short_lang.empty())
            for (const auto &kv : item.colorNames)
                if (kv.first.compare(0, short_lang.size(), short_lang) == 0 && !kv.second.empty())
                    return kv.second;
        if (auto it = item.colorNames.find("en"); it != item.colorNames.end() && !it->second.empty())
            return it->second;
        for (const auto &kv : item.colorNames)
            if (!kv.second.empty())
                return kv.second;
        return {};
    };
    auto matches = [&want](const FilamentColorItem &item) {
        if (item.colorData.colors.size() != want.size())
            return false;
        for (size_t i = 0; i < want.size(); ++i)
            if (co_upper_hex(item.colorData.colors[i]) != want[i])
                return false;
        return true;
    };
    if (family != nullptr)
        for (const FilamentColorItem &item : family->colors)
            if (matches(item))
                if (std::string n = name_of(item); !n.empty())
                    return n;
    for (const FilamentColorInfo &info : library)
        for (const FilamentColorItem &item : info.colors)
            if (matches(item))
                if (std::string n = name_of(item); !n.empty())
                    return n;
    return {};
}

std::string resolve_color_name(const std::string &library_name, const std::string &user_name, const std::string &hex,
                               const std::function<std::string(const char *)> &translate)
{
    if (!co_trim(library_name).empty())
        return co_trim(library_name);
    if (!co_trim(user_name).empty())
        return co_trim(user_name);
    const ColorNameMatch m = nearest_color_name(hex);
    if (m.name.empty())
        return {};
    const std::string name = translate ? translate(m.name.c_str()) : m.name;
    // "≈": the nearest of the built-in names, not the maker's own.
    return m.exact ? name : std::string("\xE2\x89\x88 ") + name;
}

// ------------------------------------------------------------------------------------------------
// Label text
// ------------------------------------------------------------------------------------------------

std::string product_name(const std::string &preset_name, const std::string &vendor)
{
    std::string name = co_trim(GetFilamentMatchName(preset_name));
    const std::string v = co_trim(vendor);
    if (name.empty())
        return co_trim(preset_name);
    if (!v.empty()) {
        const std::string lname = co_lower(name), lv = co_lower(v);
        if (lname.size() > lv.size() && lname.compare(0, lv.size(), lv) == 0 && std::isspace((unsigned char) lname[lv.size()]))
            return co_trim(name.substr(v.size()));
        // "Bambu PLA Basic" from "Bambu Lab": the maker's first word starts the name.
        const size_t vs = lv.find(' '), ns = lname.find(' ');
        if (vs != std::string::npos && ns != std::string::npos && lname.compare(0, ns, lv, 0, vs) == 0 && ns == vs)
            return co_trim(name.substr(ns));
    }
    return name;
}

std::string format_weight(double grams, const std::string &unit)
{
    char buf[48];
    if (grams < 10.)
        std::snprintf(buf, sizeof(buf), "%.1f %s", grams, unit.c_str());
    else
        std::snprintf(buf, sizeof(buf), "%.0f %s", grams, unit.c_str());
    return buf;
}

std::string format_price(double amount, const std::string &currency)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.2f", amount);
    return currency + buf;
}

CalloutLabel build_callout_label(const Callout &callout, const LabelFilament &f, const CalloutFields &fields, const LabelWords &words)
{
    CalloutLabel label;
    if (!co_trim(callout.title).empty())
        label.title = co_trim(callout.title);
    else if (f.mixed) {
        if (fields.brand || fields.product)
            label.title = words.mixed_title;
    } else {
        const std::string brand   = fields.brand ? co_trim(f.vendor) : std::string();
        const std::string product = fields.product ? product_name(f.preset_name, f.vendor) : std::string();
        label.title               = brand;
        if (!product.empty())
            label.title += (label.title.empty() ? "" : " ") + product;
    }
    if (fields.color_name)
        label.color_name = f.color_name;
    if (fields.hex && !f.colors.empty()) {
        label.swatches = f.colors;
        label.hex      = co_upper_hex(f.colors.front());
    }
    std::vector<std::string> parts;
    if (f.mixed && !f.mixed_parts.empty())
        parts.push_back(f.mixed_parts);
    if (fields.type) {
        if (!co_trim(f.type).empty())
            parts.push_back(co_trim(f.type));
        if (!f.standard_finish && !f.finish.empty() && co_lower(f.type).find(co_lower(f.finish)) == std::string::npos)
            parts.push_back(f.finish);
    }
    if (fields.weight && f.weight_g >= 0.)
        parts.push_back(format_weight(f.weight_g, words.grams));
    if (fields.price && f.price >= 0.)
        parts.push_back(format_price(f.price, words.currency));
    if ((callout.source == CalloutSource::ToolChange || callout.source == CalloutSource::ColorChange) && callout.z_from >= 0.) {
        char z[32];
        std::snprintf(z, sizeof(z), "%.1f", callout.z_from);
        std::string from = words.from_height;
        if (const size_t at = from.find("%1%"); at != std::string::npos)
            from.replace(at, 3, z);
        else
            from += std::string(" ") + z;
        parts.push_back(from);
    }
    for (size_t i = 0; i < parts.size(); ++i)
        label.details += (i == 0 ? "" : " \xC2\xB7 ") + parts[i];   // " · "
    label.note = co_trim(callout.note);
    return label;
}

std::string callout_first_line(const CalloutLabel &label)
{
    if (label.title.empty())
        return label.color_name;
    if (label.color_name.empty())
        return label.title;
    return label.title + " \xE2\x80\x93 " + label.color_name;   // " – "
}

double hex_luminance(const std::string &hex, double fallback)
{
    std::array<int, 3> rgb;
    if (!co_hex_rgb(hex, rgb))
        return fallback;
    return (0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]) / 255.;
}

std::string auto_ink(double luminance) { return luminance >= 0.55 ? "#1E1E1E" : "#F5F5F5"; }

// ------------------------------------------------------------------------------------------------
// Layout
// ------------------------------------------------------------------------------------------------

namespace {

double co_cross(const PointD &o, const PointD &a, const PointD &b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }

bool co_on_segment(const PointD &a, const PointD &b, const PointD &p)
{
    return std::min(a.x, b.x) - 1e-9 <= p.x && p.x <= std::max(a.x, b.x) + 1e-9 && std::min(a.y, b.y) - 1e-9 <= p.y &&
           p.y <= std::max(a.y, b.y) + 1e-9;
}

// The tops of labels stacked in this order, as near their ideal tops as can be (least squares,
// the pool-adjacent-violators algorithm), `gap` apart, inside [lo, hi]. False when they cannot fit.
bool co_pack_column(const std::vector<double> &ideal, const std::vector<double> &h, double gap, double lo, double hi, std::vector<double> &top)
{
    const size_t n = ideal.size();
    top.assign(n, lo);
    if (n == 0)
        return true;
    std::vector<double> offset(n, 0.);
    for (size_t i = 1; i < n; ++i)
        offset[i] = offset[i - 1] + h[i - 1] + gap;
    const double total = offset[n - 1] + h[n - 1];
    // Ordered, non-overlapping tops are a non-decreasing sequence once each loses its offset.
    struct Block { double sum; int count; };
    std::vector<Block> blocks;
    for (size_t i = 0; i < n; ++i) {
        blocks.push_back({ ideal[i] - offset[i], 1 });
        while (blocks.size() > 1) {
            const Block &b = blocks.back(), &a = blocks[blocks.size() - 2];
            if (a.sum / a.count <= b.sum / b.count)
                break;
            Block m{ a.sum + b.sum, a.count + b.count };
            blocks.pop_back();
            blocks.back() = m;
        }
    }
    const double zmax = std::max(lo, hi - total);
    size_t i = 0;
    for (const Block &b : blocks)
        for (int k = 0; k < b.count; ++k, ++i)
            top[i] = std::clamp(b.sum / b.count, lo, zmax) + offset[i];
    return total <= hi - lo + 1e-9;
}

// The free height of a column: the frame less its margins, less any reserved box over the column.
void co_column_bounds(const LayoutParams &p, double x0, double x1, double &lo, double &hi)
{
    lo = p.margin;
    hi = p.frame_h - p.margin;
    for (const RectD &r : p.reserved) {
        if (r.empty() || r.right() <= x0 || r.x >= x1)
            continue;
        if (r.y + 0.5 * r.h < 0.5 * p.frame_h)
            lo = std::max(lo, r.bottom() + p.gap);
        else
            hi = std::min(hi, r.y - p.gap);
    }
}

double co_column_width(const std::vector<LayoutLabel> &labels, const std::vector<int> &members)
{
    double w = 0.;
    for (int i : members)
        w = std::max(w, labels[size_t(i)].w);
    return w;
}

bool co_column_fits(const std::vector<LayoutLabel> &labels, const std::vector<int> &members, const LayoutParams &p, int side, bool limit_count)
{
    if (members.empty())
        return true;
    if (limit_count && int(members.size()) > std::max(1, p.max_per_column))
        return false;
    const double w  = co_column_width(labels, members);
    const double x0 = side < 0 ? p.margin : p.frame_w - p.margin - w;
    double lo, hi;
    co_column_bounds(p, x0, x0 + w, lo, hi);
    double total = 0.;
    for (int i : members)
        total += labels[size_t(i)].h;
    total += p.gap * double(members.size() - 1);
    return total <= hi - lo + 1e-9;
}

} // namespace

bool segments_cross(const PointD &a, const PointD &b, const PointD &c, const PointD &d)
{
    const double d1 = co_cross(c, d, a), d2 = co_cross(c, d, b), d3 = co_cross(a, b, c), d4 = co_cross(a, b, d);
    if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0)))
        return true;
    const double eps = 1e-9;
    if (std::abs(d1) <= eps && co_on_segment(c, d, a)) return true;
    if (std::abs(d2) <= eps && co_on_segment(c, d, b)) return true;
    if (std::abs(d3) <= eps && co_on_segment(a, b, c)) return true;
    if (std::abs(d4) <= eps && co_on_segment(a, b, d)) return true;
    return false;
}

bool polylines_cross(const std::vector<PointD> &a, const std::vector<PointD> &b)
{
    for (size_t i = 0; i + 1 < a.size(); ++i)
        for (size_t j = 0; j + 1 < b.size(); ++j)
            if (segments_cross(a[i], a[i + 1], b[j], b[j + 1]))
                return true;
    return false;
}

CalloutLayoutResult layout_callouts(const std::vector<LayoutLabel> &labels, const LayoutParams &p)
{
    CalloutLayoutResult res;
    const size_t        n = labels.size();
    res.labels.resize(n);
    if (n == 0 || p.frame_w <= 0. || p.frame_h <= 0.)
        return res;

    // Pinned labels stay where the user put them (inside the frame), with a straight leader to
    // their nearer side.
    std::vector<int> free_items, anchored;
    for (size_t i = 0; i < n; ++i) {
        const LayoutLabel &l = labels[i];
        if (l.pinned && p.mode != CalloutLayout::Legend) {
            PlacedLabel &pl = res.labels[i];
            pl.box = { std::clamp(l.pinned_pos.x, 0., std::max(0., p.frame_w - l.w)), std::clamp(l.pinned_pos.y, 0., std::max(0., p.frame_h - l.h)),
                       l.w, l.h };
            if (l.has_anchor) {
                const double cy = pl.box.y + 0.5 * pl.box.h;
                const PointD end = l.anchor.x < pl.box.x + 0.5 * pl.box.w ? PointD{ pl.box.x, cy } : PointD{ pl.box.right(), cy };
                pl.leader = { l.anchor, end };
            }
            continue;
        }
        free_items.push_back(int(i));
        if (l.has_anchor)
            anchored.push_back(int(i));
    }

    double center = p.model_center_x;
    if (center < 0.) {
        if (anchored.empty())
            center = 0.5 * p.frame_w;
        else {
            double mn = std::numeric_limits<double>::max(), mx = -mn;
            for (int i : anchored) {
                mn = std::min(mn, labels[size_t(i)].anchor.x);
                mx = std::max(mx, labels[size_t(i)].anchor.x);
            }
            center = 0.5 * (mn + mx);
        }
    }

    bool legend = p.mode == CalloutLayout::Legend;
    // --- columns -------------------------------------------------------------------------------
    std::vector<int> left, right;
    if (!legend && !free_items.empty()) {
        // Anchors left of the model's middle go left, the rest right: the left column is a prefix
        // of the anchors sorted by x, so no leader of one column can reach the other's.
        std::vector<int> by_x = anchored;
        std::stable_sort(by_x.begin(), by_x.end(), [&labels](int a, int b) { return labels[size_t(a)].anchor.x < labels[size_t(b)].anchor.x; });
        int k = int(std::count_if(by_x.begin(), by_x.end(), [&labels, center](int i) { return labels[size_t(i)].anchor.x < center; }));
        std::vector<int> unanchored;
        for (int i : free_items)
            if (!labels[size_t(i)].has_anchor)
                unanchored.push_back(i);
        const bool limit = p.mode == CalloutLayout::Auto;
        auto split = [&](int kk) {
            left.assign(by_x.begin(), by_x.begin() + kk);
            right.assign(by_x.begin() + kk, by_x.end());
            right.insert(right.end(), unanchored.begin(), unanchored.end());
        };
        // Balance: move the label whose anchor is nearest the middle across while a column overflows.
        bool fits = false;
        int  direction = 0;
        for (int iter = 0; iter <= int(n) + 1; ++iter) {
            split(k);
            const bool fl = co_column_fits(labels, left, p, -1, limit);
            const bool fr = co_column_fits(labels, right, p, 1, limit);
            if (fl && fr) {
                fits = true;
                break;
            }
            if (!fl && !fr)
                break;
            const int step = fl ? 1 : -1;
            if (direction != 0 && step != direction)
                break;   // both ways overflow in turn: nothing balances it
            direction = step;
            k += step;
            if (k < 0 || k > int(by_x.size()))
                break;
        }
        if (!fits) {
            if (p.mode == CalloutLayout::Auto)
                legend = true;
            else {
                // Columns asked for: keep them, squeezed (labels may overlap at the ends).
                k = int(std::count_if(by_x.begin(), by_x.end(), [&labels, center](int i) { return labels[size_t(i)].anchor.x < center; }));
                split(k);
            }
        }
    }

    if (!legend) {
        for (int side : { -1, 1 }) {
            std::vector<int> &members = side < 0 ? left : right;
            if (members.empty())
                continue;
            const double w      = co_column_width(labels, members);
            const double x0     = side < 0 ? p.margin : p.frame_w - p.margin - w;
            const double x_edge = side < 0 ? x0 + w : x0;   // where every leader meets its label
            const double x_land = x_edge - side * (p.leader == LeaderStyle::Elbow ? p.landing : 0.);
            double lo, hi;
            co_column_bounds(p, x0, x0 + w, lo, hi);

            auto ideal_y = [&labels](int i) { return labels[size_t(i)].has_anchor ? labels[size_t(i)].anchor.y : 1e9; };
            std::vector<int> order = members;
            std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return ideal_y(a) < ideal_y(b); });

            auto place = [&](const std::vector<int> &ord, bool bent) {
                std::vector<double> ideal, hs, tops;
                for (int i : ord) {
                    const LayoutLabel &l = labels[size_t(i)];
                    ideal.push_back(l.has_anchor ? l.anchor.y - 0.5 * l.h : hi);
                    hs.push_back(l.h);
                }
                co_pack_column(ideal, hs, p.gap, lo, hi, tops);
                // Bent leaders turn just past the column's outermost anchor.
                double turn = 0.;
                if (bent) {
                    double extreme = side < 0 ? std::numeric_limits<double>::max() : -std::numeric_limits<double>::max();
                    for (int i : ord)
                        if (labels[size_t(i)].has_anchor)
                            extreme = side < 0 ? std::min(extreme, labels[size_t(i)].anchor.x) : std::max(extreme, labels[size_t(i)].anchor.x);
                    const double room = side < 0 ? extreme - x_land : x_land - extreme;
                    turn = extreme + side * std::max(0., std::min(p.gap, 0.5 * room));
                }
                for (size_t k = 0; k < ord.size(); ++k) {
                    const int          i = ord[k];
                    const LayoutLabel &l = labels[size_t(i)];
                    PlacedLabel       &pl = res.labels[size_t(i)];
                    pl.side = side;
                    pl.box  = { side < 0 ? x_edge - l.w : x_edge, tops[k], l.w, l.h };
                    pl.leader.clear();
                    if (!l.has_anchor)
                        continue;
                    const double cy = tops[k] + 0.5 * l.h;
                    pl.leader.push_back(l.anchor);
                    if (bent)
                        pl.leader.push_back({ turn, l.anchor.y });
                    pl.leader.push_back({ x_land, cy });
                    if (x_land != x_edge)
                        pl.leader.push_back({ x_edge, cy });
                }
            };
            auto first_crossing = [&](const std::vector<int> &ord, size_t &a, size_t &b) {
                for (size_t i = 0; i < ord.size(); ++i)
                    for (size_t j = i + 1; j < ord.size(); ++j)
                        if (polylines_cross(res.labels[size_t(ord[i])].leader, res.labels[size_t(ord[j])].leader)) {
                            a = i;
                            b = j;
                            return true;
                        }
                return false;
            };

            // Straight leaders in anchor order; where two cross, the two labels swap places (each
            // swap shortens the leaders, so this settles). If it does not, the leaders bend: out to
            // the side level with their anchor first, which can never cross.
            place(order, false);
            size_t a = 0, b = 0;
            bool   crossing = first_crossing(order, a, b);
            for (int iter = 0; crossing && iter < 4 * int(order.size() * order.size()) + 4; ++iter) {
                std::swap(order[a], order[b]);
                place(order, false);
                crossing = first_crossing(order, a, b);
            }
            if (crossing) {
                std::stable_sort(order.begin(), order.end(), [&](int x, int y) { return ideal_y(x) < ideal_y(y); });
                place(order, true);
                res.bent = true;
            }
        }
        return res;
    }

    // --- the numbered legend -------------------------------------------------------------------
    res.legend = true;
    std::vector<int> entries;
    for (size_t i = 0; i < n; ++i)
        entries.push_back(int(i));
    // The key goes on the side away from the model.
    const int    side  = center < 0.5 * p.frame_w ? 1 : -1;
    double       row_w = 0.;
    for (int i : entries)
        row_w = std::max(row_w, p.legend_badge + labels[size_t(i)].w);
    const double pad = p.gap;
    double lo, hi;
    co_column_bounds(p, side < 0 ? p.margin : p.frame_w - p.margin - row_w - 2. * pad,
                  side < 0 ? p.margin + row_w + 2. * pad : p.frame_w - p.margin, lo, hi);
    // Rows top to bottom; a key too tall for the frame goes on in another column, inwards.
    std::vector<std::vector<int>> cols(1);
    double y = lo + pad;
    for (int i : entries) {
        const double h = labels[size_t(i)].h;
        if (!cols.back().empty() && y + h > hi - pad) {
            cols.emplace_back();
            y = lo + pad;
        }
        cols.back().push_back(i);
        y += h + p.gap;
    }
    double box_w = 2. * pad, box_h = 0.;
    std::vector<double> col_w;
    for (const std::vector<int> &c : cols) {
        double cw = 0., ch = 0.;
        for (int i : c) {
            cw = std::max(cw, p.legend_badge + labels[size_t(i)].w);
            ch += labels[size_t(i)].h + p.gap;
        }
        col_w.push_back(cw);
        box_w += cw + (col_w.size() > 1 ? 2. * pad : 0.);
        box_h = std::max(box_h, ch - p.gap + 2. * pad);
    }
    const double bx = side < 0 ? p.margin : p.frame_w - p.margin - box_w;
    res.legend_box  = { bx, lo, box_w, box_h };
    double x        = bx + pad;
    int    number   = 1;
    for (size_t c = 0; c < cols.size(); ++c) {
        double yy = lo + pad;
        for (int i : cols[c]) {
            PlacedLabel &pl = res.labels[size_t(i)];
            pl.side   = 0;
            pl.number = number++;
            pl.box    = { x + p.legend_badge, yy, labels[size_t(i)].w, labels[size_t(i)].h };
            pl.leader.clear();
            yy += labels[size_t(i)].h + p.gap;
        }
        x += col_w[c] + 2. * pad;
    }
    return res;
}

} // namespace Showcase
} // namespace Slic3r
