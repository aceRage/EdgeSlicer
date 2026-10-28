#version 140

// Showcase realism 1a (tests/design_showcase_realism.md 2.1): the post pass. Drawn over
// m_background's full-screen quad with background.vs, one texel per pixel (the viewport is the
// source's size).
//
// mode 0: the lit objects' HDR layer (RGBA16F: linear light, premultiplied by coverage) gets the
//         exposure, the tone curve and the sRGB encoding, and is laid over the display layer
//         (backdrop, plate, floor) with the same blending the objects used before (rgb: SRC_ALPHA,
//         ONE_MINUS_SRC_ALPHA; alpha: ONE, ONE_MINUS_SRC_ALPHA). Pixels with no object are left alone.
// mode 1: a plain copy (the live view's finished frame onto the window).
//
// Hooks for 1g (vignette, grain, chromatic aberration) go after the tone curve; depth of field and
// bloom run before this pass, on the linear layer. libslic3r/Showcase/ShowcaseTone.cpp mirrors the
// curves for the unit tests.

uniform sampler2D source;
uniform int   mode;
uniform int   tone_curve;   // 0 Khronos PBR Neutral, 1 filmic (ToneCurve)
uniform float exposure;     // linear multiplier (2^EV)

in vec2 tex_coord;
out vec4 out_color;

// Khronos PBR Neutral: a filament's colour under a white light stays that colour.
vec3 tone_neutral(vec3 color)
{
    const float start_compression = 0.8 - 0.04;
    const float desaturation = 0.15;
    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    float peak = max(color.r, max(color.g, color.b));
    if (peak < start_compression)
        return color;
    const float d = 1.0 - start_compression;
    float new_peak = 1.0 - d * d / (peak + d - start_compression);
    color *= new_peak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - new_peak) + 1.0);
    return mix(color, vec3(new_peak), g);
}

// AgX-style filmic: log2 over 16.5 stops, a sigmoid, and the inset / outset matrices that roll the
// brightest highlights off towards white.
vec3 tone_filmic(vec3 c)
{
    const mat3 inset = mat3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                            0.0784335999999992, 0.878468636469772, 0.0784336,
                            0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const mat3 outset = mat3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                             -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                             -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float min_ev = -12.47393;
    const float max_ev = 4.026069;
    c = inset * max(c, vec3(0.0));
    c = clamp(log2(max(c, vec3(1e-10))), min_ev, max_ev);
    c = (c - min_ev) / (max_ev - min_ev);
    vec3 x2 = c * c;
    vec3 x4 = x2 * x2;
    c = 15.5 * x4 * x2 - 40.14 * x4 * c + 31.96 * x4 - 6.868 * x2 * c + 0.4298 * x2 + 0.1191 * c - 0.00232;
    c = outset * c;
    return pow(clamp(c, 0.0, 1.0), vec3(2.2));
}

vec3 linear_to_srgb(vec3 c)
{
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

void main()
{
    vec4 s = texelFetch(source, ivec2(gl_FragCoord.xy), 0);
    if (mode == 1) {
        out_color = s;
        return;
    }
    if (s.a <= 0.0)
        discard;
    float a = min(s.a, 1.0);
    vec3 c = max(s.rgb / a, vec3(0.0)) * exposure;
    c = tone_curve == 1 ? tone_filmic(c) : tone_neutral(c);
    // 1g: vignette, grain and chromatic aberration go here (all off).
    out_color = vec4(linear_to_srgb(c), a);
}
