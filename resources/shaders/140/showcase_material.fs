#version 140

// Showcase tab (S2): the filament materials of a presentation render (design 2.5).
//
// - GGX specular with a Lambert diffuse, per filament finish (libslic3r/Showcase/ShowcaseMaterial):
//   roughness, F0, a colour-tinted highlight (silk, metallic), an anisotropic lobe streaked across
//   the layer lines (silk), a clear coat (gloss) and translucency (transparent, D15).
// - The key, fill and rim lights, fixed in the world (D14); the key casts the shadow map.
// - Image-based light from a bundled, pre-filtered environment (D3): SH irradiance for the diffuse
//   part, the GGX-prefiltered cube map (one roughness per mip) with the analytic split-sum
//   approximation for the specular part.
// - Procedural layer lines on the mesh at the object's real layer heights (design 2.6.2), faded
//   out below ~1.5 px a layer so far shots do not moire (layer_fade, as layer_line_fade() in C++).
// - 1c: the silk sheen follows the lines. Walls keep the horizontal tangent (the perimeters); flat
//   top and bottom faces take the skin's line direction from the print settings (skin_mode_*,
//   Showcase::skin_tangent_mode()), turned layer by layer as the slicer turns it, or the rings of a
//   concentric skin; patterns without one direction get a round highlight.
// - Linear lighting. 1a (tests/design_showcase_realism.md 2.1): with hdr_output the shader writes
//   linear light into the objects' RGBA16F layer and the post pass (showcase_post.fs) applies the
//   exposure, the tone curve and the sRGB encoding. Without it (the floor's mirror pass, or no float
//   buffers) it does those itself: exposure, the Khronos PBR Neutral tone map (keeps filament
//   colours) or the filmic curve, sRGB out.
// - 1b: the block between the SHOWCASE_MATERIAL_COMMON markers is also the toolpaths' lighting.
//   ShowcaseRenderer cuts it out of this file (Showcase::extract_glsl_block) and hands it, with
//   showcase_toolpath.glsl, to libvgcode as the prelude of its smooth-path fragment shader. Keep it
//   free of names libvgcode's shader declares (camera_position, exposure, shadow_map, world_position,
//   UP, LUMA: Showcase::toolpath_prelude_problems() lists them) and of the varyings of this file.
// - Multi-colour P4: the blocks under #ifdef SHOWCASE_MULTICOLOUR colour a coextruded filament's
//   surfaces without a slice. They exist only in the "showcase_material_mc" program (see
//   mc_mesh_albedo()); with no coextruded slot the plain program draws, unchanged.
//
// Optional: when it does not compile the Showcase canvas draws its objects with phong as in S1.

uniform vec4 uniform_color;          // the filament colour, sRGB (GLModel sets it)

// Material (MaterialParams).
uniform float mat_roughness;
uniform float mat_f0;
uniform float mat_metal_tint;
uniform float mat_anisotropy;
uniform float mat_clearcoat;
uniform float mat_alpha;
uniform float mat_line_shade;
uniform float mat_fiber_noise;

uniform vec3  camera_position;       // world
uniform float exposure;              // linear multiplier (2^EV); not used with hdr_output
// 1a: the opaque pass writes full coverage into the HDR layer (blending is off there).
uniform bool  force_opaque;

// The key light's shadow map (unit 5, as phong).
uniform sampler2D shadow_map;
uniform mat4  shadow_light_vp;
uniform float shadow_intensity;      // 0 = no shadow map
uniform float shadow_map_texel;
uniform float shadow_softness;       // PCF footprint scale

// Layer lines. layer_mode 0 off, 1 uniform (first + steps), 2 table (unit 7, R32F tops).
uniform int       layer_mode;
uniform float     line_depth;
uniform float     first_layer_height;
uniform float     layer_height;
uniform sampler2D layer_table;
uniform int       layer_count;
uniform int       layer_table_width;

