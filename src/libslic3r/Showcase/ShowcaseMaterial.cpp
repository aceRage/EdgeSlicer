#include "ShowcaseMaterial.hpp"

#include "../PrintConfig.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <initializer_list>

namespace Slic3r {
namespace Showcase {

namespace {

const char *const FINISH_NAMES[]      = { "standard", "matte", "silk", "gloss", "transparent", "metallic", "carbon_fiber" };
const char *const LAYER_LINES_NAMES[] = { "off", "subtle", "real", "exaggerated" };

template<class E, size_t N> const char *material_enum_name(E v, const char *const (&names)[N])
{
    const int i = int(v);
    return (i >= 0 && size_t(i) < N) ? names[i] : names[0];
}

template<class E, size_t N> bool material_enum_parse(const std::string &s, E &v, const char *const (&names)[N])
{
    for (size_t i = 0; i < N; ++i)
        if (s == names[i]) {
            v = E(int(i));
            return true;
        }
    return false;
}

// Lower-case alphanumeric words: "PLA-CF10 @U1" -> pla, cf10, u1.
std::vector<std::string> words(const std::string &text)
{
    std::vector<std::string> out;
    std::string              w;
    for (unsigned char c : text) {
        if (std::isalnum(c))
            w.push_back(char(std::tolower(c)));
        else if (!w.empty()) {
            out.push_back(w);
            w.clear();
        }
    }
    if (!w.empty())
        out.push_back(w);
    return out;
}

bool has_word(const std::vector<std::string> &ws, std::initializer_list<const char *> any)
{
    for (const std::string &w : ws)
        for (const char *a : any)
            if (w == a)
                return true;
    return false;
}

// "cf", "gf", or the same followed by a fibre percentage ("cf10", "gf30").
bool fibre_word(const std::string &w)
{
    if (w.size() < 2 || (w.compare(0, 2, "cf") != 0 && w.compare(0, 2, "gf") != 0))
        return false;
    return std::all_of(w.begin() + 2, w.end(), [](char c) { return std::isdigit((unsigned char) c) != 0; });
}

float smoothstep01(float e0, float e1, float x)
{
    const float t = std::clamp((x - e0) / (e1 - e0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

} // namespace

const char *to_string(Finish v) { return material_enum_name(v, FINISH_NAMES); }

const std::vector<std::string> &reserved_finish_names()
{
    static const std::vector<std::string> names = { "multicolour_silk" };
    return names;
}
bool        from_string(const std::string &s, Finish &v) { return material_enum_parse(s, v, FINISH_NAMES); }
const char *to_string(LayerLines v) { return material_enum_name(v, LAYER_LINES_NAMES); }
bool        from_string(const std::string &s, LayerLines &v) { return material_enum_parse(s, v, LAYER_LINES_NAMES); }

std::vector<Finish> all_finishes()
{
    return { Finish::Standard, Finish::Matte, Finish::Silk, Finish::Gloss, Finish::Transparent, Finish::Metallic, Finish::CarbonFiber };
}

Finish infer_finish(const std::string &preset_name, const std::string &filament_type)
{
    std::vector<std::string> ws = words(preset_name);
    const std::vector<std::string> type = words(filament_type);
    ws.insert(ws.end(), type.begin(), type.end());

    if (has_word(ws, { "translucent", "transparent", "clear", "crystal" }))
        return Finish::Transparent;
    if (has_word(ws, { "silk", "silky", "shiny", "satin" }))
        return Finish::Silk;
    if (has_word(ws, { "metal", "metallic", "bronze", "copper", "steel", "brass", "aluminium", "aluminum", "iron" }))
        return Finish::Metallic;
    if (has_word(ws, { "carbon", "fiber", "fibre" }) || std::any_of(ws.begin(), ws.end(), fibre_word))
        return Finish::CarbonFiber;
    if (has_word(ws, { "matte", "matt", "aero" }))
        return Finish::Matte;
    if (has_word(ws, { "petg", "pctg", "pet", "gloss", "glossy" }))
        return Finish::Gloss;
    return Finish::Standard;
}

float default_toolpath_smoothness(Finish finish)
{
    switch (finish) {
    case Finish::Matte: return 0.5f;
    case Finish::Silk: return 0.8f;
    case Finish::Gloss: return 0.75f;
    case Finish::Transparent: return 0.7f;
    case Finish::Metallic: return 0.7f;
    case Finish::CarbonFiber: return 0.45f;
    default: return 0.6f;
    }
}

ToolpathLook toolpath_look(Finish finish, float smoothness)
{
    ToolpathLook l;
    const float s = std::clamp(smoothness < 0.f || !std::isfinite(smoothness) ? default_toolpath_smoothness(finish) : smoothness, 0.f, 1.f);
    l.smoothness  = s;
    if (s <= 0.f)
        return l;   // all off: Preview's look
    // Design 2.6.3: the skin overlap at the finish's default Smoothness, scaled with the slider.
    float overlap = 0.12f;   // standard 1.12
    float bulge   = 2.f;     // rounded
    float groove  = 0.6f;
    switch (finish) {
    case Finish::Matte: overlap = 0.10f; groove = 0.8f; break;   // matte hides less
    case Finish::Silk: overlap = 0.18f; bulge = 3.f; groove = 0.45f; l.anisotropy = 0.75f * s; break;
    case Finish::Gloss: overlap = 0.15f; groove = 0.5f; break;
    case Finish::Transparent: overlap = 0.15f; groove = 0.4f; break;
    case Finish::Metallic: overlap = 0.14f; groove = 0.5f; l.anisotropy = 0.2f * s; break;
    case Finish::CarbonFiber: overlap = 0.10f; groove = 0.8f; break;
    default: break;
    }
    const float k  = s / std::max(0.05f, default_toolpath_smoothness(finish));   // 1 at the finish's default
    l.skin_overlap = 1.f + std::min(0.3f, overlap * k);
    l.skin_flatten = std::clamp(s * 1.1f, 0.f, 1.f);
    l.bulge        = 2.f + (bulge - 2.f) * s + 2.f * std::max(0.f, s - 0.5f);   // flatter beads as it smooths
    l.groove       = std::clamp(1.f - (1.f - groove) * std::min(1.f, k), 0.f, 1.f);
    l.ssao_skin    = std::clamp(1.f - 0.8f * s, 0.f, 1.f);
    return l;
}

MaterialParams material_params(Finish finish)
{
    MaterialParams p;
    switch (finish) {
    default:
    case Finish::Standard:
        p.roughness  = 0.55f;
        p.f0         = 0.04f;
        p.line_shade = 0.45f;
        break;
    case Finish::Matte:
        p.roughness  = 0.85f;
        p.f0         = 0.02f;
        p.line_shade = 0.75f;
        break;
    case Finish::Silk:
        p.roughness  = 0.35f;
        p.f0         = 0.06f;
        p.metal_tint = 0.6f;
        p.anisotropy = 0.75f;
        p.line_shade = 0.3f;
        break;
    case Finish::Gloss:
        p.roughness  = 0.30f;
        p.f0         = 0.05f;
        p.clearcoat  = 0.5f;
        p.line_shade = 0.4f;
        break;
    case Finish::Transparent:
        p.roughness  = 0.25f;
        p.f0         = 0.04f;
        p.clearcoat  = 0.3f;
        p.alpha      = 0.55f;
        p.line_shade = 0.2f;
        break;
    case Finish::Metallic:
        p.roughness  = 0.40f;
        p.f0         = 0.5f;
        p.metal_tint = 1.f;
        p.anisotropy = 0.2f;
        p.line_shade = 0.4f;
        break;
    case Finish::CarbonFiber:
        p.roughness   = 0.90f;
        p.f0          = 0.03f;
        p.fiber_noise = 0.6f;
        p.line_shade  = 0.6f;
        break;
    }
    return p;
}

std::array<float, 3> color_temperature_rgb(float kelvin)
{
    // Tanner Helland's fit of the Planckian locus (sRGB, 0..255), turned linear.
    const double t = double(std::clamp(std::isfinite(kelvin) ? kelvin : 6500.f, 1000.f, 40000.f)) / 100.;
    double r, g, b;
    if (t <= 66.) {
        r = 255.;
        g = 99.4708025861 * std::log(t) - 161.1195681661;
    } else {
        r = 329.698727446 * std::pow(t - 60., -0.1332047592);
        g = 288.1221695283 * std::pow(t - 60., -0.0755148492);
    }
    if (t >= 66.)
        b = 255.;
    else if (t <= 19.)
        b = 0.;
    else
        b = 138.5177312231 * std::log(t - 10.) - 305.0447927307;
    auto lin = [](double v) {
        const double c = std::clamp(v, 0., 255.) / 255.;
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    std::array<double, 3> c{ lin(r), lin(g), lin(b) };
    const double          m = std::max({ c[0], c[1], c[2], 1e-6 });
    return { float(c[0] / m), float(c[1] / m), float(c[2] / m) };
}

float layer_line_depth(LayerLines v)
{
    switch (v) {
    case LayerLines::Subtle: return 0.15f;
    case LayerLines::Real: return 0.35f;
    case LayerLines::Exaggerated: return 0.7f;
    default: return 0.f;
    }
}

float layer_line_fade(float layer_px) { return std::isfinite(layer_px) ? smoothstep01(1.f, 2.f, layer_px) : 1.f; }

float layer_pixels(float layer_height, float dz_per_px)
{
    if (!(layer_height > 0.f))
        return 0.f;
    return layer_height / std::max(std::abs(dz_per_px), 1e-9f);
}

LayerTable::LayerTable(const std::vector<double> &boundaries)
{
    for (size_t i = 1; i < boundaries.size(); i += 2)
        if (m_tops.empty() || float(boundaries[i]) > m_tops.back())
            m_tops.push_back(float(boundaries[i]));
    m_uniform = true;
    if (m_tops.size() > 2) {
        const float h = m_tops[1] - m_tops[0];
        for (size_t i = 2; i < m_tops.size() && m_uniform; ++i)
            m_uniform = std::abs((m_tops[i] - m_tops[i - 1]) - h) <= 0.001f;
    }
}

LayerSpan LayerTable::layer_at(float z) const
{
    LayerSpan s;
    if (m_tops.empty())
        return s;
    if (z > m_tops.back()) {
        // Past the last layer: that layer continued, so the stripes do not stop at the top.
        const float h = m_tops.size() > 1 ? m_tops.back() - m_tops[m_tops.size() - 2] : m_tops.back();
        if (h <= 0.f)
            return { m_tops.size() > 1 ? m_tops[m_tops.size() - 2] : 0.f, m_tops.back() };
        const float k = std::floor((z - m_tops.back()) / h);
        s.bottom      = m_tops.back() + k * h;
        s.top         = s.bottom + h;
        return s;
    }
    // The first top at or above z.
    const size_t i = size_t(std::lower_bound(m_tops.begin(), m_tops.end(), z) - m_tops.begin());
    s.top          = m_tops[i];
    s.bottom       = i == 0 ? 0.f : m_tops[i - 1];
    return s;
}

int LayerTable::index_at(float z) const
{
    if (m_tops.empty())
        return 0;
    const int last = int(m_tops.size()) - 1;
    if (z > m_tops.back()) {
        const float h = m_tops.size() > 1 ? m_tops.back() - m_tops[m_tops.size() - 2] : m_tops.back();
        return h > 0.f ? last + 1 + int(std::floor((z - m_tops.back()) / h)) : last;
    }
    return int(std::lower_bound(m_tops.begin(), m_tops.end(), z) - m_tops.begin());
}

int uniform_layer_index_at(float z, float first_layer_height, float layer_height)
{
    if (first_layer_height <= 0.f)
        first_layer_height = layer_height;
    if (z < first_layer_height || layer_height <= 0.f)
        return 0;
    return 1 + int(std::floor((z - first_layer_height) / layer_height));
}

LayerSpan uniform_layer_at(float z, float first_layer_height, float layer_height)
{
    if (first_layer_height <= 0.f)
        first_layer_height = layer_height;
    if (z < first_layer_height || layer_height <= 0.f)
        return { 0.f, first_layer_height };
    const float k = std::floor((z - first_layer_height) / layer_height);
    LayerSpan   s;
    s.bottom = first_layer_height + k * layer_height;
    s.top    = s.bottom + layer_height;
    return s;
}

float layer_phase(const LayerSpan &span, float z)
{
    const float h = span.height();
    return h > 0.f ? std::clamp((z - span.bottom) / h, 0.f, 1.f) : 0.f;
}

// --- skin direction (1c) -------------------------------------------------------------------------

namespace {

// [0, 180): a line and its reverse.
float skin_line_angle(double deg)
{
    if (!std::isfinite(deg))
        return 0.f;
    double a = std::fmod(deg, 180.);
    if (a < 0.)
        a += 180.;
    if (a >= 180.)
        a -= 180.;
    return float(a);
}

// A plain rotate template ("0,90", "0 45 90"): its angles; false for the metalanguage or rubbish.
bool skin_plain_template(const std::string &text, std::vector<double> &out)
{
    out.clear();
    std::string token;
    auto flush = [&]() {
        if (token.empty())
            return true;
        char        *end = nullptr;
        const double v   = std::strtod(token.c_str(), &end);
        if (end == nullptr || *end != '\0' || !std::isfinite(v))
            return false;
        out.push_back(v);
        token.clear();
        return true;
    };
    for (char c : text) {
        if (c == ',' || std::isspace((unsigned char) c)) {
            if (!flush())
                return false;
        } else if (std::isdigit((unsigned char) c) || c == '.')
            token += c;
        else
            return false;   // + - % * @ ' " c m / N n Z z $ L l U u Q q ~ ^ | # : the rotate metalanguage
    }
    return flush() && !out.empty();
}

} // namespace

float SkinTangent::angle_at(int layer) const
{
    if (pattern != SkinPattern::Lines || angles_deg.empty())
        return 0.f;
    const int n = int(angles_deg.size());
    const int i = ((layer % n) + n) % n;
    return angles_deg[size_t(i)];
}

std::array<float, 2> SkinTangent::direction_at(int layer) const
{
    if (pattern != SkinPattern::Lines || angles_deg.empty())
        return { 0.f, 0.f };
    const double a = double(angle_at(layer)) * 3.14159265358979323846 / 180.;
    return { float(std::cos(a)), float(std::sin(a)) };
}

std::array<float, 2> SkinTangent::ring_direction(float x, float y, float cx, float cy)
{
    const float dx = x - cx, dy = y - cy;
    const float l  = std::sqrt(dx * dx + dy * dy);
    if (!(l > 1e-6f))
        return { 0.f, 0.f };
    return { -dy / l, dx / l };
}

SkinTangent skin_tangent_mode(const SkinSettings &s)
{
    SkinTangent t;
    const std::string &p = s.pattern;
    if (p == "concentric" || p == "spiralinset") {
        t.pattern = SkinPattern::Concentric;
        return t;
    }
    const bool aligned = p == "alignedrectilinear";
    if (!(aligned || p == "rectilinear" || p == "monotonic" || p == "monotonicline"))
        return t;   // Isotropic
    t.zigzag = !(p == "monotonic" || p == "monotonicline");

    std::vector<double> angles;
    if (s.surface_direction >= 0.)
        angles = { s.surface_direction };   // fixed, no turn per layer
    else if (!s.rotate_template.empty()) {
        if (!skin_plain_template(s.rotate_template, angles))
            return SkinTangent();           // the metalanguage: not modelled
    } else if (aligned)
        angles = { s.solid_infill_direction };
    else
        angles = { s.solid_infill_direction, s.solid_infill_direction + 90. };

    const double offset = s.align_to_model ? s.model_rotation_deg : 0.;
    t.pattern = SkinPattern::Lines;
    for (double a : angles)
        t.angles_deg.push_back(skin_line_angle(a + offset));
    return t;
}

SkinSettings skin_settings(const DynamicPrintConfig &config, bool top, double model_rotation_deg)
{
    SkinSettings s;
    const char *pattern_key   = top ? "top_surface_pattern" : "bottom_surface_pattern";
    const char *direction_key = top ? "top_layer_direction" : "bottom_layer_direction";
    if (const ConfigOption *o = config.option(pattern_key))
        s.pattern = o->serialize();
    if (const ConfigOptionFloat *o = config.option<ConfigOptionFloat>("solid_infill_direction"))
        s.solid_infill_direction = o->value;
    if (const ConfigOptionFloat *o = config.option<ConfigOptionFloat>(direction_key))
        s.surface_direction = o->value;
    if (const ConfigOptionString *o = config.option<ConfigOptionString>("solid_infill_rotate_template"))
        s.rotate_template = o->value;
    if (const ConfigOptionBool *o = config.option<ConfigOptionBool>("align_infill_direction_to_model"))
        s.align_to_model = o->value;
    s.model_rotation_deg = model_rotation_deg;
    return s;
}

} // namespace Showcase
} // namespace Slic3r
