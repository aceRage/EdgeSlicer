// Glow-in-the-dark filaments, phase G1: the auto-suggest rule, the vibrant glow colour, the per-slot
// resolution of the visual-only keys and the lights-off colours Preview uses.
// tests/design_multicolour_filament_view.md section 10. The keys' strip, 3MF round trip and Bambu
// export are with the other visual-only keys in test_multicolour_filament.cpp.

#include <catch2/catch.hpp>

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Showcase/GlowFilament.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Glow;
using Catch::Matchers::WithinAbs;

namespace {

// Hue in turns [0, 1) of an sRGB colour; -1 for a grey.
float hue_of(const RGB &c)
{
    const float mx = std::max({ c[0], c[1], c[2] });
    const float mn = std::min({ c[0], c[1], c[2] });
    if (mx - mn < 1e-6f)
        return -1.f;
    float h;
    if (mx == c[0])
        h = std::fmod((c[1] - c[2]) / (mx - mn), 6.f);
    else if (mx == c[1])
        h = (c[2] - c[0]) / (mx - mn) + 2.f;
    else
        h = (c[0] - c[1]) / (mx - mn) + 4.f;
    h /= 6.f;
    return h < 0.f ? h + 1.f : h;
}
float saturation_of(const RGB &c)
{
    const float mx = std::max({ c[0], c[1], c[2] });
    return mx > 0.f ? (mx - std::min({ c[0], c[1], c[2] })) / mx : 0.f;
}
float value_of(const RGB &c) { return std::max({ c[0], c[1], c[2] }); }

DynamicPrintConfig three_slots()
{
    DynamicPrintConfig cfg;
    visual_only_resize_slots(cfg, 3);
    return cfg;
}

} // namespace

TEST_CASE("Presets whose name or type contains Glow are suggested as glow filaments", "[Glow]")
{
    CHECK(suggests_glow("Polymaker PolyLite Glow PLA", ""));
    CHECK(suggests_glow("eSUN glow in the dark PLA", "PLA"));
    CHECK(suggests_glow("Generic PLA", "PLA-GLOW"));
    CHECK(suggests_glow("GLOW", ""));
    CHECK(suggests_glow("Bambu PLA Glow @BBL X1C", ""));
    CHECK_FALSE(suggests_glow("Generic PLA", "PLA"));
    CHECK_FALSE(suggests_glow("Bambu PLA Silk @BBL X1C", "PLA"));
    CHECK_FALSE(suggests_glow("Glossy PETG", "PETG"));   // "gloss" is not "glow"
    CHECK_FALSE(suggests_glow("", ""));
}

TEST_CASE("filament_glow: -1 follows the preset, 0 and 1 override it", "[Glow]")
{
    CHECK(resolve_glow(GLOW_AUTO, "Polymaker Glow PLA", "PLA"));
    CHECK_FALSE(resolve_glow(GLOW_AUTO, "Generic PLA", "PLA"));
    // The user can turn a glow preset off, and any preset on.
    CHECK_FALSE(resolve_glow(GLOW_OFF, "Polymaker Glow PLA", "PLA"));
    CHECK(resolve_glow(GLOW_ON, "Generic PLA", "PLA"));
}