// 1c: the top / bottom skin's lines (SkinTangent). Mode 0 isotropic, 1 lines, 2 concentric. Lines:
// layer i runs at skin_angles[base + i % count] radians (base 0 top, 8 bottom).
uniform int   skin_mode_top;
uniform int   skin_mode_bottom;
uniform int   skin_count_top;
uniform int   skin_count_bottom;
uniform float skin_angles[16];
uniform vec2  skin_center;           // concentric: the rings' centre (world XY)

// Nothing under the floor (and the mirrored pass keeps to its side of it).
uniform bool  clip_floor;
// S5, the layer build-up's fallback without a slice: nothing above build_clip_z (very large = off),
// and the inside seen through the cut is lit as a flat cap facing up, so the print reads solid.
uniform float build_clip_z;

in vec4 world_pos;
in vec3 world_normal;

out vec4 out_color;

// ---- BEGIN SHOWCASE_MATERIAL_COMMON ----
// Shared with the toolpaths (1b). Uniforms and functions only; nothing from outside the block.

const float PI = 3.14159265;

// Lights: direction TO the light (world) and linear colour x intensity. Light 0 is the key.
uniform int   light_count;
uniform vec3  light_dir[3];
uniform vec3  light_color[3];
// 1a: linear light out for the post pass; else tone-mapped sRGB as before.
uniform bool  hdr_output;
uniform int   tone_curve;            // 0 Khronos PBR Neutral, 1 filmic (ToneCurve)

// Environment (unit 6; the toolpaths' own unit, see ShowcaseRenderer).
uniform bool        use_env;
uniform samplerCube env_map;
uniform float       env_max_lod;
uniform float       env_intensity;
uniform float       env_rotation;    // radians around +Z
uniform vec3        env_sh[9];

