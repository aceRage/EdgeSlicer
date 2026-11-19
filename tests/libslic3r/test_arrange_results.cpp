// arrangement::arrange() writes each item's result back to that same item. An outline with fewer
// than three corners is not handed to the packer; the results used to be written back by position,
// so every object after it took the place of the next one and the last kept its old place.

#include <catch2/catch.hpp>

#include "libslic3r/Arrange.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExPolygon.hpp"

using namespace Slic3r;
using namespace Slic3r::arrangement;

namespace {

ArrangePolygon square(double side_mm)
{
    ArrangePolygon ap;
    Polygon        outline;
    const coord_t  s = scaled(side_mm);
    outline.points   = {Point(0, 0), Point(s, 0), Point(s, s), Point(0, s)};
    ap.poly          = ExPolygon(outline);
    ap.bed_idx       = 0;
    ap.inflation     = scaled(3.);
    return ap;
}

} // namespace

TEST_CASE("An outline with fewer than three corners does not shift the results of the other items", "[Arrange]")
{
    ArrangePolygons items;
    items.push_back(square(50.));

    ArrangePolygon line;
    line.poly.contour.points = {Point(0, 0), Point(scaled(30.), 0)};
    line.translation         = {scaled(1234.), scaled(567.)};
    line.bed_idx             = 0;
    items.push_back(line);

    items.push_back(square(60.));

    ArrangeParams params;
    int           passes = 0;
    params.stopcondition = [&passes]() { return ++passes > 100000; };
    const BoundingBox bed{Point(0, 0), Point(scaled(250.), scaled(250.))};
    arrange(items, {}, bed, params);

    // The line could not be arranged: it is reported so and keeps its place.
    CHECK(items[1].bed_idx == UNARRANGED);
    CHECK(items[1].translation == Vec2crd(scaled(1234.), scaled(567.)));

    // The squares each got their own result: on the bed, apart from each other.
    REQUIRE(items[0].bed_idx == 0);
    REQUIRE(items[2].bed_idx == 0);
    const BoundingBox a = items[0].transformed_poly().contour.bounding_box();
    const BoundingBox b = items[2].transformed_poly().contour.bounding_box();
    CHECK(a.size().x() == scaled(50.));
    CHECK(b.size().x() == scaled(60.));
    CHECK(bed.contains(a));
    CHECK(bed.contains(b));
    CHECK_FALSE(a.overlap(b));
}
