#version 140

// Showcase tab: the backdrop behind the model (solid colour, vertical or radial gradient, an image,
// the transparency checkerboard of the live view, or the bundled environment). Drawn over
// m_background's full-screen quad with background.vs. Everything is laid out relative to the safe
// frame, so the live view shows the backdrop the export will have.

uniform int  mode;          // 0 solid, 1 vertical gradient, 2 radial gradient, 3 image, 4 checkerboard, 5 environment
uniform vec4 color1;        // solid; gradient top / centre; image letterbox
uniform vec4 color2;        // gradient bottom / edge
uniform vec4 frame_rect;    // the safe frame in [0,1] viewport coordinates: x0, y0, x1, y1
uniform float frame_aspect; // frame width / height
uniform sampler2D image_tex;
uniform vec4 uv_transform;  // image: uv = f * xy + zw, f = position in the frame (0..1)
uniform bool tile;
uniform vec2 viewport_size; // pixels (checkerboard)

// S2: a soft focus look. 0 = sharp; the image is mipmapped and image_lod is the level to read.
uniform float blur;
uniform float image_lod;

// S2: the environment behind the model, seen through the camera (unit 6).
uniform samplerCube env_map;
uniform mat4  inv_view_projection;
uniform vec3  camera_position;
uniform float env_max_lod;
uniform float env_rotation;
uniform float env_exposure;
// 1a: the post pass's tone curve, so the environment behind the model matches the model.
uniform int   tone_curve;            // 0 Khronos PBR Neutral, 1 filmic (ToneCurve)

in vec2 tex_coord;
out vec4 out_color;

vec3 to_cube(vec3 w) { return vec3(w.x, w.z, -w.y); }

vec3 env_rotate(vec3 w)
{
    float c = cos(-env_rotation), s = sin(-env_rotation);
    return vec3(c * w.x - s * w.y, s * w.x + c * w.y, w.z);
}

vec3 tone_map(vec3 color)
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

// AgX-style filmic (showcase_post.fs, ShowcaseTone.cpp): the optional curve (ToneCurve::Filmic).
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

vec3 tone_curve_map(vec3 c) { return tone_curve == 1 ? tone_filmic(c) : tone_map(c); }

vec3 linear_to_srgb(vec3 c)
{
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

vec4 sample_image(vec2 uv)
{
    if (blur <= 0.0)
        return textureLod(image_tex, uv, 0.0);   // sharp: the full picture, as S1 drew it
    // A small disc of taps over the chosen mip level hides the mip's blockiness.
    vec2 texel = vec2(exp2(image_lod)) / vec2(textureSize(image_tex, 0));
    vec4 sum = vec4(0.0);
    const int TAPS = 12;
    for (int i = 0; i < TAPS; ++i) {
        float a = 6.2831853 * float(i) / float(TAPS);
        sum += textureLod(image_tex, uv + vec2(cos(a), sin(a)) * texel * 1.5, image_lod);
    }
    return mix(textureLod(image_tex, uv, image_lod), sum / float(TAPS), 0.65);
}

void main()
{
    vec2 f = (tex_coord - frame_rect.xy) / max(frame_rect.zw - frame_rect.xy, vec2(1e-6));
    vec4 c = color1;
    if (mode == 1) {
        c = mix(color2, color1, clamp(f.y, 0.0, 1.0));
    } else if (mode == 2) {
        vec2 d = (f - 0.5) * vec2(frame_aspect, 1.0);
        float r = length(d) / max(length(vec2(frame_aspect, 1.0) * 0.5), 1e-6);
        c = mix(color1, color2, smoothstep(0.0, 1.0, clamp(r, 0.0, 1.0)));
    } else if (mode == 3) {
        vec2 uv = f * uv_transform.xy + uv_transform.zw;
        if (tile)
            uv = fract(uv);
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
            c = color1;
        else
            c = sample_image(vec2(uv.x, 1.0 - uv.y));
    } else if (mode == 4) {
        vec2 cell = floor(tex_coord * viewport_size / 12.0);
        float k = mod(cell.x + cell.y, 2.0);
        c = vec4(mix(vec3(0.80), vec3(0.92), k), 1.0);
    } else if (mode == 5) {
        vec4 p = inv_view_projection * vec4(tex_coord * 2.0 - 1.0, 1.0, 1.0);
        vec3 dir = normalize(p.xyz / p.w - camera_position);
        vec4 t = textureLod(env_map, to_cube(env_rotate(dir)), clamp(blur, 0.0, 1.0) * env_max_lod);
        c = vec4(linear_to_srgb(tone_curve_map(t.rgb * t.a * 16.0 * env_exposure)), 1.0);
    }
    out_color = vec4(c.rgb, 1.0);
}
