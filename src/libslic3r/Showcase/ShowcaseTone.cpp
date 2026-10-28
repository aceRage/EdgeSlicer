#include "ShowcaseTone.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r {
namespace Showcase {

const char *to_string(ToneCurve v)
{
    switch (v) {
    case ToneCurve::Filmic: return "filmic";
    default:
    case ToneCurve::Neutral: return "neutral";
    }
}

bool from_string(const std::string &s, ToneCurve &v)
{
    if (s == "neutral") {
        v = ToneCurve::Neutral;
        return true;
    }
    if (s == "filmic") {
        v = ToneCurve::Filmic;
        return true;
    }
    return false;
}

namespace {

constexpr float NEUTRAL_START = 0.8f - 0.04f;
constexpr float NEUTRAL_DESAT = 0.15f;

float min3(const Rgb &c) { return std::min(c[0], std::min(c[1], c[2])); }
float max3(const Rgb &c) { return std::max(c[0], std::max(c[1], c[2])); }

} // namespace

// Mirrors tone_neutral() in showcase_post.fs (and the copies in showcase_material.fs and
// showcase_background.fs).
Rgb tone_neutral(Rgb c)
{
    const float x      = min3(c);
    const float offset = x < 0.08f ? x - 6.25f * x * x : 0.04f;
    for (float &v : c)
        v -= offset;
    const float peak = max3(c);
    if (peak < NEUTRAL_START)
        return c;
    const float d        = 1.f - NEUTRAL_START;
    const float new_peak = 1.f - d * d / (peak + d - NEUTRAL_START);
    const float g        = 1.f - 1.f / (NEUTRAL_DESAT * (peak - new_peak) + 1.f);
    for (float &v : c)
        v = v * (new_peak / peak) * (1.f - g) + new_peak * g;
    return c;
}

// Mirrors tone_filmic() in showcase_post.fs. The matrices are column-major, as GLSL's mat3().
Rgb tone_filmic(Rgb c)
{
    static const float inset[3][3]  = { { 0.842479062253094f, 0.0423282422610123f, 0.0423756549057051f },
                                        { 0.0784335999999992f, 0.878468636469772f, 0.0784336f },
                                        { 0.0792237451477643f, 0.0791661274605434f, 0.879142973793104f } };
    static const float outset[3][3] = { { 1.19687900512017f, -0.0528968517574562f, -0.0529716355144438f },
                                        { -0.0980208811401368f, 1.15190312990417f, -0.0980434501171241f },
                                        { -0.0990297440797205f, -0.0989611768448433f, 1.15107367264116f } };
    const float min_ev = -12.47393f, max_ev = 4.026069f;
    auto mul = [](const float (&m)[3][3], const Rgb &v) {
        Rgb r{ 0.f, 0.f, 0.f };
        for (int col = 0; col < 3; ++col)
            for (int row = 0; row < 3; ++row)
                r[row] += m[col][row] * v[col];
        return r;
    };
    for (float &v : c)
        v = std::max(v, 0.f);
    c = mul(inset, c);
    for (float &v : c) {
        const float e = std::clamp(std::log2(std::max(v, 1e-10f)), min_ev, max_ev);
        const float x = (e - min_ev) / (max_ev - min_ev);
        const float x2 = x * x, x4 = x2 * x2;
        v = 15.5f * x4 * x2 - 40.14f * x4 * x + 31.96f * x4 - 6.868f * x2 * x + 0.4298f * x2 + 0.1191f * x - 0.00232f;
    }
    c = mul(outset, c);
    for (float &v : c)
        v = std::pow(std::clamp(v, 0.f, 1.f), 2.2f);
    return c;
}

Rgb tone_map(ToneCurve curve, const Rgb &c) { return curve == ToneCurve::Filmic ? tone_filmic(c) : tone_neutral(c); }

float srgb_to_linear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

float linear_to_srgb(float c)
{
    c = std::clamp(c, 0.f, 1.f);
    return c < 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;
}

float round_to_half(float v)
{
    if (!std::isfinite(v) || v == 0.f)
        return v;
    const float a = std::fabs(v);
    if (a >= 65520.f)
        return std::copysign(std::numeric_limits<float>::infinity(), v);
    int e = 0;
    std::frexp(a, &e);   // a = m 2^e, m in [0.5, 1)
    // 11 significant bits for normals; below 2^-14 the step stays 2^-24 (subnormals).
    const float step = std::ldexp(1.f, std::max(e, -13) - 11);
    return std::copysign(std::nearbyint(a / step) * step, v);
}

} // namespace Showcase
} // namespace Slic3r
