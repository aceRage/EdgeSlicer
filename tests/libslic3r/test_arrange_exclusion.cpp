// Arrange and bed exclusion zones (bed_exclude_area): the arranged objects must stay off every
// zone, on every bed they go to, while still filling the rest of the bed and going on to further
// beds. The zones are handed to arrangement::arrange() as ArrangeJob does it
// (PartPlateList::preprocess_exclude_areas + update_unselected_items_inflation): a virtual,
// fixed item per zone and per bed, at its place on the bed, grown by the 1 mm exclusion gap.

#include <catch2/catch.hpp>

#include "libslic3r/Arrange.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::arrangement;

namespace {

// Bambu Lab X1/P1 size bed.
const BoundingBox BED{Point(0, 0), Point(scaled(256.), scaled(256.))};
constexpr int     BEDS = 16;

struct Zone
{
    double x0, y0, x1, y1;
};

Polygon zone_polygon(const Zone &z)
{
    return Polygon(Points{Point(scaled(z.x0), scaled(z.y0)), Point(scaled(z.x1), scaled(z.y0)), Point(scaled(z.x1), scaled(z.y1)),
                          Point(scaled(z.x0), scaled(z.y1))});
}

// What ArrangeJob puts into the fixed items for the zones.
ArrangePolygons zone_items(const std::vector<Zone> &zones)
{
    ArrangePolygons out;
    for (size_t i = 0; i < zones.size(); ++i)
        for (int bed = 0; bed < BEDS; ++bed) {
            ArrangePolygon ap;
            ap.poly.contour   = zone_polygon(zones[i]);
            ap.translation    = Vec2crd(0, 0);
            ap.is_virt_object = true;
            ap.bed_idx        = bed;
            ap.height         = 1;
            ap.name           = "ExcludedRegion" + std::to_string(i);
            ap.inflation      = scaled(1.); // update_unselected_items_inflation's exclusion gap
            out.emplace_back(std::move(ap));
        }
    return out;
}

ArrangePolygons cubes(int count, double size_mm)
{
    ArrangePolygons out;
    for (int i = 0; i < count; ++i) {
        ArrangePolygon ap;
        ap.poly.contour = Polygon(Points{Point(0, 0), Point(scaled(size_mm), 0), Point(scaled(size_mm), scaled(size_mm)), Point(0, scaled(size_mm))});
        ap.inflation    = scaled(1.); // the 1 mm arrange clearance of an object without supports
        ap.bed_idx      = 0;
        ap.height       = size_mm;
        ap.extrude_ids  = {1};
        ap.name         = "cube" + std::to_string(i);
        out.emplace_back(std::move(ap));
    }
    return out;
}

ArrangeParams job_like_params()
{
    ArrangeParams params; // final align on, as ArrangeJob arranges
    params.parallel      = false;
    auto passes          = std::make_shared<int>(0);
    params.stopcondition = [passes]() { return ++*passes > 1000000; };
    return params;
}

double overlap_mm2(const Polygon &a, const Polygon &b)
{
    double area = 0.;
    for (const Polygon &p : intersection(a, b))
        area += std::abs(p.area());
    return area * SCALING_FACTOR * SCALING_FACTOR;
}

struct Result
{
    std::vector<int> per_bed = std::vector<int>(BEDS + 1, 0);
};

// Arranges `count` cubes around the zones and checks every placed cube: on a bed, inside it, off
// every zone.
Result arrange_and_check(const std::vector<Zone> &zones, int count, double size_mm)
{
    ArrangePolygons items = cubes(count, size_mm);
    arrange(items, zone_items(zones), BED, job_like_params());

    Result r;
    for (const ArrangePolygon &ap : items) {
        INFO(ap.name << " on bed " << ap.bed_idx);
        REQUIRE(ap.bed_idx >= 0);
        REQUIRE(ap.bed_idx < BEDS);
        ++r.per_bed[ap.bed_idx];
        const Polygon     placed = ap.transformed_poly().contour;
        const BoundingBox bb     = placed.bounding_box();
        INFO("at " << unscaled(bb.min.x()) << ", " << unscaled(bb.min.y()) << " - " << unscaled(bb.max.x()) << ", " << unscaled(bb.max.y()));
        CHECK(bb.min.x() >= BED.min.x() - SCALED_EPSILON);
        CHECK(bb.min.y() >= BED.min.y() - SCALED_EPSILON);
        CHECK(bb.max.x() <= BED.max.x() + SCALED_EPSILON);
        CHECK(bb.max.y() <= BED.max.y() + SCALED_EPSILON);
        for (const Zone &z : zones)
            CHECK(overlap_mm2(placed, zone_polygon(z)) < 0.01);
    }
    return r;
}

} // namespace

TEST_CASE("Arranged objects stay off a corner exclusion zone and overflow to further beds", "[Arrange]")
{
    // Bambu Lab X1 / P1: the front-left corner.
    const std::vector<Zone> zones = {{0., 0., 18., 28.}};
    // 40 mm cubes, 42 mm apart: 36 fit an empty 256 mm bed, one fewer with the corner taken.
    const Result r = arrange_and_check(zones, 50, 40.);
    INFO("bed 0: " << r.per_bed[0] << ", bed 1: " << r.per_bed[1]);
    CHECK(r.per_bed[0] >= 20);
    CHECK(r.per_bed[1] > 0);
}

TEST_CASE("Arranged objects stay off an exclusion strip along the whole left edge", "[Arrange]")
{
    const std::vector<Zone> zones = {{0., 0., 10., 256.}};
    // 5 columns of 42 mm fit the 245 mm left beside the strip: 30 a bed.
    const Result r = arrange_and_check(zones, 45, 40.);
    INFO("bed 0: " << r.per_bed[0] << ", bed 1: " << r.per_bed[1]);
    CHECK(r.per_bed[0] >= 20);
    CHECK(r.per_bed[1] > 0);
}

TEST_CASE("Arranged objects stay off a corner zone together with an edge strip", "[Arrange]")
{
    // The Bambu Lab base machine profile: a 28 mm corner and an 8 mm strip up the left edge.
    const std::vector<Zone> zones = {{0., 0., 28., 28.}, {0., 28., 8., 256.}};
    const Result            r     = arrange_and_check(zones, 45, 40.);
    INFO("bed 0: " << r.per_bed[0] << ", bed 1: " << r.per_bed[1]);
    CHECK(r.per_bed[0] >= 20);
    CHECK(r.per_bed[1] > 0);
}

TEST_CASE("A pile packed around a large corner zone is not centred onto it", "[Arrange]")
{
    // A big zone makes the packed pile lopsided, the case where centring the pile on the bed
    // moves it furthest. The pile fills about one bed.
    const std::vector<Zone> zones = {{0., 0., 90., 90.}};
    const Result            r     = arrange_and_check(zones, 30, 36.);
    INFO("bed 0: " << r.per_bed[0] << ", bed 1: " << r.per_bed[1]);
    CHECK(r.per_bed[0] >= 20);
}

TEST_CASE("A small pile beside an edge strip is centred on the free part of the bed", "[Arrange]")
{
    // Few objects: the centring still happens, just never onto the strip.
    const std::vector<Zone> zones = {{0., 0., 10., 256.}};
    const Result            r     = arrange_and_check(zones, 4, 40.);
    CHECK(r.per_bed[0] == 4);
}
