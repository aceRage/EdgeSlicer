// EdgeSlicer, mixed filament wall patterns: the too few walls notice (libslic3r/MixedPatternWalls.hpp).
#include <catch2/catch.hpp>

#include <string>
#include <vector>

#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/MixedPatternWalls.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r;
using Catch::Matchers::Contains;
using Catch::Matchers::WithinAbs;

namespace {

Point pt(double x, double y) { return Point(coord_t(scale_(x)), coord_t(scale_(y))); }

ExPolygons strip(double w, double h)
{
    ExPolygon ex;
    ex.contour.points = { pt(0., 0.), pt(w, 0.), pt(w, h), pt(0., h) };
    return { ex };
}

double area_mm2(const ExPolygons &expolygons)
{
    double a = 0.;
    for (const ExPolygon &ex : expolygons)
        a += ex.area() * SCALING_FACTOR * SCALING_FACTOR;
    return a;
}

const coord_t kWidth   = scale_(0.45);
const coord_t kSpacing = scale_(0.40);

} // namespace

TEST_CASE("Mixed pattern walls: where fewer walls fit", "[MixedPatternWalls]")
{
    // Three classic walls need 0.45 + 4 * 0.40 = 2.05 mm (reported under 0.95 of that, 1.95 mm).
    SECTION("a wide outline has no thin areas") {
        CHECK(areas_with_fewer_walls(strip(20., 20.), kWidth, kSpacing, 3, false).empty());
    }
    SECTION("a strip too thin for the third wall is reported whole") {
        const ExPolygons thin = areas_with_fewer_walls(strip(20., 1.2), kWidth, kSpacing, 3, false);
        REQUIRE_FALSE(thin.empty());
        CHECK_THAT(area_mm2(thin), WithinAbs(24., 0.5));
    }
    SECTION("the same strip holds two walls") {
        CHECK(areas_with_fewer_walls(strip(20., 1.2), kWidth, kSpacing, 2, false).empty());
    }
    SECTION("one wall always fits as far as this check goes") {
        CHECK(areas_with_fewer_walls(strip(20., 0.3), kWidth, kSpacing, 1, false).empty());
    }
    SECTION("Arachne fits a centre line where classic does not") {
        CHECK_FALSE(areas_with_fewer_walls(strip(20., 1.8), kWidth, kSpacing, 3, false).empty());
        CHECK(areas_with_fewer_walls(strip(20., 1.8), kWidth, kSpacing, 3, true).empty());
    }
    SECTION("a thin fin on a wide body: only the fin") {
        ExPolygon ex;
        ex.contour.points = { pt(0., 0.), pt(20., 0.), pt(20., 20.), pt(11., 20.), pt(11., 30.), pt(10., 30.), pt(10., 20.), pt(0., 20.) };
        const ExPolygons thin = areas_with_fewer_walls({ ex }, kWidth, kSpacing, 3, false);
        REQUIRE_FALSE(thin.empty());
        const double a = area_mm2(thin);
        CHECK(a > 9.);
        CHECK(a < 12.);
    }
}

TEST_CASE("Mixed pattern walls: notice text", "[MixedPatternWalls]")
{
    MixedPatternWallShortfall f;
    f.filament_id  = 6;
    f.walls_needed = 3;

    SECTION("nothing found, no notice") {
        CHECK(mixed_pattern_walls_notice({}, "part").empty());
        CHECK(mixed_pattern_walls_notice({ f }, "part").empty());
    }
    SECTION("setting too low") {
        f.walls_set = 2;
        const std::string text = mixed_pattern_walls_notice({ f }, "part");
        CHECK_THAT(text, Contains("part: mixed filament 6"));
        CHECK_THAT(text, Contains("pattern for 3 walls"));
        CHECK_THAT(text, Contains("prints 2"));
    }
    SECTION("thin areas need some area before they are mentioned") {
        f.thin_layers  = 1;
        f.thin_area    = 3.;
        f.first_thin_z = 1.2;
        CHECK(mixed_pattern_walls_notice({ f }, "part").empty());
        f.thin_layers = 12;
        f.thin_area   = 200.;
        const std::string text = mixed_pattern_walls_notice({ f }, "part");
        CHECK_THAT(text, Contains("thin areas on 12 layers"));
        CHECK_THAT(text, Contains("1.2 mm"));
    }
}
