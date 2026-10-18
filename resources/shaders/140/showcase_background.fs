#version 140

// Showcase tab: the backdrop behind the model (solid colour, vertical or radial gradient, an image,
// or the transparency checkerboard of the live view). Drawn over m_background's full-screen quad
// with background.vs. Everything is laid out relative to the safe frame, so the live view shows
// the backdrop the export will have.

uniform int  mode;          // 0 solid, 1 vertical gradient, 2 radial gradient, 3 image, 4 checkerboard
uniform vec4 color1;        // solid; gradient top / centre; image letterbox
uniform vec4 color2;        // gradient bottom / edge
uniform vec4 frame_rect;    // the safe frame in [0,1] viewport coordinates: x0, y0, x1, y1
uniform float frame_aspect; // frame width / height
uniform sampler2D image_tex;
uniform vec4 uv_transform;  // image: uv = f * xy + zw, f = position in the frame (0..1)
uniform bool tile;
uniform vec2 viewport_size; // pixels (checkerboard)

in vec2 tex_coord;
out vec4 out_color;

void main()
{
    vec2 f = (tex_coord - frame_rect.xy) / max(frame_rect.zw - frame_rect.xy, vec2(1e-6));
    vec4 c = color1;
    if (mode == 1) {
        c = mix(color2, color1, clamp(f.y, 0.0, 1.0));
    } else if (mode == 2) {
        vec2 d = (f - 0.5) * vec2(frame_aspect, 1.0);
        float r = length(d) / max(length(vec2(frame_aspect, 1.0) * 0.5), 1e-6);
        c = mix(color1, color2, smoothstep(0.0, 1.0, clamp(r, 0.0, 1.0)));
    } else if (mode == 3) {
        vec2 uv = f * uv_transform.xy + uv_transform.zw;
        if (tile)
            uv = fract(uv);
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
            c = color1;
        else
            c = texture(image_tex, vec2(uv.x, 1.0 - uv.y));
    } else if (mode == 4) {
        vec2 cell = floor(tex_coord * viewport_size / 12.0);
        float k = mod(cell.x + cell.y, 2.0);
        c = vec4(mix(vec3(0.80), vec3(0.92), k), 1.0);
    }
    out_color = vec4(c.rgb, 1.0);
}
