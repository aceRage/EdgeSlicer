#version 110

// Orca #13704 (realistic view): EdgeSlicer's gouraud shader with per-pixel lighting.

const vec3 ZERO = vec3(0.0, 0.0, 0.0);
//BBS: add grey and orange
//const vec3 GREY = vec3(0.9, 0.9, 0.9);
const vec3 ORANGE = vec3(0.8, 0.4, 0.0);
const vec3 LightRed = vec3(0.78, 0.0, 0.0);
const vec3 LightBlue = vec3(0.73, 1.0, 1.0);
const float EPSILON = 0.0001;

// Orca #13704: per-pixel (Phong) lighting for the realistic view.
#define INTENSITY_CORRECTION 0.6
#define PHONG_BRIGHTNESS     1.0

// normalized values for (-0.6/1.31, 0.6/1.31, 1./1.31)
const vec3 LIGHT_TOP_DIR = vec3(-0.4574957, 0.4574957, 0.7624929);
#define LIGHT_TOP_DIFFUSE    (0.85 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SPECULAR   (0.35 * INTENSITY_CORRECTION)
#define LIGHT_TOP_SHININESS  32.0

// normalized values for (1./1.43, 0.2/1.43, 1./1.43)
const vec3 LIGHT_FRONT_DIR = vec3(0.6985074, 0.1397015, 0.6985074);
#define LIGHT_FRONT_DIFFUSE  (0.35 * INTENSITY_CORRECTION)
#define LIGHT_FRONT_SPECULAR (0.12 * INTENSITY_CORRECTION)
#define LIGHT_FRONT_SHININESS 16.0

#define INTENSITY_AMBIENT    0.25

struct PrintVolumeDetection
{
	// 0 = rectangle, 1 = circle, 2 = custom, 3 = invalid
	int type;
    // type = 0 (rectangle):
    // x = min.x, y = min.y, z = max.x, w = max.y
    // type = 1 (circle):
    // x = center.x, y = center.y, z = radius
	vec4 xy_data;
    // x = min z, y = max z
	vec2 z_data;
};

struct SlopeDetection
{
    bool actived;
	float normal_z;
    mat3 volume_world_normal_matrix;
};

uniform vec4 uniform_color;
uniform bool use_color_clip_plane;
uniform vec4 uniform_color_clip_plane_1;
uniform vec4 uniform_color_clip_plane_2;
uniform SlopeDetection slope;

// Curved cut sheet (cut gizmo, Surface = Curved). When active the two halves are
// split by a height field z = f(u,v) sampled over the cut plane, not by the flat
// plane itself: curved_sheet_matrix takes a world point into the cut plane's own
// frame, (x,y) are mapped onto [0,1]^2 over +-curved_sheet_half_size and the
// texture holds f in mm along the plane normal. The comparison has to happen per
// FRAGMENT - a per-vertex dot would only bend as finely as the mesh is
// tessellated, which is exactly the flat-looking preview this replaces.
uniform bool  curved_sheet_active;
uniform sampler2D curved_sheet_tex;
uniform mat4  curved_sheet_matrix;
// Phase 2: the sheet's domain is a RECTANGLE fitted to the cut's cross-section,
// so u and v have their own half extent.
uniform vec2  curved_sheet_half_size;
// 0.0: the texture holds f in mm. Otherwise it holds (f/range + 1)/2, which is
// what the GL 2.1 fallback has to do - GL_LUMINANCE is fixed point on [0,1].
uniform float curved_sheet_range;
// DRAWN cut field (cut gizmo, Surface = Draw; 2026-09-12). A ruled strip swept along a
// drawn stroke is NOT a height field over the plane, so curved_sheet_tex above cannot
// describe it - there is no single-valued (u,v) -> z. What there is instead is the
// question the split itself asks: IS THE POINT INSIDE THE CUTTER SOLID. That is baked
// into a 3D SIGN FIELD over the part's bounding box in the cut plane's frame, NEGATIVE
// on the upper half, which is the same convention color_clip_plane already uses.
//
// draw_field_matrix takes a world point into the plane frame; origin and size normalise
// it into the texture's [0,1]^3. The fetch is NEAREST-filtered on the C++ side, because
// interpolating a two-valued sign puts a ragged, camera-dependent band across the
// boundary.
uniform bool      draw_field_active;
uniform sampler3D draw_field_tex;
uniform mat4      draw_field_matrix;
uniform vec3      draw_field_origin;
uniform vec3      draw_field_size;

