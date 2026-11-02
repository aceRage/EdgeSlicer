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
// - G1: glow-in-the-dark filaments (libslic3r/Showcase/GlowFilament.hpp). With the scene's lights off
//   (scene_dim > 0) every surface keeps only 1 - scene_dim of the light, and a glow slot adds its glow
//   colour (glow_emission()) on top, scaled by the project-wide glow_intensity, through the same HDR
//   post pass. scene_dim = 0 is the plain material, so a program that never sets it is unchanged.
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
// G1: the slot drawn, as glow_emission() reads it (0 = no glow).
uniform vec4  glow_slot;

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

// Realism 1d (Showcase::effective_translucency): light through the plastic. All 0 = the plain material.
uniform float mat_transmission;      // share of the light behind a wall that comes through it
uniform float mat_density;           // Beer's law, per mm
uniform float mat_sss;               // soft wrap of the light round the edges
// The distance from the camera to the nearest back face of a translucent object (R32F, mm; 0 = none),
// drawn before the objects at half size (ShowcaseRenderer::render_thickness). Unit 8.
uniform bool      use_thickness;
uniform sampler2D thickness_map;
uniform vec4      thickness_viewport;   // origin x, y and size w, h of the frame it was drawn for
// Realism 1e (MaterialParams): sheen, flakes and the speckle of a filled filament. All 0 = off.
uniform float mat_sheen;
uniform float mat_sparkle;
uniform float mat_speckle;
uniform int   mat_speckle_kind;      // 1 stone (marble), 2 wood

// Realism 1f (Showcase::Imperfections): 0 = off. The seam table (unit 9, RGBA32F, one texel a seam
// point x, y, z, sorted by z; Showcase::seam_points) comes from the plate's slice.
uniform float     imp_wobble;
uniform float     imp_seam;
uniform float     imp_colour;
uniform sampler2D seam_table;
uniform int       seam_count;
uniform int       seam_table_width;

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

// G1: 0 = the lights are on; otherwise the share of the light taken away (1 - the ambient level).
uniform float scene_dim;
// G1: the project-wide glow intensity, 1 = 100%.
uniform float glow_intensity;

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

// --- glow in the dark (G1) -----------------------------------------------------------------------

// The more vibrant version of an sRGB colour: the same hue, saturation s + (1 - s) * 0.5, value
// v + (1 - v) * 0.8; a grey glows phosphor green. GlowFilament.hpp vibrant() with the same constants
// (SAT_LIFT, VAL_LIFT, GREY_SAT, GREY_GLOW).
vec3 glow_vibrant(vec3 c)
{
    c = clamp(c, 0.0, 1.0);
    float mx = max(c.r, max(c.g, c.b));
    float mn = min(c.r, min(c.g, c.b));
    float s = mx > 0.0 ? (mx - mn) / mx : 0.0;
    if (s < 0.08)
        return vec3(124.0 / 255.0, 1.0, 107.0 / 255.0);
    float s2 = s + (1.0 - s) * 0.5;
    float v2 = mx + (1.0 - mx) * 0.8;
    return v2 * (vec3(1.0 - s2) + s2 * ((c - vec3(mn)) / (mx - mn)));
}

// What a glow slot emits, linear. glow.x: 0 not a glow slot, 1 the vibrant version of base_srgb (the
// filament's own colour as drawn, so a split slot glows in each of its colours), 2 the explicit colour
// in glow.yzw (linear).
vec3 glow_emission(vec4 glow, vec3 base_srgb)
{
    if (glow.x < 0.5)
        return vec3(0.0);
    if (glow.x < 1.5)
        return srgb_to_linear(glow_vibrant(base_srgb)) * glow_intensity;
    return glow.yzw * glow_intensity;
}

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

// --- realism 1d: light through the plastic ----------------------------------------------------------

// How thick the plastic is along the view ray here (mm): the nearest back face minus this front face.
// Without the thickness pass (the mirror) or where it found nothing behind, a typical wall.
float wall_thickness()
{
    const float fallback = 1.6;   // Showcase::DEFAULT_WALL_THICKNESS
    if (!use_thickness)
        return fallback;
    vec2  uv   = (gl_FragCoord.xy - thickness_viewport.xy) / max(thickness_viewport.zw, vec2(1.0));
    float back = texture(thickness_map, uv).r;
    float front = length(world_pos.xyz - camera_position);
    if (back <= 0.0 || back < front - 0.05)
        return fallback;   // nothing behind, or another object's back face in front of this one
    return max(back - front, 0.05);
}

