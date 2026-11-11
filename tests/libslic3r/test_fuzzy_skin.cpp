#include <catch2/catch.hpp>

#include "libslic3r/Arachne/utils/ExtrusionLine.hpp"
#include "libslic3r/Feature/FuzzySkin/FuzzySkin.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <libnoise/noise.h>
#include <libnoise/noisegen.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

using namespace Slic3r;

namespace {

// Closed 20 mm square loop at a uniform width.
Arachne::ExtrusionJunctions square_loop(coord_t width)
{
    const coord_t s = scaled<coord_t>(20.);
    return {{Point(0, 0), width, 0}, {Point(s, 0), width, 0}, {Point(s, s), width, 0}, {Point(0, s), width, 0}, {Point(0, 0), width, 0}};
}

FuzzySkinConfig thick_fuzzy_config(FuzzySkinMode mode, NoiseType noise_type, double thickness_mm)
{
    FuzzySkinConfig cfg{};
    cfg.type              = FuzzySkinType::All;
    cfg.thickness         = scaled<coord_t>(thickness_mm);
    cfg.point_distance    = scaled<coord_t>(0.3);
    cfg.fuzzy_first_layer = true;
    cfg.noise_type        = noise_type;
    cfg.noise_scale       = 1.0;
    cfg.noise_octaves     = 4;
    cfg.noise_persistence = 0.5;
    cfg.mode              = mode;
    return cfg;
}

} // namespace

// Extrusion and Combined mode add noise to each junction's width. A junction narrower than
// height * (1 - PI/4) makes Flow::rounded_rectangle_extrusion_spacing() throw and fails the slice.
// The fuzz thickness is 3x the line width so the clamp is hit on every run regardless of RNG seed.
// Ridged multifractal is covered because its output is not bounded to [-1, 1], so it scales past
// the configured thickness; the floor has to hold for any noise value, not just an in-range one.
TEST_CASE("Fuzzy skin extrusion width is floored at the minimum the flow accepts", "[FuzzySkin]") {
    using namespace Slic3r::Feature::FuzzySkin;

    const double layer_height = GENERATE(0.08, 0.2, 0.28);
    const auto   mode         = GENERATE(FuzzySkinMode::Extrusion, FuzzySkinMode::Combined);
    const auto   noise_type   = GENERATE(NoiseType::Classic, NoiseType::Perlin, NoiseType::Billow, NoiseType::RidgedMulti, NoiseType::Voronoi);
    CAPTURE(layer_height, int(mode), int(noise_type));

    const double line_width_mm = 0.42;
    auto         loop          = square_loop(scaled<coord_t>(line_width_mm));
    fuzzy_extrusion_line(loop, /*slice_z*/ 1.0, layer_height, thick_fuzzy_config(mode, noise_type, 3 * line_width_mm));

    REQUIRE(loop.size() > 100);

    const auto   narrowest    = std::min_element(loop.begin(), loop.end(), [](const auto& a, const auto& b) { return a.w < b.w; });
    const double narrowest_mm = unscaled<double>(narrowest->w);
    const double floor_mm     = layer_height * (1. - 0.25 * PI);
    CAPTURE(narrowest_mm, floor_mm);

    CHECK(narrowest_mm < line_width_mm); // the clamp was exercised
    CHECK(narrowest_mm > floor_mm);
    CHECK_NOTHROW(Flow::rounded_rectangle_extrusion_spacing(float(narrowest_mm), float(layer_height)));
}

// Displacement mode only moves points; widths must pass through unchanged.
TEST_CASE("Fuzzy skin displacement mode leaves widths untouched", "[FuzzySkin]") {
    using namespace Slic3r::Feature::FuzzySkin;

    const coord_t width = scaled<coord_t>(0.42);
    auto          loop  = square_loop(width);
    fuzzy_extrusion_line(loop, /*slice_z*/ 1.0, /*layer_height*/ 0.2, thick_fuzzy_config(FuzzySkinMode::Displacement, NoiseType::Classic, 1.26));

    REQUIRE(loop.size() > 100);
    CHECK(std::all_of(loop.begin(), loop.end(), [width](const auto& j) { return j.w == width; }));
}

