#version 140

// Showcase tab: the floor. A shadow catcher writes only the shadow (black with the shadow as alpha,
// so it composites over any backdrop, transparent ones included); a solid, grid or photo floor is
// lit by the key light and darkened by the same depth shadow map the objects use. The disc fades
// out at its rim, so it never shows an edge.
//
// S2: photo floors (a bundled texture or the user's picture, tiled in millimetres), the contact
// shadow under the objects (a blurred top-down footprint, so a transparent export keeps its
// "sitting on the table" shadow in the alpha) and the glossy floor (the objects drawn mirrored into
// reflection_tex at the viewport's size, blurred through its mips by the floor's roughness).

uniform sampler2D shadow_map;
uniform mat4  shadow_light_vp;
uniform float shadow_map_texel;
uniform bool  use_shadow;
uniform float shadow_softness;   // PCF footprint scale (0 = 1)

uniform int   mode;            // 1 shadow catcher, 2 solid, 3 grid, 4 texture / picture
uniform vec4  floor_color;
uniform float shadow_strength; // 0..1
uniform float fade_start;      // radius (0..1 of the disc) where the fade begins
uniform float grid_spacing;    // mm
uniform vec3  light_dir;       // world, towards the light
uniform vec3  light_tint;      // the key's colour (S2; black means white)

uniform sampler2D floor_tex;   // mode 4, sRGB, tiled
uniform float     tile_mm;

uniform sampler2D contact_tex;     // the blurred footprint, 1 = touching
uniform bool      use_contact;
uniform vec4      contact_rect;    // world x0, y0, width, height the texture covers
uniform float     contact_strength;

uniform sampler2D reflection_tex;  // the mirrored objects, premultiplied over transparent black
uniform bool      use_reflection;
uniform float     reflection_strength;
uniform float     reflection_lod;
uniform vec2      viewport_origin;
uniform vec2      viewport_size;
uniform vec3      camera_position;

in vec4 world_pos;
in vec2 local_pos;

out vec4 out_color;

// Fraction of the 5x5 PCF kernel occluded from the light (as printbed_shadow.fs).
float shadow_occlusion()
{
    if (!use_shadow)
        return 0.0;
    vec4 lp = shadow_light_vp * world_pos;
    vec3 proj = lp.xyz / lp.w;
    proj = proj * 0.5 + 0.5;
    if (proj.z > 1.0 || proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 || proj.y > 1.0)
        return 0.0;
    float bias = 0.0004;
    float step_size = shadow_map_texel * (shadow_softness > 0.0 ? shadow_softness : 1.0);
    float sum = 0.0;
    for (int x = -2; x <= 2; ++x) {
        for (int y = -2; y <= 2; ++y) {
            float closest = texture(shadow_map, proj.xy + vec2(float(x), float(y)) * step_size).r;
            sum += (proj.z - bias > closest) ? 1.0 : 0.0;
        }
    }
    return sum / 25.0;
}

float contact_occlusion()
{
    if (!use_contact)
        return 0.0;
    vec2 uv = (world_pos.xy - contact_rect.xy) / max(contact_rect.zw, vec2(1e-4));
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        return 0.0;
    return clamp(texture(contact_tex, uv).r, 0.0, 1.0) * contact_strength;
}

// The mirrored objects at this pixel, premultiplied (rgb * a, a).
vec4 reflection()
{
    if (!use_reflection || reflection_strength <= 0.0)
        return vec4(0.0);
    vec2 uv = (gl_FragCoord.xy - viewport_origin) / max(viewport_size, vec2(1.0));
    vec4 r = textureLod(reflection_tex, uv, reflection_lod);
    // Fresnel: a floor mirrors more at grazing angles.
    vec3 v = normalize(camera_position - world_pos.xyz);
    float fresnel = 0.35 + 0.65 * pow(1.0 - clamp(v.z, 0.0, 1.0), 3.0);
    return r * reflection_strength * fresnel;
}

void main()
{
    float r = length(local_pos);
    float fade = 1.0 - smoothstep(fade_start, 1.0, r);
    if (fade <= 0.0)
        discard;

    // Shadow and contact shadow combine like two layers of shade.
    float occ = shadow_strength * shadow_occlusion();
    float contact = contact_occlusion();
    float dark = 1.0 - (1.0 - occ) * (1.0 - contact);
    vec4 refl = reflection();

    if (mode == 1) {
        // Catcher: the reflection over the shadow, both as coverage.
        float a_shadow = dark * fade;
        float a_refl = clamp(refl.a, 0.0, 1.0) * fade;
        float a = a_refl + a_shadow * (1.0 - a_refl);
        if (a <= 0.001)
            discard;
        vec3 c = refl.rgb * fade / max(a, 1e-4);
        out_color = vec4(clamp(c, 0.0, 1.0), a);
        return;
    }

    vec3 base = floor_color.rgb;
    if (mode == 3) {
        // Anti-aliased lines every grid_spacing mm, stronger every fifth one.
        vec2 g = world_pos.xy / max(grid_spacing, 0.1);
        vec2 w = max(fwidth(g), vec2(1e-5));
        vec2 d = abs(fract(g - 0.5) - 0.5) / w;
        float line = 1.0 - min(min(d.x, d.y), 1.0);
        vec2 g5 = g / 5.0;
        vec2 w5 = max(fwidth(g5), vec2(1e-5));
        vec2 d5 = abs(fract(g5 - 0.5) - 0.5) / w5;
        float major = 1.0 - min(min(d5.x, d5.y), 1.0);
        float lum = dot(base, vec3(0.299, 0.587, 0.114));
        vec3 ink = lum > 0.5 ? base * 0.72 : base + vec3(0.18);
        base = mix(base, ink, max(line * 0.55, major * 0.9));
    } else if (mode == 4) {
        base = texture(floor_tex, world_pos.xy / max(tile_mm, 1.0)).rgb;
    }
    vec3 tint = dot(light_tint, vec3(1.0)) > 0.0 ? mix(vec3(1.0), light_tint, 0.35) : vec3(1.0);
    float lambert = 0.80 + 0.20 * clamp(light_dir.z, 0.0, 1.0);
    vec3 c = base * tint * lambert * (1.0 - dark);
    // The reflection sits on top of the lit floor.
    c = c * (1.0 - clamp(refl.a, 0.0, 1.0)) + refl.rgb;
    out_color = vec4(clamp(c, 0.0, 1.0), fade);
}