// The light the plastic lets through towards the eye (linear): the key light from behind the wall and
// the environment on the far side, dimmed by Beer's law over the wall's thickness and coloured by the
// filament; plus a soft wrap of the key light round the edges (the "fast subsurface" of game engines).
vec3 translucent_light(vec3 n, vec3 v, vec3 albedo, float thickness, out float t_share)
{
    t_share = exp(-mat_density * thickness);
    vec3 result = vec3(0.0);
    if (light_count <= 0)
        return result;
    vec3  l   = normalize(light_dir[0]);
    vec3  key = light_color[0];
    vec3  tint = mix(vec3(1.0), albedo, 0.85);
    if (mat_transmission > 0.0) {
        // Seen against the light: brightest where the eye looks into the key through the wall.
        float behind = pow(clamp(dot(v, -normalize(l + n * 0.35)), 0.0, 1.0), 3.0);
        vec3  far    = use_env ? env_diffuse(-n) * env_intensity : plain_diffuse(-n);
        result += (key * behind * 1.4 + far * 0.8) * tint * albedo * t_share * mat_transmission;
    }
    if (mat_sss > 0.0) {
        float nl    = dot(n, l);
        float wrap  = 0.5;
        float extra = max((nl + wrap) / (1.0 + wrap), 0.0) - max(nl, 0.0);
        // Thin parts let more of it round; a thick block only softens its terminator.
        float thin = mat_transmission > 0.0 ? mix(0.6, 1.0, t_share) : 0.6;
        result += albedo * key * extra * mat_sss * thin;
    }
    return result;
}

// --- realism 1e: sheen, flakes, speckle -------------------------------------------------------------

vec3 hash33(vec3 p)
{
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.xxy + p.yxx) * p.zyx);
}

float value_noise(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = hash33(i).x, n100 = hash33(i + vec3(1, 0, 0)).x, n010 = hash33(i + vec3(0, 1, 0)).x, n110 = hash33(i + vec3(1, 1, 0)).x;
    float n001 = hash33(i + vec3(0, 0, 1)).x, n101 = hash33(i + vec3(1, 0, 1)).x, n011 = hash33(i + vec3(0, 1, 1)).x, n111 = hash33(i + vec3(1, 1, 1)).x;
    return mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y), mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y), f.z);
}

float fbm3(vec3 p)
{
    float s = 0.0, a = 0.5;
    for (int i = 0; i < 4; ++i) {
        s += a * value_noise(p);
        p = p * 2.03 + vec3(1.7, 9.2, 3.1);
        a *= 0.5;
    }
    return s;
}

// A velvet-like lobe (Charlie distribution, Neubelt visibility, as KHR_materials_sheen): fibres and
// matte powders catch the light at grazing angles.
vec3 sheen_light(vec3 n, vec3 v, vec3 albedo, float key_shade)
{
    vec3  color = mix(vec3(1.0), albedo, 0.5) * mat_sheen;
    float r     = 0.45;
    float inv_r = 1.0 / r;
    float nv    = max(dot(n, v), 1e-4);
    vec3  s     = vec3(0.0);
    for (int i = 0; i < 3; ++i) {
        if (i >= light_count)
            break;
        vec3  l  = normalize(light_dir[i]);
        float nl = dot(n, l);
        if (nl <= 0.0)
            continue;
        vec3  h  = normalize(l + v);
        float nh = dot(n, h);
        float d  = (2.0 + inv_r) * pow(max(1.0 - nh * nh, 1e-4), 0.5 * inv_r) / (2.0 * PI);
        float vis = 1.0 / (4.0 * (nl + nv - nl * nv));
        s += d * vis * nl * light_color[i] * (i == 0 ? key_shade : 1.0);
    }
    vec3 ambient = use_env ? env_diffuse(n) * env_intensity : plain_diffuse(n);
    s += ambient * pow(1.0 - nv, 4.0) * 0.6;
    return s * color;
}

