#ifndef slic3r_Showcase_MultiColourGlsl_hpp_
#define slic3r_Showcase_MultiColourGlsl_hpp_

// Multi-colour (coextruded) filaments, phase P3: the GLSL every view shares
// (tests/design_multicolour_filament_view.md 7.1). Showcase toolpath mode uses it; mesh mode (P4,
// SLIC3R_MC_MESH_GLSL below) builds on it, and the Preview toggle (P5) is meant to reuse it.
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
//   uniform vec4 mc_global2;      top flatten (BeadParams::flatten of the other beads), unused x3
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
vec3 mc_bead_colour(int slot, float sx, float sy, vec3 R, vec3 D, float twist_turns, float blend, float k, float visible, float flatten) { \
    if (mc_count(slot) <= 1) return mc_colour(slot, 0); \
    if (flatten != 1.0) { \
        float a = 1.0 - pow(max(1.0 - abs(sy), 0.0), flatten); \
        sy = sy < 0.0 ? -a : a; \
    } \
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
                            mc_global.z, wall ? mc_global.w : 1.0, wall ? 1.0 : mc_global2.x); \
    if (role == 7) c = mix(c, mc_average(slot), mc_params[slot].w); \
    return c; \
} \
float mc_to_srgb(float c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; } \
vec3 mc_to_srgb3(vec3 c) { return vec3(mc_to_srgb(max(c.x, 0.0)), mc_to_srgb(max(c.y, 0.0)), mc_to_srgb(max(c.z, 0.0))); }

// Phase P4: Showcase mesh mode (design 7.3), the model without a slice. Same rules as above (the
// common subset of GLSL and C++), on top of SLIC3R_MC_SHARED_GLSL. Every surface is mapped to the bead
// the toolpaths would draw there, then coloured by mc_bead_colour() with the toolpaths' parameters, so
// a mesh point and the toolpath fragment at the same place agree:
//  * a wall: the outside of an outer-wall bead (s.x = 1) whose right is the horizontal normal and whose
//    travel runs anticlockwise seen from above (wall_dir +1, Orca's default) or clockwise (-1); s.y is
//    the height within the layer (0 without layer lines);
//  * a top (side +1) or bottom (side -1) face: the skin line through the point, lines `spacing` mm
//    apart across the skin direction (tx, ty); with zigzag, every other line runs the other way. The
//    point's place across its line gives s.x, and s.y follows the drawn diamond, 1 - |s.x|;
//  * slopes between |n.z| 0.5 and 0.9 blend the two; skin 0 (concentric or patterns without one
//    direction) shows the slot's average colour on the faces.
#define SLIC3R_MC_MESH_GLSL \
vec3 mc_mesh_wall(int slot, float nx, float ny, float sy, float wall_dir, float twist_turns) { \
    float l = sqrt(nx * nx + ny * ny); \
    if (l < 1e-4) return mc_average(slot); \
    vec3 R = vec3(nx / l, ny / l, 0.0); \
    vec3 D = vec3(-R.y * wall_dir, R.x * wall_dir, 0.0); \
    return mc_bead_colour(slot, 1.0, sy, R, D, twist_turns, mc_global.x, mc_global.z, mc_global.w, 1.0); \
} \
vec3 mc_mesh_skin(int slot, float px, float py, float tx, float ty, bool zigzag, float spacing, float side, float twist_turns) { \
    float tl = sqrt(tx * tx + ty * ty); \
    if (tl < 1e-4 || spacing <= 0.0) return mc_average(slot); \
    tx = tx / tl; \
    ty = ty / tl; \
    float q = (px * ty - py * tx) / spacing; \
    float m = floor(q); \
    float f = q - m; \
    float d = 1.0; \
    if (zigzag && m - 2.0 * floor(0.5 * m) > 0.5) d = -1.0; \
    float sx = d * (2.0 * f - 1.0); \
    float sy = side * (1.0 - abs(sx)); \
    return mc_bead_colour(slot, sx, sy, vec3(d * ty, -d * tx, 0.0), vec3(d * tx, d * ty, 0.0), twist_turns, mc_global.y, \
                          mc_global.z, 1.0, mc_global2.x); \
} \
vec3 mc_mesh_colour(int slot, float nx, float ny, float nz, float px, float py, float sy, float wall_dir, int skin, \
                    float tx, float ty, bool zigzag, float spacing, float twist_turns) { \
    if (mc_count(slot) <= 1) return mc_colour(slot, 0); \
    float w = smoothstep(0.5, 0.9, abs(nz)); \
    vec3 side_colour = w < 1.0 ? mc_mesh_wall(slot, nx, ny, sy, wall_dir, twist_turns) : vec3(0.0); \
    if (w <= 0.0) return side_colour; \
    vec3 face = skin == 1 ? mc_mesh_skin(slot, px, py, tx, ty, zigzag, spacing, nz > 0.0 ? 1.0 : -1.0, twist_turns) : \
                            mc_average(slot); \
    return w >= 1.0 ? face : mix(side_colour, face, w); \
}

namespace Slic3r {

class ConfigBase;
namespace MultiColour {

constexpr int GLSL_SLOTS = 16;   // libvgcode's extruder cap (the role buffer keeps min(extruder, 15))

struct UniformTable
{
    std::array<float, GLSL_SLOTS * MAX_COLOURS * 4> colors{};  // mc_colors
    std::array<float, GLSL_SLOTS * 4>               bounds{};  // mc_bounds
    std::array<float, GLSL_SLOTS * 4>               params{};  // mc_params
    std::array<float, 4>                            global{};  // mc_global
    std::array<float, 4>                            global2{}; // mc_global2
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

// ---- P4: Showcase mesh mode (design 7.3) ----

// SLIC3R_MC_MESH_GLSL as text.
const char *glsl_mesh_functions();
// What showcase_material.fs gets for mesh mode, right after its #version line: "#define
// SHOWCASE_MULTICOLOUR 1" (the file's own mesh-mode block is under #ifdef SHOWCASE_MULTICOLOUR, so the
// file alone still compiles as the plain material), the uniforms, the shared and the mesh functions.
const std::string &glsl_mesh_prelude();
// The fragment source `fs` with glsl_mesh_prelude() put after its first line, which must be the
// #version directive; empty when it is not.
std::string mesh_fragment_source(const std::string &fs);

// What a mesh needs from the print settings to stand in for the toolpaths.
struct MeshSkin
{
    float top_spacing    = 0.4f;   // mm between neighbouring top-skin lines
    float bottom_spacing = 0.4f;   // the same for bottom faces (the first layer's lines)
    float wall_dir       = 1.f;    // +1: walls run anticlockwise seen from above (auto, ccw); -1: clockwise
};
// From a full config with the object's own settings applied; extruder is 1-based (its nozzle).
// The line width of the surface (top_surface_line_width, initial_layer_line_width; 0 = line_width, then
// the slicer's automatic width; percentages of the nozzle), less the overlap of rounded beads at that
// layer height, as Flow::spacing() works it out.
MeshSkin mesh_skin(const ConfigBase &config, int extruder);

} // namespace MultiColour
} // namespace Slic3r

#endif // slic3r_Showcase_MultiColourGlsl_hpp_