vec3 srgb_to_linear(vec3 c)
{
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

vec3 linear_to_srgb(vec3 c)
{
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

// Khronos PBR Neutral: a filament's colour under a white light stays that colour.
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

// --- environment ---------------------------------------------------------------------------------

vec3 env_rotate(vec3 w)
{
    float c = cos(-env_rotation), s = sin(-env_rotation);
    return vec3(c * w.x - s * w.y, s * w.x + c * w.y, w.z);
}

// World (Z up) to GL cube space (Y up), as the baked maps are laid out.
vec3 to_cube(vec3 w) { return vec3(w.x, w.z, -w.y); }

vec3 env_diffuse(vec3 n)
{
    n = env_rotate(n);
    vec3 r = env_sh[0] * 0.282095
           + env_sh[1] * (0.488603 * n.y) + env_sh[2] * (0.488603 * n.z) + env_sh[3] * (0.488603 * n.x)
           + env_sh[4] * (1.092548 * n.x * n.y) + env_sh[5] * (1.092548 * n.y * n.z)
           + env_sh[6] * (0.315392 * (3.0 * n.z * n.z - 1.0)) + env_sh[7] * (1.092548 * n.x * n.z)
           + env_sh[8] * (0.546274 * (n.x * n.x - n.y * n.y));
    return max(r, vec3(0.0));
}

vec3 decode_rgbm(vec4 t) { return t.rgb * t.a * 16.0; }

vec3 env_specular(vec3 r, float roughness)
{
    return decode_rgbm(textureLod(env_map, to_cube(env_rotate(r)), roughness * env_max_lod));
}

// Without an environment: a soft studio sky over a darker ground.
vec3 plain_diffuse(vec3 n) { return mix(vec3(0.16), vec3(0.42), n.z * 0.5 + 0.5); }
vec3 plain_specular(vec3 r, float roughness)
{
    float sky = smoothstep(-0.2, 0.6, r.z);
    return mix(vec3(0.1), vec3(0.9), sky) * mix(1.0, 0.5, roughness);
}

// Karis' analytic fit of the split-sum BRDF integral (no LUT texture needed).
vec3 env_brdf(vec3 f0, float roughness, float n_dot_v)
{
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * n_dot_v)) * r.x + r.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

// --- BRDF ---------------------------------------------------------------------------------------------

vec3 fresnel(vec3 f0, float cos_theta) { return f0 + (1.0 - f0) * pow(1.0 - clamp(cos_theta, 0.0, 1.0), 5.0); }

float ggx(float n_dot_h, float a)
{
    float a2 = a * a;
    float d = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

// Anisotropic GGX: at along the tangent (the layer direction), ab across it.
float ggx_aniso(vec3 h, vec3 n, vec3 t, vec3 b, float at, float ab)
{
    float th = dot(t, h), bh = dot(b, h), nh = dot(n, h);
    float d = th * th / (at * at) + bh * bh / (ab * ab) + nh * nh;
    return 1.0 / (PI * at * ab * d * d);
}

float smith_visibility(float n_dot_v, float n_dot_l, float a)
{
    float k = a * 0.5;
    float gv = n_dot_v / (n_dot_v * (1.0 - k) + k);
    float gl = n_dot_l / (n_dot_l * (1.0 - k) + k);
    return gv * gl / max(4.0 * n_dot_v * n_dot_l, 1e-4);
}

float hash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// --- the material ------------------------------------------------------------------------------------

// One surface point of a finish (MaterialParams), lit by the three lights and the environment.
// n, v: unit normal and direction to the eye; t: the anisotropic lobe's tangent (along the lines;
// any unit vector across n when anisotropy is 0); key_shade: the key light's shadow (1 = lit).
// albedo is linear; roughness already carries the fibre speckle.
void shade_material(vec3 n, vec3 v, vec3 t, vec3 albedo, float roughness, float f0_value, float metal_tint,
                    float anisotropy, float clearcoat, float key_shade, out vec3 diffuse, out vec3 specular)
{
    float n_dot_v = max(dot(n, v), 1e-4);
    vec3  f0 = mix(vec3(f0_value), albedo, metal_tint);
    float kd = 1.0 - 0.7 * metal_tint;
    float a = roughness * roughness;

    // Tangent frame for the anisotropic lobe: t along the lines, b across them.
    vec3 b = cross(n, t);
    float at = max(a * (1.0 - 0.7 * anisotropy), 0.002);
    float ab = min(a * (1.0 + 2.0 * anisotropy), 1.0);

    diffuse = vec3(0.0);
    specular = vec3(0.0);
    for (int i = 0; i < 3; ++i) {
        if (i >= light_count)
            break;
        vec3 l = normalize(light_dir[i]);
        float n_dot_l = dot(n, l);
        if (n_dot_l <= 0.0)
            continue;
        float shade = i == 0 ? key_shade : 1.0;
        vec3 radiance = light_color[i] * shade;
        vec3 h = normalize(l + v);
        float d = anisotropy > 0.0 ? ggx_aniso(h, n, t, b, at, ab) : ggx(max(dot(n, h), 0.0), a);
        vec3 f = fresnel(f0, dot(h, v));
        specular += d * smith_visibility(n_dot_v, n_dot_l, a) * f * n_dot_l * radiance;
        // 1c: without an environment the sliding band comes from the lights alone: a Kajiya-Kay
        // streak with the line as the fibre (S5's toolpath silk).
        if (anisotropy > 0.0 && !use_env) {
            float th = dot(t, h);
            specular += anisotropy * 0.35 * pow(sqrt(max(1.0 - th * th, 0.0)), 80.0) * n_dot_l * f * radiance;
        }
        // Clear coat: a sharp second lobe (gloss, transparent).
        if (clearcoat > 0.0) {
            float ac = 0.08 * 0.08;
            specular += clearcoat * ggx(max(dot(n, h), 0.0), ac) * smith_visibility(n_dot_v, n_dot_l, ac) *
                        fresnel(vec3(0.04), dot(h, v)) * n_dot_l * radiance;
        }
        diffuse += albedo * kd * n_dot_l * radiance;
    }

    // Image-based light. 1c: an anisotropic finish reflects the environment off a normal bent
    // towards the plane across the lines, sampled at a roughness raised across them: the usual
    // approximation (Filament's bent normal; there is no anisotropic prefiltered map). The
    // reflection then slides along the lines as the view moves, the silk band.
    vec3 r = reflect(-v, n);
    vec3 r_aniso = r;
    if (anisotropy > 0.0) {
        vec3 across = cross(t, v);
        vec3 bent = cross(across, t);
        r_aniso = reflect(-v, normalize(mix(n, bent, clamp(anisotropy, 0.0, 1.0))));
    }
    float env_rough = clamp(roughness * (1.0 + 0.6 * anisotropy), 0.0, 1.0);
    vec3 ambient = use_env ? env_diffuse(n) * env_intensity : plain_diffuse(n);
    vec3 reflected = use_env ? env_specular(r_aniso, env_rough) * env_intensity : plain_specular(r_aniso, env_rough);
    diffuse += albedo * kd * ambient;
    specular += reflected * env_brdf(f0, env_rough, n_dot_v);
    if (clearcoat > 0.0) {
        vec3 coat = use_env ? env_specular(r, 0.08) * env_intensity : plain_specular(r, 0.08);
        specular += clearcoat * coat * env_brdf(vec3(0.04), 0.08, n_dot_v);
    }
}

// What the shader writes: linear light for the post pass (hdr_output), else exposure x the tone
// curve x sRGB. exposure_mul is 2^EV; opaque writes full coverage into the HDR layer.
vec4 material_output(vec3 lit, float alpha, float exposure_mul, bool opaque)
{
    if (hdr_output) {
        // Linear light; the post pass applies the exposure, the curve and the encoding. Kept inside
        // the half float range (and NaN-free) so one bad pixel cannot spread through later blurs.
        vec3 radiance = min(max(lit * 0.8, vec3(0.0)), vec3(60000.0));
        return vec4(radiance, opaque ? 1.0 : alpha);
    }
    vec3 color = lit * exposure_mul * 0.8;
    return vec4(linear_to_srgb(tone_curve_map(color)), alpha);
}
// ---- END SHOWCASE_MATERIAL_COMMON ----

// --- shadows ---------------------------------------------------------------------------------------

float key_shadow(vec3 n)
{
    if (shadow_intensity <= 0.0)
        return 1.0;
    vec4 lp = shadow_light_vp * world_pos;
    vec3 proj = lp.xyz / lp.w * 0.5 + 0.5;
    if (proj.z > 1.0 || proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 || proj.y > 1.0)
        return 1.0;
    float n_dot_l = dot(n, normalize(light_dir[0]));
    float bias = mix(0.0004, 0.004, clamp(1.0 - n_dot_l, 0.0, 1.0));
    float step_size = shadow_map_texel * max(shadow_softness, 0.25);
    float sum = 0.0;
    for (int x = -2; x <= 2; ++x)
        for (int y = -2; y <= 2; ++y) {
            float closest = texture(shadow_map, proj.xy + vec2(float(x), float(y)) * step_size).r;
            sum += (proj.z - bias > closest) ? 1.0 : 0.0;
        }
    return 1.0 - shadow_intensity * (sum / 25.0);
}

// --- layer lines ----------------------------------------------------------------------------------------

float layer_top(int i)
{
    return texelFetch(layer_table, ivec2(i % layer_table_width, i / layer_table_width), 0).r;
}

// The layer containing z: [bottom, top] (LayerTable::layer_at / uniform_layer_at in C++).
vec2 layer_at(float z)
{
    if (layer_mode == 2 && layer_count > 0 && layer_table_width > 0) {
        int hi = layer_count - 1;
        float last = layer_top(hi);
        if (z > last) {
            float prev = hi > 0 ? layer_top(hi - 1) : 0.0;
            float h = max(last - prev, 1e-4);
            float k = floor((z - last) / h);
            return vec2(last + k * h, last + (k + 1.0) * h);
        }
        int lo = 0;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (layer_top(mid) < z)
                lo = mid + 1;
            else
                hi = mid;
        }
        return vec2(lo > 0 ? layer_top(lo - 1) : 0.0, layer_top(lo));
    }
    float first = first_layer_height > 0.0 ? first_layer_height : layer_height;
    if (z < first || layer_height <= 0.0)
        return vec2(0.0, first);
    float k = floor((z - first) / layer_height);
    return vec2(first + k * layer_height, first + (k + 1.0) * layer_height);
}

// 1c: the index of the layer containing z, 0 = the first (LayerTable::index_at /
// uniform_layer_index_at in C++); 0 without layer information.
int layer_index_at(float z)
{
    if (layer_mode == 2 && layer_count > 0 && layer_table_width > 0) {
        int hi = layer_count - 1;
        float last = layer_top(hi);
        if (z > last) {
            float prev = hi > 0 ? layer_top(hi - 1) : 0.0;
            float h = max(last - prev, 1e-4);
            return hi + 1 + int(floor((z - last) / h));
        }
        int lo = 0;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (layer_top(mid) < z)
                lo = mid + 1;
            else
                hi = mid;
        }
        return lo;
    }
    if (layer_mode != 1)
        return 0;
    float first = first_layer_height > 0.0 ? first_layer_height : layer_height;
    if (z < first || layer_height <= 0.0)
        return 0;
    return 1 + int(floor((z - first) / layer_height));
}