// Flakes in world-space cells of 0.15 mm (Showcase::SPARKLE_CELL_MM), so they stay on the part as the
// camera moves. Each cell holds a flake with probability mat_sparkle, tilted at random: a tight
// highlight that winks. Under about a pixel a cell fades to the flakes' average haze
// (Showcase::sparkle_lod), so a video does not fizz. `cell_px` comes from the caller (derivatives
// are taken in uniform control flow).
vec3 sparkle_light(vec3 n, vec3 v, vec3 albedo, float key_shade, float cell_px)
{
    const float cell = 0.15;
    vec3  id   = floor(world_pos.xyz / cell);
    vec3  r1   = hash33(id);
    vec3  r2   = hash33(id + 17.0);
    float on   = step(1.0 - mat_sparkle, r1.x);
    vec3  fn   = normalize(n + (r2 - 0.5) * 1.3);
    float lod  = smoothstep(0.75, 2.0, cell_px);
    vec3  tint = mix(vec3(1.0), albedo, 0.35 + 0.4 * r1.y);   // some flakes take the colour, some stay silver
    vec3  glint = vec3(0.0), haze = vec3(0.0);
    for (int i = 0; i < 3; ++i) {
        if (i >= light_count)
            break;
        vec3  l  = normalize(light_dir[i]);
        float nl = dot(n, l);
        if (nl <= 0.0)
            continue;
        vec3  h  = normalize(l + v);
        vec3  rad = light_color[i] * (i == 0 ? key_shade : 1.0);
        glint += pow(max(dot(fn, h), 0.0), 900.0) * 60.0 * nl * rad;
        haze  += pow(max(dot(n, h), 0.0), 24.0) * 0.6 * nl * rad;
    }
    vec3 rf = reflect(-v, fn);
    vec3 env_glint = use_env ? env_specular(rf, 0.03) * env_intensity : plain_specular(rf, 0.03);
    glint += env_glint * 0.8 * smoothstep(0.6, 1.0, r1.z);
    vec3 flakes = glint * on;
    return tint * mix(haze * mat_sparkle, flakes, lod);
}

// The colour of a filled filament: marble's veins and flecks, wood fill's fibres and grain.
// `fleck_px`: pixels across one 0.35 mm fleck (the flecks fade to their average below 1.5 px).
vec3 speckle_albedo(vec3 albedo, float fleck_px)
{
    vec3  p    = world_pos.xyz;
    float fine = smoothstep(0.75, 1.5, fleck_px);
    if (mat_speckle_kind == 1) {
        // Veins: thin dark bands along a warped plane; flecks of darker and lighter grains.
        float warp = fbm3(p * 0.08) * 6.0;
        float band = abs(sin((p.x * 0.6 + p.y * 0.35 + p.z * 0.25) * 0.18 + warp));
        float vein = pow(1.0 - band, 10.0);
        vec3  id   = hash33(floor(p / 0.35));
        float dark = step(0.86, id.x) * fine;
        float lite = step(0.93, id.y) * fine;
        vec3  c    = mix(albedo, albedo * 0.32, vein * 0.75 * mat_speckle);
        c = mix(c, c * 0.25, dark * 0.8 * mat_speckle);
        c = mix(c, mix(c, vec3(1.0), 0.6), lite * 0.6 * mat_speckle);
        // Averaged flecks where they are under a pixel.
        return mix(c, c * (1.0 - 0.06 * mat_speckle), 1.0 - fine);
    }
    // Wood: low-frequency shade, short darker fibres along the layers, dark specks.
    float shade  = fbm3(p * 0.15);
    vec3  c      = albedo * mix(1.0, 0.78 + 0.45 * shade, mat_speckle);
    vec3  fid    = hash33(floor(vec3(p.x / 1.2, p.y / 1.2, p.z / 0.3)));
    float fibre  = step(0.82, fid.x) * fine;
    vec3  speck  = hash33(floor(p / 0.35) + 5.0);
    float dot_on = step(0.9, speck.z) * fine;
    c = mix(c, c * 0.6, fibre * 0.5 * mat_speckle);
    c = mix(c, c * 0.35, dot_on * 0.7 * mat_speckle);
    return mix(c, c * (1.0 - 0.08 * mat_speckle), 1.0 - fine);
}

// --- realism 1f: imperfections ------------------------------------------------------------------------

float hash_layer(float i, float salt) { return fract(sin(i * 12.9898 + salt * 78.233) * 43758.5453); }

// The sideways shift of the layers (in units of the amplitude) at the layer coordinate li (the layer's
// index plus the height within it): a slow drift over about five layers, and a little of each layer's
// own that changes near the layer's top, where the next bead starts.
vec2 wobble_offset(float li)
{
    float k  = floor(li / 5.0);
    float f  = fract(li / 5.0);
    f = f * f * (3.0 - 2.0 * f);
    vec2  a  = vec2(hash_layer(k, 1.0), hash_layer(k, 2.0)) - 0.5;
    vec2  b  = vec2(hash_layer(k + 1.0, 1.0), hash_layer(k + 1.0, 2.0)) - 0.5;
    float il = floor(li);
    vec2  j0 = vec2(hash_layer(il, 3.0), hash_layer(il, 4.0));
    vec2  j1 = vec2(hash_layer(il + 1.0, 3.0), hash_layer(il + 1.0, 4.0));
    vec2  j  = mix(j0, j1, smoothstep(0.7, 1.0, fract(li))) - 0.5;
    return 2.0 * (mix(a, b, f) + 0.5 * j);
}

