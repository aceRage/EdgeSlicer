// Multi-colour (coextruded) filaments, phase P1: the reference model, the visual-only keys, their slot
// helpers, the 3MF round trip and the Bambu export. tests/design_multicolour_filament_view.md.
// The no-G-code guarantee through a real slice is in tests/fff_print/test_multicolour_visual_only.cpp.

#include <catch2/catch.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Format/BambuExport.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/Showcase/MultiColourFilament.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include <algorithm>
#include <cmath>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::MultiColour;
using Catch::Matchers::WithinAbs;

namespace {

const RGB RED_L   = srgb_to_linear(RGB{ 1.f, 0.f, 0.f });
const RGB YELLOW_L = srgb_to_linear(RGB{ 1.f, 1.f, 0.f });
const RGB BLUE_L  = srgb_to_linear(RGB{ 0.f, 0.f, 1.f });
const RGB GREEN_L = srgb_to_linear(RGB{ 0.f, 1.f, 0.f });

Filament dual(float rotation_deg = 0.f, float twist = 0.f)
{
    Filament f;
    f.n               = 2;
    f.colour[0]       = RED_L;
    f.colour[1]       = YELLOW_L;
    f.proportion      = { 0.5f, 0.5f, 0.f, 0.f };
    f.rotation_deg    = rotation_deg;
    f.twist_deg_per_m = twist;
    return f;
}

float dist(const RGB &a, const RGB &b)
{
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

bool same(const RGB &a, const RGB &b, float tol = 1e-4f) { return dist(a, b) < tol; }

// An outer wall facing the outward unit normal (nx, ny), travelled with the material on the side
// given: a contour run anticlockwise has the outside on the right of travel (sx = +1), a hole run
// anticlockwise on the left (sx = -1). Either way the visible face is the outward one.
RGB wall(const Filament &f, float nx, float ny, bool outside_on_right, float sy = 0.f, float length_m = 0.f)
{
    // Right of travel (dx, dy) is (dy, -dx). Outside on the right: R = n, D = (-ny, nx).
    const float rx = outside_on_right ? nx : -nx;
    const float ry = outside_on_right ? ny : -ny;
    const float dx = -ry, dy = rx; // R = (dy, -dx)
    return f.bead_colour(outside_on_right ? 1.f : -1.f, sy, rx, ry, dx, dy, length_m);
}

// The top of a line travelling (dx, dy).
RGB top(const Filament &f, float dx, float dy) { return f.bead_colour(0.f, 1.f, dy, -dx, dx, dy); }

} // namespace

TEST_CASE("Printer directions are turns from the front, anticlockwise from above", "[MultiColour]")
{
    CHECK_THAT(direction_turns(0.f, -1.f), WithinAbs(0., 1e-6));
    CHECK_THAT(direction_turns(1.f, 0.f), WithinAbs(0.25, 1e-6));
    CHECK_THAT(direction_turns(0.f, 1.f), WithinAbs(0.5, 1e-6));
    CHECK_THAT(direction_turns(-1.f, 0.f), WithinAbs(0.75, 1e-6));
}

TEST_CASE("sRGB and linear RGB convert both ways", "[MultiColour]")
{
    for (float c : { 0.f, 0.02f, 0.2f, 0.5f, 0.9f, 1.f })
        CHECK_THAT(linear_to_srgb(srgb_to_linear(c)), WithinAbs(c, 1e-5));
    RGB c;
    REQUIRE(parse_hex("#FF8000", c));
    CHECK_THAT(c[0], WithinAbs(1., 1e-6));
    CHECK_THAT(c[1], WithinAbs(128. / 255., 1e-6));
    CHECK_THAT(c[2], WithinAbs(0., 1e-6));
    CHECK(parse_hex("#ff8000cc", c));
    CHECK_FALSE(parse_hex("FF8000", c));
    CHECK_FALSE(parse_hex("#FF80", c));
    CHECK_FALSE(parse_hex("#GG8000", c));
}

TEST_CASE("Colour proportions parse, format and snap to the editor's steps", "[MultiColour]")
{
    SECTION("valid values are normalised")
    {
        const auto p = parse_split("30|70", 2);
        CHECK_THAT(p[0], WithinAbs(0.3, 1e-6));
        CHECK_THAT(p[1], WithinAbs(0.7, 1e-6));
        const auto q = parse_split(" 1 | 1 | 2 ", 3);
        CHECK_THAT(q[0], WithinAbs(0.25, 1e-6));
        CHECK_THAT(q[2], WithinAbs(0.5, 1e-6));
    }
    SECTION("empty, a wrong count or bad numbers give the equal split")
    {
        for (const char *bad : { "", "50", "50|50|0", "40|x", "40|-60", "40|", "nan|1", "1e400|1" }) {
            CAPTURE(bad);
            const auto p = parse_split(bad, 2);
            CHECK_THAT(p[0], WithinAbs(0.5, 1e-6));
            CHECK_THAT(p[1], WithinAbs(0.5, 1e-6));
        }
        const auto one = parse_split("30|70", 1);
        CHECK(one[0] == 1.f);
    }
    SECTION("format keeps the default slot default")
    {
        CHECK(format_split({ 0.5f, 0.5f, 0.f, 0.f }, 2).empty());
        CHECK(format_split({ 1.f, 0.f, 0.f, 0.f }, 1).empty());
        CHECK(format_split({ 0.3f, 0.7f, 0.f, 0.f }, 2) == "30|70");
        const auto back = parse_split(format_split({ 0.4f, 0.35f, 0.25f, 0.f }, 3), 3);
        CHECK_THAT(back[1], WithinAbs(0.35, 1e-6));
    }
    SECTION("quantize: 5% steps, at least 5% each, summing to 100")
    {
        const auto a = quantize_percent({ 1.f / 3.f, 1.f / 3.f, 1.f / 3.f, 0.f }, 3);
        CHECK(a[0] + a[1] + a[2] == 100);
        const auto b = quantize_percent({ 0.99f, 0.005f, 0.005f, 0.f }, 3);
        CHECK(b[1] == MIN_PERCENT);
        CHECK(b[2] == MIN_PERCENT);
        CHECK(b[0] == 90);
        const auto c = quantize_percent({ 0.32f, 0.68f, 0.f, 0.f }, 2);
        CHECK(c[0] == 30);
        CHECK(c[1] == 70);
        for (int n = 1; n <= MAX_COLOURS; ++n) {
            std::mt19937 rng(1234u + unsigned(n));
            std::uniform_real_distribution<float> u(0.f, 1.f);
            for (int k = 0; k < 200; ++k) {
                std::array<float, MAX_COLOURS> p{ u(rng), u(rng), u(rng), u(rng) };
                const auto q = quantize_percent(p, n);
                int sum = 0;
                for (int i = 0; i < n; ++i) {
                    CHECK(q[i] >= (n == 1 ? 100 : MIN_PERCENT));
                    CHECK(q[i] % STEP_PERCENT == 0);
                    sum += q[i];
                }
                CHECK(sum == 100);
            }
        }
    }
}

TEST_CASE("The cross-section gives each slice its colour and blends softly at the boundaries", "[MultiColour]")
{
    Filament f;
    f.n          = 3;
    f.colour     = { RED_L, GREEN_L, BLUE_L, RGB{} };
    f.proportion = { 0.25f, 0.25f, 0.5f, 0.f };
    CHECK_THAT(f.boundary(1), WithinAbs(0.25, 1e-6));
    CHECK_THAT(f.boundary(2), WithinAbs(0.5, 1e-6));
    CHECK(f.boundary(3) == 1.f);

    CHECK(same(f.colour_at(0.125f), RED_L));
    CHECK(same(f.colour_at(0.375f), GREEN_L));
    CHECK(same(f.colour_at(0.75f), BLUE_L));
    CHECK(same(f.colour_at(1.125f), RED_L)); // wraps
    CHECK(same(f.colour_at(-0.25f), BLUE_L));

    // Half way at a boundary, from either side, and continuous across it (and across the wrap).
    const RGB half = { 0.5f * (RED_L[0] + GREEN_L[0]), 0.5f * (RED_L[1] + GREEN_L[1]), 0.5f * (RED_L[2] + GREEN_L[2]) };
    CHECK(same(f.colour_at(0.25f), half));
    for (float b : { 0.25f, 0.5f, 1.f }) {
        CAPTURE(b);
        CHECK(dist(f.colour_at(b - 1e-5f), f.colour_at(b + 1e-5f)) < 0.01f);
    }
    // Blending only within half the blend width of a boundary.
    CHECK(same(f.colour_at(0.25f + 0.5f * BLEND_TURNS + 1e-3f), GREEN_L));

    // Never wider than the narrowest slice.
    f.proportion = { 0.02f, 0.49f, 0.49f, 0.f };
    CHECK_THAT(f.blend_turns(), WithinAbs(0.02, 1e-6));
    CHECK(same(f.colour_at(0.01f), RED_L, 1e-3f));

    // Average, area-weighted.
    f.proportion = { 0.25f, 0.25f, 0.5f, 0.f };
    const RGB avg = f.average();
    CHECK_THAT(avg[2], WithinAbs(0.5 * BLUE_L[2], 1e-5));
    CHECK_THAT(avg[0], WithinAbs(0.25 * RED_L[0], 1e-5));
}

TEST_CASE("A wall shows the slice that faces the same way, whatever the travel direction", "[MultiColour]")
{
    // Rotation 0 (D3): the middle of colour 1 (red) faces the printer's front.
    const Filament f = dual();
    CHECK(same(wall(f, 0.f, -1.f, true), RED_L));    // front face of a contour
    CHECK(same(wall(f, 0.f, -1.f, false), RED_L));   // a hole's wall facing the front
    CHECK(same(wall(f, 0.f, 1.f, true), YELLOW_L));  // back face
    CHECK(same(wall(f, 0.f, 1.f, false), YELLOW_L));

    // Rotation 90: red faces the right, yellow the left.
    const Filament r = dual(90.f);
    CHECK(same(wall(r, 1.f, 0.f, true), RED_L));
    CHECK(same(wall(r, -1.f, 0.f, true), YELLOW_L));
    // A full turn is the same as none.
    CHECK(same(wall(dual(360.f), 0.f, -1.f, true), RED_L));

    // The stack-up only tilts the colour near a slice boundary: the right face (a boundary at
    // rotation 0) differs from top to bottom of the bead, the front face does not.
    CHECK(same(wall(f, 0.f, -1.f, true, -0.3f), RED_L));
    CHECK(same(wall(f, 0.f, -1.f, true, 0.3f), RED_L));
    CHECK(dist(wall(f, 1.f, 0.f, true, -1.f), wall(f, 1.f, 0.f, true, 1.f)) > 0.1f);
}

TEST_CASE("Top surfaces show the trailing slice, so zig-zag lines alternate and monotonic lines do not", "[MultiColour]")
{
    // Red faces the right: a line laid +X trails its left (yellow), a line laid -X trails its right (red).
    const Filament f = dual(90.f);
    CHECK(same(top(f, 1.f, 0.f), YELLOW_L));
    CHECK(same(top(f, -1.f, 0.f), RED_L));
    // Monotonic: every line the same way, the same colour.
    CHECK(same(top(f, 1.f, 0.f), top(f, 1.f, 0.f)));
    // The bottom shows the leading slice.
    CHECK(same(f.bead_colour(0.f, -1.f, 0.f, -1.f, 1.f, 0.f), RED_L));
}

TEST_CASE("Twist turns the strand per metre of extruded filament", "[MultiColour]")
{
    const Filament f = dual(0.f, 360.f); // one turn per metre
    CHECK(same(wall(f, 0.f, -1.f, true, 0.f, 0.f), RED_L));
    CHECK(same(wall(f, 0.f, -1.f, true, 0.f, 0.5f), YELLOW_L));
    CHECK(same(wall(f, 0.f, -1.f, true, 0.f, 1.f), RED_L));
    // Twist 0 never drifts.
    CHECK(same(wall(dual(), 0.f, -1.f, true, 0.f, 123.f), RED_L));
    // Positive twist turns colour 1 anticlockwise: a quarter metre brings red to the right face.
    CHECK(same(wall(f, 1.f, 0.f, true, 0.f, 0.25f), RED_L));
}

TEST_CASE("A single-colour slot is exactly its colour everywhere", "[MultiColour]")
{
    Filament f;
    f.colour[0] = BLUE_L;
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-1.f, 1.f);
    for (int i = 0; i < 100; ++i) {
        const float a = u(rng) * 3.14159f;
        CHECK(same(f.bead_colour(u(rng), u(rng), std::cos(a), std::sin(a), -std::sin(a), std::cos(a), 5.f), BLUE_L, 1e-6f));
    }
}

