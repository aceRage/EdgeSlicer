#version 140

// Showcase tab: the floor. A shadow catcher writes only the shadow (black with the shadow as alpha,
// so it composites over any backdrop, transparent ones included); a solid or grid floor is lit by
// the key light and darkened by the same depth shadow map the objects use. The disc fades out at
// its rim, so it never shows an edge.

uniform sampler2D shadow_map;
uniform mat4  shadow_light_vp;
uniform float shadow_map_texel;
uniform bool  use_shadow;

uniform int   mode;            // 1 shadow catcher, 2 solid, 3 grid
uniform vec4  floor_color;
uniform float shadow_strength; // 0..1
uniform float fade_start;      // radius (0..1 of the disc) where the fade begins
uniform float grid_spacing;    // mm
uniform vec3  light_dir;       // world, towards the light

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
    float sum = 0.0;
    for (int x = -2; x <= 2; ++x) {
        for (int y = -2; y <= 2; ++y) {
            float closest = texture(shadow_map, proj.xy + vec2(float(x), float(y)) * shadow_map_texel).r;
            sum += (proj.z - bias > closest) ? 1.0 : 0.0;
        }
    }
    return sum / 25.0;
}

void main()
{
    float r = length(local_pos);
    float fade = 1.0 - smoothstep(fade_start, 1.0, r);
    if (fade <= 0.0)
        discard;

    float occ = shadow_occlusion();
    if (mode == 1) {
        float a = shadow_strength * occ * fade;
        if (a <= 0.001)
            discard;
        out_color = vec4(0.0, 0.0, 0.0, a);
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
    }
    float lambert = 0.80 + 0.20 * clamp(light_dir.z, 0.0, 1.0);
    vec3 c = base * lambert * (1.0 - shadow_strength * occ);
    out_color = vec4(clamp(c, 0.0, 1.0), fade);
}
