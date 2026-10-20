#version 140

// Showcase tab (S2): the contact shadow's footprint. The objects are drawn from straight above,
// orthographic, into a small texture with MAX blending; what is close to the floor writes close to
// 1, what is contact_height or more above it writes nothing. Blurred afterwards by
// showcase_blur.fs, the floor reads it as the soft darkening right under the model (design 2.3).
// Vertex shader: showcase_material.vs.

uniform float contact_height;   // mm

in vec4 world_pos;

out vec4 out_color;

void main()
{
    float h = max(world_pos.z, 0.0) / max(contact_height, 1e-3);
    if (h >= 1.0)
        discard;
    float v = (1.0 - h) * (1.0 - h);
    out_color = vec4(v, v, v, 1.0);
}