// Phase 2, side visibility. Per-half alpha for the two colour-clip sides, so a
// half can be shown solid (1.0), ghosted (~0.25) or hidden. A NEGATIVE value
// means hidden: the fragment is discarded outright, which is the only way to see
// through a half rather than through it dimly. The C++ side sets 1.0/1.0 by
// default, so the flat cut and every other colour-clip user is untouched.
uniform float color_clip_side_alpha_1;
uniform float color_clip_side_alpha_2;

#ifdef ENABLE_ENVIRONMENT_MAP
    uniform sampler2D environment_tex;
    uniform bool use_environment_tex;
#endif // ENABLE_ENVIRONMENT_MAP

uniform PrintVolumeDetection print_volume;

varying vec3 clipping_planes_dots;
varying float color_clip_plane_dot;

varying vec4 world_pos;
varying float world_normal_z;
varying vec3 eye_normal;
varying vec3 eye_position;

float soft_circle(vec2 p, vec2 center, float radius, float blur)
{
    float dist = distance(p, center);
    return 1.0 - smoothstep(radius - blur, radius, dist);
}

// A soft round "window" highlight reflected off the surface.
vec3 compute_window_reflection(vec3 normal, vec3 view_dir)
{
    vec3 light_dir = normalize(LIGHT_TOP_DIR);
    vec3 reflect_light = normalize(reflect(-light_dir, normal));

    // UV coordinates for the reflection
    vec2 uv = (reflect_light.xy / (1.0 + max(reflect_light.z, 0.3))) * 2.2;

    vec2 grad = fwidth(uv) * 0.8;
    float blur = 0.12 + grad.x * 1.5;

    float window_light = soft_circle(uv, vec2(0.0, 0.0), 0.5, blur);

    float fresnel = pow(1.0 - max(dot(normal, view_dir), 0.0), 1.2);
    float facing = smoothstep(-0.4, 0.6, reflect_light.z);

    float intensity = window_light * (0.15 + 0.15 * fresnel) * facing;
    intensity = clamp(intensity, 0.0, 0.25);

    return vec3(intensity);
}

