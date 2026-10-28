#ifndef slic3r_Showcase_MultiColourGlsl_hpp_
#define slic3r_Showcase_MultiColourGlsl_hpp_

// Multi-colour (coextruded) filaments, phase P3: the GLSL every view shares
// (tests/design_multicolour_filament_view.md 7.1). Showcase toolpath mode uses it now; mesh mode
// (P4) and the Preview toggle (P5) are meant to reuse the same functions.
//
// One source for GLSL and C++: the functions are written once in SLIC3R_MC_SHARED_GLSL, in the
// common subset of GLSL 1.50 and C++ (no swizzles, no // comments, no preprocessor). glsl_functions()
// is that text as a string; the unit test compiles the same macro as C++ against a small vec3/vec4
// shim and checks it against MultiColour::Filament::bead_colour() (test_multicolour_glsl.cpp), so the
// shader and the reference model cannot drift apart.
//
// Inputs are uniform arrays, one entry per filament slot (the libvgcode extruder cap of 16):
//   uniform vec4 mc_colors[64];   4 colours per slot, linear RGB (w unused)
//   uniform vec4 mc_bounds[16];   cumulative slice boundaries b1, b2, b3 (turns), colour count n in w
//   uniform vec4 mc_params[16];   rotation (turns), twist (turns per mm3 of extruded volume),
//                                 narrowest slice (turns, caps the blend), ironing pull to the average
//   uniform vec4 mc_global;       wall blend, top blend (turns), k, wall visible height
// pack_uniforms() fills them. A slot with n <= 1 takes the plain path (its own base colour).
//
// Twist and, later, gradient filaments (P6) run on the cumulative extruded volume of the slot, in
// mm3, per vertex (libvgcode::Viewer::set_extruded_volume_enabled()). Volume rather than filament
// length keeps libvgcode free of filament diameters: twist_turns_per_mm3() folds the diameter into
// the rate. A gradient's palette cycle (filament length per colour cycle) converts the same way.

#include "MultiColourFilament.hpp"

#include <array>
#include <string>
#include <vector>

