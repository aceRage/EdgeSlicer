// Showcase realism, the rest of phase 1 (tests/design_showcase_realism.md 2.4 to 2.8): translucency
// (1d), the new finishes (1e), imperfections (1f), the camera effects (1g), the depth of field's
// foreground bleed and highlights (1h-2), HDRI downloads and the user's own HDRIs (1h-3). The GL-free
// maths and the scene keys; the pixels are checked by tests/showcase/golden.sh.

#include <catch2/catch.hpp>

#include "libslic3r/Showcase/ShowcaseMaterial.hpp"
#include "libslic3r/Showcase/ShowcasePanelRules.hpp"
#include "libslic3r/Showcase/ShowcaseScene.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <limits>
#include <set>

using namespace Slic3r;
using namespace Slic3r::Showcase;
using Catch::Matchers::WithinAbs;
using nlohmann::json;

namespace {

Scene round_trip(const Scene &s)
{
    Scene back;
    REQUIRE(scene_from_json(scene_to_json(s), back));
    return back;
}

json scene_json(const Scene &s) { return json::parse(scene_to_json(s)); }

} // namespace

// ------------------------------------------------------------------------------------------------
// 1d: translucency
// ------------------------------------------------------------------------------------------------

TEST_CASE("Showcase 1d: Beer-Lambert transmittance", "[Showcase][Realism]")
{
    CHECK(transmittance(0.f, 5.f) == 1.f);           // no density: clear
    CHECK(transmittance(1.f, 0.f) == 1.f);
    CHECK_THAT(transmittance(1.f, 1.f), WithinAbs(std::exp(-1.f), 1e-6));
    CHECK_THAT(transmittance(0.5f, 2.f), WithinAbs(std::exp(-1.f), 1e-6));
    // A thin wall lets much more through than a thick one (the 0.8 mm vs 5 mm part of the design).
    const float thin = transmittance(0.55f, 0.8f), thick = transmittance(0.55f, 5.f);
    CHECK(thin > 0.6f);
    CHECK(thick < 0.1f);
    CHECK(transmittance(1.f, -3.f) == 1.f);          // never more than all of it
    CHECK(transmittance(1.f, std::nanf("")) == 1.f);
}

TEST_CASE("Showcase 1d: translucency is off by default except for translucent filaments", "[Showcase][Realism]")
{
    for (Finish f : all_finishes()) {
        INFO(to_string(f));
        const Translucency t = effective_translucency(f, 0.f);
        if (f == Finish::Translucent) {
            CHECK(t.transmission > 0.5f);
            CHECK(t.density > 0.f);
            CHECK(t.sss > 0.f);
        } else {
            // At the default 0 nothing changes: no thickness pass, no extra light.
            CHECK_FALSE(t.active());
            CHECK(t.transmission == 0.f);
            CHECK(t.sss == 0.f);
        }
        // Never more than all the light.
        const Translucency full = effective_translucency(f, 1.f);
        CHECK(full.transmission <= 1.f);
        CHECK(full.sss <= 1.f);
        CHECK(full.density >= 0.f);
        // Out of range is clamped.
        const Translucency over = effective_translucency(f, 7.f);
        CHECK(over.transmission == full.transmission);
        CHECK(effective_translucency(f, std::nanf("")).transmission == effective_translucency(f, 0.f).transmission);
    }
    // Plain PLA gets soft edges and a little glow; metal pigment stops the light.
    CHECK(effective_translucency(Finish::Standard, 1.f).sss > 0.f);
    CHECK(effective_translucency(Finish::Standard, 1.f).transmission > 0.f);
    CHECK_FALSE(effective_translucency(Finish::Metallic, 1.f).active());
    // A clear plastic lets more through, and lets it through more easily, than a plain one.
    CHECK(effective_translucency(Finish::Transparent, 1.f).transmission > effective_translucency(Finish::Standard, 1.f).transmission);
    CHECK(effective_translucency(Finish::Transparent, 1.f).density < effective_translucency(Finish::Standard, 1.f).density);
    // Filled plastics only soften their edges.
    CHECK(effective_translucency(Finish::CarbonFiber, 1.f).transmission == 0.f);
    CHECK(effective_translucency(Finish::CarbonFiber, 1.f).sss > 0.f);
}

