// Multi-colour (coextruded) filaments, phase P3: the shared GLSL (Showcase/MultiColourGlsl.hpp).
// tests/design_multicolour_filament_view.md 7.1.
//
// The GLSL functions are compiled here as C++ (the same macro text the shader gets) against a small
// vec3/vec4 shim, then compared with the reference model MultiColour::Filament at random inputs.

#include <catch2/catch.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Showcase/MultiColourFilament.hpp"
#include "libslic3r/Showcase/MultiColourGlsl.hpp"
#include "libslic3r/Showcase/ShowcaseMaterialGlsl.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::MultiColour;
using Catch::Matchers::WithinAbs;

namespace mc_glsl_shim {

struct vec3
{
    float x = 0.f, y = 0.f, z = 0.f;
    vec3() = default;
    explicit vec3(double s) : x(float(s)), y(float(s)), z(float(s)) {}
    vec3(double a, double b, double c) : x(float(a)), y(float(b)), z(float(c)) {}
};
struct vec4
{
    float x = 0.f, y = 0.f, z = 0.f, w = 0.f;
};
inline vec3  operator+(const vec3 &a, const vec3 &b) { return vec3(a.x + b.x, a.y + b.y, a.z + b.z); }
inline vec3  operator*(const vec3 &a, double s) { return vec3(a.x * s, a.y * s, a.z * s); }
inline float floor(float v) { return std::floor(v); }
inline float min(float a, float b) { return std::min(a, b); }
inline float max(float a, float b) { return std::max(a, b); }
inline float sqrt(float v) { return std::sqrt(v); }
inline float abs(float v) { return std::abs(v); }
inline float pow(float a, float b) { return std::pow(a, b); }
inline float atan(float y, float x) { return std::atan2(y, x); }
inline float smoothstep(float e0, float e1, float v)
{
    const float t = std::clamp((v - e0) / (e1 - e0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}
inline vec3 mix(const vec3 &a, const vec3 &b, double t) { return vec3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t); }

// The uniforms.
vec4 mc_colors[64];
vec4 mc_bounds[16];
vec4 mc_params[16];
vec4 mc_global;
vec4 mc_global2;

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244 4305) // GLSL literals are doubles in C++
#endif
SLIC3R_MC_SHARED_GLSL
SLIC3R_MC_MESH_GLSL
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

void load(const UniformTable &t)
{
    for (int i = 0; i < 64; ++i)
        mc_colors[i] = { t.colors[i * 4], t.colors[i * 4 + 1], t.colors[i * 4 + 2], t.colors[i * 4 + 3] };
    for (int i = 0; i < 16; ++i) {
        mc_bounds[i] = { t.bounds[i * 4], t.bounds[i * 4 + 1], t.bounds[i * 4 + 2], t.bounds[i * 4 + 3] };
        mc_params[i] = { t.params[i * 4], t.params[i * 4 + 1], t.params[i * 4 + 2], t.params[i * 4 + 3] };
    }
    mc_global  = { t.global[0], t.global[1], t.global[2], t.global[3] };
    mc_global2 = { t.global2[0], t.global2[1], t.global2[2], t.global2[3] };
}

} // namespace mc_glsl_shim

namespace {

namespace sh = mc_glsl_shim;

float max_diff(const sh::vec3 &a, const RGB &b)
{
    return std::max({ std::abs(a.x - b[0]), std::abs(a.y - b[1]), std::abs(a.z - b[2]) });
}

Filament random_filament(std::mt19937 &rng)
{
    std::uniform_real_distribution<float> u01(0.f, 1.f);
    Filament f;
    f.n = std::uniform_int_distribution<int>(2, MAX_COLOURS)(rng);
    float sum = 0.f;
    for (int i = 0; i < f.n; ++i) {
        f.colour[i]     = { u01(rng), u01(rng), u01(rng) };
        f.proportion[i] = 0.05f + u01(rng);
        sum += f.proportion[i];
    }
    for (int i = 0; i < f.n; ++i)
        f.proportion[i] /= sum;
    f.rotation_deg    = -720.f + 1440.f * u01(rng);
    f.twist_deg_per_m = u01(rng) < 0.3f ? 0.f : -200.f + 400.f * u01(rng);
    return f;
}

float filament_area(float d) { return 3.14159265358979323846f * 0.25f * d * d; }

} // namespace