// 1c: the silk lobe's tangent on a flat top (up) or bottom face: the skin's lines there. False for
// a skin without one direction (the highlight is then round).
bool skin_tangent(vec3 n, out vec3 t)
{
    bool  top  = n.z > 0.0;
    int   mode = top ? skin_mode_top : skin_mode_bottom;
    t = vec3(1.0, 0.0, 0.0);
    if (mode == 1) {
        int count = top ? skin_count_top : skin_count_bottom;
        // The layer that made the face: just below a top face, just above a bottom one.
        int i = layer_index_at(world_pos.z + (top ? -0.001 : 0.001));
        int k = count > 0 ? i - (i / count) * count : 0;
        float a = skin_angles[(top ? 0 : 8) + k];
        t = vec3(cos(a), sin(a), 0.0);
    } else if (mode == 2) {
        vec2 r = world_pos.xy - skin_center;
        if (length(r) > 1e-4)
            t = vec3(-r.y, r.x, 0.0) / length(r);
    } else
        return false;
    t = t - n * dot(t, n);
    t = length(t) > 1e-4 ? normalize(t) : normalize(cross(vec3(0.0, 1.0, 0.0), n));
    return true;
}

// 0 at 1 px a layer and below, 1 at 2 px and above (layer_line_fade() in C++).
float layer_fade(float layer_px) { return smoothstep(1.0, 2.0, layer_px); }

