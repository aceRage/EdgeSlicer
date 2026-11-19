// Showcase realism 1a (tests/design_showcase_realism.md 2.1): the post pass's tone curves and the
// half-float HDR layer (src/libslic3r/Showcase/ShowcaseTone.{hpp,cpp}, mirrored by
// resources/shaders/140/showcase_post.fs).

#include <catch2/catch.hpp>

#include "libslic3r/Showcase/ShowcaseScene.hpp"
#include "libslic3r/Showcase/ShowcaseTone.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <random>

using namespace Slic3r::Showcase;
using Catch::Matchers::WithinAbs;

namespace {

int encode8(float linear) { return int(std::lround(linear_to_srgb(linear) * 255.f)); }

} // namespace

TEST_CASE("Showcase tone curve names round trip", "[Showcase]")
{
    for (ToneCurve c : { ToneCurve::Neutral, ToneCurve::Filmic }) {
        ToneCurve back = ToneCurve::Neutral;
        REQUIRE(from_string(to_string(c), back));
        CHECK(back == c);
    }
    ToneCurve v = ToneCurve::Filmic;
    CHECK_FALSE(from_string("aces", v));
    CHECK(v == ToneCurve::Filmic);
    // D-R1: a new scene keeps PBR Neutral.
    CHECK(Lighting().tone == ToneCurve::Neutral);
}

TEST_CASE("Showcase PBR Neutral keeps colours and compresses highlights", "[Showcase]")
{
    SECTION("mid tones only lose the toe offset") {
        const Rgb out = tone_neutral({ 0.5f, 0.3f, 0.2f });
        CHECK_THAT(out[0], WithinAbs(0.46, 1e-6));
        CHECK_THAT(out[1], WithinAbs(0.26, 1e-6));
        CHECK_THAT(out[2], WithinAbs(0.16, 1e-6));
    }
    SECTION("black stays black") {
        const Rgb out = tone_neutral({ 0.f, 0.f, 0.f });
        CHECK(out[0] == 0.f);
        CHECK(out[2] == 0.f);
    }
    SECTION("bright light approaches white without reaching it") {
        float prev = 0.f;
        for (float v : { 0.5f, 1.f, 2.f, 8.f, 64.f, 1000.f }) {
            const Rgb out = tone_neutral({ v, v, v });
            CHECK(out[0] > prev);
            CHECK(out[0] < 1.f);
            prev = out[0];
        }
        CHECK(prev > 0.999f);
    }
}

TEST_CASE("Showcase filmic curve rolls highlights off", "[Showcase]")
{
    float prev = -1.f;
    for (float ev = -10.f; ev <= 6.f; ev += 0.5f) {
        const float v   = std::exp2(ev);
        const Rgb   out = tone_filmic({ v, v, v });
        CHECK(out[0] >= prev);   // monotonic for grey
        CHECK(out[0] >= 0.f);
        CHECK(out[0] <= 1.f);
        CHECK_THAT(out[0], WithinAbs(out[1], 1e-3));   // grey stays grey
        prev = out[0];
    }
    CHECK(tone_filmic({ 0.f, 0.f, 0.f })[0] < 0.001f);
    CHECK(tone_filmic({ 64.f, 64.f, 64.f })[0] > 0.97f);
    // Middle grey lands in the middle, a little brighter than Neutral's.
    const float grey = tone_filmic({ 0.18f, 0.18f, 0.18f })[0];
    CHECK(grey > 0.1f);
    CHECK(grey < 0.4f);
    // A saturated highlight desaturates towards white instead of clipping one channel.
    const Rgb hot = tone_filmic({ 40.f, 2.f, 1.f });
    CHECK(hot[1] > 0.3f);
    CHECK(tone_map(ToneCurve::Filmic, { 0.3f, 0.3f, 0.3f })[0] == tone_filmic({ 0.3f, 0.3f, 0.3f })[0]);
    CHECK(tone_map(ToneCurve::Neutral, { 0.3f, 0.3f, 0.3f })[0] == tone_neutral({ 0.3f, 0.3f, 0.3f })[0]);
}

TEST_CASE("Showcase sRGB transfer", "[Showcase]")
{
    for (int i = 0; i <= 255; ++i) {
        const float c = float(i) / 255.f;
        CHECK(int(std::lround(linear_to_srgb(srgb_to_linear(c)) * 255.f)) == i);
    }
    CHECK_THAT(linear_to_srgb(2.f), WithinAbs(1., 1e-6));   // clamped to 1 first
    CHECK(linear_to_srgb(-1.f) == 0.f);
}

TEST_CASE("Showcase half floats", "[Showcase]")
{
    CHECK(round_to_half(1.f) == 1.f);
    CHECK(round_to_half(65504.f) == 65504.f);
    CHECK(std::isinf(round_to_half(70000.f)));
    CHECK(round_to_half(1.f + 1.f / 4096.f) == 1.f);           // below half an ulp (2^-10)
    CHECK(round_to_half(1.f + 3.f / 2048.f) == 1.f + 1.f / 512.f);   // 1.5 ulp rounds to even
    CHECK(round_to_half(std::ldexp(1.f, -24)) == std::ldexp(1.f, -24));   // smallest subnormal
    CHECK(round_to_half(-0.333333f) == -round_to_half(0.333333f));
}

// What 1a must not change: the material shader used to tone-map in 32-bit floats and write 8 bits;
// now its linear light goes through the RGBA16F layer first. The half float costs at most one 8-bit
// step, and only rarely.
TEST_CASE("Showcase HDR layer precision", "[Showcase]")
{
    std::mt19937                          rng(1234);
    std::uniform_real_distribution<float> stops(-14.f, 6.f);
    std::uniform_real_distribution<float> unit(0.f, 1.f);
    int worst = 0, changed = 0;
    const int n = 20000;
    for (ToneCurve curve : { ToneCurve::Neutral, ToneCurve::Filmic })
        for (int i = 0; i < n; ++i) {
            Rgb c;
            for (float &v : c)
                v = unit(rng) < 0.1f ? 0.f : std::exp2(stops(rng));
            const Rgb ref  = tone_map(curve, c);
            const Rgb half = tone_map(curve, { round_to_half(c[0]), round_to_half(c[1]), round_to_half(c[2]) });
            int       d    = 0;
            for (int k = 0; k < 3; ++k)
                d = std::max(d, std::abs(encode8(ref[k]) - encode8(half[k])));
            worst = std::max(worst, d);
            changed += d > 0 ? 1 : 0;
        }
    CHECK(worst <= 1);
    CHECK(changed < 2 * n / 20);   // under 5 % of the colours move by that one step
}

TEST_CASE("Showcase export memory counts the HDR layer", "[Showcase]")
{
    // 8 bytes a sample for RGBA16F on top of RGBA8 + D24S8.
    CHECK(export_memory_estimate(1000, 1000, 4) == size_t(1000) * 1000 * (16 * 4 + 16 + 8 + 4));
    CHECK(export_memory_estimate(1000, 1000, 0) == size_t(1000) * 1000 * (16 + 8 + 4));
    // A 1080p High export keeps 4x MSAA inside the 1 GB budget (design 2.1: ~0.6-0.8 GB).
    const ExportPlan p = plan_export(1920, 1080, Quality::High, 8, 540);
    CHECK(p.msaa == 4);
    CHECK(p.estimated_bytes < double(size_t(1) << 30));
}