TEST_CASE("The shared GLSL is one text for the shader and the C++ check", "[MultiColour][Glsl]")
{
    const std::string fns = glsl_functions();
    CHECK(fns.find("vec3 mc_bead_colour(") != std::string::npos);
    CHECK(fns.find("vec3 mc_fragment_colour(") != std::string::npos);
    CHECK(fns.find("//") == std::string::npos);   // a line comment would swallow the stringified rest
    const std::string &prelude = glsl_prelude();
    CHECK(prelude.find("uniform vec4 mc_colors[64];") != std::string::npos);
    CHECK(prelude.find("vec3 external_base_color(vec3 base_color, vec2 section, vec3 right, vec3 dir, float extruded_volume, int look, int role)") !=
          std::string::npos);
    CHECK(prelude.find("#version") == std::string::npos);   // libvgcode puts the prelude after its own
}

TEST_CASE("The coextruded-colour prelude shares no name with libvgcode's shader or realism 1b's prelude", "[MultiColour][Glsl]")
{
    // Both preludes go into the same program (libvgcode::Viewer::set_base_color_prelude/set_fragment_prelude).
    const std::vector<std::string> mine = { "mc_colors", "mc_bounds", "mc_params", "mc_global", "mc_global2", "external_base_color" };
    for (const std::string &n : Slic3r::Showcase::libvgcode_fragment_globals()) {
        INFO(n);
        CHECK(n.rfind("mc_", 0) != 0);
        CHECK(std::find(mine.begin(), mine.end(), n) == mine.end());
    }
    auto repo_file = [](const std::string &relative) {
        std::ifstream     in(std::string(TEST_DATA_DIR) + "/../../" + relative, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    };
    std::string       error;
    const std::string lighting = Slic3r::Showcase::toolpath_prelude(repo_file("resources/shaders/140/showcase_material.fs"),
                                                                    repo_file("resources/shaders/140/showcase_toolpath.glsl"), &error);
    INFO(error);
    REQUIRE_FALSE(lighting.empty());
    CHECK(lighting.find("mc_") == std::string::npos);
    CHECK(lighting.find("external_base_color") == std::string::npos);
    for (const std::string &n : Slic3r::Showcase::glsl_global_names(lighting)) {
        INFO(n);
        CHECK(std::find(mine.begin(), mine.end(), n) == mine.end());
    }
}

TEST_CASE("The shader's bead colour matches the reference model at random inputs", "[MultiColour][Glsl]")
{
    std::mt19937                          rng(20261028);
    std::uniform_real_distribution<float> u11(-1.f, 1.f), u01(0.f, 1.f);
    float                                 worst = 0.f;
    for (int iter = 0; iter < 10000; ++iter) {
        const Filament f    = random_filament(rng);
        const int      slot = std::uniform_int_distribution<int>(0, GLSL_SLOTS - 1)(rng);
        std::vector<Filament> slots(GLSL_SLOTS);
        slots[slot] = f;
        const float d = u01(rng) < 0.5f ? 1.75f : 2.85f;
        sh::load(pack_uniforms(slots, std::vector<float>(GLSL_SLOTS, d)));

        const float sx = u11(rng), sy = u11(rng);
        const float ra = 6.2831853f * u01(rng), da = 6.2831853f * u01(rng);
        const float rx = std::cos(ra), ry = std::sin(ra), dx = std::cos(da), dy = std::sin(da);
        const float length_m = 5.f * u01(rng);
        const bool  wall     = u01(rng) < 0.5f;
        const BeadParams bp  = bead_params(wall);

        // The bead function, with the twist the shader works out from the extruded volume.
        const float twist_turns = f.twist_deg_per_m * length_m / 360.f;
        const sh::vec3 g = sh::mc_bead_colour(slot, sx, sy, sh::vec3(rx, ry, 0.), sh::vec3(dx, dy, 0.), twist_turns, bp.blend_turns,
                                              bp.stack_up_k, bp.visible_height, bp.flatten);
        const RGB      c = f.bead_colour(sx, sy, rx, ry, dx, dy, length_m, bp);
        worst = std::max(worst, max_diff(g, c));

        // The fragment entry point: the role picks the bead kind, the volume gives the twist.
        const int      role   = wall ? 2 : 6;   // ExternalPerimeter, TopSolidInfill
        const float    volume = length_m * 1000.f * filament_area(d);
        const sh::vec3 h      = sh::mc_fragment_colour(slot, role, sx, sy, sh::vec3(rx, ry, 0.), sh::vec3(dx, dy, 0.), volume);
        worst = std::max(worst, max_diff(h, c));
    }
    INFO("largest channel difference (linear RGB): " << worst);
    CHECK(worst < 2e-3f);
}

TEST_CASE("The shader keeps single-colour slots and the other roles as the model says", "[MultiColour][Glsl]")
{
    Filament plain;
    plain.colour[0] = srgb_to_linear(RGB{ 0.2f, 0.4f, 0.6f });
    Filament dual;
    dual.n          = 2;
    dual.colour[0]  = srgb_to_linear(RGB{ 1.f, 0.f, 0.f });
    dual.colour[1]  = srgb_to_linear(RGB{ 1.f, 1.f, 0.f });
    dual.proportion = { 0.5f, 0.5f, 0.f, 0.f };
    const UniformTable t = pack_uniforms({ plain, dual }, {});
    CHECK_FALSE(t.any_twist);
    CHECK(t.any_multi);
    sh::load(t);

    SECTION("a single-colour slot is its colour, whatever the bead")
    {
        CHECK(sh::mc_count(0) == 1);
        CHECK(max_diff(sh::mc_fragment_colour(0, 2, 1.f, 0.f, sh::vec3(1., 0., 0.), sh::vec3(0., 1., 0.), 0.f), plain.colour[0]) < 1e-6f);
        CHECK(sh::mc_count(5) == 1);   // slots past the input are plain too
    }
    SECTION("ironing is pulled towards the average")
    {
        const RGB top = dual.bead_colour(0.f, 1.f, 0.f, -1.f, 1.f, 0.f, 0.f, bead_params(false));
        const RGB avg = dual.average();
        RGB       want;
        for (int c = 0; c < 3; ++c)
            want[c] = top[c] + (avg[c] - top[c]) * IRONING_TO_AVERAGE;
        CHECK(max_diff(sh::mc_fragment_colour(1, 7, 0.f, 1.f, sh::vec3(0., -1., 0.), sh::vec3(1., 0., 0.), 0.f), want) < 1e-5f);
    }
    SECTION("a vertical segment has no stack-up, only its sideways face")
    {
        const RGB want = dual.bead_colour(1.f, 0.7f, 1.f, 0.f, 0.f, 0.f, 0.f, bead_params(false));
        CHECK(max_diff(sh::mc_fragment_colour(1, 6, 1.f, 0.7f, sh::vec3(1., 0., 0.), sh::vec3(0., 0.05, 1.), 0.f), want) < 1e-5f);
    }
    SECTION("sRGB out matches the model's conversion")
    {
        for (float v : { 0.f, 0.002f, 0.2f, 0.5f, 1.f })
            CHECK_THAT(sh::mc_to_srgb(v), WithinAbs(linear_to_srgb(v), 1e-5));
    }
}

TEST_CASE("Twist per mm3 is the twist per metre of this filament's length", "[MultiColour][Glsl]")
{
    // 1 m of 1.75 mm filament is 2405.28 mm3; 360 degrees per metre is then one turn per 2405.28 mm3.
    CHECK_THAT(twist_turns_per_mm3(360.f, 1.75f) * 1000.f * filament_area(1.75f), WithinAbs(1.0, 1e-5));
    CHECK_THAT(twist_turns_per_mm3(-90.f, 2.85f) * 1000.f * filament_area(2.85f), WithinAbs(-0.25, 1e-5));
    CHECK(twist_turns_per_mm3(0.f, 1.75f) == 0.f);
    CHECK_THAT(twist_turns_per_mm3(360.f, 0.f), WithinAbs(twist_turns_per_mm3(360.f, 1.75f), 1e-12));   // bad diameter = 1.75
}

TEST_CASE("Walls blend wide and show their face colour; tops blend narrow (P0 tuning)", "[MultiColour]")
{
    Filament f;
    f.n          = 2;
    f.colour[0]  = srgb_to_linear(RGB{ 1.f, 0.f, 0.f });
    f.colour[1]  = srgb_to_linear(RGB{ 1.f, 1.f, 0.f });
    f.proportion = { 0.5f, 0.5f, 0.f, 0.f };
    // Red centred on the front (-Y): the boundaries face right (+X) and left (-X).
    const RGB red = f.colour[0], yellow = f.colour[1];
    auto near = [](const RGB &a, const RGB &b) { return std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]) < 1e-4f; };

    SECTION("6 degrees past a boundary a wall is still blending, a top is already one colour")
    {
        // The facing direction 6 degrees past the right boundary, towards the back (yellow).
        const float a  = (90.f + 6.f) * 3.14159265f / 180.f;   // from the front, anticlockwise
        const float nx = std::sin(a), ny = -std::cos(a);
        const RGB   w  = f.bead_colour(1.f, 0.f, nx, ny, -ny, nx, 0.f, bead_params(true));
        const RGB   t  = f.bead_colour(1.f, 0.f, nx, ny, -ny, nx, 0.f, bead_params(false));
        CHECK_FALSE(near(w, yellow));
        CHECK(near(t, yellow));
    }
    SECTION("a cube face 45 degrees from the boundaries is one colour from the bottom to the top of its beads")
    {
        // The front-right face of a cube turned 45 degrees: normal (1, -1) / sqrt 2, red.
        const float nx = 0.70710678f, ny = -0.70710678f;
        for (float sy : { -1.f, -0.5f, 0.f, 0.5f, 1.f }) {
            INFO("sy " << sy);
            CHECK(near(f.bead_colour(1.f, sy, nx, ny, -ny, nx, 0.f, bead_params(true)), red));
        }
        // With the full stack-up the bottom of every wall bead would show the boundary blend.
        CHECK_FALSE(near(f.bead_colour(1.f, -1.f, nx, ny, -ny, nx, 0.f, BeadParams()), red));
    }
    SECTION("tops keep the full stack-up (k = 1): zig-zag lines alternate, monotonic lines do not")
    {
        // Lines along X: the top shows the trailing side, -X for +X travel (left boundary, mixed),
        // so turn the split a quarter: red centred on the right (+X).
        Filament g     = f;
        g.rotation_deg = 90.f;
        const RGB plus  = g.bead_colour(0.f, 1.f, 0.f, -1.f, 1.f, 0.f, 0.f, bead_params(false));   // +X travel: trailing -X
        const RGB minus = g.bead_colour(0.f, 1.f, 0.f, 1.f, -1.f, 0.f, 0.f, bead_params(false));   // -X travel: trailing +X
        CHECK(near(plus, yellow));
        CHECK(near(minus, red));
    }
    SECTION("a monotonic top seen from above is its trailing colour with a thin edge of the other")
    {
        // +X travel, trailing -X; the boundary 45 degrees from the trailing side (red centred at
        // 315 degrees). Across the drawn diamond (s.y = 1 - |s.x|), count the samples that read yellow.
        Filament g     = f;
        g.rotation_deg = 315.f;
        auto yellow_share = [&](const BeadParams &bp) {
            int yellow_n = 0, n = 0;
            for (float sx = -1.f; sx <= 1.f; sx += 0.01f, ++n) {
                const RGB c = g.bead_colour(sx, 1.f - std::abs(sx), 0.f, -1.f, 1.f, 0.f, 0.f, bp);
                yellow_n += c[1] > 0.5f * (red[1] + yellow[1]) ? 1 : 0;
            }
            return float(yellow_n) / float(n);
        };
        BeadParams unflattened = bead_params(false);
        unflattened.flatten    = 1.f;
        const float flat = yellow_share(bead_params(false));
        INFO("yellow share, flattened " << flat << ", as drawn " << yellow_share(unflattened));
        CHECK(flat > 0.f);       // the edge is there
        CHECK(flat < 0.18f);     // and thin
        CHECK(yellow_share(unflattened) > 0.2f);
    }
}