#ifdef SHOWCASE_MULTICOLOUR
// Multi-colour (coextruded) filaments, P4 (tests/design_multicolour_filament_view.md 7.3): the mesh's
// stand-in for the toolpaths' colours, without a slice. Only in the "showcase_material_mc" program,
// which ShowcaseRenderer builds with MultiColour::glsl_mesh_prelude() after the #version line (the
// uniforms mc_colors... and the shared functions mc_*); this file alone is the plain material.
// World space, so turning an object on the plate turns its colours, as on a real printer.
uniform int   mc_slot;               // the filament slot drawn (0-based); -1 = its plain colour
uniform float mc_wall_dir;           // +1 walls anticlockwise seen from above (Orca's default), -1 clockwise
uniform float mc_spacing_top;        // mm between neighbouring top-skin lines
uniform float mc_spacing_bottom;     // the same under overhangs and bridges (solid infill)
uniform float mc_spacing_first;      // the same on the first layer, the face on the bed
uniform bool  mc_zigzag_top;         // neighbouring skin lines run opposite ways (rectilinear)
uniform bool  mc_zigzag_bottom;
uniform float mc_volume_per_mm;      // twist: mm3 of filament per mm of height, from the bed (a guess)
uniform float mc_layer_height;       // mm, when there is no layer table (mc_skin_weight())
uniform float mc_wall_band;          // mm across the walls (outer + inner), mc_skin_weight()

