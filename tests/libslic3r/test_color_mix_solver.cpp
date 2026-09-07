#include <catch2/catch.hpp>

#include "ColorSolver.hpp"
#include "libslic3r/filament_mixer.h"

#include <array>
#include <cmath>
#include <string>
#include <vector>

// Tests for the vendored, trimmed ColorSolver (deps_src/colorsolver) that backs
// the batch colour-match dialog's "closest reachable mix" search.
//
// The four properties the plan asks for:
//   * endpoint exactness  - a 100%/0% mix is the filament's own colour, bit-for-bit
//   * determinism         - same inputs twice, same weights, bit-for-bit
//   * monotonicity        - walking a ramp between two filaments never reverses
//   * a golden case       - a known target reproduced inside a stated dE2000
//
// plus the one that justifies the change at all: the solved mix is closer to the
// target than the nearest single filament, which is what the import-side matcher
// (libslic3r/ObjColorMatch.hpp, CIE76) settles for.

namespace {

using Rgb = std::array<float, 3>;

Rgb rgb_from_hex(const std::string &hex)
{
    const size_t off = (!hex.empty() && hex.front() == '#') ? 1 : 0;
    auto byte_at = [&](size_t i) {
        return float(std::stoi(hex.substr(off + i * 2, 2), nullptr, 16)) / 255.f;
    };
    return { byte_at(0), byte_at(1), byte_at(2) };
}

// ---- CIELab + dE2000 -------------------------------------------------------
// Transcribed from src/slic3r/Utils/ColorSpaceConvert.cpp (RGB2Lab / DeltaE00),
// which is the metric the batch dialog reports.  It cannot be linked here: its
// translation unit pulls in wx/colordlg.h, and these are the libslic3r tests.
// Keeping the same formula means the numbers below are the same numbers the
// dialog shows.

struct Lab { double L, a, b; };

double pivot_rgb(double n) { return (n > 0.04045 ? std::pow((n + 0.055) / 1.055, 2.4) : n / 12.92) * 100.0; }
double pivot_xyz(double n) { return n > 0.008856 ? std::pow(n, 1.0 / 3.0) : (7.787 * n) + (16.0 / 116.0); }

Lab rgb_to_lab(const Rgb &rgb)
{
    const double r = pivot_rgb(rgb[0]), g = pivot_rgb(rgb[1]), b = pivot_rgb(rgb[2]);
    const double X = 0.412453 * r + 0.357580 * g + 0.180423 * b;
    const double Y = 0.212671 * r + 0.715160 * g + 0.072169 * b;
    const double Z = 0.019334 * r + 0.119193 * g + 0.950227 * b;
    const double x = pivot_xyz(X / 95.047), y = pivot_xyz(Y / 100.000), z = pivot_xyz(Z / 108.883);
    return { 116.0 * y - 16.0, 500.0 * (x - y), 200.0 * (y - z) };
}

double delta_e_2000(const Lab &p, const Lab &q)
{
    auto rad2deg = [](double rad) { return 360.0 * rad / (2.0 * M_PI); };
    auto deg2rad = [](double deg) { return (2.0 * M_PI * deg) / 360.0; };

    const double avgL = (p.L + q.L) / 2.0;
    const double c1 = std::hypot(p.a, p.b), c2 = std::hypot(q.a, q.b);
    const double avgC = (c1 + c2) / 2.0;
    const double g = (1.0 - std::sqrt(std::pow(avgC, 7) / (std::pow(avgC, 7) + std::pow(25.0, 7)))) / 2.0;

    const double a1p = p.a * (1.0 + g), a2p = q.a * (1.0 + g);
    const double c1p = std::hypot(a1p, p.b), c2p = std::hypot(a2p, q.b);
    const double avgCp = (c1p + c2p) / 2.0;

    double h1p = rad2deg(std::atan2(p.b, a1p)); if (h1p < 0.0) h1p += 360.0;
    double h2p = rad2deg(std::atan2(q.b, a2p)); if (h2p < 0.0) h2p += 360.0;

    const double avghp = std::abs(h1p - h2p) > 180.0 ? (h1p + h2p + 360.0) / 2.0 : (h1p + h2p) / 2.0;
    const double t = 1.0 - 0.17 * std::cos(deg2rad(avghp - 30.0)) + 0.24 * std::cos(deg2rad(2.0 * avghp)) +
                     0.32 * std::cos(deg2rad(3.0 * avghp + 6.0)) - 0.2 * std::cos(deg2rad(4.0 * avghp - 63.0));

    double deltahp = h2p - h1p;
    if (std::abs(deltahp) > 180.0) deltahp += (h2p <= h1p) ? 360.0 : -360.0;
    const double deltalp = q.L - p.L;
    const double deltacp = c2p - c1p;
    deltahp = 2.0 * std::sqrt(c1p * c2p) * std::sin(deg2rad(deltahp) / 2.0);

    const double sl = 1.0 + ((0.015 * std::pow(avgL - 50.0, 2)) / std::sqrt(20.0 + std::pow(avgL - 50.0, 2)));
    const double sc = 1.0 + 0.045 * avgCp;
    const double sh = 1.0 + 0.015 * avgCp * t;

    const double deltaro = 30.0 * std::exp(-(std::pow((avghp - 275.0) / 25.0, 2)));
    const double rc = 2.0 * std::sqrt(std::pow(avgCp, 7) / (std::pow(avgCp, 7) + std::pow(25.0, 7)));
    const double rt = -rc * std::sin(2.0 * deg2rad(deltaro));

    return std::sqrt(std::pow(deltalp / sl, 2) + std::pow(deltacp / sc, 2) + std::pow(deltahp / sh, 2) +
                     rt * (deltacp / sc) * (deltahp / sh));
}

double delta_e_2000(const Rgb &lhs, const Rgb &rhs) { return delta_e_2000(rgb_to_lab(lhs), rgb_to_lab(rhs)); }

// The import-side matcher's metric: plain CIE76 in Lab, nearest single filament.
// libslic3r/ObjColorMatch.hpp's obj_color_distance, in the same units.
double delta_e_76(const Rgb &lhs, const Rgb &rhs)
{
    const Lab a = rgb_to_lab(lhs), b = rgb_to_lab(rhs);
    return std::sqrt((a.L - b.L) * (a.L - b.L) + (a.a - b.a) * (a.a - b.a) + (a.b - b.b) * (a.b - b.b));
}

// What the fork does today for a single-filament answer: nearest palette entry
// under CIE76.  Returns the chosen filament's colour.
Rgb nearest_single_filament_cie76(const std::vector<Rgb> &palette, const Rgb &target)
{
    size_t best = 0;
    double best_d = delta_e_76(palette[0], target);
    for (size_t i = 1; i < palette.size(); ++i) {
        const double d = delta_e_76(palette[i], target);
        if (d < best_d) { best_d = d; best = i; }
    }
    return palette[best];
}

// A four-slot printer.  Chosen to be a real spread rather than a flattering one:
// a saturated cyan, a magenta, a yellow and a white, i.e. the Full-Spectrum-style
// set the batch dialog's Recommended mode is built around.
const std::vector<Rgb> &cmyw_palette()
{
    static const std::vector<Rgb> palette = {
        rgb_from_hex("#0086D6"), // cyan
        rgb_from_hex("#E6007E"), // magenta
        rgb_from_hex("#FFE800"), // yellow
        rgb_from_hex("#FFFFFF")  // white
    };
    return palette;
}

// The colour the solver says a weight vector will print as, read back out of the
// candidate set rather than re-mixed, so the test measures what the dialog would
// actually show.
Rgb candidate_rgb(const Slic3r::ColorSolverCandidateSet &set, size_t idx)
{
    return { set.rgbs[idx * 3 + 0], set.rgbs[idx * 3 + 1], set.rgbs[idx * 3 + 2] };
}

} // namespace