vec4 seam_point(int i) { return texelFetch(seam_table, ivec2(i - (i / seam_table_width) * seam_table_width, i / seam_table_width), 0); }

// How far (mm, across the wall) this point is from the seam of its layer, and the direction away from
// it. The seam points sit at the top of their layer (the nozzle's height), so the search runs from
// this height up one layer.
float seam_distance(vec3 p, float h, out vec2 away)
{
    away = vec2(0.0);
    if (seam_count <= 0 || seam_table_width <= 0)
        return 1e9;
    float lo_z = p.z - 0.01;
    int lo = 0, hi = seam_count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (seam_point(mid).z < lo_z)
            lo = mid + 1;
        else
            hi = mid;
    }
    float best = 1e9;
    for (int k = 0; k < 48; ++k) {
        int i = lo + k;
        if (i >= seam_count)
            break;
        vec4 s = seam_point(i);
        if (s.z > p.z + h + 0.01)
            break;
        vec2  d = p.xy - s.xy;
        float l = length(d);
        if (l < best) {
            best = l;
            away = l > 1e-5 ? d / l : vec2(0.0);
        }
    }
    return best;
}

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
    if (mc_slot >= 0 && mc_slot < 16 && mc_is_gradient(mc_slot))
        // P6: a gradient runs along the filament, so the height stands in for its length (volume per mm
        // of height from the bed, a guess like the twist's).
        albedo = mc_gradient_colour(mc_slot, max(world_pos.z, 0.0) * mc_volume_per_mm);
    else if (mc_slot >= 0 && mc_slot < 16 && mc_count(mc_slot) > 1)
        albedo = mc_mesh_albedo(n, !gl_FrontFacing && build_clip_z < 1e9);