TEST_CASE("Showcase 1d: a translucent filament throws a lighter, coloured shadow", "[Showcase][Realism]")
{
    const std::array<float, 3> red{ { 0.8f, 0.05f, 0.05f } };
    const std::array<float, 3> none = shadow_transmittance(red, 0.f);
    CHECK(none[0] == 0.f);   // opaque: a full shadow
    CHECK(none[1] == 0.f);
    const std::array<float, 3> t = shadow_transmittance(red, 0.85f);
    CHECK(t[0] > t[1]);      // red light comes through
    CHECK(t[0] > 0.5f);
    CHECK(t[1] > 0.f);       // a little of everything
    for (float c : shadow_transmittance({ { 2.f, -1.f, 0.5f } }, 3.f)) {
        CHECK(c >= 0.f);
        CHECK(c <= 1.f);
    }
}

TEST_CASE("Showcase 1d: the translucency key round-trips and is written only when set", "[Showcase][Realism]")
{
    Scene s = make_scene(PresetId::StudioWhite, 0);
    CHECK(s.materials.translucency == 0.f);
    CHECK_FALSE(scene_json(s)["materials"].contains("translucency"));
    s.materials.translucency = 0.4f;
    CHECK(scene_json(s)["materials"]["translucency"].get<float>() == 0.4f);
    Scene back = round_trip(s);
    CHECK(back.materials.translucency == 0.4f);
    CHECK(back == s);
    // Clamped on read.
    json j = scene_json(s);
    j["materials"]["translucency"] = 9.0;
    Scene clamped;
    REQUIRE(scene_from_json(j.dump(), clamped));
    CHECK(clamped.materials.translucency == 1.f);
    // Set back to 0 it is dropped again.
    back.materials.translucency = 0.f;
    CHECK_FALSE(scene_json(round_trip(back))["materials"].contains("translucency"));
}

TEST_CASE("Showcase 1d: the Translucency row is for the model", "[Showcase][Realism][panel]")
{
    Scene s = make_scene(PresetId::StudioWhite, 0);
    CHECK(panel_visibility(s, false).mat_translucency);
    s.source = "toolpaths";
    CHECK_FALSE(panel_visibility(s, false).mat_translucency);
}

// ------------------------------------------------------------------------------------------------
// 1e: finishes
// ------------------------------------------------------------------------------------------------

TEST_CASE("Showcase 1e: the new finishes have stable names and keep the old ones", "[Showcase][Realism]")
{
    // The JSON names of the seven older finishes and their enum values never change.
    CHECK(std::string(to_string(Finish::Standard)) == "standard");
    CHECK(std::string(to_string(Finish::CarbonFiber)) == "carbon_fiber");
    CHECK(int(Finish::CarbonFiber) == 6);
    const std::pair<Finish, const char *> names[] = { { Finish::Translucent, "translucent" }, { Finish::Satin, "satin" },
                                                      { Finish::Sparkle, "sparkle" },         { Finish::Marble, "marble" },
                                                      { Finish::Wood, "wood" },               { Finish::MatteTextured, "matte_textured" } };
    for (const auto &[f, n] : names) {
        CHECK(std::string(to_string(f)) == n);
        Finish back = Finish::Standard;
        CHECK(from_string(n, back));
        CHECK(back == f);
    }
    // Every finish is offered once, and no reserved name is taken.
    std::set<std::string> seen;
    for (Finish f : all_finishes())
        CHECK(seen.insert(to_string(f)).second);
    CHECK(seen.size() == 13);
    for (const std::string &r : reserved_finish_names())
        CHECK(seen.count(r) == 0);
}

TEST_CASE("Showcase 1e: the older finishes keep their parameters", "[Showcase][Realism]")
{
    // The goldens of every older scene depend on these being exactly as before 1e.
    for (Finish f : { Finish::Standard, Finish::Matte, Finish::Silk, Finish::Gloss, Finish::Transparent, Finish::Metallic, Finish::CarbonFiber }) {
        INFO(to_string(f));
        const MaterialParams p = material_params(f);
        CHECK(p.transmission == 0.f);
        CHECK(p.density == 0.f);
        CHECK(p.sss == 0.f);
        CHECK(p.sheen == 0.f);
        CHECK(p.sparkle == 0.f);
        CHECK(p.speckle == 0.f);
        CHECK(p.speckle_kind == SpeckleKind::None);
    }
    CHECK(material_params(Finish::Standard).roughness == 0.55f);
    CHECK(material_params(Finish::Transparent).alpha == 0.55f);
    CHECK(material_params(Finish::CarbonFiber).fiber_noise == 0.6f);
}