using namespace Slic3r;

TEST_CASE("color mix solver: a pure mix is the filament's own colour", "[color_mix_solver]")
{
    const std::vector<Rgb> &palette = cmyw_palette();

    SECTION("mix_color_solver_components with one non-zero weight is bit-for-bit exact") {
        for (size_t component = 0; component < palette.size(); ++component) {
            std::vector<int> units(palette.size(), 0);
            units[component] = 40;
            const Rgb mixed = mix_color_solver_components(palette, units);
            REQUIRE(mixed[0] == palette[component][0]);
            REQUIRE(mixed[1] == palette[component][1]);
            REQUIRE(mixed[2] == palette[component][2]);
        }
        // Float weights take the same path.
        std::vector<float> w(palette.size(), 0.f);
        w[1] = 1.f;
        const Rgb mixed = mix_color_solver_components(palette, w);
        REQUIRE(mixed[0] == palette[1][0]);
        REQUIRE(mixed[1] == palette[1][1]);
        REQUIRE(mixed[2] == palette[1][2]);
    }

    SECTION("asking for a filament's own colour returns 100% of that filament") {
        const ColorSolverCandidateSet set = build_color_solver_candidates(palette);
        REQUIRE_FALSE(set.empty());
        for (size_t component = 0; component < palette.size(); ++component) {
            const std::vector<float> weights = solve_color_solver_weights_for_target(
                set, palette[component], ColorSolverLookupMode::ClosestMix, ColorSolverMode::OklabSoftCap4Dark4);
            REQUIRE(weights.size() == palette.size());
            REQUIRE(weights[component] == Approx(1.f));
            for (size_t other = 0; other < palette.size(); ++other)
                if (other != component)
                    REQUIRE(weights[other] == 0.f);

            // and the mix the dialog would display is that filament, exactly
            const size_t idx = solve_color_solver_candidate_for_target(set, palette[component],
                                                                       ColorSolverMode::OklabSoftCap4Dark4);
            REQUIRE(idx != size_t(-1));
            const Rgb mixed = candidate_rgb(set, idx);
            REQUIRE(mixed[0] == palette[component][0]);
            REQUIRE(mixed[1] == palette[component][1]);
            REQUIRE(mixed[2] == palette[component][2]);
            REQUIRE(delta_e_2000(mixed, palette[component]) == 0.0);
        }
    }
}

