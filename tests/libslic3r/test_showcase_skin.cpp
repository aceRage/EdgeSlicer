// Showcase realism 1c: the top / bottom skin's line direction from the print settings
// (Showcase::skin_tangent_mode, skin_settings) and the layer index the shader mirrors
// (LayerTable::index_at, uniform_layer_index_at), in src/libslic3r/Showcase/ShowcaseMaterial.

#include <catch2/catch.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Showcase/ShowcaseMaterial.hpp"

#include <algorithm>
#include <cmath>

using namespace Slic3r;
using namespace Slic3r::Showcase;
using Catch::Matchers::WithinAbs;

namespace {

SkinTangent skin(const std::string &pattern, double direction = 45., const std::string &rotate_template = "",
                 double surface_direction = -1., bool align = false, double rotation = 0.)
{
    SkinSettings s;
    s.pattern                = pattern;
    s.solid_infill_direction = direction;
    s.rotate_template        = rotate_template;
    s.surface_direction      = surface_direction;
    s.align_to_model         = align;
    s.model_rotation_deg     = rotation;
    return skin_tangent_mode(s);
}

void check_angles(const SkinTangent &t, std::initializer_list<float> expected)
{
    REQUIRE(t.pattern == SkinPattern::Lines);
    REQUIRE(t.angles_deg.size() == expected.size());
    size_t i = 0;
    for (float e : expected) {
        INFO("angle " << i);
        CHECK_THAT(t.angles_deg[i++], WithinAbs(e, 1e-4));
    }
}

} // namespace

TEST_CASE("Skin lines turn 90 degrees a layer by default", "[Showcase][ShowcaseSkin]")
{
    const SkinTangent t = skin("monotonicline");
    check_angles(t, { 45.f, 135.f });
    CHECK_FALSE(t.zigzag);
    CHECK_THAT(t.angle_at(0), WithinAbs(45., 1e-4));
    CHECK_THAT(t.angle_at(1), WithinAbs(135., 1e-4));
    CHECK_THAT(t.angle_at(6), WithinAbs(45., 1e-4));
    const std::array<float, 2> d = t.direction_at(0);
    CHECK_THAT(d[0], WithinAbs(std::sqrt(0.5), 1e-5));
    CHECK_THAT(d[1], WithinAbs(std::sqrt(0.5), 1e-5));

    check_angles(skin("monotonic", 0.), { 0.f, 90.f });
    CHECK_FALSE(skin("monotonic").zigzag);
    const SkinTangent rect = skin("rectilinear", 10.);
    check_angles(rect, { 10.f, 100.f });
    CHECK(rect.zigzag);
}

TEST_CASE("Aligned rectilinear and a set surface direction do not turn", "[Showcase][ShowcaseSkin]")
{
    check_angles(skin("alignedrectilinear", 30.), { 30.f });
    CHECK(skin("alignedrectilinear").zigzag);
    // top_layer_direction / bottom_layer_direction win over the solid infill direction and a template.
    check_angles(skin("monotonicline", 45., "", 20.), { 20.f });
    check_angles(skin("rectilinear", 45., "0,90", 70.), { 70.f });
}

TEST_CASE("A plain rotate template gives each layer its absolute angle", "[Showcase][ShowcaseSkin]")
{
    // Fill.cpp: angle = template[layer % n]; the solid infill direction is not added.
    check_angles(skin("monotonicline", 45., "0,90"), { 0.f, 90.f });
    const SkinTangent t = skin("rectilinear", 45., "0, 45 ,90");
    check_angles(t, { 0.f, 45.f, 90.f });
    CHECK_THAT(t.angle_at(4), WithinAbs(45., 1e-4));
    check_angles(skin("alignedrectilinear", 45., "15"), { 15.f });
    // The rotate metalanguage is not modelled: the highlight goes round.
    for (const char *tpl : { "+30", "-30", "45%", "0,90#", "30/90", "x" }) {
        INFO(tpl);
        CHECK(skin("monotonicline", 45., tpl).pattern == SkinPattern::Isotropic);
    }
}

TEST_CASE("Align to model adds the object's rotation", "[Showcase][ShowcaseSkin]")
{
    check_angles(skin("monotonicline", 45., "", -1., true, 30.), { 75.f, 165.f });
    // Kept in [0, 180): a line and its reverse.
    check_angles(skin("alignedrectilinear", 170., "", -1., true, 30.), { 20.f });
    check_angles(skin("alignedrectilinear", 10., "", -1., true, -30.), { 160.f });
    // Without the option the rotation is ignored.
    check_angles(skin("alignedrectilinear", 10., "", -1., false, 30.), { 10.f });
}