TEST_CASE("Showcase 1e: what the new finishes do", "[Showcase][Realism]")
{
    const MaterialParams tl = material_params(Finish::Translucent);
    CHECK(tl.alpha < 1.f);
    CHECK(tl.alpha > material_params(Finish::Transparent).alpha);   // milky, not glass
    CHECK(tl.transmission > 0.f);
    CHECK(material_params(Finish::Sparkle).sparkle > 0.f);
    CHECK(material_params(Finish::Satin).sheen > 0.f);
    CHECK(material_params(Finish::Satin).roughness < material_params(Finish::Matte).roughness);
    CHECK(material_params(Finish::Satin).roughness > material_params(Finish::Silk).roughness);
    CHECK(material_params(Finish::Marble).speckle_kind == SpeckleKind::Stone);
    CHECK(material_params(Finish::Wood).speckle_kind == SpeckleKind::Wood);
    CHECK(material_params(Finish::MatteTextured).sheen > 0.f);
    CHECK(material_params(Finish::MatteTextured).fiber_noise > 0.f);
    for (Finish f : all_finishes()) {
        const ToolpathLook l = toolpath_look(f, -1.f);
        CHECK(l.smoothness > 0.f);
        CHECK(l.smoothness <= 1.f);
    }
}

TEST_CASE("Showcase 1e: the finish is guessed from the new words", "[Showcase][Realism]")
{
    struct Case { const char *name; const char *type; Finish expected; };
    const Case cases[] = {
        { "Eryone Galaxy PLA Black", "PLA", Finish::Sparkle },
        { "Sunlu Glitter PETG", "PETG", Finish::Sparkle },
        { "Starlight Blue PLA", "PLA", Finish::Sparkle },
        { "Polymaker PolyLite PLA Natural", "PLA", Finish::Translucent },
        { "Clear PETG", "PETG", Finish::Translucent },
        { "Generic PETG Transparent", "PETG", Finish::Transparent },
        { "Satin PLA Red", "PLA", Finish::Satin },
        { "Eryone Marble PLA", "PLA", Finish::Marble },
        { "Granite PLA", "PLA", Finish::Marble },
        { "Generic Wood PLA", "PLA-WOOD", Finish::Wood },
        { "Natural Wood PLA", "PLA", Finish::Wood },   // the fill wins over "natural"
        { "Proto-pasta Copperfill", "PLA", Finish::Metallic },
        { "Generic PETG", "PETG", Finish::Gloss },     // unchanged
        { "Generic PLA", "PLA", Finish::Standard },    // unchanged
    };
    for (const Case &c : cases) {
        INFO(c.name << " / " << c.type);
        CHECK(infer_finish(c.name, c.type) == c.expected);
    }
    // Whole words: a "Stonewall" brand is not stone, "MatterHackers" is not matte, "Unclear" is not clear.
    CHECK(infer_finish("Stonewall PLA", "PLA") == Finish::Standard);
    CHECK(infer_finish("MatterHackers Build PLA", "PLA") == Finish::Standard);
    CHECK(infer_finish("Unclear PLA", "PLA") == Finish::Standard);
}

TEST_CASE("Showcase 1e: flakes fade to their haze under a pixel", "[Showcase][Realism]")
{
    CHECK(sparkle_lod(0.f) == 0.f);
    CHECK(sparkle_lod(0.75f) == 0.f);
    CHECK(sparkle_lod(2.f) == 1.f);
    CHECK(sparkle_lod(50.f) == 1.f);
    CHECK(sparkle_lod(1.f) > 0.f);
    CHECK(sparkle_lod(1.f) < sparkle_lod(1.5f));
    CHECK(sparkle_lod(std::numeric_limits<float>::infinity()) == 1.f);
}