TEST_CASE("color mix solver: the same inputs give the same weights", "[color_mix_solver]")
{
    const std::vector<Rgb> &palette = cmyw_palette();
    const Rgb target = rgb_from_hex("#7A5C3F");

    const ColorSolverCandidateSet first  = build_color_solver_candidates(palette);
    const ColorSolverCandidateSet second = build_color_solver_candidates(palette);

    REQUIRE(first.candidate_count() == second.candidate_count());
    REQUIRE(first.total_units == second.total_units);
    REQUIRE(first.rgbs == second.rgbs);
    REQUIRE(first.weights == second.weights);

    for (ColorSolverMode mode : { ColorSolverMode::RGB, ColorSolverMode::Oklab, ColorSolverMode::OklabSoftCap4Dark4 }) {
        for (ColorSolverLookupMode lookup : { ColorSolverLookupMode::ClosestMix, ColorSolverLookupMode::BlendClosestTwo }) {
            const std::vector<float> a = solve_color_solver_weights_for_target(first, target, lookup, mode);
            const std::vector<float> b = solve_color_solver_weights_for_target(second, target, lookup, mode);
            const std::vector<float> c = solve_color_solver_weights_for_target(first, target, lookup, mode);
            REQUIRE(a == b);
            REQUIRE(a == c);
        }
    }

    // The cache must hand back the identical set, not an equal-but-rebuilt one.
    ColorSolverCandidateCache cache;
    const ColorSolverCandidateSet &cached_a = color_solver_candidates(cache, palette);
    const ColorSolverCandidateSet &cached_b = color_solver_candidates(cache, palette);
    REQUIRE(&cached_a == &cached_b);
    REQUIRE(cached_a.rgbs == first.rgbs);
}

TEST_CASE("color mix solver: a ramp between two filaments never reverses", "[color_mix_solver]")
{
    // Two components only, so the answer is one number: how much of the second
    // filament.  Targets are the true reachable ramp, sampled every 5%.
    const std::vector<Rgb> palette = { rgb_from_hex("#0086D6"), rgb_from_hex("#FFE800") };
    const ColorSolverCandidateSet set = build_color_solver_candidates(palette);
    REQUIRE_FALSE(set.empty());

    float previous = -1.f;
    for (int percent = 0; percent <= 100; percent += 5) {
        std::vector<float> target_weights = { 1.f - float(percent) / 100.f, float(percent) / 100.f };
        const Rgb target = mix_color_solver_components(palette, target_weights);

        const std::vector<float> solved = solve_color_solver_weights_for_target(
            set, target, ColorSolverLookupMode::ClosestMix, ColorSolverMode::OklabSoftCap4Dark4);
        REQUIRE(solved.size() == 2);
        REQUIRE(solved[0] + solved[1] == Approx(1.f));
        // Monotone: more of the second filament asked for, never less returned.
        REQUIRE(solved[1] >= previous - 1e-6f);
        previous = solved[1];

        // And it lands on the ramp point it was asked for, not somewhere else.
        const size_t idx = solve_color_solver_candidate_for_target(set, target, ColorSolverMode::OklabSoftCap4Dark4);
        REQUIRE(idx != size_t(-1));
        REQUIRE(delta_e_2000(candidate_rgb(set, idx), target) < 1.0);
    }
    REQUIRE(previous == Approx(1.f));
}