// cap: the inside seen through the build-up's cut, lit as a flat face (no skin lines there).
// n is the interpolated (smooth) normal: ShowcaseRenderer draws coextruded volumes with per-corner
// normals, so the colour follows the surface instead of jumping triangle by triangle.
vec3 mc_mesh_albedo(vec3 n, bool cap)
{
    // Derivatives first, while the whole quad still runs the same code.
    float z     = world_pos.z;
    float z_px  = max(fwidth(z), 1e-6);
    vec2  xy_px = max(fwidth(world_pos.xy), vec2(1e-6));

    // The layer here, and the height within it (s.y) with the layer lines. The colour bands s.y makes
    // in the transition zones alias sooner than the groove shading: they fade out below 8 px a layer.
    float h  = mc_layer_height;
    float sy = 0.0;
    if (layer_mode != 0) {
        vec2 layer = layer_at(z);
        h = max(layer.y - layer.x, 1e-4);
        if (line_depth > 0.0)
            sy = (2.0 * clamp((z - layer.x) / h, 0.0, 1.0) - 1.0) * smoothstep(3.0, 8.0, h / z_px);
    }
    // Walls or skin: on the slicer's terms, a slope is stacked walls until each layer steps in by
    // more than the band of walls (mc_skin_weight() in MultiColourGlsl.hpp).
    float w = cap ? 1.0 : mc_skin_weight(n.z, h, mc_wall_band);

    // The skin's lines on a top or bottom face (skin_tangent() above, without the sheen's projection).
    bool  top     = n.z > 0.0;
    int   mode    = top ? skin_mode_top : skin_mode_bottom;
    float spacing = top ? mc_spacing_top : (z < 0.05 ? mc_spacing_first : mc_spacing_bottom);
    bool  zigzag  = top ? mc_zigzag_top : mc_zigzag_bottom;
    vec2  p       = world_pos.xy;
    vec2  dir     = vec2(0.0);
    if (mode == 1) {
        int count = top ? skin_count_top : skin_count_bottom;
        int i = layer_index_at(z + (top ? -0.001 : 0.001));
        int k = count > 0 ? i - (i / count) * count : 0;
        float a = skin_angles[(top ? 0 : 8) + k];
        dir = vec2(cos(a), sin(a));
    }
    // Concentric and other patterns without one direction: the slot's average (design 7.3).
    int skin = mode == 1 && !cap ? 1 : 0;
    // Stripes closer than a few pixels on screen would alias into a crosshatch: fade them to the
    // face's average. The pitch is measured across the lines, from the screen-space derivatives.
    if (skin == 1) {
        float across = abs(dir.y) * xy_px.x + abs(dir.x) * xy_px.y;
        if (smoothstep(2.0, 4.0, spacing / max(across, 1e-6)) <= 0.0)
            skin = 0;
    }

    float twist = mc_params[mc_slot].y * max(z, 0.0) * mc_volume_per_mm;
    vec3  c = mc_mesh_colour(mc_slot, n.x, n.y, n.z, p.x, p.y, sy, mc_wall_dir, skin, dir.x, dir.y, zigzag, spacing, twist, w);
    if (skin == 1 && w > 0.0) {
        float across = abs(dir.y) * xy_px.x + abs(dir.x) * xy_px.y;
        float fade   = smoothstep(2.0, 4.0, spacing / max(across, 1e-6));
        if (fade < 1.0)
            c = mix(mc_mesh_colour(mc_slot, n.x, n.y, n.z, p.x, p.y, sy, mc_wall_dir, 0, 0.0, 0.0, false, spacing, twist, w), c, fade);
    }
    return c;
}
#endif