TEST_CASE("Showcase 1e: a new finish override round-trips; an unknown one falls back", "[Showcase][Realism]")
{
    Scene s = make_scene(PresetId::StudioWhite, 0);
    const Finish sparkle = Finish::Sparkle;
    s.materials.set_override(2, &sparkle);
    Scene back = round_trip(s);
    REQUIRE(back.materials.override_for(2) != nullptr);
    CHECK(*back.materials.override_for(2) == Finish::Sparkle);
    // A finish from a newer build is not taken: the inference applies.
    json j = scene_json(s);
    j["materials"]["filaments"] = json::array({ json{ { "slot", 1 }, { "finish", "holographic" } } });
    Scene newer;
    REQUIRE(scene_from_json(j.dump(), newer));
    CHECK(newer.materials.override_for(1) == nullptr);
}

// ------------------------------------------------------------------------------------------------
// 1f: imperfections
// ------------------------------------------------------------------------------------------------

namespace {
PathPoint pt(PathKind k, float x, float y, float z, int e = 0)
{
    PathPoint p;
    p.kind     = k;
    p.x        = x;
    p.y        = y;
    p.z        = z;
    p.extruder = e;
    return p;
}
} // namespace

TEST_CASE("Showcase 1f: the seam table is sorted by height, without duplicates", "[Showcase][Realism]")
{
    const std::vector<PathPoint> moves = {
        pt(PathKind::Extrude, 1, 1, 0.2f), pt(PathKind::Seam, 5, 5, 0.4f), pt(PathKind::Seam, 3, 3, 0.2f),
        pt(PathKind::Seam, 3, 3, 0.2f),    // the same point twice
        pt(PathKind::Travel, 9, 9, 0.4f),  pt(PathKind::Seam, 1, 2, 0.4f),
    };
    const std::vector<SeamPoint> s = seam_points(moves);
    REQUIRE(s.size() == 3);
    CHECK(s[0].z == 0.2f);
    CHECK(s[1].z == 0.4f);
    CHECK(s[1].x == 1.f);   // by x within a layer
    CHECK(s[2].x == 5.f);
    CHECK(seam_points({}).empty());
    // Thinned evenly past the cap, still sorted.
    std::vector<PathPoint> many;
    for (int i = 0; i < 1000; ++i)
        many.push_back(pt(PathKind::Seam, float(i % 7), 0.f, 0.2f * float(i)));
    const std::vector<SeamPoint> thin = seam_points(many, 100);
    CHECK(thin.size() == 100);
    for (size_t i = 1; i < thin.size(); ++i)
        CHECK(thin[i].z >= thin[i - 1].z);
}

TEST_CASE("Showcase 1f: strings hang on travels across gaps", "[Showcase][Realism]")
{
    // Island A ends at (0, 0), a retraction, a travel to (10, 0), island B starts: one candidate.
    std::vector<PathPoint> moves;
    for (int n = 0; n < 400; ++n) {
        const float z = 0.2f * float(n + 1);
        moves.push_back(pt(PathKind::Extrude, -5.f, 0.f, z, 1));
        moves.push_back(pt(PathKind::Extrude, 0.f, 0.f, z, 1));
        moves.push_back(pt(PathKind::Other, 0.f, 0.f, z));          // retract
        moves.push_back(pt(PathKind::Travel, 10.f, 0.f, z));
        moves.push_back(pt(PathKind::Other, 10.f, 0.f, z));         // unretract
        moves.push_back(pt(PathKind::Extrude, 15.f, 0.f, z, 1));
        // A short hop (under 2 mm) and a long one (over 30 mm) never get a string.
        moves.push_back(pt(PathKind::Travel, 16.f, 0.f, z));
        moves.push_back(pt(PathKind::Extrude, 18.f, 0.f, z, 1));
        moves.push_back(pt(PathKind::Travel, 80.f, 0.f, z));
        moves.push_back(pt(PathKind::Extrude, 85.f, 0.f, z, 1));
    }
    CHECK(string_wisps(moves, 0.f).empty());   // off
    const std::vector<StringWisp> some = string_wisps(moves, 1.f, 10000);
    REQUIRE_FALSE(some.empty());
    // About a quarter of the candidates, and only the 0 -> 10 mm hops.
    CHECK(some.size() <= 150);   // a quarter of 400 on average
    for (const StringWisp &w : some) {
        CHECK(w.a[0] == 0.f);
        CHECK(w.b[0] == 10.f);
        CHECK(w.a[2] == w.b[2]);
        CHECK(w.sag > 0.f);
        CHECK(w.extruder == 1);
    }
    // The same ones every time (no flicker in a video); fewer at a lower strength; capped.
    CHECK(string_wisps(moves, 1.f, 10000).size() == some.size());
    CHECK(string_wisps(moves, 0.3f, 10000).size() < some.size());
    CHECK(string_wisps(moves, 1.f, 5).size() == 5);
    // A travel that changes layer is no string.
    std::vector<PathPoint> up = { pt(PathKind::Extrude, 0, 0, 0.2f), pt(PathKind::Travel, 10, 0, 0.4f), pt(PathKind::Extrude, 12, 0, 0.4f) };
    CHECK(string_wisps(up, 1.f).empty());
}