// ---- P4: Showcase mesh mode (SLIC3R_MC_MESH_GLSL, design 7.3) ----

namespace {

std::string read_repo_file(const std::string &relative)
{
    std::ifstream     in(std::string(TEST_DATA_DIR) + "/../../" + relative, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The red / yellow dual silk of the P0 prints, equal split, at a rotation.
Filament p0_dual(float rotation_deg)
{
    Filament f;
    f.n            = 2;
    f.colour[0]    = srgb_to_linear(RGB{ 0.843f, 0.149f, 0.118f });
    f.colour[1]    = srgb_to_linear(RGB{ 0.961f, 0.745f, 0.106f });
    f.proportion   = { 0.5f, 0.5f, 0.f, 0.f };
    f.rotation_deg = rotation_deg;
    return f;
}

} // namespace

TEST_CASE("The mesh functions and the material shader fit together", "[MultiColour][Glsl][Mesh]")
{
    const std::string fns = glsl_mesh_functions();
    CHECK(fns.find("vec3 mc_mesh_colour(") != std::string::npos);
    CHECK(fns.find("//") == std::string::npos);

    const std::string &prelude = glsl_mesh_prelude();
    CHECK(prelude.rfind("#define SHOWCASE_MULTICOLOUR 1\n", 0) == 0);
    CHECK(prelude.find("uniform vec4 mc_colors[64];") != std::string::npos);
    CHECK(prelude.find("vec3 mc_bead_colour(") != std::string::npos);
    CHECK(prelude.find("external_base_color") == std::string::npos);   // libvgcode's hook only
    CHECK(prelude.find("#version") == std::string::npos);

    const std::string fs = read_repo_file("resources/shaders/140/showcase_material.fs");
    REQUIRE_FALSE(fs.empty());
    CHECK(fs.find("#ifdef SHOWCASE_MULTICOLOUR") != std::string::npos);
    const std::string src = mesh_fragment_source(fs);
    REQUIRE_FALSE(src.empty());
    CHECK(src.rfind("#version 140", 0) == 0);
    const size_t define_at = src.find("#define SHOWCASE_MULTICOLOUR 1");
    CHECK(define_at != std::string::npos);
    CHECK(define_at < src.find("uniform vec4 uniform_color"));   // before the file's own code
    CHECK(src.size() == fs.size() + prelude.size());

    // The file declares its mesh-mode uniforms itself; the prelude must not, nor the other way round.
    const std::vector<std::string> fs_names = Slic3r::Showcase::glsl_global_names(fs);
    for (const char *mine : { "mc_slot", "mc_wall_dir", "mc_spacing_top", "mc_spacing_bottom", "mc_zigzag_top", "mc_zigzag_bottom",
                              "mc_volume_per_mm", "mc_mesh_albedo" }) {
        INFO(mine);
        CHECK(std::find(fs_names.begin(), fs_names.end(), std::string(mine)) != fs_names.end());
        CHECK(prelude.find(mine) == std::string::npos);
    }
    for (const std::string &n : fs_names) {
        INFO(n);
        if (n.rfind("mc_", 0) == 0)
            CHECK(prelude.find(" " + n + "(") == std::string::npos);   // no function of the prelude redeclared
    }

    // Not a shader: nothing to put the prelude after.
    CHECK(mesh_fragment_source("void main() {}\n").empty());
    CHECK(mesh_fragment_source("").empty());
    CHECK(mesh_fragment_source("#version 140").empty());
    CHECK(mesh_fragment_source("\r\n#version 140\r\nvoid main() {}\r\n").find("#version 140\r\n#define SHOWCASE_MULTICOLOUR") !=
          std::string::npos);
}

TEST_CASE("A mesh wall is the outer-wall bead the toolpaths draw there", "[MultiColour][Glsl][Mesh]")
{
    std::mt19937                          rng(20261029);
    std::uniform_real_distribution<float> u11(-1.f, 1.f), u01(0.f, 1.f);
    float                                 worst = 0.f;
    for (int iter = 0; iter < 5000; ++iter) {
        const Filament f    = random_filament(rng);
        const int      slot = std::uniform_int_distribution<int>(0, GLSL_SLOTS - 1)(rng);
        std::vector<Filament> slots(GLSL_SLOTS);
        slots[slot] = f;
        sh::load(pack_uniforms(slots, {}));

        // A wall normal, a little up or down (still a wall), its height in the layer and the direction.
        const float a        = 6.2831853f * u01(rng);
        const float nz       = 0.45f * u11(rng);
        const float h        = std::sqrt(1.f - nz * nz);
        const float nx       = h * std::cos(a), ny = h * std::sin(a);
        const float sy       = u11(rng);
        const float wall_dir = u01(rng) < 0.5f ? 1.f : -1.f;
        const float volume   = u01(rng) < 0.5f ? 0.f : 3000.f * u01(rng);

        // The toolpath: an outer wall (role 2) whose right is the horizontal normal, run anticlockwise
        // (right = outside) or clockwise; its outside is s.x = 1.
        const float    rx = std::cos(a), ry = std::sin(a);
        const sh::vec3 R(rx, ry, 0.), D(-ry * wall_dir, rx * wall_dir, 0.);
        const sh::vec3 want = sh::mc_fragment_colour(slot, 2, 1.f, sy, R, D, volume);
        const float    twist = volume * sh::mc_params[slot].y;
        const sh::vec3 got  = sh::mc_mesh_colour(slot, nx, ny, nz, 0.f, 0.f, sy, wall_dir, 1, 1.f, 0.f, true, 0.4f, twist, 0.f);
        worst = std::max({ worst, std::abs(got.x - want.x), std::abs(got.y - want.y), std::abs(got.z - want.z) });
    }
    INFO("largest channel difference: " << worst);
    CHECK(worst < 1e-4f);   // R comes from the normal here, from cos / sin there
}

TEST_CASE("A mesh top or bottom face is the skin bead the toolpaths draw there", "[MultiColour][Glsl][Mesh]")
{
    std::mt19937                          rng(1029);
    std::uniform_real_distribution<float> u11(-1.f, 1.f), u01(0.f, 1.f);
    float                                 worst = 0.f;
    for (int iter = 0; iter < 5000; ++iter) {
        const Filament f    = random_filament(rng);
        const int      slot = std::uniform_int_distribution<int>(0, GLSL_SLOTS - 1)(rng);
        std::vector<Filament> slots(GLSL_SLOTS);
        slots[slot] = f;
        sh::load(pack_uniforms(slots, {}));

        const float a       = 6.2831853f * u01(rng);
        const float tx      = std::cos(a), ty = std::sin(a);
        const float spacing = 0.3f + 0.3f * u01(rng);
        const bool  zigzag  = u01(rng) < 0.5f;
        const bool  top     = u01(rng) < 0.5f;
        // A point on line m, at fraction t across it (from its left to its right, looking along T).
        const int   m     = std::uniform_int_distribution<int>(-40, 40)(rng);
        const float t     = 0.02f + 0.96f * u01(rng);
        const float along = 20.f * u11(rng);
        const float px    = (float(m) + t) * spacing * ty + along * tx;
        const float py    = -(float(m) + t) * spacing * tx + along * ty;

        // The bead: odd lines of a zig-zag run against T. s.x is +1 on the right of travel, s.y the
        // drawn diamond's height seen from above (top) or below (bottom). The diamond is the same
        // profile on both sides, so this pins the line, its direction and s.x, not the s.y model
        // (P3's flatten test covers that).
        const float    d    = zigzag && (m % 2 != 0) ? -1.f : 1.f;
        const float    sx   = d * (2.f * t - 1.f);
        const float    sy   = (top ? 1.f : -1.f) * (1.f - std::abs(sx));
        const sh::vec3 want = sh::mc_fragment_colour(slot, top ? 6 : 5, sx, sy, sh::vec3(d * ty, -d * tx, 0.), sh::vec3(d * tx, d * ty, 0.), 0.f);
        const sh::vec3 got  = sh::mc_mesh_colour(slot, 0.f, 0.f, top ? 1.f : -1.f, px, py, 0.3f, 1.f, 1, tx * 3.f, ty * 3.f, zigzag, spacing, 0.f, 1.f);
        worst = std::max({ worst, std::abs(got.x - want.x), std::abs(got.y - want.y), std::abs(got.z - want.z) });
    }
    INFO("largest channel difference: " << worst);
    CHECK(worst < 2e-3f);   // the line fraction is recovered from a world position in float
}

TEST_CASE("Mesh mode shows the P0 prints' look", "[MultiColour][Glsl][Mesh]")
{
    auto near = [](const sh::vec3 &a, const RGB &b) { return std::abs(a.x - b[0]) + std::abs(a.y - b[1]) + std::abs(a.z - b[2]) < 1e-4f; };

    SECTION("the cube: front and left faces red, right and back yellow, at -45 degrees (photo 09)")
    {
        const Filament f = p0_dual(-45.f);
        sh::load(pack_uniforms({ f }, {}));
        const RGB red = f.colour[0], yellow = f.colour[1];
        CHECK(near(sh::mc_mesh_colour(0, 0.f, -1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 1, 1.f, 0.f, true, 0.4f, 0.f, 0.f), red));      // front
        CHECK(near(sh::mc_mesh_colour(0, -1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f, 1, 1.f, 0.f, true, 0.4f, 0.f, 0.f), red));      // left
        CHECK(near(sh::mc_mesh_colour(0, 1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f, 1, 1.f, 0.f, true, 0.4f, 0.f, 0.f), yellow));    // right
        CHECK(near(sh::mc_mesh_colour(0, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 1, 1.f, 0.f, true, 0.4f, 0.f, 0.f), yellow));    // back
        // Turning the cube 45 degrees on the plate puts its front-right face on the boundary: a blend.
        CHECK(near(sh::mc_mesh_colour(0, 0.70710678f, -0.70710678f, 0.f, 0.f, 0.f, 0.f, 1.f, 1, 1.f, 0.f, true, 0.4f, 0.f, 0.f), red) == false);
    }
    SECTION("tops: zig-zag lines alternate, monotonic lines do not (photos 07, 08)")
    {
        // Red centred on the right (+X); lines along X, line centres at y = -(m + 0.5) spacing.
        const Filament f = p0_dual(90.f);
        sh::load(pack_uniforms({ f }, {}));
        const RGB red = f.colour[0], yellow = f.colour[1];
        const float s = 0.4f;
        auto top_at = [&](int m, bool zigzag) { return sh::mc_mesh_colour(0, 0.f, 0.f, 1.f, 3.f, -(float(m) + 0.5f) * s, 0.f, 1.f, 1, 1.f, 0.f, zigzag, s, 0.f, 1.f); };
        CHECK(near(top_at(0, true), yellow));   // +X travel: the trailing -X side on top
        CHECK(near(top_at(1, true), red));      // -X travel
        CHECK(near(top_at(2, true), yellow));
        CHECK(near(top_at(0, false), yellow));
        CHECK(near(top_at(1, false), yellow));
        // Seen from below the leading side shows instead.
        CHECK(near(sh::mc_mesh_colour(0, 0.f, 0.f, -1.f, 3.f, -0.5f * s, 0.f, 1.f, 1, 1.f, 0.f, true, s, 0.f, 1.f), red));
    }
    SECTION("faces without one line direction show the average; slopes blend the wall and the face")
    {
        const Filament f = p0_dual(90.f);
        sh::load(pack_uniforms({ f }, {}));
        CHECK(near(sh::mc_mesh_colour(0, 0.f, 0.f, 1.f, 3.f, 1.f, 0.f, 1.f, 0, 0.f, 0.f, false, 0.4f, 0.f, 1.f), f.average()));
        CHECK(near(sh::mc_mesh_colour(0, 0.f, 0.f, 1.f, 3.f, 1.f, 0.f, 1.f, 1, 0.f, 0.f, true, 0.4f, 0.f, 1.f), f.average()));   // no direction
        const float    nz = 0.7f, nh = std::sqrt(1.f - nz * nz);
        const sh::vec3 wall  = sh::mc_mesh_wall(0, nh, 0.f, 0.2f, 1.f, 0.f);
        const sh::vec3 face  = sh::mc_mesh_skin(0, 3.f, -0.2f, 1.f, 0.f, true, 0.4f, 1.f, 0.f);
        const sh::vec3 slope = sh::mc_mesh_colour(0, nh, 0.f, nz, 3.f, -0.2f, 0.2f, 1.f, 1, 1.f, 0.f, true, 0.4f, 0.f, 0.5f);
        const sh::vec3 mid   = sh::mix(wall, face, 0.5);   // w = 0.5 halfway
        CHECK_THAT(slope.x, WithinAbs(mid.x, 1e-5));
        CHECK_THAT(slope.y, WithinAbs(mid.y, 1e-5));
        CHECK_THAT(slope.z, WithinAbs(mid.z, 1e-5));
    }
    SECTION("a single-colour slot is its colour everywhere")
    {
        Filament plain;
        plain.colour[0] = srgb_to_linear(RGB{ 0.2f, 0.4f, 0.6f });
        sh::load(pack_uniforms({ plain }, {}));
        CHECK(near(sh::mc_mesh_colour(0, 0.f, 0.f, 1.f, 3.f, 1.f, 0.f, 1.f, 1, 1.f, 0.f, true, 0.4f, 0.f, 1.f), plain.colour[0]));
        CHECK(near(sh::mc_mesh_colour(0, 1.f, 0.f, 0.f, 3.f, 1.f, 0.5f, 1.f, 1, 1.f, 0.f, true, 0.4f, 0.f, 0.f), plain.colour[0]));
    }
}

TEST_CASE("Slopes are walls until a layer steps past the band of walls", "[MultiColour][Glsl][Mesh]")
{
    // 0.2 mm layers, two walls of 0.42 mm: skin shows once a layer steps in by more than 0.84 mm.
    const float h = 0.2f, band = 0.84f;
    auto weight_at_deg = [&](float slope_deg) {   // slope from horizontal
        const float a = slope_deg * 3.14159265f / 180.f;
        return sh::mc_skin_weight(std::cos(a), h, band);
    };
    CHECK(sh::mc_skin_weight(0.f, h, band) == 0.f);            // a vertical wall
    CHECK(sh::mc_skin_weight(1.f, h, band) == 1.f);            // a flat top
    CHECK(sh::mc_skin_weight(-1.f, h, band) == 1.f);           // a flat bottom
    CHECK(weight_at_deg(45.f) == 0.f);                         // 0.2 mm a layer: walls (a bunny's back)
    CHECK(weight_at_deg(25.f) == 0.f);                         // 0.43 mm a layer: still walls
    CHECK(weight_at_deg(5.f) == 1.f);                          // 2.3 mm a layer: skin
    // The switch is where the step equals the band, atan(0.2 / 0.84) = 13.4 degrees, and narrow.
    CHECK_THAT(weight_at_deg(13.393f), WithinAbs(0.5, 0.02));
    CHECK(weight_at_deg(16.f) == 0.f);
    CHECK(weight_at_deg(11.f) == 1.f);
    // Thinner layers step less: the same slope stays a wall longer.
    CHECK(sh::mc_skin_weight(std::cos(12.f * 3.14159265f / 180.f), 0.1f, band) == 0.f);
    // Without the settings: the old normal-based blend.
    CHECK_THAT(sh::mc_skin_weight(0.7f, 0.f, band), WithinAbs(0.5, 1e-5));
}

TEST_CASE("Mesh mode reads the skin's line spacing and the wall direction from the config", "[MultiColour][Mesh]")
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("nozzle_diameter", new ConfigOptionFloats({ 0.4, 0.6 }));
    cfg.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    cfg.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.3));
    cfg.set_key_value("line_width", new ConfigOptionFloatOrPercent(0.45, false));
    cfg.set_key_value("top_surface_line_width", new ConfigOptionFloatOrPercent(0.42, false));
    cfg.set_key_value("initial_layer_line_width", new ConfigOptionFloatOrPercent(0.5, false));
    cfg.set_key_value("internal_solid_infill_line_width", new ConfigOptionFloatOrPercent(0.44, false));
    const double overlap = 1. - 0.25 * 3.14159265358979323846;

    MeshSkin s = mesh_skin(cfg, 1);
    CHECK_THAT(s.top_spacing, WithinAbs(0.42 - 0.2 * overlap, 1e-5));
    CHECK_THAT(s.bottom_spacing, WithinAbs(0.44 - 0.2 * overlap, 1e-5));
    CHECK_THAT(s.first_spacing, WithinAbs(0.5 - 0.3 * overlap, 1e-5));
    CHECK(s.wall_dir == 1.f);

    // Percentages are of the extruder's nozzle; 0 falls back to line_width, then the automatic width.
    cfg.set_key_value("top_surface_line_width", new ConfigOptionFloatOrPercent(100., true));
    CHECK_THAT(mesh_skin(cfg, 2).top_spacing, WithinAbs(0.6 - 0.2 * overlap, 1e-5));
    cfg.set_key_value("top_surface_line_width", new ConfigOptionFloatOrPercent(0., false));
    CHECK_THAT(mesh_skin(cfg, 1).top_spacing, WithinAbs(0.45 - 0.2 * overlap, 1e-5));
    cfg.set_key_value("line_width", new ConfigOptionFloatOrPercent(0., false));
    CHECK_THAT(mesh_skin(cfg, 1).top_spacing, WithinAbs(0.4 - 0.2 * overlap, 1e-5));   // top: the nozzle width

    // The band of walls: the outer wall's spacing plus the inner walls'.
    cfg.set_key_value("outer_wall_line_width", new ConfigOptionFloatOrPercent(0.42, false));
    cfg.set_key_value("inner_wall_line_width", new ConfigOptionFloatOrPercent(0.45, false));
    cfg.set_key_value("wall_loops", new ConfigOptionInt(3));
    s = mesh_skin(cfg, 1);
    CHECK_THAT(s.wall_band, WithinAbs((0.42 - 0.2 * overlap) + 2. * (0.45 - 0.2 * overlap), 1e-5));
    CHECK_THAT(s.layer_height, WithinAbs(0.2, 1e-6));

    cfg.set_key_value("wall_direction", new ConfigOptionEnum<WallDirection>(WallDirection::Clockwise));
    CHECK(mesh_skin(cfg, 1).wall_dir == -1.f);
    cfg.set_key_value("wall_direction", new ConfigOptionEnum<WallDirection>(WallDirection::CounterClockwise));
    CHECK(mesh_skin(cfg, 1).wall_dir == 1.f);

    // Nothing set: a 0.4 mm nozzle's defaults, anticlockwise.
    const MeshSkin d = mesh_skin(DynamicPrintConfig(), 1);
    CHECK(d.top_spacing > 0.3f);
    CHECK(d.bottom_spacing > 0.3f);
    CHECK(d.first_spacing > 0.3f);
    CHECK(d.wall_dir == 1.f);
}