void main()
{
    if (any(lessThan(clipping_planes_dots, ZERO)))
        discard;

    vec4 color;
	if (use_color_clip_plane) {
		float side = color_clip_plane_dot;
		if (draw_field_active) {
			// The DRAWN field wins over the sheet: a cut is Curved or Draw, never both,
			// and giving the newer one precedence means a stale sheet texture left over
			// from a mode switch cannot colour a drawn cut.
			vec3 local = (draw_field_matrix * vec4(world_pos.xyz, 1.0)).xyz;
			vec3 uvw = (local - draw_field_origin) / draw_field_size;
			// Outside the field the part does not exist, so clamping continues the
			// border value rather than snapping back to the flat plane at the box edge.
			float v = texture3D(draw_field_tex, clamp(uvw, 0.0, 1.0)).r;
			// The GL 2.1 fallback stores 0 for upper and 1 for lower (GL_LUMINANCE is
			// fixed point on [0,1] and cannot hold -1), so the test is against the
			// midpoint and works for both encodings: -1 and 0 are both below 0.5.
			side = v - 0.5;
		}
		else if (curved_sheet_active) {
			vec3 local = (curved_sheet_matrix * vec4(world_pos.xyz, 1.0)).xyz;
			vec2 uv = local.xy / (2.0 * curved_sheet_half_size) + vec2(0.5, 0.5);
			// Outside the sheet's domain the height field is not defined; clamp
			// so the split continues along the border value rather than snapping
			// back to the flat plane at the sheet's edge.
			float h = texture2D(curved_sheet_tex, clamp(uv, 0.0, 1.0)).r;
			if (curved_sheet_range > 0.0)
				h = (2.0 * h - 1.0) * curved_sheet_range;
			// color_clip_plane points along -normal (see GLVolumeCollection::
			// set_color_clip_plane), so keep the same sign convention here.
			side = h - local.z;
		}
		bool first = side < 0.0;
		// Side visibility. Hidden is a discard, not an alpha of zero: a
		// zero-alpha fragment still writes depth and would keep hiding whatever
		// is behind it, which is the whole point of hiding the near half.
		float side_alpha = first ? color_clip_side_alpha_1 : color_clip_side_alpha_2;
		if (side_alpha < 0.0)
			discard;
		color.rgb = first ? uniform_color_clip_plane_1.rgb : uniform_color_clip_plane_2.rgb;
		color.a = uniform_color.a * side_alpha;
    }
    else
	    color = uniform_color;

    if (slope.actived) {
         if(world_pos.z<0.1&&world_pos.z>-0.1)
         {
                color.rgb = LightBlue;
                color.a = 0.8;
         }
         else if( world_normal_z < slope.normal_z - EPSILON)
         {
                color.rgb = color.rgb * 0.5 + LightRed * 0.5;
                color.a = 0.8;
         }
    }
    // if the fragment is outside the print volume -> use darker color
	vec3 pv_check_min = ZERO;
	vec3 pv_check_max = ZERO;
    if (print_volume.type == 0) {
		// rectangle
		pv_check_min = world_pos.xyz - vec3(print_volume.xy_data.x, print_volume.xy_data.y, print_volume.z_data.x);
		pv_check_max = world_pos.xyz - vec3(print_volume.xy_data.z, print_volume.xy_data.w, print_volume.z_data.y);
	}
	else if (print_volume.type == 1) {
		// circle
		float delta_radius = print_volume.xy_data.z - distance(world_pos.xy, print_volume.xy_data.xy);
		pv_check_min = vec3(delta_radius, 0.0, world_pos.z - print_volume.z_data.x);
		pv_check_max = vec3(0.0, 0.0, world_pos.z - print_volume.z_data.y);
	}
	color.rgb = (any(lessThan(pv_check_min, ZERO)) || any(greaterThan(pv_check_max, ZERO))) ? mix(color.rgb, ZERO, 0.3333) : color.rgb;

    vec3 normal = normalize(eye_normal);
    vec3 view_dir = normalize(-eye_position);

    float NdotL_top = max(dot(normal, LIGHT_TOP_DIR), 0.0);
    float diffuse = INTENSITY_AMBIENT + NdotL_top * LIGHT_TOP_DIFFUSE;
    vec3 half_top = normalize(LIGHT_TOP_DIR + view_dir);
    float specular = LIGHT_TOP_SPECULAR * pow(max(dot(normal, half_top), 0.0), LIGHT_TOP_SHININESS);

    float NdotL_front = max(dot(normal, LIGHT_FRONT_DIR), 0.0);
    diffuse += NdotL_front * LIGHT_FRONT_DIFFUSE;
    vec3 half_front = normalize(LIGHT_FRONT_DIR + view_dir);
    specular += LIGHT_FRONT_SPECULAR * pow(max(dot(normal, half_front), 0.0), LIGHT_FRONT_SHININESS);
    vec3 window_reflection = compute_window_reflection(normal, view_dir);

    // SSAO is applied in a post-process pass; the base lighting stays as it is here.
    float shade = 1.0;

#ifdef ENABLE_ENVIRONMENT_MAP
    if (use_environment_tex)
        gl_FragColor = vec4(clamp((0.45 * texture2D(environment_tex, normalize(eye_normal).xy * 0.5 + 0.5).xyz + window_reflection + 0.8 * color.rgb * diffuse) * PHONG_BRIGHTNESS * shade, vec3(0.0), vec3(1.0)), color.a);
    else
#endif
        gl_FragColor = vec4(clamp((vec3(specular) + window_reflection + color.rgb * diffuse) * PHONG_BRIGHTNESS * shade, vec3(0.0), vec3(1.0)), color.a);
}