TEST_CASE("Showcase 1f: a string sags between its ends; the seam bump is smooth", "[Showcase][Realism]")
{
    StringWisp w;
    w.a   = { 0.f, 0.f, 5.f };
    w.b   = { 10.f, 0.f, 5.f };
    w.sag = 0.5f;
    CHECK(wisp_point(w, 0.f) == w.a);
    CHECK(wisp_point(w, 1.f) == w.b);
    const std::array<float, 3> mid = wisp_point(w, 0.5f);
    CHECK_THAT(mid[0], WithinAbs(5.f, 1e-6));
    CHECK_THAT(mid[2], WithinAbs(4.5f, 1e-6));   // the full sag at the middle
    CHECK(wisp_point(w, 0.25f)[2] > mid[2]);

    CHECK(seam_bump(0.f, SEAM_WIDTH_MM) == 1.f);
    CHECK(seam_bump(SEAM_WIDTH_MM, SEAM_WIDTH_MM) == 0.f);
    CHECK(seam_bump(5.f, SEAM_WIDTH_MM) == 0.f);
    CHECK(seam_bump(-0.1f, SEAM_WIDTH_MM) == seam_bump(0.1f, SEAM_WIDTH_MM));
    CHECK(seam_bump(0.1f, SEAM_WIDTH_MM) > seam_bump(0.3f, SEAM_WIDTH_MM));
    CHECK(seam_bump(0.1f, 0.f) == 0.f);
    CHECK(seam_height_mm(0.f) == 0.f);
    CHECK(seam_height_mm(1.f) > 0.f);
    CHECK(seam_height_mm(5.f) == seam_height_mm(1.f));
    CHECK(wobble_amplitude_mm(0.f) == 0.f);
    CHECK(wobble_amplitude_mm(1.f) <= 0.05f);   // the design's 0.02-0.05 mm
}

TEST_CASE("Showcase 1f: the imperfections round-trip and are written only when on", "[Showcase][Realism]")
{
    Scene s = make_scene(PresetId::StudioWhite, 0);
    CHECK_FALSE(s.materials.imperfections.any());
    CHECK_FALSE(scene_json(s)["materials"].contains("imperfections"));
    s.materials.imperfections.wobble  = 0.3f;
    s.materials.imperfections.strings = 1.f;
    const json j = scene_json(s);
    REQUIRE(j["materials"].contains("imperfections"));
    CHECK(j["materials"]["imperfections"]["wobble"].get<float>() == 0.3f);
    Scene back = round_trip(s);
    CHECK(back.materials.imperfections == s.materials.imperfections);
    CHECK(back == s);
    // Unknown keys inside are kept; out of range is clamped.
    json k = j;
    k["materials"]["imperfections"]["elephant_foot"] = 0.5;
    k["materials"]["imperfections"]["seam"]          = -2.0;
    Scene read;
    REQUIRE(scene_from_json(k.dump(), read));
    CHECK(read.materials.imperfections.seam == 0.f);
    CHECK(json::parse(scene_to_json(read))["materials"]["imperfections"]["elephant_foot"].get<double>() == 0.5);
    // All back to 0: dropped.
    back.materials.imperfections = Imperfections();
    CHECK_FALSE(scene_json(round_trip(back))["materials"].contains("imperfections"));
}

TEST_CASE("Showcase 1f: the imperfection rows are for the model", "[Showcase][Realism][panel]")
{
    Scene s = make_scene(PresetId::StudioWhite, 0);
    CHECK(panel_visibility(s, false).mat_imperfections);
    s.source = "toolpaths";
    CHECK_FALSE(panel_visibility(s, false).mat_imperfections);
}
