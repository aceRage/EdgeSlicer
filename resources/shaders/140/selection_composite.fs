#version 140

// Reduced-resolution selection mask: a = selection coverage, r = coverage of the other
// (unselected) objects and parts, used to keep the Glow off neighbours.
uniform sampler2D mask_texture;
uniform sampler2D edge_texture;
uniform sampler2D glow_texture;
// Full-resolution selection mask (a = selection coverage), used by the Thin outline.
uniform sampler2D full_mask_texture;
uniform vec3 outline_color;
uniform float fill_alpha;
uniform float edge_strength;
uniform float glow_strength;
// > 0 selects the Thin style: outline width in framebuffer pixels (already DPI scaled).
uniform float thin_outline_width;
// Faint dark rim just outside the Thin outline, so it stays readable on light surfaces.
uniform float thin_rim_width;
uniform float thin_rim_alpha;
uniform vec2 full_texel_size;

const float fillGray = 0.4;
const float outlineExclusionLow = 0.25;
const float outlineExclusionHigh = 0.75;
const float diagonal = 0.70710678;

in vec2 tex_coord;
out vec4 frag_color;

float fullCoverage(vec2 offsetPixels)
{
    return texture(full_mask_texture, tex_coord + offsetPixels * full_texel_size).a;
}

// Largest selection coverage on a ring of the given radius in pixels (8 directions).
float ringCoverage(float radius)
{
    float d = radius * diagonal;
    float axes = max(max(fullCoverage(vec2(radius, 0.0)), fullCoverage(vec2(-radius, 0.0))),
                     max(fullCoverage(vec2(0.0, radius)), fullCoverage(vec2(0.0, -radius))));
    float diagonals = max(max(fullCoverage(vec2(d, d)), fullCoverage(vec2(-d, d))),
                          max(fullCoverage(vec2(d, -d)), fullCoverage(vec2(-d, -d))));
    return max(axes, diagonals);
}

void main()
{
    if (thin_outline_width > 0.0)
    {
        // Thin: a crisp outline just outside the full-resolution silhouette, no Glow.
        float coverage = fullCoverage(vec2(0.0));
        float outside = 1.0 - coverage;
        float line = clamp(max(ringCoverage(thin_outline_width), ringCoverage(thin_outline_width * 0.5)) * outside, 0.0, 1.0);
        float rim = 0.0;
        if (thin_rim_alpha > 0.0)
            rim = clamp(ringCoverage(thin_outline_width + thin_rim_width) * outside * (1.0 - line), 0.0, 1.0) * thin_rim_alpha;

        float fillCoverage = coverage * fill_alpha;
        float compositeAlpha = clamp(fillCoverage + line + rim, 0.0, 1.0);
        vec3 compositeColor = vec3(fillGray) * fillCoverage + outline_color * line;
        frag_color = vec4(compositeColor, compositeAlpha);
        return;
    }

    vec4 mask = texture(mask_texture, tex_coord);
    float coverage = mask.a;
    float neighbourCoverage = mask.r;
    float edgeCoverage = texture(edge_texture, tex_coord).a;
    float outlineExclusion = smoothstep(outlineExclusionLow, outlineExclusionHigh, coverage);
    float outsideFill = 1.0 - outlineExclusion;
    float fillCoverage = coverage * fill_alpha;
    float edgeAlpha = clamp(edge_strength * edgeCoverage * outsideFill, 0.0, 1.0);
    float glowAlpha = 0.0;
    if (glow_strength > 0.0)
    {
        // The Glow only lights the background, never a neighbouring object or part.
        float glowCoverage = texture(glow_texture, tex_coord).a;
        glowAlpha = clamp(glow_strength * glowCoverage * outsideFill * (1.0 - neighbourCoverage), 0.0, 1.0);
    }

    // Precompose normal-alpha Fill and Edge followed by additive Glow.
    float compositeAlpha = fillCoverage + edgeAlpha * (1.0 - fillCoverage);
    vec3 compositeColor = vec3(fillGray) * fillCoverage * (1.0 - edgeAlpha) + outline_color * (edgeAlpha + glowAlpha);
    frag_color = vec4(compositeColor, compositeAlpha);
}