TEST_CASE("A slot is read from the project config with defaults for anything missing", "[MultiColour][Config]")
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("filament_colour", new ConfigOptionStrings{ "#FF0000", "#00FF00", "#0000FF", "#FFFFFF" });
    cfg.set_key_value("filament_multi_colors", new ConfigOptionStrings{ "#FF0000|#FFFF00", "#00FF00|#0000FF|#FF0000",
                                                                        "#0000FF|#FFFFFF", "#000000|#111111|#222222|#333333|#444444" });
    cfg.set_key_value("filament_colour_mode", new ConfigOptionInts{ 0, 0, 1, 0 });

    SECTION("without the visual-only keys: equal split, angle 0, no twist")
    {
        const Filament a = from_config(cfg, 0);
        CHECK(a.n == 2);
        CHECK(same(a.colour[0], RED_L));
        CHECK(same(a.colour[1], YELLOW_L));
        CHECK_THAT(a.proportion[0], WithinAbs(0.5, 1e-6));
        CHECK(a.rotation_deg == 0.f);
        CHECK(a.twist_deg_per_m == 0.f);
        const Filament b = from_config(cfg, 1);
        CHECK(b.n == 3);
        CHECK_THAT(b.proportion[2], WithinAbs(1. / 3., 1e-6));
    }
    SECTION("gradient slots and slots beyond the vectors are single colours")
    {
        const Filament g = from_config(cfg, 2);
        CHECK(g.n == 1);
        CHECK(same(g.colour[0], BLUE_L));
        const Filament five = from_config(cfg, 3);
        CHECK(five.n == MAX_COLOURS);
        const Filament none = from_config(cfg, 9);
        CHECK(none.n == 1);
    }
    SECTION("the keys' values per slot; short vectors default the rest")
    {
        cfg.set_key_value("filament_colour_split", new ConfigOptionStrings{ "30|70", "25|25|50" });
        cfg.set_key_value("filament_colour_angle", new ConfigOptionFloats{ 137., -45. });
        cfg.set_key_value("filament_colour_twist", new ConfigOptionFloats{ 90. });
        const Filament a = from_config(cfg, 0);
        CHECK_THAT(a.proportion[0], WithinAbs(0.3, 1e-6));
        CHECK(a.rotation_deg == 137.f);
        CHECK(a.twist_deg_per_m == 90.f);
        const Filament b = from_config(cfg, 1);
        CHECK_THAT(b.proportion[2], WithinAbs(0.5, 1e-6));
        CHECK(b.rotation_deg == -45.f);
        CHECK(b.twist_deg_per_m == 0.f);
        const Filament d = from_config(cfg, 3);
        CHECK_THAT(d.proportion[0], WithinAbs(0.25, 1e-6));
        CHECK(d.rotation_deg == 0.f);
    }
}