// ---------------------------------------------------------------------------------------------------
// Ripple (Orca #13471) on open runs: a painted split of a wall, a straight thin wall.
// ---------------------------------------------------------------------------------------------------
namespace {

// Ripple: amplitude 0.3 mm, a sample every 0.3 mm, two waves along the run.
FuzzySkinConfig ripple_config()
{
    FuzzySkinConfig cfg               = thick_fuzzy_config(FuzzySkinMode::Displacement, NoiseType::Ripple, 0.3);
    cfg.ripples_per_layer             = 2;
    cfg.ripple_offset                 = 0.;
    cfg.layers_between_ripple_offset  = 1;
    cfg.layer_id                      = 1;
    return cfg;
}

// Largest distance of any point from the y = 0 line the run lies on, in mm.
double max_offset_mm(const Points& pts)
{
    double m = 0.;
    for (const Point& p : pts)
        m = std::max(m, std::abs(unscaled<double>(p.y())));
    return m;
}

// Whether the points never step back along x, i.e. the run never doubles back on itself.
bool never_backtracks(const Points& pts)
{
    for (size_t i = 1; i < pts.size(); ++i)
        if (pts[i].x() < pts[i - 1].x())
            return false;
    return true;
}

Points junction_points(const Arachne::ExtrusionJunctions& junctions)
{
    Points pts;
    for (const auto& j : junctions)
        pts.push_back(j.p);
    return pts;
}

} // namespace

TEST_CASE("Ripple reaches an open run of only two points", "[FuzzySkin][Ripple]") {
    using namespace Slic3r::Feature::FuzzySkin;

    // Classic: a painted split hands the run over without its two ends, which the caller puts back.
    Points run{Point(0, 0), Point(scaled<coord_t>(10.), 0)};
    fuzzy_polyline(run, /*closed*/ false, /*slice_z*/ 1.0, ripple_config());
    CHECK(run.size() > 20);
    CHECK(max_offset_mm(run) > 0.1);
    CHECK(never_backtracks(run));

    // Arachne: a straight open line (a thin wall) keeps its own two ends around the samples.
    const coord_t w = scaled<coord_t>(0.42);
    Arachne::ExtrusionJunctions line{{Point(0, 0), w, 0}, {Point(scaled<coord_t>(10.), 0), w, 0}};
    fuzzy_extrusion_line(line, /*slice_z*/ 1.0, /*layer_height*/ 0.2, ripple_config());
    REQUIRE(line.size() > 20);
    CHECK(line.front().p == Point(0, 0));
    CHECK(line.back().p == Point(scaled<coord_t>(10.), 0));
    CHECK(max_offset_mm(junction_points(line)) > 0.1);
    CHECK(never_backtracks(junction_points(line)));
}

TEST_CASE("Ripple on a run too short for a sample does not backtrack", "[FuzzySkin][Ripple]") {
    using namespace Slic3r::Feature::FuzzySkin;

    // 0.2 mm long, shorter than one 0.3 mm step: nothing to sample. The classic run comes back as its own
    // interior vertex, without the padding a closed loop gets (which went back to the next-to-last point).
    Points run{Point(0, 0), Point(scaled<coord_t>(0.1), 0), Point(scaled<coord_t>(0.2), 0)};
    fuzzy_polyline(run, /*closed*/ false, /*slice_z*/ 1.0, ripple_config());
    CHECK(run == Points{Point(scaled<coord_t>(0.1), 0)});

    const coord_t w = scaled<coord_t>(0.42);
    Arachne::ExtrusionJunctions line{{Point(0, 0), w, 0}, {Point(scaled<coord_t>(0.1), 0), w, 0}, {Point(scaled<coord_t>(0.2), 0), w, 0}};
    fuzzy_extrusion_line(line, /*slice_z*/ 1.0, /*layer_height*/ 0.2, ripple_config());
    CHECK(line.size() == 3);
    CHECK(never_backtracks(junction_points(line)));
}

TEST_CASE("Ripple's first sample on an open run is not its start point", "[FuzzySkin][Ripple]") {
    using namespace Slic3r::Feature::FuzzySkin;

    Points run{Point(0, 0), Point(scaled<coord_t>(10.), 0)};
    fuzzy_polyline(run, /*closed*/ false, /*slice_z*/ 1.0, ripple_config());
    REQUIRE_FALSE(run.empty());
    // The first sample sits one step (0.3 mm) along the run.
    CHECK(unscaled<double>(run.front().x()) > 0.29);

    const coord_t w = scaled<coord_t>(0.42);
    Arachne::ExtrusionJunctions line{{Point(0, 0), w, 0}, {Point(scaled<coord_t>(10.), 0), w, 0}};
    fuzzy_extrusion_line(line, /*slice_z*/ 1.0, /*layer_height*/ 0.2, ripple_config());
    REQUIRE(line.size() > 2);
    // After the line's own start junction, the first sample is one step along, not a second copy of the start.
    CHECK(unscaled<double>(line[1].p.x()) > 0.29);
}