void main()
{
    if (clip_floor && world_pos.z < -0.001)
        discard;
    if (world_pos.z > build_clip_z)
        discard;

    vec3 n = normalize(world_normal);
    if (!gl_FrontFacing)
        n = build_clip_z < 1e9 ? vec3(0.0, 0.0, 1.0) : -n;
    vec3 v = normalize(camera_position - world_pos.xyz);
    vec3 albedo = srgb_to_linear(uniform_color.rgb);
#ifdef SHOWCASE_MULTICOLOUR
    // P4: a coextruded slot's colours, from the geometric normal (before the layer lines tilt it).
    if (mc_slot >= 0 && mc_slot < 16 && mc_count(mc_slot) > 1)
        albedo = mc_mesh_albedo(n, !gl_FrontFacing && build_clip_z < 1e9);
#endif

    // Layer lines: tilt the normal with the slope of a rounded bead and darken the groove between
    // two beads; only on walls (wall-ness 1 - |n.z|).
    float groove_shade = 1.0;
    float wall = 1.0 - abs(n.z);
    if (layer_mode != 0 && line_depth > 0.0 && wall > 0.02) {
        float z = world_pos.z;
        vec2  layer = layer_at(z);
        float h = max(layer.y - layer.x, 1e-4);
        float fade = layer_fade(h / max(fwidth(z), 1e-6));
        float t = clamp((z - layer.x) / h, 0.0, 1.0);
        float amp = line_depth * fade * wall;
        // Bead profile sin(pi t): the lower half faces down, the upper half up.
        n = normalize(n - vec3(0.0, 0.0, 1.0) * cos(PI * t) * amp * 1.4);
        float groove = pow(1.0 - sin(PI * t), 3.0);
        groove_shade = 1.0 - mat_line_shade * min(1.0, line_depth * 1.6) * groove * fade * wall;
    }

    // Carbon / glass fibre: a fine speckle in the colour and the roughness.
    float roughness = clamp(mat_roughness, 0.03, 1.0);
    if (mat_fiber_noise > 0.0) {
        float speck = hash12(floor(world_pos.xy * 12.0) + floor(world_pos.z * 12.0) * 17.0);
        albedo *= 1.0 - mat_fiber_noise * 0.18 * speck;
        roughness = clamp(roughness - mat_fiber_noise * 0.25 * step(0.93, speck), 0.03, 1.0);
    }

    // The anisotropic lobe's tangent follows the layers (horizontal) on walls; 1c: on flat top and
    // bottom faces it follows the skin's lines, or the lobe goes round where they have no direction.
    vec3 up = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 t = normalize(cross(up, n));
    float anisotropy = mat_anisotropy;
    if (anisotropy > 0.0 && abs(n.z) > 0.9) {
        vec3 skin_t;
        if (skin_tangent(n, skin_t))
            t = skin_t;
        else
            anisotropy = 0.0;
    }

    vec3 diffuse, specular;
    shade_material(n, v, t, albedo, roughness, mat_f0, mat_metal_tint, anisotropy, mat_clearcoat, key_shadow(n),
                   diffuse, specular);

    vec3 lit = diffuse * groove_shade + specular * mix(1.0, groove_shade, 0.5);

    // Translucent: the body lets light through, the rim reflects (Fresnel edge brightening).
    float n_dot_v = max(dot(n, v), 1e-4);
    float alpha = uniform_color.a * mat_alpha;
    if (alpha < 1.0) {
        float edge = pow(1.0 - n_dot_v, 4.0);
        alpha = clamp(mix(alpha, 1.0, edge * 0.7) + dot(specular, vec3(0.3)) * 0.5, 0.0, 1.0);
    }

    out_color = material_output(lit, alpha, exposure, force_opaque);
}