TEST_CASE("The default glow colour is a more vibrant version of the filament's own colour", "[Glow][Vibrant]")
{
    SECTION("same hue, higher saturation and value")
    {
        const std::vector<RGB> colours = {
            { 0.2f, 0.3f, 0.6f }, { 0.1f, 0.1f, 0.3f }, { 0.6f, 0.2f, 0.2f }, { 0.3f, 0.5f, 0.3f },
            { 0.9f, 0.8f, 0.3f }, { 0.4f, 0.1f, 0.5f }, { 0.8f, 0.4f, 0.1f }, { 0.f, 0.4f, 0.4f },
            { 0.7f, 0.5f, 0.55f }, { 1.f, 0.f, 0.f },
        };
        for (const RGB &c : colours) {
            INFO(c[0] << " " << c[1] << " " << c[2]);
            const RGB v = vibrant(c);
            CHECK_THAT(hue_of(v), WithinAbs(hue_of(c), 2e-3));
            CHECK(saturation_of(v) >= saturation_of(c) - 1e-5f);
            CHECK(value_of(v) >= value_of(c) - 1e-5f);
            // "more vibrant" is a real step, unless the colour is already at the limit
            CHECK((saturation_of(v) > saturation_of(c) + 0.01f || saturation_of(c) > 0.99f));
            CHECK(value_of(v) > 0.8f);
        }
    }
    SECTION("blue becomes bright blue, green bright green")
    {
        const RGB blue = vibrant(RGB{ 0.1f, 0.2f, 0.5f });
        CHECK(blue[2] > 0.85f);
        CHECK(blue[2] > blue[0]);
        CHECK(blue[2] > blue[1]);
        const RGB green = vibrant(RGB{ 0.2f, 0.45f, 0.25f });
        CHECK(green[1] > 0.85f);
        CHECK(green[1] > green[0]);
        CHECK(green[1] > green[2]);
    }
    SECTION("a worked value: 33 4D 99")
    {
        // HSV of (0.2, 0.3, 0.6): s = 2/3, v = 0.6. s' = 2/3 + 1/3 * 0.5 = 5/6, v' = 0.6 + 0.4 * 0.8 = 0.92.
        // The channels sit at v' (1 - s') and v', the middle one at the same fraction (0.25) between them.
        const RGB v = vibrant(RGB{ 0.2f, 0.3f, 0.6f });
        CHECK_THAT(v[0], WithinAbs(0.92 / 6., 1e-5));
        CHECK_THAT(v[1], WithinAbs(0.92 * (1. / 6. + 5. / 6. * 0.25), 1e-5));
        CHECK_THAT(v[2], WithinAbs(0.92, 1e-5));
    }
    SECTION("a colour that is already vivid stays as it is")
    {
        const RGB v = vibrant(RGB{ 0.f, 0.f, 1.f });
        CHECK_THAT(v[0], WithinAbs(0., 1e-6));
        CHECK_THAT(v[1], WithinAbs(0., 1e-6));
        CHECK_THAT(v[2], WithinAbs(1., 1e-6));
    }
    SECTION("greys have no hue and glow phosphor green")
    {
        for (const RGB &grey : { RGB{ 1.f, 1.f, 1.f }, RGB{ 0.f, 0.f, 0.f }, RGB{ 0.5f, 0.5f, 0.5f }, RGB{ 0.95f, 0.96f, 0.95f } })
            CHECK(vibrant(grey) == GREY_GLOW);
    }
    SECTION("hex in, hex out")
    {
        std::string out;
        REQUIRE(vibrant_hex("#334D99", out));
        CHECK(out.size() == 7);
        CHECK(out[0] == '#');
        CHECK(out == "#2759EB");   // (0.153, 0.349, 0.92) * 255 = (39.1, 88.9, 234.6)
        CHECK(vibrant_hex("#FFFFFF", out));
        CHECK(out == "#7CFF6B");
        std::string untouched = "keep";
        CHECK_FALSE(vibrant_hex("not a colour", untouched));
        CHECK(untouched == "keep");
    }
}

TEST_CASE("A slot's glow setting comes from the project keys and the preset", "[Glow][Config]")
{
    DynamicPrintConfig cfg = three_slots();
    cfg.option<ConfigOptionInts>("filament_glow")->values = { -1, 0, 1 };
    cfg.option<ConfigOptionStrings>("filament_glow_colour")->values = { "", "#00ff00", "bogus" };

    SECTION("auto follows the preset; off and on override it")
    {
        CHECK(slot_from_config(cfg, 0, "Polymaker Glow PLA", "PLA").on);
        CHECK_FALSE(slot_from_config(cfg, 0, "Generic PLA", "PLA").on);
        CHECK_FALSE(slot_from_config(cfg, 1, "Polymaker Glow PLA", "PLA").on);
        CHECK(slot_from_config(cfg, 2, "Generic PLA", "PLA").on);
    }
    SECTION("the colour: empty means derived, a valid colour is kept, a bad one is ignored")
    {
        CHECK(slot_from_config(cfg, 0, "", "").colour.empty());
        CHECK(slot_from_config(cfg, 1, "", "").colour == "#00FF00");
        CHECK(slot_from_config(cfg, 2, "", "").colour.empty());
    }
    SECTION("missing keys and slots past the vectors take the defaults (auto, derived)")
    {
        const DynamicPrintConfig empty;
        CHECK(slot_from_config(empty, 0, "Glow PLA", "").on);
        CHECK_FALSE(slot_from_config(empty, 0, "PLA", "").on);
        CHECK(slot_from_config(cfg, 7, "Glow PLA", "").on);
        CHECK(slot_from_config(cfg, 7, "Glow PLA", "").colour.empty());
    }
    SECTION("the glow colour of a slot")
    {
        Slot derived;
        derived.on = true;
        CHECK(derived.glow_srgb(RGB{ 0.f, 0.f, 1.f }) == vibrant(RGB{ 0.f, 0.f, 1.f }));
        Slot custom;
        custom.on     = true;
        custom.colour = "#FF8000";
        const RGB c   = custom.glow_srgb(RGB{ 0.f, 0.f, 1.f });
        CHECK_THAT(c[0], WithinAbs(1., 1e-6));
        CHECK_THAT(c[1], WithinAbs(128. / 255., 1e-6));
        CHECK_THAT(c[2], WithinAbs(0., 1e-6));
    }
}

