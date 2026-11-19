#version 150

// The full-screen quad for showcase_post_ms.fs (as background.vs, at the fragment shader's GLSL
// version so strict drivers link the pair).

in vec3 v_position;

out vec2 tex_coord;

void main()
{
    tex_coord = v_position.xy * 0.5 + 0.5;
    gl_Position = vec4(v_position, 1.0);
}