TEST_CASE("The visual-only keys are defined, project-only and invisible to the static config", "[MultiColour][Config]")
{
    const std::set<std::string> &keys = visual_only_option_keys();
    CHECK(keys == std::set<std::string>{ "filament_colour_split", "filament_colour_angle", "filament_colour_twist",
                                         "filament_glow", "filament_glow_colour", "filament_glow_intensity" });
    const DynamicPrintConfig full = DynamicPrintConfig::full_print_config();
    for (const std::string &key : keys) {
        INFO(key);
        const ConfigOptionDef *def = print_config_def.get(key);
        REQUIRE(def != nullptr);
        CHECK(def->cli == ConfigOptionDef::nocli);
        // One value per slot, except the glow intensity, which is for the whole project (G1).
        CHECK(def->default_value->is_vector() == (key != "filament_glow_intensity"));
        CHECK(is_visual_only_option(key));
        // Not part of the static PrintConfig: print_config_diffs and the placeholder parser never see them.
        CHECK_FALSE(FullPrintConfig::defaults().has(key));
        CHECK_FALSE(full.has(key));
        // Not a filament-preset vector either: DynamicPrintConfig::set_num_filaments() never touches them.
        const auto &fkeys = print_config_def.filament_option_keys();
        CHECK(std::find(fkeys.begin(), fkeys.end(), key) == fkeys.end());
    }
    CHECK_FALSE(is_visual_only_option("filament_multi_colors"));
    CHECK_FALSE(is_visual_only_option("filament_colour"));
}