#endif
    // G1: what the slot shines with the lights off (nothing otherwise), from the colour drawn here.
    vec3 glow_light = scene_dim > 0.0 ? glow_emission(glow_slot, linear_to_srgb(albedo)) : vec3(0.0);

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

    // 1e: how many millimetres one pixel spans here (flakes and flecks fade below a pixel).
    float px_mm = max(length(fwidth(world_pos.xyz)), 1e-6);
    float dz_px = max(fwidth(world_pos.z), 1e-6);
    // 1e: marble and wood fills.
    if (mat_speckle > 0.0)
        albedo = speckle_albedo(albedo, 0.35 / px_mm);

    // Carbon / glass fibre: a fine speckle in the colour and the roughness.
    float roughness = clamp(mat_roughness, 0.03, 1.0);
    if (mat_fiber_noise > 0.0) {
        float speck;
        if (mat_sheen > 0.0)
            // 1e, textured matte: short streaks along the layers instead of dots.
            speck = hash12(floor(world_pos.xy * 4.0) + floor(world_pos.z * 12.0) * 17.0);
        else
            speck = hash12(floor(world_pos.xy * 12.0) + floor(world_pos.z * 12.0) * 17.0);
        albedo *= 1.0 - mat_fiber_noise * 0.18 * speck;
        roughness = clamp(roughness - mat_fiber_noise * 0.25 * step(0.93, speck), 0.03, 1.0);
    }

    // Realism 1f: imperfections, each only when on. World and layer coordinates only (never the
    // screen), so they stay put in a video, and they fade with the layer lines' "pixels a layer".
    if (imp_wobble > 0.0 || imp_seam > 0.0 || imp_colour > 0.0) {
        float z  = world_pos.z;
        float h  = 0.2;
        float li = z / 0.2;
        if (layer_mode != 0) {
            vec2 layer = layer_at(z);
            h  = max(layer.y - layer.x, 1e-4);
            li = float(layer_index_at(z)) + clamp((z - layer.x) / h, 0.0, 1.0);
        }
        float lfade = layer_fade(h / dz_px);
        float wall_i = 1.0 - abs(n.z);
        // Z-banding: each layer shifted a little sideways tilts the wall at its edges.
        if (imp_wobble > 0.0 && wall_i > 0.02) {
            float amp   = 0.05 * imp_wobble;   // Showcase::wobble_amplitude_mm
            float d0    = dot(wobble_offset(li - 0.25), n.xy);
            float d1    = dot(wobble_offset(li + 0.25), n.xy);
            float slope = (d1 - d0) / (0.5 * h) * amp;
            n = normalize(n - vec3(0.0, 0.0, 1.0) * slope * 3.0 * lfade * wall_i);
            albedo *= 1.0 + 0.05 * imp_wobble * dot(wobble_offset(li), vec2(0.7071)) * lfade;
        }
        // The seam: a line of small bumps, a touch glossier, where the slicer put it.
        if (imp_seam > 0.0 && seam_count > 0 && wall_i > 0.02) {
            const float w = 0.45;   // Showcase::SEAM_WIDTH_MM
            vec2  away;
            float d = seam_distance(world_pos.xyz, h, away);
            if (d < w) {
                float sfade = smoothstep(0.75, 1.5, w / px_mm);
                float x     = clamp(1.0 - d / w, 0.0, 1.0);
                float b     = x * x * (3.0 - 2.0 * x);
                float slope = 0.1 * imp_seam * 6.0 * x * (1.0 - x) / w;   // Showcase::seam_height_mm over the width
                // Each layer's blob is round: strongest in the middle of the layer.
                float mid   = sin(PI * clamp(fract(li), 0.0, 1.0));
                n = normalize(n + vec3(away, 0.0) * slope * 2.0 * mid * sfade * wall_i);
                roughness = clamp(roughness - 0.15 * b * imp_seam * sfade, 0.03, 1.0);
                groove_shade *= 1.0 - 0.1 * imp_seam * sfade * smoothstep(0.4, 0.0, d / w);
            }
        }
        // Slight colour and gloss drift: low-frequency noise and a little per layer.
        if (imp_colour > 0.0) {
            vec3  p  = world_pos.xyz;
            float lo = value_noise(p * 0.04) - 0.5;
            float pl = hash_layer(floor(li), 5.0) - 0.5;
            albedo *= 1.0 + imp_colour * (0.07 * lo + 0.025 * pl * lfade);
            vec3 hue = vec3(value_noise(p * 0.03 + 11.0), value_noise(p * 0.03 + 23.0), value_noise(p * 0.03 + 37.0)) - 0.5;
            albedo *= 1.0 + imp_colour * 0.03 * hue;
            roughness = clamp(roughness + imp_colour * 0.12 * (value_noise(p * 0.08 + 5.0) - 0.5), 0.03, 1.0);
        }
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
    float key_sh = key_shadow(n);
    shade_material(n, v, t, albedo, roughness, mat_f0, mat_metal_tint, anisotropy, mat_clearcoat, key_sh,
                   diffuse, specular);

    vec3 lit = (diffuse * groove_shade + specular * mix(1.0, groove_shade, 0.5)) * (1.0 - scene_dim) + glow_light;

    // Realism 1d / 1e, each only when its finish has it (so the seven older finishes are unchanged).
    float t_share = 1.0;
    bool  extra_on = mat_sheen > 0.0 || mat_sparkle > 0.0 || ((mat_transmission > 0.0 || mat_sss > 0.0) && gl_FrontFacing);
    if (extra_on) {
        vec3 extra = vec3(0.0);
        if (mat_sheen > 0.0)
            extra += sheen_light(n, v, albedo, key_sh);
        if (mat_sparkle > 0.0)
            extra += sparkle_light(n, v, albedo, key_sh, 0.15 / px_mm);
        if ((mat_transmission > 0.0 || mat_sss > 0.0) && gl_FrontFacing)
            extra += translucent_light(n, v, albedo, wall_thickness(), t_share);
        lit += extra * groove_shade * (1.0 - scene_dim);
    }

    // Translucent: the body lets light through, the rim reflects (Fresnel edge brightening).
    float n_dot_v = max(dot(n, v), 1e-4);
    float alpha = uniform_color.a * mat_alpha;
    if (alpha < 1.0) {
        float edge = pow(1.0 - n_dot_v, 4.0);
        alpha = clamp(mix(alpha, 1.0, edge * 0.7) + dot(specular, vec3(0.3)) * 0.5, 0.0, 1.0);
        // 1d: thick plastic hides more of what is behind it than a thin wall.
        if (mat_transmission > 0.0)
            alpha = clamp(mix(alpha, 1.0, (1.0 - t_share) * 0.5), 0.0, 1.0);
    }

    out_color = material_output(lit, alpha, exposure, force_opaque);
}