// ---------------------------------------------------------------------------------------------------
// Voronoi noise. libnoise's value noise (IntValueNoise3D) hashes with integer arithmetic that is meant
// to wrap, and masks the result to 31 bits. libnoise 1.0 does that on int, where overflow is undefined
// behaviour: AppleClang dropped the mask, ValueNoise3D ranged over (-3, 1] on arm64 and [-1, 3) on
// x86_64 instead of [-1, 1], and Voronoi fuzzy skin (the only user of value noise) moved walls by up
// to three times the thickness. deps/libnoise/0001-value-noise-unsigned.patch makes the arithmetic
// unsigned.
//
// If these cases fail, the libnoise this build links was built without the patch by a compiler that
// drops the mask: rebuild the libnoise dependency.
// ---------------------------------------------------------------------------------------------------
namespace {

// IntValueNoise3D as libnoise documents it (0 to 2147483647): every operation wraps modulo 2^32.
int32_t wrapping_int_value_noise(int32_t x, int32_t y, int32_t z, int32_t seed)
{
    uint32_t n = (1619u * uint32_t(x) + 31337u * uint32_t(y) + 6971u * uint32_t(z) + 1013u * uint32_t(seed)) & 0x7fffffffu;
    n          = (n >> 13) ^ n;
    return int32_t((n * (n * n * 60493u + 19990303u) + 1376312589u) & 0x7fffffffu);
}

// The Voronoi module the way get_noise_module() in FuzzySkin.cpp sets it up.
noise::module::Voronoi fuzzy_voronoi_module(double noise_scale)
{
    noise::module::Voronoi voronoi;
    voronoi.SetFrequency(1 / noise_scale);
    voronoi.SetDisplacement(1.0);
    return voronoi;
}

// A seeded jitter for one test case (the noise itself is not random; the point spacing is).
struct FuzzyTestSeed
{
    explicit FuzzyTestSeed(uint64_t seed) { Feature::FuzzySkin::set_test_random_seed(seed); }
    ~FuzzyTestSeed() { Feature::FuzzySkin::set_test_random_seed(std::nullopt); }
};

// The 20 mm square the Voronoi cases fuzz, away from the origin so the noise sees varied coordinates.
constexpr double kSquareMin = 100.;
constexpr double kSquareMax = 120.;

// Distance in mm from a point to the outline of that square.
double distance_to_square_mm(const Point& p)
{
    const double x  = unscaled<double>(p.x());
    const double y  = unscaled<double>(p.y());
    const double dx = std::max({kSquareMin - x, 0., x - kSquareMax});
    const double dy = std::max({kSquareMin - y, 0., y - kSquareMax});
    if (dx > 0. || dy > 0.)
        return std::hypot(dx, dy);
    return std::min({x - kSquareMin, kSquareMax - x, y - kSquareMin, kSquareMax - y});
}

} // namespace

