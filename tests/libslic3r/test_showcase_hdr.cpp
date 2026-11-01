// Showcase realism 1h-1 (tests/design_showcase_realism.md 2.8): the HDRI loader, the SH projection,
// the sun, the prefiltered atlas and the manifest (src/libslic3r/Showcase/ShowcaseHdr.{hpp,cpp}).
// GL-free; the renderer's use of it (the backdrop, the key light) is checked by the golden stills.

#include <catch2/catch.hpp>

#include "libslic3r/Showcase/ShowcaseAssets.hpp"
#include "libslic3r/Showcase/ShowcaseHdr.hpp"
#include "libslic3r/Showcase/ShowcaseScene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r::Showcase;
using Catch::Matchers::WithinAbs;


namespace {

using V3 = std::array<float, 3>;

float dot3(const V3 &a, const V3 &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

V3 unit(V3 v)
{
    const float l = std::sqrt(dot3(v, v));
    return { v[0] / l, v[1] / l, v[2] / l };
}

// A panorama from a function of the world direction, sampled at the pixel centres.
template<class F> HdrImage make_image(int w, int h, F f)
{
    HdrImage img;
    img.width  = w;
    img.height = h;
    img.rgb.resize(size_t(w) * size_t(h) * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const V3 c = f(equirect_direction((float(x) + 0.5f) / float(w), (float(y) + 0.5f) / float(h)));
            for (int k = 0; k < 3; ++k)
                img.rgb[(size_t(y) * size_t(w) + size_t(x)) * 3 + size_t(k)] = c[size_t(k)];
        }
    return img;
}

// The reference image of the SH numbers below (computed with scripts/showcase/bake_showcase_assets.py:
// equirect_dirs + diffuse_sh at 64 x 32).
V3 reference_radiance(const V3 &d)
{
    static const V3 s = unit({ 0.5f, -0.3f, 0.8f });
    const float     blob = 3.f * std::pow(std::max(0.f, dot3(d, s)), 8.f);
    return { 0.2f + 0.1f * d[2] + blob, 0.3f + blob * 0.9f, 0.5f - 0.2f * d[0] + blob * 0.7f };
}

float luma(const V3 &c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

// The atlas texel a world direction falls in (the GL cube face table), decoded.
V3 atlas_lookup(const HdrEnvironment &env, int level, const V3 &world)
{
    const V3    g = world_to_cube(world);
    const float ax = std::abs(g[0]), ay = std::abs(g[1]), az = std::abs(g[2]);
    int   face;
    float ma, sc, tc;
    if (ax >= ay && ax >= az) {
        face = g[0] >= 0 ? 0 : 1;
        ma = ax;
        sc = face == 0 ? -g[2] : g[2];
        tc = -g[1];
    } else if (ay >= az) {
        face = g[1] >= 0 ? 2 : 3;
        ma = ay;
        sc = g[0];
        tc = face == 2 ? g[2] : -g[2];
    } else {
        face = g[2] >= 0 ? 4 : 5;
        ma = az;
        sc = face == 4 ? g[0] : -g[0];
        tc = -g[1];
    }
    const RectI r = env_atlas_face(env.face_size, env.levels, level, face);
    const int   i = std::clamp(int(std::floor((sc / ma + 1.f) * 0.5f * float(r.w))), 0, r.w - 1);
    const int   j = std::clamp(int(std::floor((tc / ma + 1.f) * 0.5f * float(r.h))), 0, r.h - 1);
    const uint8_t *p = &env.atlas_rgba[(size_t(r.y + j) * size_t(env.atlas_width) + size_t(r.x + i)) * 4];
    return decode_rgbm(p[0], p[1], p[2], p[3], 16.f);
}

std::string repo_file(const std::string &relative)
{
    const std::string path = std::string(TEST_DATA_DIR) + "/../../" + relative;
    std::ifstream     in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

TEST_CASE("Showcase HDR: Radiance files round trip, flat and run-length", "[Showcase]")
{
    for (int w : { 5, 8, 37, 300 }) {
        const HdrImage img = make_image(w, 9, [](const V3 &d) {
            // Flat stretches (runs) and gradients (dumps), over a wide dynamic range.
            const float k = d[2] > 0.3f ? 4.f : std::exp(3.f * d[0]);
            return V3{ k, d[2] > 0.3f ? 4.f : 0.5f * k, 0.01f * k };
        });
        for (bool rle : { true, false }) {
            const std::string bytes = encode_rgbe(img, rle);
            HdrImage          back;
            std::string       error;
            REQUIRE(decode_rgbe(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(), back, &error));
            REQUIRE(back.width == w);
            REQUIRE(back.height == 9);
            // RGBE keeps 8 bits of mantissa shared by the three channels: the error is relative to the
            // pixel's largest channel.
            for (size_t p = 0; p < img.rgb.size() / 3; ++p) {
                const float m = std::max(img.rgb[p * 3], std::max(img.rgb[p * 3 + 1], img.rgb[p * 3 + 2]));
                for (size_t c = 0; c < 3; ++c)
                    CHECK_THAT(back.rgb[p * 3 + c], WithinAbs(img.rgb[p * 3 + c], m / 120.f + 1e-6f));
            }
        }
    }
    // The run-length encoder actually compresses a flat picture.
    const HdrImage flat = make_image(256, 16, [](const V3 &) { return V3{ 0.5f, 0.5f, 0.5f }; });
    CHECK(encode_rgbe(flat, true).size() < encode_rgbe(flat, false).size() / 4);
}

TEST_CASE("Showcase HDR: the Radiance header variants", "[Showcase]")
{
    auto decode = [](const std::string &s, HdrImage &out, std::string *err = nullptr) {
        return decode_rgbe(reinterpret_cast<const uint8_t *>(s.data()), s.size(), out, err);
    };
    const std::string px("\x80\x80\x80\x81", 4);   // 128/256 * 2^(129-128) = 1.0 per channel... see below
    HdrImage          img;
    // One flat pixel, CRLF line ends, an EXPOSURE line and "RADIANCE" magic: 0x80 * 2^(129-136) = 1.
    REQUIRE(decode("#?RGBE\r\nFORMAT=32-bit_rle_rgbe\r\nEXPOSURE=2\r\n\r\n-Y 1 +X 1\r\n" + px, img));
    CHECK_THAT(img.rgb[0], WithinAbs(0.5, 1e-6));   // divided by the exposure
    // "+Y" is a bottom-up file: the rows are flipped.
    const std::string two = std::string("\x80\x00\x00\x81", 4) + std::string("\x00\x80\x00\x81", 4);
    REQUIRE(decode("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n+Y 2 +X 1\n" + two, img));
    CHECK(img.rgb[0] == 0.f);   // the top row is the file's last row (green)
    CHECK(img.rgb[1] > 0.f);
    // An old-style repeat run: (1,1,1,n) repeats the previous pixel n times.
    const std::string old = std::string("\x80\x80\x80\x81", 4) + std::string("\x01\x01\x01\x03", 4);
    REQUIRE(decode("#?RADIANCE\n\n-Y 1 +X 4\n" + old, img));
    CHECK(img.width == 4);
    for (size_t i = 0; i < 12; ++i)
        CHECK_THAT(img.rgb[i], WithinAbs(1.0, 1e-6));
    // Refusals: never a crash, always a reason.
    std::string error;
    CHECK_FALSE(decode("P6\n1 1\n255\n", img, &error));
    CHECK_FALSE(error.empty());
    CHECK(img.empty());
    CHECK_FALSE(decode("#?RADIANCE\nFORMAT=32-bit_rle_xyze\n\n-Y 1 +X 1\n" + px, img));
    CHECK_FALSE(decode("#?RADIANCE\n\n+X 1 -Y 1\n" + px, img));
    CHECK_FALSE(decode("#?RADIANCE\n\n-Y 99999 +X 99999\n", img));
    CHECK_FALSE(decode("#?RADIANCE\n\n-Y 2 +X 2\n" + px, img));   // truncated
    CHECK_FALSE(decode("", img));
    // A new-style scanline cut short.
    const std::string good = encode_rgbe(make_image(16, 4, [](const V3 &d) { return V3{ d[0] + 2.f, 1.f, 1.f }; }), true);
    CHECK_FALSE(decode(good.substr(0, good.size() - 9), img));
}

TEST_CASE("Showcase HDR: the equirect mapping is the bake script's", "[Showcase]")
{
    float u, v;
    // The image centre is +X, a quarter across is +Y, the top row is the zenith.
    const V3 c = equirect_direction(0.5f, 0.5f);
    CHECK_THAT(c[0], WithinAbs(1., 1e-6));
    const V3 q = equirect_direction(0.25f, 0.5f);
    CHECK_THAT(q[1], WithinAbs(1., 1e-6));
    const V3 t = equirect_direction(0.3f, 0.f);
    CHECK_THAT(t[2], WithinAbs(1., 1e-6));
    direction_to_equirect({ 0.f, 1.f, 0.f }, u, v);
    CHECK_THAT(u, WithinAbs(0.25, 1e-6));
    CHECK_THAT(v, WithinAbs(0.5, 1e-6));
    for (int i = 0; i < 50; ++i) {
        const float uu = 0.013f + 0.019f * float(i), vv = 0.02f + 0.0191f * float(i);
        direction_to_equirect(equirect_direction(uu, vv), u, v);
        CHECK_THAT(u, WithinAbs(uu, 2e-5));
        CHECK_THAT(v, WithinAbs(vv, 2e-5));
    }
}

TEST_CASE("Showcase HDR: the SH lighting matches the bake script", "[Showcase]")
{
    const HdrImage img = make_image(64, 32, reference_radiance);
    const std::array<std::array<float, 3>, 9> sh = project_hdr_sh(img);
    // diffuse_sh() of bake_showcase_assets.py on the same 64 x 32 image.
    const float expected[9][3] = {
        { 1.299949f, 1.595344f, 2.186132f },   { -0.185993f, -0.167394f, -0.130195f }, { 0.632985f, 0.446788f, 0.347502f },
        { 0.309989f, 0.278990f, -0.055785f },  { -0.063654f, -0.057289f, -0.044558f }, { -0.101846f, -0.091661f, -0.071292f },
        { 0.115529f, 0.104071f, 0.081157f },   { 0.169743f, 0.152769f, 0.118820f },    { 0.033949f, 0.030554f, 0.023764f } };
    for (size_t k = 0; k < 9; ++k)
        for (size_t c = 0; c < 3; ++c)
            CHECK_THAT(sh[k][c], WithinAbs(expected[k][c], 1e-3));
    CHECK_THAT(sh_peak_luminance(sh), WithinAbs(0.8605963, 2e-3));
    // And it lights like the brute-force Lambert integral (the script's own check): within 2 %.
    const V3 normals[4]   = { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 }, { 0.6f, 0.f, -0.8f } };
    const float direct[4][3] = { { 0.752136f, 0.737019f, 0.840225f }, { 0.507518f, 0.576766f, 0.581983f },
                                 { 0.219983f, 0.317985f, 0.513988f }, { 0.162078f, 0.313921f, 0.430888f } };
    for (int n = 0; n < 4; ++n) {
        const V3 l = eval_sh(sh, normals[n]);
        for (size_t c = 0; c < 3; ++c)
            CHECK_THAT(l[c], WithinAbs(direct[n][c], 0.02 * direct[n][c] + 1e-3));
    }
}

TEST_CASE("Showcase HDR: the procedural panoramas", "[Showcase]")
{
    for (ProceduralHdr kind : { ProceduralHdr::SkySun, ProceduralHdr::Studio, ProceduralHdr::Interior }) {
        ProceduralHdr back = ProceduralHdr::Studio;
        REQUIRE(from_string(to_string(kind), back));
        CHECK(back == kind);
    }
    ProceduralHdr unused;
    CHECK_FALSE(from_string("nope", unused));
    const HdrImage a = make_procedural_hdr(ProceduralHdr::SkySun, 512);
    const HdrImage b = make_procedural_hdr(ProceduralHdr::SkySun, 512);
    CHECK(a.width == 512);
    CHECK(a.height == 256);
    CHECK(a.rgb == b.rgb);   // deterministic
    for (float v : a.rgb)
        REQUIRE(std::isfinite(v));
    CHECK(make_procedural_hdr(ProceduralHdr::Studio, 8192).width == 8192);
    CHECK(make_procedural_hdr(ProceduralHdr::Studio, 1).width == 16);   // clamped

    // The sun: where it was put (azimuth 40, elevation 35), bright, and the dominant light.
    const HdrSun sun = find_hdr_sun(make_procedural_hdr(ProceduralHdr::SkySun, 1024));
    REQUIRE(sun.found);
    const float az = 40.f * 3.14159265f / 180.f, el = 35.f * 3.14159265f / 180.f;
    const V3    expected{ std::cos(el) * std::cos(az), std::cos(el) * std::sin(az), std::sin(el) };
    CHECK(dot3(sun.dir, expected) > std::cos(1.5f * 3.14159265f / 180.f));
    CHECK(sun.share > 0.25f);
    CHECK(sun.solid_angle < 0.05f);
    CHECK_THAT(luma(sun.color), WithinAbs(1., 1e-3));
    CHECK(sun.color[0] > sun.color[2]);   // warm
    // A studio's softbox is a dominant light too; a flat panorama and an overcast sky are not.
    CHECK(find_hdr_sun(make_procedural_hdr(ProceduralHdr::Studio, 512)).found);
    CHECK_FALSE(find_hdr_sun(make_image(64, 32, [](const V3 &) { return V3{ 1.f, 1.f, 1.f }; })).found);
    CHECK_FALSE(find_hdr_sun(make_image(128, 64, [](const V3 &d) { return V3{ 1.f + d[2], 1.f + d[2], 1.f + d[2] }; })).found);
    CHECK_FALSE(find_hdr_sun(HdrImage()).found);
}

TEST_CASE("Showcase HDR: the bake scales to the peak, fills the atlas and keeps its orientation", "[Showcase]")
{
    // A uniform panorama: every texel of every level is the scaled colour.
    {
        const HdrImage      img = make_image(64, 32, [](const V3 &) { return V3{ 0.5f, 0.25f, 0.1f }; });
        HdrEnvironment      env;
        HdrBakeOptions      opt;
        opt.face_size = 32;
        opt.levels    = 4;
        REQUIRE(bake_hdr_environment(img, opt, env));
        CHECK(env.face_size == 32);
        CHECK(env.atlas_width == env_atlas_width(32));
        CHECK(env.atlas_height == env_atlas_height(32, 4));
        CHECK_THAT(sh_peak_luminance(env.sh), WithinAbs(1.0, 2e-3));
        const float lum_in = luma({ 0.5f, 0.25f, 0.1f });
        CHECK_THAT(env.scale, WithinAbs(1.f / lum_in, 0.01f / lum_in));
        for (int level = 0; level < 4; ++level) {
            const V3 c = atlas_lookup(env, level, unit({ 0.3f, -0.5f, 0.7f }));
            CHECK_THAT(c[0], WithinAbs(0.5 * env.scale, 0.03 * 0.5 * env.scale));
            CHECK_THAT(c[1], WithinAbs(0.25 * env.scale, 0.03 * 0.25 * env.scale));
            CHECK_THAT(c[2], WithinAbs(0.1 * env.scale, 0.03 * 0.1 * env.scale + 0.01));
        }
        CHECK_FALSE(env.sun.found);
    }
    // A bright blob in one direction: the atlas is brightest there at every level, dimmest opposite.
    {
        const V3       d0 = unit({ -0.4f, 0.6f, 0.5f });
        const HdrImage img = make_image(128, 64, [&](const V3 &d) {
            const float b = 20.f * std::pow(std::max(0.f, dot3(d, d0)), 24.f);
            return V3{ 0.1f + b, 0.1f + b, 0.1f + b };
        });
        HdrEnvironment env;
        HdrBakeOptions opt;
        REQUIRE(bake_hdr_environment(img, opt, env));
        CHECK(env.levels == 5);
        for (int level = 0; level < 5; ++level) {
            const float toward = luma(atlas_lookup(env, level, d0));
            const float away   = luma(atlas_lookup(env, level, { -d0[0], -d0[1], -d0[2] }));
            const float side   = luma(atlas_lookup(env, level, unit({ d0[1], -d0[0], 0.f })));
            CHECK(toward > 2.f * away);
            CHECK(toward > side);
        }
        // The light is also in the SH: a normal facing it is brighter than the one facing away.
        CHECK(luma(eval_sh(env.sh, d0)) > 1.5f * luma(eval_sh(env.sh, { -d0[0], -d0[1], -d0[2] })));
        // Blurrier levels spread it: level 4 is dimmer than level 0 at the centre of the blob.
        CHECK(luma(atlas_lookup(env, 4, d0)) < luma(atlas_lookup(env, 0, d0)));
    }
    // Refusals.
    HdrEnvironment env;
    HdrBakeOptions opt;
    CHECK_FALSE(bake_hdr_environment(HdrImage(), opt, env));
    CHECK_FALSE(bake_hdr_environment(make_image(16, 8, [](const V3 &) { return V3{ 0.f, 0.f, 0.f }; }), opt, env));
    opt.levels = 0;
    CHECK_FALSE(bake_hdr_environment(make_image(16, 8, [](const V3 &) { return V3{ 1.f, 1.f, 1.f }; }), opt, env));
}

TEST_CASE("Showcase HDR: the sun is taken out of the light only when asked, once", "[Showcase]")
{
    const HdrImage sky = make_procedural_hdr(ProceduralHdr::SkySun, 512);
    HdrBakeOptions keep, remove;
    remove.remove_sun = true;
    HdrEnvironment with_sun, without_sun;
    REQUIRE(bake_hdr_environment(sky, keep, with_sun));
    REQUIRE(bake_hdr_environment(sky, remove, without_sun));
    REQUIRE(with_sun.sun.found);
    REQUIRE(without_sun.sun.found);
    // The same scale either way (the sky's brightness does not change with the toggle) ...
    CHECK_THAT(without_sun.scale, WithinAbs(with_sun.scale, 1e-6 * with_sun.scale));
    // ... set by the sky's own light (the sun is out of it), so the sun-in version is brighter.
    CHECK_THAT(sh_peak_luminance(without_sun.sh), WithinAbs(1.0, 2e-3));
    CHECK(sh_peak_luminance(with_sun.sh) > 1.5f);
    // ... but the light that faced the sun is smaller without it, and the sun's texel no longer stands out.
    const V3 toward = with_sun.sun.dir;
    CHECK(luma(eval_sh(without_sun.sh, toward)) < 0.7f * luma(eval_sh(with_sun.sh, toward)));
    const float peak_with    = luma(atlas_lookup(with_sun, 0, toward));
    const float peak_without = luma(atlas_lookup(without_sun, 0, toward));
    CHECK(peak_without < 0.8f * peak_with);   // the atlas clamps the sun at 16; the glow around it stays
    // The key light's strength is kept in a range that always reads.
    CHECK(with_sun.sun.key_strength >= 0.5f);
    CHECK(with_sun.sun.key_strength <= 1.5f);
    CHECK(with_sun.sun.irradiance > 0.f);
    // A panorama with no sun leaves the removal a no-op.
    const HdrImage flat = make_image(64, 32, [](const V3 &d) { return V3{ 1.f + 0.5f * d[2], 1.f, 1.f }; });
    HdrEnvironment a, b;
    REQUIRE(bake_hdr_environment(flat, keep, a));
    REQUIRE(bake_hdr_environment(flat, remove, b));
    CHECK(a.atlas_rgba == b.atlas_rgba);
}

TEST_CASE("Showcase HDR: the HDRI manifest", "[Showcase]")
{
    HdriManifest m;
    std::string  error;
    CHECK_FALSE(parse_hdri_manifest("not json", m, &error));
    CHECK(m.hdris.empty());
    CHECK_FALSE(parse_hdri_manifest(R"({"version":1})", m, &error));

    REQUIRE(parse_hdri_manifest(R"({"hdris":[
        {"id":"a","name":"A","file":"a_2k.hdr","source":"s","authors":"x","licence":"CC0 1.0"},
        {"id":"b","procedural":"studio"},
        {"id":"a","file":"dup.hdr"},
        {"id":"c","file":"../escape.hdr"},
        {"id":"d","file":"sub/dir.hdr"},
        {"id":"e","file":"C:evil.hdr"},
        {"id":"f","procedural":"bogus"},
        {"id":"g"},
        {"file":"noid.hdr"},
        42]})", m));
    REQUIRE(m.hdris.size() == 2);
    CHECK(m.find("a")->file == "a_2k.hdr");
    CHECK(m.find("a")->name == "A");
    CHECK_FALSE(m.find("a")->procedural);
    CHECK(m.find("b")->procedural);
    CHECK(m.find("b")->kind == ProceduralHdr::Studio);
    CHECK(m.find("b")->name == "b");   // no name: the id
    CHECK(m.find("c") == nullptr);
    CHECK(m.find("zzz") == nullptr);

    // The shipped manifest parses, and every file it lists exists.
    HdriManifest shipped;
    REQUIRE(parse_hdri_manifest(repo_file("resources/showcase/hdri/hdri.json"), shipped, &error));
    CHECK(shipped.hdris.size() >= 3);
    for (const HdriInfo &h : shipped.hdris) {
        CHECK_FALSE(h.name.empty());
        CHECK_FALSE(h.licence.empty());
        if (!h.procedural)
            CHECK_FALSE(repo_file(std::string("resources/") + HDRI_DIR + "/" + h.file).empty());
    }
}

TEST_CASE("Showcase HDR: sun_from_env is saved with the scene and off by default", "[Showcase]")
{
    CHECK_FALSE(Lighting().sun_from_env);
    Project p;
    Scene &s = p.ensure_scene_for_plate(0, PresetId::DeskWood);
    s.lighting.environment  = "hdri_outdoor";
    s.lighting.sun_from_env = true;
    Project back;
    REQUIRE(scenes_from_json(scenes_to_json(p), back));
    REQUIRE(back.scenes.size() == 1);
    CHECK(back.scenes[0].lighting.sun_from_env);
    CHECK(back.scenes[0].lighting.environment == "hdri_outdoor");
    CHECK(back.scenes[0] == s);
    // It takes part in equality (so the live view re-renders when it changes).
    Lighting a, b;
    b.sun_from_env = true;
    CHECK(a != b);
    // A scene written before the key existed opens with it off.
    const std::string old = R"({"schema":2,"active":"plate1","scenes":[{"id":"plate1","name":"Plate 1","plate_index":0,
        "source":"model","lighting":{"environment":"studio","azimuth":200}}]})";
    Project older;
    REQUIRE(scenes_from_json(old, older));
    REQUIRE(older.scenes.size() == 1);
    CHECK_FALSE(older.scenes[0].lighting.sun_from_env);
}
