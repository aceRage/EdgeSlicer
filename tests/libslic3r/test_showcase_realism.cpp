// Showcase realism, the rest of phase 1 (tests/design_showcase_realism.md 2.4 to 2.8): translucency
// (1d), the new finishes (1e), imperfections (1f), the camera effects (1g), the depth of field's
// foreground bleed and highlights (1h-2), HDRI downloads and the user's own HDRIs (1h-3). The GL-free
// maths and the scene keys; the pixels are checked by tests/showcase/golden.sh.

#include <catch2/catch.hpp>

#include "libslic3r/Showcase/ShowcaseHdr.hpp"
#include "libslic3r/Showcase/ShowcaseHdriLibrary.hpp"
#include "libslic3r/Showcase/ShowcaseLens.hpp"
#include "libslic3r/Showcase/ShowcaseMaterial.hpp"
#include "libslic3r/Showcase/ShowcasePanelRules.hpp"
#include "libslic3r/Showcase/ShowcaseScene.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>

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
    // About 15 % of the candidates, and only the 0 -> 10 mm hops.
    CHECK(some.size() <= 100);   // 60 of 400 on average
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

// ------------------------------------------------------------------------------------------------
// 1g: camera effects
// ------------------------------------------------------------------------------------------------

TEST_CASE("Showcase 1g: the frame radius and the vignette", "[Showcase][Realism]")
{
    CHECK_THAT(frame_radius(0.5, 0.5, 16. / 9.), WithinAbs(0., 1e-12));
    CHECK_THAT(frame_radius(0., 0., 16. / 9.), WithinAbs(1., 1e-12));
    CHECK_THAT(frame_radius(1., 1., 1.), WithinAbs(1., 1e-12));
    CHECK(frame_radius(1., 0.5, 2.) < 1.);   // the middle of a side is nearer than a corner
    // Off: nothing changes anywhere.
    for (double r : { 0., 0.5, 1. })
        CHECK(vignette_factor(0., r) == 1.);
    // On: the centre stays, the corners darken, smoothly and monotonically.
    CHECK_THAT(vignette_factor(1., 0.), WithinAbs(1., 1e-12));
    CHECK(vignette_factor(1., 1.) < 0.6);
    CHECK(vignette_factor(1., 1.) > 0.45);
    CHECK(vignette_factor(1., 0.5) > vignette_factor(1., 1.));
    CHECK(vignette_factor(0.3, 1.) > vignette_factor(1., 1.));
    CHECK(vignette_factor(5., 1.) == vignette_factor(1., 1.));
}

TEST_CASE("Showcase 1g: barrel distortion keeps the corners and the centre", "[Showcase][Realism]")
{
    const auto c = distort_frame_point(0.5, 0.5, 1.5, 1.);
    CHECK_THAT(c[0], WithinAbs(0.5, 1e-12));
    CHECK_THAT(c[1], WithinAbs(0.5, 1e-12));
    const auto k = distort_frame_point(1., 1., 1.5, 1.);
    CHECK_THAT(k[0], WithinAbs(1., 1e-12));
    CHECK_THAT(k[1], WithinAbs(1., 1e-12));
    // In between, the picture is taken from nearer the centre: magnified in the middle.
    const auto m = distort_frame_point(0.75, 0.5, 1.5, 1.);
    CHECK(m[0] < 0.75);
    CHECK(m[0] > 0.5);
    const auto off = distort_frame_point(0.75, 0.3, 1.5, 0.);
    CHECK(off[0] == 0.75);
    CHECK(off[1] == 0.3);
    CHECK(chromatic_offset_px(0., 1.) == 0.);
    CHECK_THAT(chromatic_offset_px(1., 1.), WithinAbs(1.5, 1e-12));
    CHECK(chromatic_offset_px(1., 0.) == 0.);
}

