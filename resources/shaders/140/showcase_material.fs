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
// - Linear lighting, exposure, the Khronos PBR Neutral tone map (keeps filament colours), sRGB out.
//
// Optional: when it does not compile the Showcase canvas draws its objects with phong as in S1.

const float PI = 3.14159265;

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

// Lights: direction TO the light (world) and linear colour x intensity. Light 0 is the key.
uniform int   light_count;
uniform vec3  light_dir[3];
uniform vec3  light_color[3];
uniform vec3  camera_position;       // world
uniform float exposure;              // linear multiplier (2^EV)

// The key light's shadow map (unit 5, as phong).
uniform sampler2D shadow_map;
uniform mat4  shadow_light_vp;
uniform float shadow_intensity;      // 0 = no shadow map
uniform float shadow_map_texel;
uniform float shadow_softness;       // PCF footprint scale

// Environment (unit 6).
uniform bool        use_env;
uniform samplerCube env_map;
uniform float       env_max_lod;
uniform float       env_intensity;
uniform float       env_rotation;    // radians around +Z
uniform vec3        env_sh[9];

// Layer lines. layer_mode 0 off, 1 uniform (first + steps), 2 table (unit 7, R32F tops).
uniform int       layer_mode;
uniform float     line_depth;
uniform float     first_layer_height;
uniform float     layer_height;
uniform sampler2D layer_table;
uniform int       layer_count;
uniform int       layer_table_width;

// Nothing under the floor (and the mirrored pass keeps to its side of it).
uniform bool  clip_floor;

in vec4 world_pos;
in vec3 world_normal;

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

// 0 at 1 px a layer and below, 1 at 2 px and above (layer_line_fade() in C++).
float layer_fade(float layer_px) { return smoothstep(1.0, 2.0, layer_px); }

float hash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void main()
{
    if (clip_floor && world_pos.z < -0.001)
        discard;

    vec3 n = normalize(world_normal);
    if (!gl_FrontFacing)
        n = -n;
    vec3 v = normalize(camera_position - world_pos.xyz);
    vec3 albedo = srgb_to_linear(uniform_color.rgb);

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

    float n_dot_v = max(dot(n, v), 1e-4);
    vec3  f0 = mix(vec3(mat_f0), albedo, mat_metal_tint);
    float kd = 1.0 - 0.7 * mat_metal_tint;
    float a = roughness * roughness;

    // Tangent frame for the anisotropic lobe: t follows the layers (horizontal), b climbs the wall.
    vec3 up = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 t = normalize(cross(up, n));
    vec3 b = cross(n, t);
    float at = max(a * (1.0 - 0.7 * mat_anisotropy), 0.002);
    float ab = min(a * (1.0 + 2.0 * mat_anisotropy), 1.0);

    vec3 diffuse = vec3(0.0);
    vec3 specular = vec3(0.0);
    for (int i = 0; i < 3; ++i) {
        if (i >= light_count)
            break;
        vec3 l = normalize(light_dir[i]);
        float n_dot_l = dot(n, l);
        if (n_dot_l <= 0.0)
            continue;
        float shade = i == 0 ? key_shadow(n) : 1.0;
        vec3 radiance = light_color[i] * shade;
        vec3 h = normalize(l + v);
        float d = mat_anisotropy > 0.0 ? ggx_aniso(h, n, t, b, at, ab) : ggx(max(dot(n, h), 0.0), a);
        vec3 f = fresnel(f0, dot(h, v));
        specular += d * smith_visibility(n_dot_v, n_dot_l, a) * f * n_dot_l * radiance;
        // Clear coat: a sharp second lobe (gloss, transparent).
        if (mat_clearcoat > 0.0) {
            float ac = 0.08 * 0.08;
            specular += mat_clearcoat * ggx(max(dot(n, h), 0.0), ac) * smith_visibility(n_dot_v, n_dot_l, ac) *
                        fresnel(vec3(0.04), dot(h, v)) * n_dot_l * radiance;
        }
        diffuse += albedo * kd * n_dot_l * radiance;
    }

    // Image-based light.
    vec3 r = reflect(-v, n);
    float env_rough = clamp(roughness * (1.0 + 0.6 * mat_anisotropy), 0.0, 1.0);
    vec3 ambient = use_env ? env_diffuse(n) * env_intensity : plain_diffuse(n);
    vec3 reflected = use_env ? env_specular(r, env_rough) * env_intensity : plain_specular(r, env_rough);
    diffuse += albedo * kd * ambient;
    specular += reflected * env_brdf(f0, env_rough, n_dot_v);
    if (mat_clearcoat > 0.0) {
        vec3 coat = use_env ? env_specular(r, 0.08) * env_intensity : plain_specular(r, 0.08);
        specular += mat_clearcoat * coat * env_brdf(vec3(0.04), 0.08, n_dot_v);
    }

    vec3 color = (diffuse * groove_shade + specular * mix(1.0, groove_shade, 0.5)) * exposure * 0.8;

    // Translucent: the body lets light through, the rim reflects (Fresnel edge brightening).
    float alpha = uniform_color.a * mat_alpha;
    if (alpha < 1.0) {
        float edge = pow(1.0 - n_dot_v, 4.0);
        alpha = clamp(mix(alpha, 1.0, edge * 0.7) + dot(specular, vec3(0.3)) * 0.5, 0.0, 1.0);
    }

    out_color = vec4(linear_to_srgb(tone_map(color)), alpha);
}