TEST_CASE("The slot helpers keep the visual-only keys in step with the slots", "[MultiColour][Config]")
{
    DynamicPrintConfig cfg;
    SECTION("resize creates the keys, pads with defaults and shrinks")
    {
        visual_only_resize_slots(cfg, 3);
        CHECK(cfg.option<ConfigOptionStrings>("filament_colour_split")->values == std::vector<std::string>{ "", "", "" });
        CHECK(cfg.option<ConfigOptionFloats>("filament_colour_angle")->values == std::vector<double>{ 0., 0., 0. });
        cfg.option<ConfigOptionFloats>("filament_colour_twist")->values = { 1., 2., 3. };
        // Glow (G1): new slots follow the preset (-1) and have no colour of their own; the project-wide
        // intensity is not a slot vector and is left alone.
        CHECK(cfg.option<ConfigOptionInts>("filament_glow")->values == std::vector<int>{ -1, -1, -1 });
        CHECK(cfg.option<ConfigOptionStrings>("filament_glow_colour")->values == std::vector<std::string>{ "", "", "" });
        CHECK_FALSE(cfg.has("filament_glow_intensity"));
        cfg.option<ConfigOptionInts>("filament_glow")->values = { 1, 0, -1 };
        visual_only_resize_slots(cfg, 2);
        CHECK(cfg.option<ConfigOptionInts>("filament_glow")->values == std::vector<int>{ 1, 0 });
        visual_only_resize_slots(cfg, 3);
        CHECK(cfg.option<ConfigOptionInts>("filament_glow")->values == std::vector<int>{ 1, 0, -1 });
        visual_only_resize_slots(cfg, 2);
        CHECK(cfg.option<ConfigOptionFloats>("filament_colour_twist")->values == std::vector<double>{ 1., 2. });
        visual_only_resize_slots(cfg, 4);
        CHECK(cfg.option<ConfigOptionFloats>("filament_colour_twist")->values == std::vector<double>{ 1., 2., 0., 0. });
    }
    SECTION("erase removes one slot, move reorders, strip removes them all")
    {
        cfg.set_key_value("filament_colour_split", new ConfigOptionStrings{ "a", "b", "c" });
        cfg.set_key_value("filament_colour_angle", new ConfigOptionFloats{ 10., 20., 30. });
        cfg.set_key_value("filament_colour_twist", new ConfigOptionFloats{ 1., 2., 3. });
        cfg.set_key_value("filament_glow", new ConfigOptionInts{ 1, 0, -1 });
        cfg.set_key_value("filament_glow_colour", new ConfigOptionStrings{ "#112233", "", "#445566" });
        cfg.set_key_value("filament_glow_intensity", new ConfigOptionFloat(150.));
        visual_only_move_slot(cfg, 0, 2);
        CHECK(cfg.option<ConfigOptionInts>("filament_glow")->values == std::vector<int>{ 0, -1, 1 });
        CHECK(cfg.option<ConfigOptionStrings>("filament_glow_colour")->values == std::vector<std::string>{ "", "#445566", "#112233" });
        CHECK(cfg.option<ConfigOptionFloat>("filament_glow_intensity")->value == 150.);
        CHECK(cfg.option<ConfigOptionStrings>("filament_colour_split")->values == std::vector<std::string>{ "b", "c", "a" });
        CHECK(cfg.option<ConfigOptionFloats>("filament_colour_angle")->values == std::vector<double>{ 20., 30., 10. });
        visual_only_move_slot(cfg, 2, 0);
        CHECK(cfg.option<ConfigOptionFloats>("filament_colour_twist")->values == std::vector<double>{ 1., 2., 3. });
        CHECK(cfg.option<ConfigOptionInts>("filament_glow")->values == std::vector<int>{ 1, 0, -1 });
        visual_only_erase_slot(cfg, 1);
        CHECK(cfg.option<ConfigOptionInts>("filament_glow")->values == std::vector<int>{ 1, -1 });
        CHECK(cfg.option<ConfigOptionStrings>("filament_glow_colour")->values == std::vector<std::string>{ "#112233", "#445566" });
        CHECK(cfg.option<ConfigOptionFloat>("filament_glow_intensity")->value == 150.);
        CHECK(cfg.option<ConfigOptionStrings>("filament_colour_split")->values == std::vector<std::string>{ "a", "c" });
        CHECK(cfg.option<ConfigOptionFloats>("filament_colour_angle")->values == std::vector<double>{ 10., 30. });
        CHECK(cfg.option<ConfigOptionFloats>("filament_colour_twist")->values == std::vector<double>{ 1., 3. });
        visual_only_erase_slot(cfg, 7); // out of range: nothing
        CHECK(cfg.option<ConfigOptionFloats>("filament_colour_twist")->values.size() == 2);
        strip_visual_only_options(cfg);
        for (const std::string &key : visual_only_option_keys())
            CHECK_FALSE(cfg.has(key));
    }
}

