#version 140

// Showcase realism 1g (tests/design_showcase_realism.md 2.7): the camera effects over the finished
// frame (after the post pass, SSAO and the depth of field). Drawn over m_background's full-screen quad
// with background.vs, one texel per pixel. The GL-free twins are in libslic3r/Showcase/ShowcaseLens.cpp
// (frame_radius, vignette_factor, distort_frame_point, chromatic_offset_px, grain_noise, grain_weight,
// bloom_bright); each effect is skipped at 0.
//
// mode 0: the frame (a copy, color_tex, linearly filtered) through the lens: barrel distortion and
//         colour fringes when sampling it, the bloom added in linear light, the vignette, then the
//         film grain at the output's pixels.
// mode 1: the bloom's bright pass: the HDR objects layer (linear, premultiplied, at full size) times the
//         exposure, averaged over the 4 x 4 pixels of this quarter-size texel, less the threshold.

uniform int       mode;
uniform sampler2D color_tex;
uniform sampler2D bloom_tex;     // quarter size, blurred, linear
uniform sampler2D hdr_tex;       // mode 1: the resolved HDR layer

uniform vec2  viewport_size;     // the target's size in pixels
uniform vec4  frame_px;          // the safe frame: x, y, w, h in pixels of the target
uniform float out_scale;         // target pixels per output pixel (the export's supersampling)
uniform float exposure;          // mode 1
uniform float grain;
uniform float vignette;
uniform float bloom;
uniform float chromatic;
uniform float distortion;
uniform int   seed;              // the video frame (0 for a still)

in vec2 tex_coord;
out vec4 out_color;

vec3 srgb_to_linear(vec3 c)
{
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

vec3 linear_to_srgb(vec3 c)
{
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

float grain_hash(uvec2 p, uint s)
{
    uint h = p.x * 1973u + p.y * 9277u + s * 26699u;
    h = (h ^ 61u) ^ (h >> 16u);
    h *= 9u;
    h ^= h >> 4u;
    h *= 0x27d4eb2du;
    h ^= h >> 15u;
    return float(h & 0xFFFFFFu) / 16777216.0;
}

void main()
{
    if (mode == 1) {
        ivec2 base = ivec2(gl_FragCoord.xy) * 4;
        ivec2 last = textureSize(hdr_tex, 0) - 1;
        vec3  sum  = vec3(0.0);
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                vec4 h = texelFetch(hdr_tex, min(base + ivec2(x, y), last), 0);
                sum += max(h.rgb * exposure - vec3(1.0), vec3(0.0));   // Showcase::bloom_bright
            }
        out_color = vec4(min(sum / 16.0, vec3(64.0)), 1.0);
        return;
    }

    vec2  f      = (gl_FragCoord.xy - frame_px.xy) / max(frame_px.zw, vec2(1.0));   // 0..1 across the frame
    float aspect = frame_px.z / max(frame_px.w, 1.0);
    vec2  p      = (f - 0.5) * vec2(aspect, 1.0);
    float r      = length(p) / (0.5 * sqrt(aspect * aspect + 1.0));                // 0 centre .. 1 corners

    // Barrel distortion: where this output point takes its picture from (corners stay put).
    vec2 src_f = f;
    if (distortion > 0.0) {
        float k = 0.15 * distortion;
        src_f = 0.5 + (f - 0.5) * (1.0 + k * r * r) / (1.0 + k);
    }
    vec2 uv = (frame_px.xy + src_f * frame_px.zw) / viewport_size;
    vec4 c  = texture(color_tex, uv);
    if (chromatic > 0.0 && r > 0.0) {
        // Red outwards, blue inwards, up to 1.5 output pixels in the corners.
        vec2  dir = normalize(f - 0.5);
        float off = 1.5 * chromatic * r * r * out_scale;
        c.r = texture(color_tex, uv + dir * off / viewport_size).r;
        c.b = texture(color_tex, uv - dir * off / viewport_size).b;
    }
    vec3 lin = srgb_to_linear(c.rgb);
    if (bloom > 0.0)
        lin += texture(bloom_tex, uv).rgb * bloom * 0.3;
    if (vignette > 0.0) {
        float cv = cos(atan(r * 0.6));
        lin *= 1.0 + vignette * (cv * cv * cv * cv - 1.0);
    }
    vec3 o = linear_to_srgb(lin);
    if (grain > 0.0) {
        // At the output's pixels: a supersampled export averages each block back to one value.
        uvec2 op   = uvec2(max(floor((gl_FragCoord.xy - frame_px.xy) / max(out_scale, 1.0)) + 4096.0, vec2(0.0)));
        float n    = grain_hash(op, uint(max(seed, 0))) - 0.5;
        float luma = dot(o, vec3(0.2126, 0.7152, 0.0722));
        o = clamp(o + vec3(n * grain * 0.09 * (0.25 + 3.0 * luma * (1.0 - luma))), 0.0, 1.0);
    }
    out_color = vec4(o, c.a);
}