TEST_CASE("Showcase 1g: film grain is deterministic per frame and varies between frames", "[Showcase][Realism]")
{
    // The same pixel and seed: the same value, run after run (a video frame renders the same twice).
    CHECK(grain_noise(10, 20, 3) == grain_noise(10, 20, 3));
    double sum = 0., sum2 = 0.;
    int    differ = 0;
    const int n = 4096;
    for (int i = 0; i < n; ++i) {
        const double v = grain_noise(uint32_t(i % 64), uint32_t(i / 64), 7);
        CHECK(v >= 0.);
        CHECK(v < 1.);
        sum += v;
        sum2 += v * v;
        if (grain_noise(uint32_t(i % 64), uint32_t(i / 64), 8) != v)
            ++differ;
    }
    const double mean = sum / n, var = sum2 / n - mean * mean;
    CHECK_THAT(mean, WithinAbs(0.5, 0.03));          // uniform: mean 1/2, variance 1/12
    CHECK_THAT(var, WithinAbs(1. / 12., 0.01));
    CHECK(differ > n * 9 / 10);                      // a new frame, new grain
    // Most grain in the mid-tones.
    CHECK(grain_weight(0.5) > grain_weight(0.05));
    CHECK(grain_weight(0.5) > grain_weight(0.95));
    CHECK(grain_weight(0.) > 0.);
    CHECK(bloom_bright(0.5) == 0.);
    CHECK_THAT(bloom_bright(3.), WithinAbs(2., 1e-12));
    CHECK(bloom_bright(std::nan("")) == 0.);
}

TEST_CASE("Showcase 1g: the lens keys round-trip and are written only when on", "[Showcase][Realism]")
{
    Scene s = make_scene(PresetId::StudioWhite, 0);
    CHECK_FALSE(s.lens.any());
    CHECK_FALSE(scene_json(s).contains("lens"));
    s.lens.grain     = 0.2f;
    s.lens.vignette  = 0.35f;
    s.lens.bloom     = 0.5f;
    s.lens.chromatic = 0.1f;
    s.lens.distortion = 0.25f;
    const json j = scene_json(s);
    REQUIRE(j.contains("lens"));
    CHECK(j["lens"]["vignette"].get<float>() == 0.35f);
    Scene back = round_trip(s);
    CHECK(back.lens == s.lens);
    CHECK(back == s);
    json bad = j;
    bad["lens"]["grain"] = 4.0;
    Scene clamped;
    REQUIRE(scene_from_json(bad.dump(), clamped));
    CHECK(clamped.lens.grain == 1.f);
    back.lens = LensEffects();
    CHECK_FALSE(scene_json(round_trip(back)).contains("lens"));
}

// ------------------------------------------------------------------------------------------------
// 1h-2, the deferred part
// ------------------------------------------------------------------------------------------------

TEST_CASE("Showcase 1h-2: the depth of field's spill and highlights keys", "[Showcase][Realism]")
{
    Scene s = make_scene(PresetId::StudioWhite, 0);
    s.dof.enabled = true;
    // A 1h-2 scene writes back without the new keys.
    json j = scene_json(s);
    REQUIRE(j.contains("dof"));
    CHECK_FALSE(j["dof"].contains("near_bleed"));
    CHECK_FALSE(j["dof"].contains("highlights"));
    s.dof.near_bleed = true;
    s.dof.highlights = 0.6f;
    j = scene_json(s);
    CHECK(j["dof"]["near_bleed"].get<bool>());
    CHECK(j["dof"]["highlights"].get<float>() == 0.6f);
    Scene back = round_trip(s);
    CHECK(back.dof == s.dof);
    // Highlights alone make the depth of field differ from the default (so it is written).
    Scene h = make_scene(PresetId::StudioWhite, 0);
    h.dof.highlights = 0.3f;
    CHECK(scene_json(h).contains("dof"));
}

TEST_CASE("Showcase 1h-2: the live view's draft cap on weak GPUs", "[Showcase][Realism]")
{
    CHECK(weak_gpu_renderer("Intel(R) UHD Graphics 620"));
    CHECK(weak_gpu_renderer("Intel(R) Iris(R) Xe Graphics"));
    CHECK(weak_gpu_renderer("llvmpipe (LLVM 15.0.7, 256 bits)"));
    CHECK(weak_gpu_renderer("Microsoft Basic Render Driver"));
    CHECK_FALSE(weak_gpu_renderer("Intel(R) Arc(TM) A770 Graphics"));
    CHECK_FALSE(weak_gpu_renderer("NVIDIA GeForce RTX 3060/PCIe/SSE2"));
    CHECK_FALSE(weak_gpu_renderer("AMD Radeon RX 6800 XT"));
    CHECK_FALSE(weak_gpu_renderer("Apple M2"));
    CHECK_FALSE(weak_gpu_renderer(""));
    CHECK(live_dof_taps(dof_taps(BokehQuality::Best), true) == dof_taps(BokehQuality::Draft));
    CHECK(live_dof_taps(dof_taps(BokehQuality::Best), false) == dof_taps(BokehQuality::Best));
    CHECK(live_dof_taps(dof_taps(BokehQuality::Draft), true) == dof_taps(BokehQuality::Draft));
}