TEST_CASE("The project's filament slots carry the visual-only keys through resize and delete", "[MultiColour][Config]")
{
    PresetBundle bundle;
    bundle.set_num_filaments(3, std::string("#FF0000"));
    DynamicPrintConfig &project = bundle.project_config;
    REQUIRE(project.option<ConfigOptionStrings>("filament_colour")->values.size() == 3);
    REQUIRE(project.has("filament_colour_split"));
    CHECK(project.option<ConfigOptionStrings>("filament_colour_split")->values.size() == 3);
    CHECK(project.option<ConfigOptionFloats>("filament_colour_angle")->values.size() == 3);
    CHECK(project.option<ConfigOptionFloats>("filament_colour_twist")->values.size() == 3);

    project.option<ConfigOptionStrings>("filament_colour_split")->values = { "30|70", "40|60", "20|80" };
    project.option<ConfigOptionFloats>("filament_colour_angle")->values  = { 10., 20., 30. };
    project.option<ConfigOptionFloats>("filament_colour_twist")->values  = { 1., 2., 3. };
    CHECK(project.option<ConfigOptionInts>("filament_glow")->values.size() == 3);
    CHECK(project.option<ConfigOptionStrings>("filament_glow_colour")->values.size() == 3);
    project.option<ConfigOptionInts>("filament_glow")->values = { 1, 0, -1 };

    // Delete the middle slot: the others keep their values.
    bundle.update_num_filaments(1);
    CHECK(project.option<ConfigOptionInts>("filament_glow")->values == std::vector<int>{ 1, -1 });
    CHECK(project.option<ConfigOptionStrings>("filament_colour_split")->values == std::vector<std::string>{ "30|70", "20|80" });
    CHECK(project.option<ConfigOptionFloats>("filament_colour_angle")->values == std::vector<double>{ 10., 30. });
    CHECK(project.option<ConfigOptionFloats>("filament_colour_twist")->values == std::vector<double>{ 1., 3. });

    // Grow: the new slot is default.
    bundle.set_num_filaments(3, std::string("#00FF00"));
    CHECK(project.option<ConfigOptionStrings>("filament_colour_split")->values == std::vector<std::string>{ "30|70", "20|80", "" });
    CHECK(project.option<ConfigOptionFloats>("filament_colour_angle")->values == std::vector<double>{ 10., 30., 0. });

}

TEST_CASE("Snapshot and restore keep the visual-only keys", "[MultiColour][Config]")
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("filament_colour_split", new ConfigOptionStrings{ "30|70", "", "20|80" });
    cfg.set_key_value("filament_colour_angle", new ConfigOptionFloats{ 10., 20., 30. });
    const DynamicPrintConfig snapshot = visual_only_snapshot(cfg);
    CHECK(snapshot.has("filament_colour_split"));
    CHECK_FALSE(snapshot.has("filament_colour_twist"));

    visual_only_resize_slots(cfg, 1); // what a switch to a one-slot printer does
    cfg.option<ConfigOptionFloats>("filament_colour_twist")->values = { 99. };
    visual_only_restore(cfg, snapshot, 3);
    CHECK(cfg.option<ConfigOptionStrings>("filament_colour_split")->values == std::vector<std::string>{ "30|70", "", "20|80" });
    CHECK(cfg.option<ConfigOptionFloats>("filament_colour_angle")->values == std::vector<double>{ 10., 20., 30. });
    // A key the snapshot lacked comes back as defaults, sized to the slots.
    CHECK(cfg.option<ConfigOptionFloats>("filament_colour_twist")->values == std::vector<double>{ 0., 0., 0. });
}