TEST_CASE("The global glow intensity", "[Glow][Config]")
{
    DynamicPrintConfig cfg;
    CHECK_THAT(intensity_from_config(cfg), WithinAbs(1., 1e-6));   // missing = 100%
    cfg.set_key_value("filament_glow_intensity", new ConfigOptionFloat(150.));
    CHECK_THAT(intensity_from_config(cfg), WithinAbs(1.5, 1e-6));
    cfg.set_key_value("filament_glow_intensity", new ConfigOptionFloat(0.));
    CHECK_THAT(intensity_from_config(cfg), WithinAbs(0., 1e-6));
    cfg.set_key_value("filament_glow_intensity", new ConfigOptionFloat(9999.));
    CHECK_THAT(intensity_from_config(cfg), WithinAbs(3., 1e-6));
    cfg.set_key_value("filament_glow_intensity", new ConfigOptionFloat(-5.));
    CHECK_THAT(intensity_from_config(cfg), WithinAbs(0., 1e-6));
    CHECK(print_config_def.get("filament_glow_intensity")->default_value->serialize() == "100");
}

TEST_CASE("The bundle's slots: the auto rule through the filament presets", "[Glow][Config]")
{
    PresetBundle bundle;
    bundle.set_num_filaments(3, std::string("#FF0000"));
    REQUIRE(bundle.filament_presets.size() == 3);
    bundle.filament_presets[1] = "Polymaker PolyLite Glow PLA";
    const std::string sig_before = signature(bundle);

    std::vector<Slot> slots = slots_from_bundle(bundle);
    REQUIRE(slots.size() == 3);
    CHECK_FALSE(slots[0].on);
    CHECK(slots[1].on);
    CHECK_FALSE(slots[2].on);

    // The user turns it off for this slot, and another on.
    bundle.project_config.option<ConfigOptionInts>("filament_glow")->values = { 1, 0, -1 };
    slots = slots_from_bundle(bundle);
    CHECK(slots[0].on);
    CHECK_FALSE(slots[1].on);
    CHECK(signature(bundle) != sig_before);

    // A different preset changes the signature too (the auto rule reads its name).
    const std::string sig_a = signature(bundle);
    bundle.filament_presets[2] = "Eryone Glow PLA";
    CHECK(signature(bundle) != sig_a);

    CHECK(any_glow(slots, { 0 }));
    CHECK_FALSE(any_glow(slots, { 1 }));
    CHECK_FALSE(any_glow(slots, { 1, 5 }));
    CHECK(any_glow(slots, { 1, 0 }));
}

TEST_CASE("The shaders' glow table", "[Glow][Uniforms]")
{
    std::vector<Slot> slots(4);
    slots[1].on = true;
    slots[2].on = true;
    slots[2].colour = "#FF0000";
    const UniformTable t = pack_uniforms(slots, 1.5f);
    CHECK(t.any);
    CHECK_THAT(t.intensity, WithinAbs(1.5, 1e-6));
    CHECK(t.slot[0 * 4] == 0.f);
    CHECK(t.slot[1 * 4] == 1.f);   // its own colour, made vibrant by the shader
    CHECK(t.slot[2 * 4] == 2.f);   // an explicit colour, linear
    CHECK_THAT(t.slot[2 * 4 + 1], WithinAbs(1., 1e-6));
    CHECK_THAT(t.slot[2 * 4 + 2], WithinAbs(0., 1e-6));
    CHECK(t.slot[3 * 4] == 0.f);
    CHECK_FALSE(pack_uniforms(std::vector<Slot>(3), 1.f).any);
    CHECK_THAT(pack_uniforms(slots, -2.f).intensity, WithinAbs(0., 1e-6));
}