// The extrusion roles the functions test are libvgcode's EGCodeExtrusionRole values:
// 1 Perimeter, 2 ExternalPerimeter, 3 OverhangPerimeter, 20 OverSupportPerimeter (walls), 7 Ironing.
#define SLIC3R_MC_SHARED_GLSL \
float mc_wrap(float t) { t = t - floor(t); if (t >= 1.0) t = 0.0; return t; } \
int mc_count(int slot) { return int(mc_bounds[slot].w + 0.5); } \
vec3 mc_colour(int slot, int i) { vec4 c = mc_colors[slot * 4 + i]; return vec3(c.x, c.y, c.z); } \
float mc_bound(int slot, int i) { \
    vec4 b = mc_bounds[slot]; \
    if (i <= 0) return 0.0; \
    if (i >= mc_count(slot)) return 1.0; \
    if (i == 1) return b.x; \
    if (i == 2) return b.y; \
    return b.z; \
} \
vec3 mc_average(int slot) { \
    int n = mc_count(slot); \
    if (n <= 1) return mc_colour(slot, 0); \
    vec3 sum = vec3(0.0); \
    for (int i = 0; i < 4; ++i) { \
        if (i < n) sum = sum + mc_colour(slot, i) * (mc_bound(slot, i + 1) - mc_bound(slot, i)); \
    } \
    return sum; \
} \
vec3 mc_colour_at(int slot, float phi, float blend) { \
    int n = mc_count(slot); \
    if (n <= 1) return mc_colour(slot, 0); \
    phi = mc_wrap(phi); \
    int i = n - 1; \
    for (int j = 0; j < 3; ++j) { \
        if (i > 0 && phi < mc_bound(slot, i)) i = i - 1; \
    } \
    float h = 0.5 * min(blend, mc_params[slot].z); \
    float ds = phi - mc_bound(slot, i); \
    float de = mc_bound(slot, i + 1) - phi; \
    if (h > 0.0 && ds < h) return mix(mc_colour(slot, (i + n - 1) % n), mc_colour(slot, i), smoothstep(-h, h, ds)); \
    if (h > 0.0 && de < h) return mix(mc_colour(slot, i), mc_colour(slot, (i + 1) % n), smoothstep(-h, h, -de)); \
    return mc_colour(slot, i); \
} \
float mc_direction_turns(float x, float y) { return mc_wrap(atan(y, x) * 0.15915494309189535 + 0.25); } \
vec3 mc_bead_colour(int slot, float sx, float sy, vec3 R, vec3 D, float twist_turns, float blend, float k, float visible) { \
    if (mc_count(slot) <= 1) return mc_colour(slot, 0); \
    sy = sy * visible; \
    float vx = sx * R.x - k * sy * D.x; \
    float vy = sx * R.y - k * sy * D.y; \
    if (vx * vx + vy * vy < 1e-12) { \
        vx = R.x; \
        vy = R.y; \
        if (vx * vx + vy * vy < 1e-12) return mc_average(slot); \
    } \
    float phi = mc_wrap(mc_direction_turns(vx, vy) - mc_params[slot].x - twist_turns + 0.5 * mc_bound(slot, 1)); \
    return mc_colour_at(slot, phi, blend); \
} \
bool mc_is_wall(int role) { return role == 1 || role == 2 || role == 3 || role == 20; } \
vec3 mc_fragment_colour(int slot, int role, float sx, float sy, vec3 R, vec3 D, float volume) { \
    if (mc_count(slot) <= 1) return mc_colour(slot, 0); \
    bool wall = mc_is_wall(role); \
    float rl = sqrt(R.x * R.x + R.y * R.y); \
    vec3 Rh = rl > 1e-4 ? vec3(R.x / rl, R.y / rl, 0.0) : vec3(1.0, 0.0, 0.0); \
    float dl = sqrt(D.x * D.x + D.y * D.y); \
    vec3 Dh = dl > 0.1 ? vec3(D.x / dl, D.y / dl, 0.0) : vec3(0.0, 0.0, 0.0); \
    vec3 c = mc_bead_colour(slot, sx, sy, Rh, Dh, volume * mc_params[slot].y, wall ? mc_global.x : mc_global.y, \
                            mc_global.z, wall ? mc_global.w : 1.0); \
    if (role == 7) c = mix(c, mc_average(slot), mc_params[slot].w); \
    return c; \
} \
float mc_to_srgb(float c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; } \
vec3 mc_to_srgb3(vec3 c) { return vec3(mc_to_srgb(max(c.x, 0.0)), mc_to_srgb(max(c.y, 0.0)), mc_to_srgb(max(c.z, 0.0))); }

namespace Slic3r {
namespace MultiColour {

constexpr int GLSL_SLOTS = 16;   // libvgcode's extruder cap (the role buffer keeps min(extruder, 15))

struct UniformTable
{
    std::array<float, GLSL_SLOTS * MAX_COLOURS * 4> colors{};  // mc_colors
    std::array<float, GLSL_SLOTS * 4>               bounds{};  // mc_bounds
    std::array<float, GLSL_SLOTS * 4>               params{};  // mc_params
    std::array<float, 4>                            global{};  // mc_global
    bool any_multi = false;   // some slot has 2 or more colours
    bool any_twist = false;   // some multi-colour slot twists (the volume buffer is needed)
};

// Twist per mm3 of extruded volume, in turns, for a twist in degrees per metre of filament of this
// diameter (mm; <= 0 or not finite = 1.75).
float twist_turns_per_mm3(float twist_deg_per_m, float diameter_mm);
// The table for slots[0..] (filament slot i = slots[i]); diameters_mm[i] is slot i's filament
// diameter (missing = 1.75). Slots past the input, or past GLSL_SLOTS, are single-colour.
UniformTable pack_uniforms(const std::vector<Filament> &slots, const std::vector<float> &diameters_mm);

// SLIC3R_MC_SHARED_GLSL as text.
const char *glsl_functions();
// The prelude for libvgcode::Viewer::set_base_color_prelude(): the uniforms, the shared functions
// and external_base_color(), which hands a multi-colour slot's colour back in the viewer's sRGB.
const std::string &glsl_prelude();

} // namespace MultiColour
} // namespace Slic3r

#endif // slic3r_Showcase_MultiColourGlsl_hpp_