TEST_CASE("Switching printer and back keeps each slot's split, angle and twist", "[MultiColour][Config]")
{
    // The Tab printer-switch flow (Tab::select_preset, remember_printer_config): save the slots,
    // update_selections() for the new printer (it re-sizes them to that printer's remembered count),
    // then put the project's slots back.
    PresetBundle bundle;
    bundle.set_num_filaments(3, std::string("#FF0000"));
    DynamicPrintConfig &project = bundle.project_config;
    project.option<ConfigOptionStrings>("filament_multi_colors")->values = { "#FF0000|#FFFF00", "#00FF00|#0000FF", "#000000|#FFFFFF" };
    project.option<ConfigOptionStrings>("filament_colour_split")->values = { "30|70", "40|60", "20|80" };
    project.option<ConfigOptionFloats>("filament_colour_angle")->values  = { 10., 20., 30. };
    project.option<ConfigOptionFloats>("filament_colour_twist")->values  = { 1., 2., 3. };

    const std::vector<std::string> old_presets = bundle.filament_presets;
    const std::vector<std::string> old_colours = project.option<ConfigOptionStrings>("filament_colour")->values;
    const std::vector<std::string> old_multi   = project.option<ConfigOptionStrings>("filament_multi_colors")->values;
    const std::vector<int>         old_modes   = project.option<ConfigOptionInts>("filament_colour_mode")->values;
    const DynamicPrintConfig       old_visual  = visual_only_snapshot(project);

    AppConfig app_config; // remembers no filaments for the printer: one slot
    bundle.update_selections(app_config);
    CHECK(project.option<ConfigOptionStrings>("filament_colour_split")->values.size() == bundle.filament_presets.size());

    bundle.filament_presets = old_presets;
    project.option<ConfigOptionStrings>("filament_colour")->values       = old_colours;
    project.option<ConfigOptionStrings>("filament_multi_colors")->values = old_multi;
    project.option<ConfigOptionInts>("filament_colour_mode")->values     = old_modes;
    visual_only_restore(project, old_visual, old_presets.size());

    CHECK(project.option<ConfigOptionStrings>("filament_colour_split")->values == std::vector<std::string>{ "30|70", "40|60", "20|80" });
    CHECK(project.option<ConfigOptionFloats>("filament_colour_angle")->values == std::vector<double>{ 10., 20., 30. });
    CHECK(project.option<ConfigOptionFloats>("filament_colour_twist")->values == std::vector<double>{ 1., 2., 3. });
    // And they survive the slot alignment the switch ends with.
    bundle.set_num_filaments(3, std::string());
    CHECK(project.option<ConfigOptionFloats>("filament_colour_angle")->values == std::vector<double>{ 10., 20., 30. });
}

TEST_CASE("A slot given new colours starts with an equal split; angle and twist stay", "[MultiColour][Config]")
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("filament_multi_colors", new ConfigOptionStrings{ "#FF0000|#FFFF00", "#00FF00|#0000FF", "#000000|#FFFFFF" });
    cfg.set_key_value("filament_colour_split", new ConfigOptionStrings{ "30|70", "40|60", "20|80" });
    cfg.set_key_value("filament_colour_angle", new ConfigOptionFloats{ 10., 20., 30. });
    cfg.set_key_value("filament_colour_twist", new ConfigOptionFloats{ 1., 2., 3. });
    const std::vector<std::string> old_multi = cfg.option<ConfigOptionStrings>("filament_multi_colors")->values;

    // Slot 2 replaced 2 -> 2 colours (an AMS sync, the colour picker, a recommended palette).
    cfg.option<ConfigOptionStrings>("filament_multi_colors")->values[1] = "#FF00FF|#00FFFF";
    reset_colour_split_where_colours_changed(cfg, old_multi);
    CHECK(cfg.option<ConfigOptionStrings>("filament_colour_split")->values == std::vector<std::string>{ "30|70", "", "20|80" });
    CHECK(cfg.option<ConfigOptionFloats>("filament_colour_angle")->values == std::vector<double>{ 10., 20., 30. });
    CHECK(cfg.option<ConfigOptionFloats>("filament_colour_twist")->values == std::vector<double>{ 1., 2., 3. });

    // Unchanged colours: nothing to reset.
    reset_colour_split_where_colours_changed(cfg, cfg.option<ConfigOptionStrings>("filament_multi_colors")->values);
    CHECK(cfg.option<ConfigOptionStrings>("filament_colour_split")->values[0] == "30|70");
}

