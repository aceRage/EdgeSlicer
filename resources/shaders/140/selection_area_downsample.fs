#version 140

uniform sampler2D source_texture;
uniform vec2 sample_offset_uv;
uniform bool sample_horizontal;
uniform bool sample_vertical;

in vec2 tex_coord;
out vec4 frag_color;

// Area-averages the selection coverage (a) and the neighbour coverage (r) together.
vec2 coverageAt(vec2 uv)
{
    vec4 texel = texture(source_texture, uv);
    return vec2(texel.r, texel.a);
}

void main()
{
    vec2 coverage;
    if (sample_horizontal && sample_vertical)
    {
        coverage = coverageAt(tex_coord + vec2(-sample_offset_uv.x, -sample_offset_uv.y));
        coverage += coverageAt(tex_coord + vec2(sample_offset_uv.x, -sample_offset_uv.y));
        coverage += coverageAt(tex_coord + vec2(-sample_offset_uv.x, sample_offset_uv.y));
        coverage += coverageAt(tex_coord + vec2(sample_offset_uv.x, sample_offset_uv.y));
        coverage *= 0.25;
    }
    else if (sample_horizontal)
    {
        coverage = coverageAt(tex_coord - vec2(sample_offset_uv.x, 0.0));
        coverage += coverageAt(tex_coord + vec2(sample_offset_uv.x, 0.0));
        coverage *= 0.5;
    }
    else if (sample_vertical)
    {
        coverage = coverageAt(tex_coord - vec2(0.0, sample_offset_uv.y));
        coverage += coverageAt(tex_coord + vec2(0.0, sample_offset_uv.y));
        coverage *= 0.5;
    }
    else
        coverage = coverageAt(tex_coord);

    frag_color = vec4(coverage.x, 0.0, 0.0, coverage.y);
}