TEST_CASE("Concentric skins and patterns without a direction", "[Showcase][ShowcaseSkin]")
{
    CHECK(skin("concentric").pattern == SkinPattern::Concentric);
    CHECK(skin("spiralinset").pattern == SkinPattern::Concentric);
    const std::array<float, 2> ring = SkinTangent::ring_direction(5.f, 0.f, 0.f, 0.f);
    CHECK_THAT(ring[0], WithinAbs(0., 1e-6));
    CHECK_THAT(ring[1], WithinAbs(1., 1e-6));
    const std::array<float, 2> centre = SkinTangent::ring_direction(1.f, 1.f, 1.f, 1.f);
    CHECK(centre[0] == 0.f);
    CHECK(centre[1] == 0.f);
    for (const char *p : { "hilbertcurve", "archimedeanchords", "octagramspiral", "", "nonsense" }) {
        INFO(p);
        const SkinTangent t = skin(p);
        CHECK(t.pattern == SkinPattern::Isotropic);
        CHECK(t.angles_deg.empty());
        CHECK(t.angle_at(3) == 0.f);
        CHECK(t.direction_at(3)[0] == 0.f);
    }
}

TEST_CASE("Skin settings come from the object's print config", "[Showcase][ShowcaseSkin]")
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_deserialize_strict({ { "top_surface_pattern", "rectilinear" },
                                 { "bottom_surface_pattern", "concentric" },
                                 { "solid_infill_direction", "30" },
                                 { "top_layer_direction", "-1" },
                                 { "bottom_layer_direction", "60" },
                                 { "solid_infill_rotate_template", "" },
                                 { "align_infill_direction_to_model", "1" } });
    const SkinSettings top = skin_settings(cfg, true, 15.);
    CHECK(top.pattern == "rectilinear");
    CHECK_THAT(top.solid_infill_direction, WithinAbs(30., 1e-9));
    CHECK(top.surface_direction < 0.);
    CHECK(top.align_to_model);
    CHECK_THAT(top.model_rotation_deg, WithinAbs(15., 1e-9));
    check_angles(skin_tangent_mode(top), { 45.f, 135.f });

    const SkinSettings bottom = skin_settings(cfg, false, 0.);
    CHECK(bottom.pattern == "concentric");
    CHECK_THAT(bottom.surface_direction, WithinAbs(60., 1e-9));
    CHECK(skin_tangent_mode(bottom).pattern == SkinPattern::Concentric);

    // The defaults of a fresh config: monotonic line at 45 degrees, turning.
    const SkinSettings def = skin_settings(DynamicPrintConfig::full_print_config(), true, 0.);
    check_angles(skin_tangent_mode(def), { 45.f, 135.f });
}

TEST_CASE("The layer index the shader picks a skin angle with", "[Showcase][ShowcaseSkin]")
{
    // Uniform: a 0.2 mm first layer, then 0.2 mm.
    CHECK(uniform_layer_index_at(0.05f, 0.2f, 0.2f) == 0);
    CHECK(uniform_layer_index_at(0.199f, 0.2f, 0.2f) == 0);
    CHECK(uniform_layer_index_at(0.21f, 0.2f, 0.2f) == 1);
    CHECK(uniform_layer_index_at(0.399f, 0.2f, 0.2f) == 1);
    CHECK(uniform_layer_index_at(0.41f, 0.2f, 0.2f) == 2);
    CHECK(uniform_layer_index_at(-1.f, 0.2f, 0.2f) == 0);
    // It agrees with uniform_layer_at()'s layer.
    for (float z = 0.01f; z < 3.f; z += 0.037f) {
        INFO(z);
        const LayerSpan s = uniform_layer_at(z, 0.25f, 0.15f);
        const int       i = uniform_layer_index_at(z, 0.25f, 0.15f);
        CHECK_THAT(s.bottom, WithinAbs(i == 0 ? 0. : 0.25 + 0.15 * (i - 1), 1e-4));
    }

    // A table (variable layer heights): tops 0.2, 0.4, 0.5.
    const LayerTable table({ 0., 0.2, 0.2, 0.4, 0.4, 0.5 });
    REQUIRE_FALSE(table.uniform());
    CHECK(table.index_at(0.1f) == 0);
    CHECK(table.index_at(0.199f) == 0);
    CHECK(table.index_at(0.3f) == 1);
    CHECK(table.index_at(0.45f) == 2);
    // Past the last layer: that layer continued.
    CHECK(table.index_at(0.55f) == 3);
    CHECK(table.index_at(0.65f) == 4);
    CHECK(LayerTable().index_at(1.f) == 0);
}

TEST_CASE("The multi-colour silk finish name is kept back", "[Showcase][ShowcaseSkin]")
{
    const std::vector<std::string> &reserved = reserved_finish_names();
    REQUIRE(std::find(reserved.begin(), reserved.end(), "multicolour_silk") != reserved.end());
    for (const std::string &name : reserved) {
        Finish f;
        CHECK_FALSE(from_string(name, f));
        for (Finish known : all_finishes())
            CHECK(name != to_string(known));
    }
}