namespace {

struct Loaded3mf
{
    bool               stored = false, loaded = false;
    DynamicPrintConfig config;
};

Loaded3mf round_trip(DynamicPrintConfig store_config, const std::string &name)
{
    Model        src_model;
    ModelObject *object = src_model.add_object();
    object->name        = "cube";
    object->add_volume(make_cube(10., 10., 10.))->name = "cube";
    object->add_instance();
    object->ensure_on_bed();

    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());
    const std::string test_file = (tmp_root / name).string();

    StoreParams store_params;
    store_params.path     = test_file.c_str();
    store_params.model    = &src_model;
    store_params.config   = &store_config;
    store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    Loaded3mf out;
    out.stored = store_bbs_3mf(store_params);

    Model                     dst_model;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
    PlateDataPtrs             plate_data;
    std::vector<Preset*>      project_presets;
    bool                      is_bbl_3mf = false;
    Semver                    file_version;
    out.loaded = out.stored && load_bbs_3mf(test_file.c_str(), &out.config, &ctxt, &dst_model, &plate_data, &project_presets,
                                            &is_bbl_3mf, &file_version, nullptr,
                                            LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances |
                                                LoadStrategy::Silence);
    release_PlateData_list(plate_data);
    boost::filesystem::remove(test_file);
    return out;
}

DynamicPrintConfig two_dual_slots()
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_num_extruders(2);
    cfg.set_num_filaments(2);
    cfg.option<ConfigOptionFloats>("nozzle_diameter")->values         = { 0.4, 0.4 };
    cfg.option<ConfigOptionStrings>("filament_colour")->values        = { "#FF0000", "#00FF00" };
    cfg.option<ConfigOptionStrings>("filament_multi_colors")->values  = { "#FF0000|#FFFF00", "#00FF00|#0000FF|#FF0000" };
    cfg.option<ConfigOptionInts>("filament_colour_mode")->values      = { 0, 0 };
    return cfg;
}

} // namespace

TEST_CASE("The visual-only keys round-trip through a project 3MF", "[MultiColour][3mf]")
{
    SECTION("non-default values survive")
    {
        DynamicPrintConfig cfg = two_dual_slots();
        cfg.set_key_value("filament_colour_split", new ConfigOptionStrings{ "30|70", "25|25|50" });
        cfg.set_key_value("filament_colour_angle", new ConfigOptionFloats{ 137., -45. });
        cfg.set_key_value("filament_colour_twist", new ConfigOptionFloats{ 90., 0. });
        cfg.set_key_value("filament_glow", new ConfigOptionInts{ 1, 0 });
        cfg.set_key_value("filament_glow_colour", new ConfigOptionStrings{ "#7CFF6B", "" });
        cfg.set_key_value("filament_glow_intensity", new ConfigOptionFloat(150.));
        const Loaded3mf back = round_trip(cfg, "multicolour_keys.3mf");
        REQUIRE(back.stored);
        REQUIRE(back.loaded);
        REQUIRE(back.config.has("filament_colour_split"));
        CHECK(back.config.option<ConfigOptionStrings>("filament_colour_split")->values == std::vector<std::string>{ "30|70", "25|25|50" });
        CHECK(back.config.option<ConfigOptionFloats>("filament_colour_angle")->values == std::vector<double>{ 137., -45. });
        CHECK(back.config.option<ConfigOptionFloats>("filament_colour_twist")->values == std::vector<double>{ 90., 0. });
        // Glow (G1) comes back the same way.
        REQUIRE(back.config.has("filament_glow"));
        CHECK(back.config.option<ConfigOptionInts>("filament_glow")->values == std::vector<int>{ 1, 0 });
        CHECK(back.config.option<ConfigOptionStrings>("filament_glow_colour")->values == std::vector<std::string>{ "#7CFF6B", "" });
        REQUIRE(back.config.has("filament_glow_intensity"));
        CHECK(back.config.option<ConfigOptionFloat>("filament_glow_intensity")->value == 150.);
        const Filament b = from_config(back.config, 1);
        CHECK(b.n == 3);
        CHECK_THAT(b.proportion[2], WithinAbs(0.5, 1e-6));
        CHECK(b.rotation_deg == -45.f);
    }
    SECTION("a 3MF without them loads the defaults")
    {
        const Loaded3mf back = round_trip(two_dual_slots(), "multicolour_nokeys.3mf");
        REQUIRE(back.loaded);
        CHECK_FALSE(back.config.has("filament_colour_split"));
        CHECK_FALSE(back.config.has("filament_glow"));
        CHECK_FALSE(back.config.has("filament_glow_colour"));
        CHECK_FALSE(back.config.has("filament_glow_intensity"));
        const Filament a = from_config(back.config, 0);
        CHECK(a.n == 2);
        CHECK_THAT(a.proportion[0], WithinAbs(0.5, 1e-6));
        CHECK(a.rotation_deg == 0.f);
        CHECK(a.twist_deg_per_m == 0.f);
    }
}