TEST_CASE("Showcase 1h-2: Reset to auto shows once a ground size is typed in", "[Showcase][Realism][panel]")
{
    Scene s = make_scene(PresetId::StudioWhite, 0);
    s.background.type      = BackgroundType::Environment;
    s.lighting.environment = "studio";
    s.background.ground    = true;
    CHECK(panel_visibility(s, false).bg_ground_detail);
    CHECK_FALSE(panel_visibility(s, false).bg_ground_reset);   // already automatic
    s.background.ground_height = 300.f;
    CHECK(panel_visibility(s, false).bg_ground_reset);
    s.background.ground = false;
    CHECK_FALSE(panel_visibility(s, false).bg_ground_reset);
}

// ------------------------------------------------------------------------------------------------
// 1h-3: HDRI downloads and the user's own HDRIs
// ------------------------------------------------------------------------------------------------

namespace {

std::string read_resource(const std::string &rel)
{
    std::ifstream     f(std::string(TEST_DATA_DIR) + "/../../resources/" + rel, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

HdrImage gradient_panorama(int w, int h)
{
    HdrImage img;
    img.width  = w;
    img.height = h;
    img.rgb.resize(size_t(w) * size_t(h) * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float *p = &img.rgb[(size_t(y) * size_t(w) + size_t(x)) * 3];
            p[0]     = 0.05f + 3.f * float(x) / float(w);
            p[1]     = 0.5f + 0.25f * float(y) / float(h);
            p[2]     = (x == w / 2 && y == h / 4) ? 2000.f : 0.125f;   // a sun-bright texel
        }
    return img;
}

const uint8_t *bytes_of(const std::string &s) { return reinterpret_cast<const uint8_t *>(s.data()); }

} // namespace

TEST_CASE("Showcase 1h-3: OpenEXR scanline files read back (NONE and ZIP)", "[Showcase][Realism][HDR]")
{
    const HdrImage src = gradient_panorama(64, 32);
    for (bool zip : { false, true }) {
        INFO("zip " << zip);
        const std::string file = encode_exr(src, zip);
        HdrImage    back;
        std::string error;
        REQUIRE(decode_exr(bytes_of(file), file.size(), back, &error));
        REQUIRE(back.width == 64);
        REQUIRE(back.height == 32);
        for (size_t i = 0; i < src.rgb.size(); ++i)
            CHECK_THAT(back.rgb[i], WithinAbs(src.rgb[i], std::max(1e-3f, std::abs(src.rgb[i]) * 2e-3f)));   // half precision
        // The same through the dispatcher.
        HdrImage any;
        REQUIRE(decode_hdri_any(bytes_of(file), file.size(), any));
        CHECK(any.rgb == back.rgb);
    }
    // ZIP compresses a smooth picture.
    CHECK(encode_exr(src, true).size() < encode_exr(src, false).size());
    // A Radiance file still goes through the Radiance reader.
    const std::string rgbe = encode_rgbe(src);
    HdrImage          r;
    REQUIRE(decode_hdri_any(bytes_of(rgbe), rgbe.size(), r));
    CHECK(r.width == 64);
}

TEST_CASE("Showcase 1h-3: OpenEXR files this reader does not handle are refused cleanly", "[Showcase][Realism][HDR]")
{
    const std::string good = encode_exr(gradient_panorama(16, 8), true);
    HdrImage          out;
    std::string       error;
    CHECK_FALSE(decode_exr(nullptr, 0, out, &error));
    CHECK_FALSE(decode_exr(bytes_of(good), 20, out, &error));   // cut short
    CHECK(out.empty());
    // Tiled: the version's tile bit (0x200).
    std::string tiled = good;
    tiled[5]          = char(tiled[5] | 0x02);
    CHECK_FALSE(decode_exr(bytes_of(tiled), tiled.size(), out, &error));
    CHECK(error.find("tiled") != std::string::npos);
    // PIZ: the compression attribute's value ("compression", NUL, "compression", NUL, a 4-byte size).
    std::string  piz = good;
    const size_t at  = piz.find("compression");
    REQUIRE(at != std::string::npos);
    piz[at + 2 * 12 + 4] = char(4);
    CHECK_FALSE(decode_exr(bytes_of(piz), piz.size(), out, &error));
    CHECK(error.find("PIZ") != std::string::npos);
    // A block cut short: refused, never a crash.
    std::string bad = good;
    bad.resize(bad.size() - 10);
    CHECK_FALSE(decode_exr(bytes_of(bad), bad.size(), out, &error));
}

TEST_CASE("Showcase 1h-3: a large panorama is halved until it fits", "[Showcase][Realism][HDR]")
{
    const HdrImage big   = gradient_panorama(256, 128);
    const HdrImage halved = downscale_hdr(big, 64);
    CHECK(halved.width == 64);
    CHECK(halved.height == 32);
    // A box filter keeps the average light.
    double a = 0., b = 0.;
    for (float v : big.rgb)
        a += v;
    for (float v : halved.rgb)
        b += v;
    CHECK_THAT(b * 16., WithinAbs(a, a * 1e-4));
    CHECK(downscale_hdr(big, 1000).width == 256);   // already small enough
}

TEST_CASE("Showcase 1h-3: the download catalogue is the owner's six panoramas from our mirror", "[Showcase][Realism][HDR]")
{
    HdriManifest cat;
    std::string  error;
    REQUIRE(parse_hdri_catalogue(read_resource(std::string(HDRI_DIR) + "/" + HDRI_CATALOGUE), cat, &error));
    REQUIRE(cat.hdris.size() == 6);
    for (const char *src : { "brown_photostudio_02", "blue_photo_studio", "industrial_sunset_puresky", "kiara_1_dawn", "empty_warehouse_01",
                             "abandoned_workshop_02" }) {
        INFO(src);
        const std::string page = std::string("https://polyhaven.com/a/") + src;
        const auto it = std::find_if(cat.hdris.begin(), cat.hdris.end(), [&page](const HdriInfo &h) { return h.source == page; });
        REQUIRE(it != cat.hdris.end());
        CHECK(it->downloadable);
        CHECK(it->licence == "CC0 1.0");
        CHECK_FALSE(it->download_file.empty());
        // Published on the mirror (release showcase-hdri-1): size and checksum filled in.
        CHECK(hdri_download_published(*it));
        // Our mirror, never the Poly Haven API (D-R3).
        const std::string url = hdri_download_url(hdri_mirror_base(nullptr), *it);
        CHECK(url.find("polyhaven") == std::string::npos);
        CHECK(url.rfind(HDRI_MIRROR_BASE_URL, 0) == 0);
    }
    // No id repeats a bundled HDRI.
    HdriManifest bundled;
    REQUIRE(parse_hdri_manifest(read_resource(std::string(HDRI_DIR) + "/" + HDRI_MANIFEST), bundled, &error));
    for (const HdriInfo &h : cat.hdris)
        CHECK(bundled.find(h.id) == nullptr);
}

TEST_CASE("Showcase 1h-3: catalogue entries need a size and a checksum, and safe names", "[Showcase][Realism][HDR]")
{
    const json text = json::parse(R"JSON({"hdris":[
        {"id":"a","name":"A","file":"a.hdr","licence":"CC0","downloadable":true,
         "download":{"file":"a_2k.hdr","size":123,"sha256":"BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"}},
        {"id":"b","name":"B","file":"b.hdr","licence":"CC0","downloadable":true,"download":{"file":"b.hdr","size":0,"sha256":""}},
        {"id":"c","name":"C","file":"c.hdr","licence":"CC0","downloadable":true,"download":{"file":"../c.hdr","size":5,"sha256":""}},
        {"id":"d","name":"D","file":"d.hdr","licence":"CC0","downloadable":true,"download":{"file":"d.hdr?x=1","size":5}},
        {"id":"e","name":"E","file":"e.hdr","licence":"CC0"}
    ]})JSON");
    HdriManifest m;
    REQUIRE(parse_hdri_catalogue(text.dump(), m));
    REQUIRE(m.hdris.size() == 2);   // c and d have unsafe names, e is not a download
    CHECK(m.find("a")->download_file == "a_2k.hdr");
    CHECK(m.find("a")->download_size == 123);
    CHECK(m.find("a")->sha256 == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");   // lower case
    CHECK(hdri_download_published(*m.find("a")));
    CHECK_FALSE(hdri_download_published(*m.find("b")));
    // The mirror can be replaced (a local mock server); a slash is added.
    CHECK(hdri_mirror_base("http://127.0.0.1:8123/hdri") == "http://127.0.0.1:8123/hdri/");
    CHECK(hdri_mirror_base("") == HDRI_MIRROR_BASE_URL);
    CHECK(hdri_download_url("http://x/", *m.find("a")) == "http://x/a_2k.hdr");
}

TEST_CASE("Showcase 1h-3: a download is kept only when its size and SHA-256 match", "[Showcase][Realism][HDR]")
{
    CHECK(sha256_hex(bytes_of("abc"), 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const std::string file = encode_rgbe(gradient_panorama(32, 16));
    HdriInfo          h;
    h.id            = "x";
    h.downloadable  = true;
    h.download_size = file.size();
    h.sha256        = sha256_hex(bytes_of(file), file.size());
    CHECK(verify_hdri_download(bytes_of(file), file.size(), h) == DownloadCheck::Ok);
    std::string altered = file;
    altered[altered.size() / 2] ^= 0x01;
    CHECK(verify_hdri_download(bytes_of(altered), altered.size(), h) == DownloadCheck::HashMismatch);
    CHECK(verify_hdri_download(bytes_of(file), file.size() - 1, h) == DownloadCheck::SizeMismatch);
    CHECK(verify_hdri_download(nullptr, 0, h) == DownloadCheck::Empty);
    // Not published yet: nothing is ever kept.
    HdriInfo unpublished = h;
    unpublished.sha256.clear();
    CHECK(verify_hdri_download(bytes_of(file), file.size(), unpublished) == DownloadCheck::NotPublished);
    // The right checksum of something that is not a panorama.
    const std::string junk(200, 'j');
    HdriInfo          j = h;
    j.download_size     = junk.size();
    j.sha256            = sha256_hex(bytes_of(junk), junk.size());
    CHECK(verify_hdri_download(bytes_of(junk), junk.size(), j) == DownloadCheck::NotAnHdri);
    CHECK(std::string(to_string(DownloadCheck::HashMismatch)).find("checksum") != std::string::npos);
}

TEST_CASE("Showcase 1h-3: the user's own HDRIs", "[Showcase][Realism][HDR]")
{
    CHECK(user_hdri_slug("C:/photos/My Garage (2).EXR") == "my_garage_2");
    CHECK(user_hdri_slug("") == "hdri");
    CHECK(user_hdri_slug("___.hdr") == "hdri");
    CHECK(user_hdri_display_name("C:/photos/My Garage (2).EXR") == "My Garage (2)");
    std::vector<HdriInfo> existing(2);
    existing[0].id = "user_garage";
    existing[1].id = "user_garage_2";
    CHECK(unique_user_hdri_id("garage", existing) == "user_garage_3");
    CHECK(unique_user_hdri_id("shed", existing) == "user_shed");

    // Read, checked and kept as Radiance.
    const HdrImage    pano = gradient_panorama(64, 32);
    const std::string exr  = encode_exr(pano, true);
    HdrImage          ready;
    std::string       error;
    REQUIRE(prepare_user_hdri(bytes_of(exr), exr.size(), ready, &error));
    CHECK(ready.width == 64);
    // Black or portrait pictures are refused, and so is anything that is not a panorama.
    HdrImage black = pano;
    std::fill(black.rgb.begin(), black.rgb.end(), 0.f);
    const std::string black_file = encode_rgbe(black);
    CHECK_FALSE(prepare_user_hdri(bytes_of(black_file), black_file.size(), ready, &error));
    CHECK_FALSE(error.empty());
    const std::string tall = encode_rgbe(gradient_panorama(16, 32));
    CHECK_FALSE(prepare_user_hdri(bytes_of(tall), tall.size(), ready, &error));
    const std::string junk = "not a picture";
    CHECK_FALSE(prepare_user_hdri(bytes_of(junk), junk.size(), ready, &error));

    // Their list round-trips through the manifest format.
    HdriInfo a;
    a.id             = "user_shed";
    a.name           = "Shed at dusk";
    a.file           = "user_shed.hdr";
    a.licence        = "your own";
    a.backdrop_pitch = 5.f;
    HdriManifest back;
    REQUIRE(parse_hdri_manifest(hdri_manifest_to_json({ a }), back));
    REQUIRE(back.hdris.size() == 1);
    CHECK(back.hdris[0].id == a.id);
    CHECK(back.hdris[0].name == a.name);
    CHECK(back.hdris[0].file == a.file);
    CHECK(back.hdris[0].backdrop_pitch == 5.f);
    CHECK_FALSE(back.hdris[0].downloadable);
}
