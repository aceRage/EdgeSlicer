#version 140

// Showcase realism 1d (tests/design_showcase_realism.md 2.4): the thickness pass. The back faces of
// the translucent objects (and of plastics whose edges let light through) are drawn with the depth
// test, so each pixel keeps the nearest one, and write their distance from the camera (mm) into an
// R32F target at half the frame's size. showcase_material.fs subtracts its own front face's distance
// to get how thick the plastic is along the view ray (wall_thickness()). Uses showcase_material.vs.

uniform vec3  camera_position;
uniform bool  clip_floor;
uniform float build_clip_z;

in vec4 world_pos;
in vec3 world_normal;

out vec4 out_color;

void main()
{
    if (clip_floor && world_pos.z < -0.001)
        discard;
    if (world_pos.z > build_clip_z)
        discard;
    out_color = vec4(length(world_pos.xyz - camera_position), 0.0, 0.0, 1.0);
}