TEST_CASE("libnoise value noise wraps and stays in its documented range", "[FuzzySkin][VoronoiNoise]") {
    // Values of libnoise 1.0 built without optimization (and with -fwrapv), where the int arithmetic
    // wraps. The fourth one is the sample that came back as -1598971919.
    struct Sample { int x, y, z, seed, value; };
    static const Sample samples[] = {
        {0, 0, 0, 0, 1376312589},
        {1, 2, 3, 0, 448794225},
        {-1, -2, -3, 1, 1149505707},
        {1000, -1000, 7, 2, 548511729},
        {123456, 654321, -99, 0, 674942913},
        {46341, 46341, 46341, 7, 285047885},
        {31, -17, 255, 12345, 2076098819},
    };
    for (const Sample& s : samples) {
        CAPTURE(s.x, s.y, s.z, s.seed);
        CHECK(noise::IntValueNoise3D(s.x, s.y, s.z, s.seed) == s.value);
        CHECK(wrapping_int_value_noise(s.x, s.y, s.z, s.seed) == s.value);
        CHECK(noise::ValueNoise3D(s.x, s.y, s.z, s.seed) == 1.0 - double(s.value) / 1073741824.0);
    }

    // A grid of lattice points, with the three seeds the Voronoi module uses (seed, seed + 1, seed + 2).
    size_t negative = 0, not_wrapping = 0, out_of_range = 0;
    double lo = 0., hi = 0.;
    for (int x = -60; x <= 60; ++x)
        for (int y = -60; y <= 60; ++y)
            for (int z = -3; z <= 3; ++z)
                for (int seed = 0; seed < 3; ++seed) {
                    const int    i = noise::IntValueNoise3D(x, y, z, seed);
                    const double v = noise::ValueNoise3D(x, y, z, seed);
                    negative += i < 0;
                    not_wrapping += i != wrapping_int_value_noise(x, y, z, seed);
                    out_of_range += !(v >= -1. && v <= 1.);
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
    CAPTURE(lo, hi);
    CHECK(negative == 0);
    CHECK(not_wrapping == 0);
    CHECK(out_of_range == 0);
    // The range is used, not just respected.
    CHECK(lo < -0.99);
    CHECK(hi > 0.99);
}

TEST_CASE("Voronoi noise as fuzzy skin sets it up stays within [-1, 1]", "[FuzzySkin][VoronoiNoise]") {
    // (fuzzy_skin_scale, x, y, z in mm, value): libnoise 1.0 built without optimization, where the int
    // arithmetic wraps. Each point is more than a micron inside its cell.
    struct Sample { double scale, x, y, z, value; };
    static const Sample samples[] = {
        {1.0, 0.25, 0.25, 0.2, 0.37672762479633093},
        {1.0, 12.345, -6.789, 0.2, 0.51453837659209967},
        {1.0, -3.5, 7.25, 1.4, -0.89568399731069803},
        {1.0, 100., 100., 0.6, -0.45658540818840265},
        {1.0, -250.123456, 180.654321, 12.8, 0.018568872474133968},
        {0.3, 3., -4., 5., -0.19262758921831846},
        {0.3, 19.9, 20.1, 250., -0.79711391124874353},
        {2.5, 171.3, 44.7, 33.4, 0.55699233617633581},
    };
    for (const Sample& s : samples) {
        CAPTURE(s.scale, s.x, s.y, s.z);
        CHECK(fuzzy_voronoi_module(s.scale).GetValue(s.x, s.y, s.z) == Approx(s.value).margin(1e-12));
    }

    // A spread of points across a bed and a few layers, at a fine, the default and a coarse scale.
    const double                 noise_scale  = GENERATE(0.3, 1.0, 5.0);
    const noise::module::Voronoi voronoi      = fuzzy_voronoi_module(noise_scale);
    size_t                       out_of_range = 0;
    double                       lo = 0., hi = 0.;
    for (double x = -150.; x <= 150.; x += 1.87)
        for (double y = -150.; y <= 150.; y += 2.31)
            for (double z = 0.2; z < 3.; z += 0.4) {
                const double v = voronoi.GetValue(x, y, z);
                out_of_range += !(v >= -1. && v <= 1.);
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
    CAPTURE(noise_scale, lo, hi);
    CHECK(out_of_range == 0);
    CHECK(lo < -0.9);
    CHECK(hi > 0.9);
}

// What the user sees: with Displacement fuzz every point moves across the wall by noise * thickness,
// so no point of a fuzzed square may lie further from the square than fuzzy_skin_thickness.
TEST_CASE("Voronoi fuzzy skin stays within the thickness of the outline", "[FuzzySkin][VoronoiNoise]") {
    using namespace Slic3r::Feature::FuzzySkin;

    const FuzzyTestSeed seed(20261111);
    const double        thickness_mm = 0.3;
    const double        noise_scale  = GENERATE(0.3, 1.0, 5.0);
    FuzzySkinConfig     cfg          = thick_fuzzy_config(FuzzySkinMode::Displacement, NoiseType::Voronoi, thickness_mm);
    cfg.noise_scale                  = noise_scale;

    const coord_t a = scaled<coord_t>(kSquareMin);
    const coord_t b = scaled<coord_t>(kSquareMax);
    const coord_t w = scaled<coord_t>(0.42);
    // A scaled unit or two of rounding (the sample and its offset are each truncated to coord_t).
    const double tolerance_mm = 1e-5;

    double furthest_classic = 0., furthest_arachne = 0.;
    size_t points = 0;
    for (int layer = 1; layer <= 40; ++layer) {
        const double slice_z = 0.2 * layer;

        Points loop{Point(a, a), Point(b, a), Point(b, b), Point(a, b)};
        fuzzy_polyline(loop, /*closed*/ true, slice_z, cfg);
        REQUIRE(loop.size() > 100);
        for (const Point& p : loop)
            furthest_classic = std::max(furthest_classic, distance_to_square_mm(p));
        points += loop.size();

        Arachne::ExtrusionJunctions wall{{Point(a, a), w, 0}, {Point(b, a), w, 0}, {Point(b, b), w, 0}, {Point(a, b), w, 0},
                                         {Point(a, a), w, 0}};
        fuzzy_extrusion_line(wall, slice_z, /*layer_height*/ 0.2, cfg);
        REQUIRE(wall.size() > 100);
        for (const auto& j : wall)
            furthest_arachne = std::max(furthest_arachne, distance_to_square_mm(j.p));
        points += wall.size();
    }
    CAPTURE(noise_scale, points, furthest_classic, furthest_arachne);
    CHECK(furthest_classic <= thickness_mm + tolerance_mm);
    CHECK(furthest_arachne <= thickness_mm + tolerance_mm);
    // The fuzz does use most of its thickness.
    CHECK(furthest_classic > 0.8 * thickness_mm);
    CHECK(furthest_arachne > 0.8 * thickness_mm);
}
