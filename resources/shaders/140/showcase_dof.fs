#version 140

// Showcase realism 1h-2 (tests/design_showcase_realism.md 2.7): depth of field over the finished
// frame. Drawn over m_background's full-screen quad with background.vs, one texel per pixel.
//
// mode 0: the circle of confusion of each pixel (r: its radius in pixels, g: its distance along the
//         view axis in mm) from the depth buffer. The backdrop and the floor write no depth, so where
//         nothing was drawn the distance comes from the view ray: the floor plane Z = 0 (a floor or a
//         photo backplate), the ground-projected dome, or infinitely far (ShowcaseLens.cpp). A photo
//         backplate (and the shadow on it) keeps the focus it was taken with.
// mode 1: the gather: a golden-angle disc of `taps` samples over the pixel's own circle; a sample
//         counts where its own circle reaches this pixel, and something behind this pixel never
//         blurs more than twice as much as it, so a sharp model keeps its edge over a soft
//         background. The colours are averaged in linear light.

uniform int   mode;
uniform sampler2D depth_tex;   // mode 0: the frame's depth (D24S8 as a texture)
uniform sampler2D color_tex;   // mode 1: the finished colour (a copy)
uniform sampler2D coc_tex;     // mode 1: mode 0's output

uniform vec2  viewport_size;
uniform float near_z;
uniform float far_z;
uniform bool  ortho;
uniform mat4  inv_view_projection;
uniform vec3  camera_position;
uniform vec3  view_forward;
uniform int   bg_mode;              // 0 infinitely far, 1 the floor plane, 2 the ground dome, 3 a photo (kept sharp)
uniform vec3  ground_center;
uniform float ground_radius;
uniform float ground_floor_radius;

uniform float focus;     // mm along the view axis
uniform float k_px;      // diameter of the circle of something infinitely far
uniform float max_px;    // diameter cap
uniform int   taps;

in vec2 tex_coord;
out vec4 out_color;

const float FAR_AWAY = 65000.0;   // fits a half float

float ray_distance(vec3 o, vec3 d)
{
    if (bg_mode == 0)
        return FAR_AWAY;
    if (d.z < 0.0 && o.z > 0.0) {
        float tg = -o.z / d.z;
        if (bg_mode == 1)
            return tg;
        vec2 q = (o + tg * d).xy - ground_center.xy;
        if (dot(q, q) <= ground_floor_radius * ground_floor_radius)
            return tg;
    }
    if (bg_mode == 2) {
        vec3 oc = o - ground_center;
        float b = dot(oc, d);
        float k = dot(oc, oc) - ground_radius * ground_radius;
        float disc = b * b - k;
        if (disc >= 0.0) {
            float t = -b + sqrt(disc);
            if (t > 0.0 && (o + t * d).z >= 0.0)
                return t;
        }
    }
    return FAR_AWAY;
}

vec3 srgb_to_linear(vec3 c)
{
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

vec3 linear_to_srgb(vec3 c)
{
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

void main()
{
    ivec2 px = ivec2(gl_FragCoord.xy);
    if (mode == 0) {
        float d = texelFetch(depth_tex, px, 0).r;
        float z;
        if (d < 1.0) {
            float ndc = 2.0 * d - 1.0;
            z = ortho ? 0.5 * (ndc * (far_z - near_z) + far_z + near_z) :
                        2.0 * near_z * far_z / (far_z + near_z - ndc * (far_z - near_z));
        } else {
            vec2 uv = gl_FragCoord.xy / viewport_size;
            vec4 p = inv_view_projection * vec4(uv * 2.0 - 1.0, 1.0, 1.0);
            vec3 dir = normalize(p.xyz / p.w - camera_position);
            float t = ray_distance(camera_position, dir);
            z = t >= FAR_AWAY ? FAR_AWAY : t * max(dot(dir, view_forward), 1e-4);
            // A photo backplate has the focus it was taken with: never blurred again.
            if (bg_mode == 3)
                z = focus;
        }
        z = clamp(z, 1e-3, FAR_AWAY);
        float coc = z >= FAR_AWAY ? k_px : k_px * abs(1.0 - focus / z);
        out_color = vec4(0.5 * min(coc, max_px), z, 0.0, 1.0);
        return;
    }

    vec4  c0 = texelFetch(color_tex, px, 0);
    vec2  cz0 = texelFetch(coc_tex, px, 0).rg;
    float r0 = cz0.r;
    if (r0 < 0.5) {
        out_color = c0;
        return;
    }
    ivec2 last = ivec2(viewport_size) - 1;
    // A per-pixel turn of the disc (interleaved gradient noise) trades banding for fine grain, which
    // the export's supersampling averages away.
    float turn = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    vec4  sum  = vec4(srgb_to_linear(c0.rgb), c0.a);
    float wsum = 1.0;
    for (int i = 0; i < taps; ++i) {
        float rr = sqrt((float(i) + 0.5) / float(taps)) * r0;
        float a  = turn + float(i) * 2.39996323;
        ivec2 sp = clamp(ivec2(gl_FragCoord.xy + vec2(cos(a), sin(a)) * rr), ivec2(0), last);
        vec2  cz = texelFetch(coc_tex, sp, 0).rg;
        float rs = cz.r;
        if (cz.g > cz0.g)
            rs = min(rs, 2.0 * r0);
        float w = smoothstep(rr - 1.0, rr + 1.0, rs);
        if (w <= 0.0)
            continue;
        vec4 cs = texelFetch(color_tex, sp, 0);
        sum  += vec4(srgb_to_linear(cs.rgb), cs.a) * w;
        wsum += w;
    }
    sum /= wsum;
    out_color = vec4(linear_to_srgb(sum.rgb), sum.a);
}