TEST_CASE("Preview's lights-off colours", "[Glow][Preview]")
{
    Slot off;
    Slot on;
    on.on = true;

    SECTION("ordinary filaments dim, glow filaments shine")
    {
        const std::string dim = lights_off_hex("#3060C0", off, 1.f, 0.1f);
        const std::string lit = lights_off_hex("#3060C0", on, 1.f, 0.1f);
        RGB d, l, c;
        REQUIRE(MultiColour::parse_hex(dim, d));
        REQUIRE(MultiColour::parse_hex(lit, l));
        REQUIRE(MultiColour::parse_hex("#3060C0", c));
        CHECK(value_of(d) < 0.5f * value_of(c));
        CHECK(value_of(l) > value_of(c));
        // dim keeps the hue, the glow is the vibrant version
        CHECK_THAT(hue_of(d), WithinAbs(hue_of(c), 0.03));
        CHECK_THAT(hue_of(l), WithinAbs(hue_of(c), 0.01));
        CHECK(lit != "#3060C0");   // not the daylight colour
    }
    SECTION("the intensity scales the glow, and zero is dark")
    {
        RGB strong, weak, none;
        REQUIRE(MultiColour::parse_hex(lights_off_hex("#3060C0", on, 1.f, 0.1f), strong));
        REQUIRE(MultiColour::parse_hex(lights_off_hex("#3060C0", on, 0.3f, 0.1f), weak));
        REQUIRE(MultiColour::parse_hex(lights_off_hex("#3060C0", on, 0.f, 0.1f), none));
        CHECK(value_of(strong) > value_of(weak));
        CHECK(value_of(none) < 1e-3f);
    }
    SECTION("an explicit glow colour wins over the derived one")
    {
        on.colour = "#FF0000";
        CHECK(lights_off_hex("#3060C0", on, 1.f, 0.1f) == "#FF0000");
    }
    SECTION("not a colour: returned as it came")
    {
        CHECK(lights_off_hex("auto", on, 1.f, 0.1f) == "auto");
    }

    SECTION("a split slot: every colour glows in its own vibrant version")
    {
        MultiColour::Filament f;
        f.n         = 2;
        f.colour[0] = MultiColour::srgb_to_linear(RGB{ 0.6f, 0.2f, 0.2f });
        f.colour[1] = MultiColour::srgb_to_linear(RGB{ 0.2f, 0.3f, 0.6f });
        f.proportion = { 0.5f, 0.5f, 0.f, 0.f };
        MultiColour::UniformTable table = MultiColour::pack_uniforms({ f, f }, { 1.75f, 1.75f });
        const MultiColour::UniformTable plain = table;
        std::vector<Slot> slots(2);
        slots[0].on = true;   // slot 0 glows, slot 1 is ordinary
        apply_lights_off(table, slots, 1.f, 0.1f);
        for (int i = 0; i < 2; ++i) {
            const RGB src = MultiColour::linear_to_srgb(RGB{ plain.colors[size_t(i) * 4], plain.colors[size_t(i) * 4 + 1], plain.colors[size_t(i) * 4 + 2] });
            const RGB got = MultiColour::linear_to_srgb(RGB{ table.colors[size_t(i) * 4], table.colors[size_t(i) * 4 + 1], table.colors[size_t(i) * 4 + 2] });
            const RGB want = vibrant(src);
            INFO("colour " << i);
            CHECK_THAT(got[0], WithinAbs(want[0], 1e-4));
            CHECK_THAT(got[1], WithinAbs(want[1], 1e-4));
            CHECK_THAT(got[2], WithinAbs(want[2], 1e-4));
            // slot 1: dimmed to a tenth of the light
            const size_t j = (size_t(MultiColour::MAX_COLOURS) + size_t(i)) * 4;
            CHECK_THAT(table.colors[j], WithinAbs(plain.colors[j] * 0.1, 1e-6));
            CHECK_THAT(table.colors[j + 2], WithinAbs(plain.colors[j + 2] * 0.1, 1e-6));
        }
    }
}