TEST_CASE("Export Bambu 3MF drops the visual-only keys (D8)", "[MultiColour][BambuExport]")
{
    DynamicPrintConfig cfg = two_dual_slots();
    cfg.set_key_value("filament_colour_split", new ConfigOptionStrings{ "30|70", "25|25|50" });
    cfg.set_key_value("filament_colour_angle", new ConfigOptionFloats{ 137., -45. });
    cfg.set_key_value("filament_colour_twist", new ConfigOptionFloats{ 90., 0. });
    cfg.set_key_value("filament_glow", new ConfigOptionInts{ 1, 0 });
    cfg.set_key_value("filament_glow_colour", new ConfigOptionStrings{ "#7CFF6B", "" });
    cfg.set_key_value("filament_glow_intensity", new ConfigOptionFloat(150.));

    BambuExport::Context      ctx = BambuExport::Context::from_project(cfg);
    BambuExport::Report       report;
    const BambuExport::Config out = BambuExport::convert_project(cfg, ctx, report);
    for (const std::string &key : visual_only_option_keys()) {
        INFO(key);
        CHECK(out.find(key) == out.end());
        CHECK(BambuExport::find_key(key) == nullptr);
        CHECK(report.dropped.count(key) == 1);
    }
    // The colours themselves still go, under Bambu's name.
    CHECK(out.find("filament_multi_colour") != out.end());
}

TEST_CASE("Colour palette and HSV helpers (P2 picker)", "[MultiColour][Palette]")
{
    const std::vector<NamedColour> &palette = basic_palette();
    CHECK(palette.size() == 56);
    std::set<std::string> seen;
    for (const NamedColour &c : palette) {
        std::array<int, 3> rgb;
        INFO(c.name);
        CHECK(parse_rgb8(c.hex, rgb));
        CHECK(format_hex8(rgb[0], rgb[1], rgb[2]) == std::string(c.hex));
        CHECK(seen.insert(c.hex).second);
    }
    SECTION("HSV round trip") {
        for (int r = 0; r < 256; r += 51)
            for (int g = 0; g < 256; g += 51)
                for (int b = 0; b < 256; b += 51) {
                    const auto hsv = rgb8_to_hsv(r, g, b);
                    const auto rgb = hsv_to_rgb8(hsv[0], hsv[1], hsv[2]);
                    CHECK(rgb[0] == r);
                    CHECK(rgb[1] == g);
                    CHECK(rgb[2] == b);
                }
        CHECK(hsv_to_rgb8(0., 1., 1.) == std::array<int, 3>{ 255, 0, 0 });
        CHECK(hsv_to_rgb8(120., 1., 1.) == std::array<int, 3>{ 0, 255, 0 });
        CHECK(hsv_to_rgb8(240., 1., 0.5) == std::array<int, 3>{ 0, 0, 128 });
        CHECK(hsv_to_rgb8(-120., 1., 1.) == std::array<int, 3>{ 0, 0, 255 });
    }
    SECTION("hex parsing") {
        std::array<int, 3> rgb;
        CHECK(parse_rgb8("  #1e90ff ", rgb));
        CHECK(rgb == std::array<int, 3>{ 30, 144, 255 });
        CHECK(parse_rgb8("FF00ff", rgb));
        CHECK_FALSE(parse_rgb8("#12345", rgb));
        CHECK_FALSE(parse_rgb8("#12345G", rgb));
        CHECK_FALSE(parse_rgb8("", rgb));
    }
}

TEST_CASE("Swatch slices follow the proportions and the rotation (P2)", "[MultiColour][Swatch]")
{
    Filament f;
    f.n = 2;
    f.proportion = { 0.5f, 0.5f, 0.f, 0.f };
    // Rotation 0: colour 1 faces the front, which is the bottom of the swatch (y down).
    CHECK(swatch_slice(f, 0.f, 6.f, 18.f, 8.f) == 0);
    CHECK(swatch_slice(f, 0.f, -6.f, 18.f, 8.f) == 1);
    // Rotation 90: colour 1 faces the right.
    f.rotation_deg = 90.f;
    CHECK(swatch_slice(f, 10.f, 0.f, 18.f, 8.f) == 0);
    CHECK(swatch_slice(f, -10.f, 0.f, 18.f, 8.f) == 1);
    // 30/70: colour 1 covers the first 30% of the width along its axis.
    f.proportion = { 0.3f, 0.7f, 0.f, 0.f };
    CHECK(swatch_slice(f, 16.f, 0.f, 18.f, 8.f) == 0);
    CHECK(swatch_slice(f, 5.f, 0.f, 18.f, 8.f) == 1);

    // A pie for three colours: colour 1 centred on the front at rotation 0.
    Filament g;
    g.n = 3;
    g.proportion = { 0.5f, 0.25f, 0.25f, 0.f };
    CHECK(swatch_slice(g, 0.f, 6.f, 18.f, 8.f) == 0);   // front
    CHECK(swatch_slice(g, 10.f, 0.f, 18.f, 8.f) == 1);  // right
    CHECK(swatch_slice(g, -10.f, -1.f, 18.f, 8.f) == 2); // left
    Filament one;
    CHECK(swatch_slice(one, 3.f, 3.f, 18.f, 8.f) == 0);
}
