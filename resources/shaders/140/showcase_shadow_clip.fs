#version 140

// Showcase tab (S5): the shadow casters during the layer build-up. Depth only, and nothing above
// the height laid so far. Vertex shader: showcase_material.vs.

uniform float clip_z;   // mm

in vec4 world_pos;

out vec4 out_color;

void main()
{
    if (world_pos.z > clip_z)
        discard;
    out_color = vec4(1.0);
}
