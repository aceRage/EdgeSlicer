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

// ---- Preview (P5): which slots a G-code uses, and when the table is rebuilt ----

namespace {
DynamicPrintConfig preview_config()
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("filament_colour", new ConfigOptionStrings{ "#FF0000", "#00FF00", "#0000FF" });
    cfg.set_key_value("filament_multi_colors", new ConfigOptionStrings{ "#FF0000|#FFFF00", "#00FF00", "#0000FF|#FFFFFF" });
    cfg.set_key_value("filament_colour_mode", new ConfigOptionInts{ 0, 0, 0 });
    return cfg;
}
std::vector<Filament> preview_slots(const DynamicPrintConfig &cfg)
{
    std::vector<Filament> slots;
    for (size_t e = 0; e < 3; ++e)
        slots.push_back(from_config(cfg, e));
    return slots;
}
} // namespace

TEST_CASE("Preview only offers the toggle when a slot the G-code uses is a split one", "[MultiColour][Preview]")
{
    const DynamicPrintConfig cfg   = preview_config();
    const UniformTable       table = pack_uniforms(preview_slots(cfg), { 1.75f, 1.75f, 1.75f });
    REQUIRE(table.any_multi);

    CHECK(uses_split(table, { 0 }));
    CHECK_FALSE(uses_split(table, { 1 }));          // a single-colour slot
    CHECK(uses_split(table, { 1, 2 }));
    CHECK_FALSE(uses_split(table, {}));
    CHECK_FALSE(uses_split(table, { 5, 15 }));      // slots past the config are single-colour
    CHECK_FALSE(uses_split(table, { 16, 200 }));    // and slots past the shader's table are ignored

    const UniformTable plain = pack_uniforms({}, {});
    CHECK_FALSE(uses_split(plain, { 0, 1, 2 }));
}

TEST_CASE("Preview builds the twist (volume) buffer only when a used split slot twists", "[MultiColour][Preview]")
{
    DynamicPrintConfig cfg = preview_config();
    cfg.set_key_value("filament_colour_twist", new ConfigOptionFloats{ 0., 90., 45. });
    const UniformTable table = pack_uniforms(preview_slots(cfg), { 1.75f, 1.75f, 1.75f });
    REQUIRE(table.any_twist);

    CHECK_FALSE(uses_twist(table, { 0 }));          // split, no twist
    CHECK_FALSE(uses_twist(table, { 1 }));          // twist set on a single-colour slot is ignored
    CHECK(uses_twist(table, { 2 }));
    CHECK(uses_twist(table, { 0, 2 }));
    CHECK_FALSE(uses_twist(table, {}));

    const UniformTable no_twist = pack_uniforms(preview_slots(preview_config()), { 1.75f, 1.75f, 1.75f });
    CHECK_FALSE(uses_twist(no_twist, { 0, 1, 2 }));
}

TEST_CASE("The Preview table is rebuilt exactly when a value it is made from changes", "[MultiColour][Preview]")
{
    DynamicPrintConfig cfg = preview_config();
    const std::string  base = config_signature(cfg);
    CHECK(config_signature(cfg) == base);
    CHECK(config_signature(preview_config()) == base);

    SECTION("each visual-only key changes it")
    {
        DynamicPrintConfig a = cfg;
        a.set_key_value("filament_colour_split", new ConfigOptionStrings{ "30|70" });
        CHECK(config_signature(a) != base);
        DynamicPrintConfig b = cfg;
        b.set_key_value("filament_colour_angle", new ConfigOptionFloats{ 45. });
        CHECK(config_signature(b) != base);
        DynamicPrintConfig c = cfg;
        c.set_key_value("filament_colour_twist", new ConfigOptionFloats{ 10. });
        CHECK(config_signature(c) != base);
    }
    SECTION("so do the colours and the colour mode")
    {
        DynamicPrintConfig a = cfg;
        a.set_key_value("filament_multi_colors", new ConfigOptionStrings{ "#FF0000|#FFFF01", "#00FF00", "#0000FF|#FFFFFF" });
        CHECK(config_signature(a) != base);
        DynamicPrintConfig b = cfg;
        b.set_key_value("filament_colour", new ConfigOptionStrings{ "#FF0001", "#00FF00", "#0000FF" });
        CHECK(config_signature(b) != base);
        DynamicPrintConfig c = cfg;
        c.set_key_value("filament_colour_mode", new ConfigOptionInts{ 1, 0, 0 });
        CHECK(config_signature(c) != base);
    }
    SECTION("an unrelated option does not")
    {
        DynamicPrintConfig a = cfg;
        a.set_key_value("nozzle_diameter", new ConfigOptionFloats{ 0.4 });
        CHECK(config_signature(a) == base);
    }
}
