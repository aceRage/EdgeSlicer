// OrcaSlicer #17304: arrange keeps objects taller than the rod clearance out of the
// Y span of a prime tower compacted by "No sparse layers". Placement only; G-code is
// unchanged. Without the keep-out, the pile gathers beside the tower and slicing then
// refuses the plate.

#include <catch2/catch.hpp>

#include "libslic3r/Arrange.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExPolygon.hpp"

#include <algorithm>

using namespace Slic3r;
using namespace Slic3r::arrangement;

namespace {

ArrangePolygons squares(size_t n, double side_mm, double height_mm)
{
    ArrangePolygons items;
    items.reserve(n);
    const coord_t side = scaled(side_mm);
    for (size_t i = 0; i < n; ++i) {
        ArrangePolygon ap;
        Polygon        outline;
        outline.points = {Point(0, 0), Point(side, 0), Point(side, side), Point(0, side)};
        ap.poly        = ExPolygon(outline);
        ap.bed_idx     = 0;
        ap.height      = height_mm;
        items.push_back(ap);
    }
    return items;
}

BoundingBox bed(double w_mm, double h_mm)
{
    return BoundingBox{Point(0, 0), Point(scaled(w_mm), scaled(h_mm))};
}

} // namespace

TEST_CASE("Objects taller than the rod clearance stay out of the Y span of a compacted prime tower", "[Arrange]")
{
    // A tower spanning Y 180..240 at the right edge of the bed, with no extra growth on its polygon.
    ArrangePolygon tower;
    Polygon        outline;
    outline.points       = {Point(scaled(180.), scaled(180.)), Point(scaled(240.), scaled(180.)), Point(scaled(240.), scaled(240.)), Point(scaled(180.), scaled(240.))};
    tower.poly           = ExPolygon(outline);
    tower.bed_idx        = 0;
    tower.is_virt_object = true;
    tower.is_wipe_tower  = true;

    ArrangeParams params;
    params.min_obj_distance        = scaled(2.);
    params.clearance_height_to_rod = 24.f;
    params.clearance_dist_to_rod   = 40.f;
    params.compacted_tower_growth  = 0;

    // The pile gathers around the tower, so without the rule the objects end up beside it.
    ArrangePolygons items = squares(4, 40., 50.);
    arrange(items, {tower}, bed(250, 250), params);

    for (const ArrangePolygon &ap : items) {
        REQUIRE(ap.bed_idx == 0);
        CHECK(ap.transformed_poly().contour.bounding_box().max.y() <= scaled(180. - 40.));
    }

    // Objects that stay under the rod may share the span.
    ArrangePolygons low = squares(4, 40., 10.);
    arrange(low, {tower}, bed(250, 250), params);
    CHECK(std::any_of(low.begin(), low.end(), [](const ArrangePolygon &ap) {
        return ap.transformed_poly().contour.bounding_box().max.y() > scaled(180. - 40.);
    }));
}