TEST_CASE("color mix solver: golden targets land inside a stated dE2000", "[color_mix_solver]")
{
    const std::vector<Rgb> &palette = cmyw_palette();
    const ColorSolverCandidateSet set = build_color_solver_candidates(palette);
    REQUIRE_FALSE(set.empty());

    // Four targets a user would plausibly ask a CMYW printer for. The bound is
    // the measured worst case rounded up, and it is the number the docs quote;
    // if a change to the mixer or the lattice makes any of these worse, this
    // fails rather than silently degrading the dialog's suggestions.
    struct GoldenCase { const char *hex; double max_delta_e; };
    static const GoldenCase cases[] = {
        { "#7A5C3F",  9.5 },  // a mid brown       - measured 8.898
        { "#4CAF50",  1.0 },  // a mid green       - measured 0.308
        { "#FF7043",  2.5 },  // a warm orange     - measured 2.043
        { "#B0BEC5",  5.0 }   // a light grey      - measured 4.217
    };

    for (const GoldenCase &c : cases) {
        const Rgb target = rgb_from_hex(c.hex);
        const size_t idx = solve_color_solver_candidate_for_target(set, target, ColorSolverMode::OklabSoftCap4Dark4);
        REQUIRE(idx != size_t(-1));
        const Rgb  mixed = candidate_rgb(set, idx);
        const double de  = delta_e_2000(mixed, target);
        INFO("target " << c.hex << " -> dE2000 " << de);
        REQUIRE(de <= c.max_delta_e);

        // The weights the dialog would write out: on the unit lattice, summing
        // to one, and every one of them reachable.
        const std::vector<float> weights = solve_color_solver_weights_for_target(
            set, target, ColorSolverLookupMode::ClosestMix, ColorSolverMode::OklabSoftCap4Dark4);
        float sum = 0.f;
        for (const float w : weights) {
            REQUIRE(w >= 0.f);
            sum += w;
        }
        REQUIRE(sum == Approx(1.f));
    }
}

TEST_CASE("color mix solver: a mix beats the nearest single filament", "[color_mix_solver]")
{
    // This is the change, stated as a measurement: for every target below, the
    // closest reachable mix is closer in dE2000 than the answer a nearest-single
    // -filament CIE76 matcher gives - which is what the fork's import-side
    // matcher (ObjColorMatch) does today.
    const std::vector<Rgb> &palette = cmyw_palette();
    const ColorSolverCandidateSet set = build_color_solver_candidates(palette);

    static const char *targets[] = { "#7A5C3F", "#4CAF50", "#FF7043", "#B0BEC5", "#8E24AA", "#00897B" };

    double sum_mix = 0.0, sum_single = 0.0;
    for (const char *hex : targets) {
        const Rgb target = rgb_from_hex(hex);
        const size_t idx = solve_color_solver_candidate_for_target(set, target, ColorSolverMode::OklabSoftCap4Dark4);
        REQUIRE(idx != size_t(-1));

        const double de_mix    = delta_e_2000(candidate_rgb(set, idx), target);
        const double de_single = delta_e_2000(nearest_single_filament_cie76(palette, target), target);
        INFO("target " << hex << ": mix dE2000 " << de_mix << " vs nearest single " << de_single);
        REQUIRE(de_mix < de_single);
        sum_mix += de_mix;
        sum_single += de_single;
    }
    INFO("mean dE2000: mix " << sum_mix / 6.0 << " vs nearest single " << sum_single / 6.0);
    REQUIRE(sum_mix < sum_single);
}

