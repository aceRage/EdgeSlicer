#version 140

// Showcase tab: the floor under the model, a unit disc scaled and placed by model_matrix.

uniform mat4 view_model_matrix;
uniform mat4 projection_matrix;
uniform mat4 model_matrix;

in vec3 v_position;

out vec4 world_pos;
out vec2 local_pos;

void main()
{
    world_pos = model_matrix * vec4(v_position, 1.0);
    local_pos = v_position.xy;
    gl_Position = projection_matrix * view_model_matrix * vec4(v_position, 1.0);
}
