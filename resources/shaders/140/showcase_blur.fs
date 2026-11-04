#version 140

// Showcase tab (S2): one direction of a separable Gaussian blur (the contact shadow's footprint).
// Drawn over m_background's full-screen quad with background.vs.

uniform sampler2D source;
uniform vec2 direction;   // one texel along the blur axis, in UV units, times the spread

in vec2 tex_coord;
out vec4 out_color;

void main()
{
    const float w0 = 0.2270270270;
    const float w1 = 0.1945945946;
    const float w2 = 0.1216216216;
    const float w3 = 0.0540540541;
    const float w4 = 0.0162162162;
    vec4 sum = texture(source, tex_coord) * w0;
    sum += (texture(source, tex_coord + direction * 1.0) + texture(source, tex_coord - direction * 1.0)) * w1;
    sum += (texture(source, tex_coord + direction * 2.0) + texture(source, tex_coord - direction * 2.0)) * w2;
    sum += (texture(source, tex_coord + direction * 3.0) + texture(source, tex_coord - direction * 3.0)) * w3;
    sum += (texture(source, tex_coord + direction * 4.0) + texture(source, tex_coord - direction * 4.0)) * w4;
    out_color = sum;
}
