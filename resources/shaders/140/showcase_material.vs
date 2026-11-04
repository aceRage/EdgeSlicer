#version 140

// Showcase tab (S2): the objects in a presentation render. Per-pixel lighting happens in
// showcase_material.fs, in world space (the lights are fixed in the world, D14).

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;
uniform mat4 volume_world_matrix;
uniform mat3 world_normal_matrix;

in vec3 v_position;
in vec3 v_normal;

out vec4 world_pos;
out vec3 world_normal;

void main()
{
    world_pos    = volume_world_matrix * vec4(v_position, 1.0);
    world_normal = normalize(world_normal_matrix * v_normal);
    gl_Position  = projection_matrix * (view_model_matrix * vec4(v_position, 1.0));
}