TEST_CASE("color mix solver: constraints are honoured", "[color_mix_solver]")
{
    const std::vector<Rgb> &palette = cmyw_palette();

    SECTION("a minimum share excludes every mix that would undershoot it") {
        ColorSolverConstraints constraints;
        constraints.min_component_percent = 15;
        const ColorSolverCandidateSet set = build_color_solver_candidates(palette, 0, constraints);
        REQUIRE_FALSE(set.empty());
        for (size_t candidate = 0; candidate < set.candidate_count(); ++candidate)
            for (size_t component = 0; component < set.component_count; ++component) {
                const float w = set.weights[candidate * set.component_count + component];
                REQUIRE((w == 0.f || w >= 0.15f - 1e-6f));
            }
    }

    SECTION("a component cap limits how many filaments a recipe may use") {
        ColorSolverConstraints constraints;
        constraints.max_components = 2;
        const ColorSolverCandidateSet set = build_color_solver_candidates(palette, 0, constraints);
        REQUIRE_FALSE(set.empty());
        for (size_t candidate = 0; candidate < set.candidate_count(); ++candidate) {
            size_t used = 0;
            for (size_t component = 0; component < set.component_count; ++component)
                if (set.weights[candidate * set.component_count + component] > 0.f)
                    ++used;
            REQUIRE(used >= 1);
            REQUIRE(used <= 2);
        }
        // Pure filaments survive the cap, so endpoint exactness still holds.
        const size_t idx = solve_color_solver_candidate_for_target(set, palette[2], ColorSolverMode::OklabSoftCap4Dark4);
        REQUIRE(idx != size_t(-1));
        REQUIRE(candidate_rgb(set, idx)[0] == palette[2][0]);
    }

    SECTION("a two-component floor keeps every recipe a real mix") {
        ColorSolverConstraints constraints;
        constraints.min_components = 2;
        constraints.max_components = 3;
        const ColorSolverCandidateSet set = build_color_solver_candidates(palette, 0, constraints);
        REQUIRE_FALSE(set.empty());
        for (size_t candidate = 0; candidate < set.candidate_count(); ++candidate) {
            size_t used = 0;
            for (size_t component = 0; component < set.component_count; ++component)
                if (set.weights[candidate * set.component_count + component] > 0.f)
                    ++used;
            REQUIRE(used >= 2);
            REQUIRE(used <= 3);
        }
    }

    SECTION("an incompatible pair is never enumerated") {
        // Say filaments 0 and 1 cannot be mixed (different material families).
        const size_t n = palette.size();
        ColorSolverConstraints constraints;
        constraints.min_components = 2;
        constraints.max_components = 3;
        constraints.allowed_pairs.assign(n * n, 1);
        constraints.allowed_pairs[0 * n + 1] = 0;
        constraints.allowed_pairs[1 * n + 0] = 0;

        const ColorSolverCandidateSet set = build_color_solver_candidates(palette, 0, constraints);
        REQUIRE_FALSE(set.empty());
        for (size_t candidate = 0; candidate < set.candidate_count(); ++candidate) {
            const float w0 = set.weights[candidate * set.component_count + 0];
            const float w1 = set.weights[candidate * set.component_count + 1];
            REQUIRE_FALSE((w0 > 0.f && w1 > 0.f));
        }
    }
}

TEST_CASE("color mix solver: the mix model is the fork's own mixer", "[color_mix_solver]")
{
    // Two components, 50/50: the solver must produce exactly what
    // Slic3r::filament_mixer_lerp produces, because the sidebar chip and the
    // preview swatch are drawn with that function. If these ever diverge the
    // user sees a suggestion that does not match its own swatch.
    const std::vector<Rgb> palette = { rgb_from_hex("#002185"), rgb_from_hex("#FCD300") };
    unsigned char r = 0, g = 0, b = 0;
    Slic3r::filament_mixer_lerp(0x00, 0x21, 0x85, 0xFC, 0xD3, 0x00, 0.5f, &r, &g, &b);

    const Rgb mixed = mix_color_solver_components(palette, std::vector<int>{ 20, 20 });
    REQUIRE(int(std::lround(mixed[0] * 255.f)) == int(r));
    REQUIRE(int(std::lround(mixed[1] * 255.f)) == int(g));
    REQUIRE(int(std::lround(mixed[2] * 255.f)) == int(b));
}
