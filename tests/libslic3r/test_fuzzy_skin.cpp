#include <catch2/catch.hpp>

#include "libslic3r/Arachne/utils/ExtrusionLine.hpp"
#include "libslic3r/Feature/FuzzySkin/FuzzySkin.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>

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
