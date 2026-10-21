#include "ShowcaseMaterial.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
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

} // namespace Showcase
} // namespace Slic3r
