// EdgeSlicer: support interface interlock ("woven band"), the pure stripe geometry.
// tests/design_support_interface_interlock.md, section 8 (unit tests on the split helper).

#include <catch2/catch.hpp>

#include <cmath>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Support/SupportInterlock.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

ExPolygon square_mm(double x0, double y0, double size)
{
    ExPolygon out;
    out.contour.points = { Point::new_scale(x0, y0), Point::new_scale(x0 + size, y0),
                           Point::new_scale(x0 + size, y0 + size), Point::new_scale(x0, y0 + size) };
    return out;
}

ExPolygon rect_mm(double x0, double y0, double w, double h)
{
    ExPolygon out;
    out.contour.points = { Point::new_scale(x0, y0), Point::new_scale(x0 + w, y0),
                           Point::new_scale(x0 + w, y0 + h), Point::new_scale(x0, y0 + h) };
    return out;
}

double area_of(const Polygons &polys)
{
    double a = 0;
    for (const ExPolygon &ex : union_ex(polys))
        a += ex.area();
    return a;
}

// The stripe coordinate range of a set of polygons.
std::pair<double, double> stripe_extent(const Polygons &polys, double angle)
{
    double lo = std::numeric_limits<double>::max(), hi = - lo;
    for (const Polygon &p : polys)
        for (const Point &pt : p.points) {
            const double u = support_interlock_stripe_coordinate(pt, angle);
            lo = std::min(lo, u);
            hi = std::max(hi, u);
        }
    return { lo, hi };
}

// 0.4 mm line spacing, 2 lines per stripe.
const double  spacing_mm = 0.4;
const coord_t line_w     = coord_t(scale_(spacing_mm));
const coord_t beam       = 2 * line_w;

} // namespace

TEST_CASE("Interlock split covers the footprint exactly once", "[SupportInterlock]")
{
    const double angle = GENERATE(0., M_PI / 4., M_PI / 2., 1.1);
    CAPTURE(angle);
    const ExPolygons footprint { square_mm(-7.3, 2.1, 15.) };
    const SupportInterlockSplit split = support_interlock_split(footprint, angle, beam, line_w);

    REQUIRE(! split.body.empty());
    REQUIRE(! split.interface.empty());
    REQUIRE(split.unsplit.empty());
    const double total = footprint.front().area();
    // Body + interface = footprint.
    CHECK_THAT(area_of(split.body) + area_of(split.interface), WithinRel(total, 1e-4));
    CHECK_THAT(area_of(union_(split.body, split.interface)), WithinRel(total, 1e-4));
    // No overlap between the two kinds.
    CHECK_THAT(area_of(intersection(split.body, split.interface)), WithinAbs(0., 1e-4 * total));
    // Roughly half each.
    CHECK_THAT(area_of(split.body) / total, WithinAbs(0.5, 0.1));
}

TEST_CASE("Interlock stripes are one beam wide and alternate body and interface", "[SupportInterlock]")
{
    const double angle = GENERATE(0., M_PI / 2., 0.3);
    CAPTURE(angle);
    const ExPolygons footprint { square_mm(-10., -10., 20.) };
    const SupportInterlockSplit split = support_interlock_split(footprint, angle, beam, line_w);

    // Every stripe that does not touch the outline is exactly one beam across, and lies on the grid.
    size_t full_stripes = 0;
    for (const ExPolygon &stripe : union_ex(split.body)) {
        const auto [lo, hi] = stripe_extent(to_polygons(stripe), angle);
        if (hi - lo < 0.9 * double(beam))
            continue; // clipped by the outline at a corner
        ++ full_stripes;
        CHECK_THAT(hi - lo, WithinAbs(double(beam), 10.));
        // Starts on an even stripe edge of the global grid.
        const double k = lo / double(beam);
        CHECK_THAT(k - std::round(k), WithinAbs(0., 1e-3));
        CHECK(int64_t(std::llround(k)) % 2 == 0);
    }
    CHECK(full_stripes >= 10);
    for (const ExPolygon &stripe : union_ex(split.interface)) {
        const auto [lo, hi] = stripe_extent(to_polygons(stripe), angle);
        if (hi - lo < 0.9 * double(beam))
            continue;
        CHECK_THAT(hi - lo, WithinAbs(double(beam), 10.));
    }
    // Point classification agrees with the split.
    for (const ExPolygon &stripe : union_ex(split.body))
        CHECK(support_interlock_is_body(stripe.contour.centroid(), angle, beam));
    for (const ExPolygon &stripe : union_ex(split.interface))
        CHECK_FALSE(support_interlock_is_body(stripe.contour.centroid(), angle, beam));
}

TEST_CASE("Interlock stripes of successive woven layers cross at 90 degrees", "[SupportInterlock]")
{
    // Stripes run along their angle: a stripe of the layer at angle a is long along a and one beam
    // across; the layer under it, at a + 90, is long along the other axis.
    const ExPolygons footprint { square_mm(0., 0., 20.) };
    const double a = 0.;
    const SupportInterlockSplit l1 = support_interlock_split(footprint, a, beam, line_w);
    const SupportInterlockSplit l2 = support_interlock_split(footprint, a + M_PI / 2., beam, line_w);
    for (const ExPolygon &stripe : union_ex(l1.body)) {
        const BoundingBox bb = get_extents(stripe);
        CHECK(bb.size().x() > 5 * bb.size().y()); // runs along X
    }
    for (const ExPolygon &stripe : union_ex(l2.body)) {
        const BoundingBox bb = get_extents(stripe);
        CHECK(bb.size().y() > 5 * bb.size().x()); // runs along Y
    }
    // Every body stripe of one layer crosses the interface stripes of the other: that is the undercut.
    CHECK(area_of(intersection(l1.body, l2.interface)) > 0.2 * footprint.front().area());
    CHECK(area_of(intersection(l1.interface, l2.body)) > 0.2 * footprint.front().area());
}

TEST_CASE("Interlock stripe phase is fixed to the global grid", "[SupportInterlock]")
{
    // Two overlapping islands, offset by an amount that is not a whole number of stripes. In their
    // overlap they must be split identically: the grid belongs to the object, not to the island.
    const double angle = 0.7;
    const ExPolygons a { square_mm(0., 0., 12.) };
    const ExPolygons b { square_mm(3.37, 1.91, 12.) };
    const SupportInterlockSplit sa = support_interlock_split(a, angle, beam, line_w);
    const SupportInterlockSplit sb = support_interlock_split(b, angle, beam, line_w);
    // Keep away from the outlines, where the sliver hand-over may differ.
    const Polygons overlap = shrink(intersection(to_polygons(a), to_polygons(b)), float(scale_(1.)));
    REQUIRE(area_of(overlap) > 0);
    const Polygons body_a = intersection(sa.body, overlap);
    const Polygons body_b = intersection(sb.body, overlap);
    CHECK(area_of(body_a) > 0);
    CHECK_THAT(area_of(diff(body_a, body_b)) + area_of(diff(body_b, body_a)), WithinAbs(0., 1e-4 * area_of(overlap)));
}

TEST_CASE("Interlock leaves islands narrower than two beams unsplit", "[SupportInterlock]")
{
    // 1.5 beams wide: a single stripe of each kind would be a comb, not a weave.
    const ExPolygons narrow { rect_mm(0., 0., 1.5 * unscale<double>(beam), 20.) };
    const SupportInterlockSplit split = support_interlock_split(narrow, 0., beam, line_w);
    CHECK(split.body.empty());
    CHECK(split.interface.empty());
    REQUIRE(! split.unsplit.empty());
    CHECK_THAT(area_of(split.unsplit), WithinRel(narrow.front().area(), 1e-6));

    // A wide island next to it is still woven.
    const ExPolygons both { narrow.front(), square_mm(10., 0., 10.) };
    const SupportInterlockSplit split2 = support_interlock_split(both, 0., beam, line_w);
    CHECK(! split2.body.empty());
    CHECK_THAT(area_of(split2.unsplit), WithinRel(narrow.front().area(), 1e-6));
}

TEST_CASE("A woven stripe holds exactly its lines, half a line in from its sides", "[SupportInterlock]")
{
    const int    lines = GENERATE(1, 2, 3);
    const double angle = GENERATE(0., M_PI / 2., 0.6);
    CAPTURE(lines, angle);
    const coord_t beam_n = coord_t(lines) * line_w;
    const ExPolygons footprint { square_mm(-8., -8., 16.) };
    const SupportInterlockSplit split = support_interlock_split(footprint, angle, beam_n, line_w);

    size_t checked = 0;
    for (const ExPolygon &stripe : union_ex(split.body)) {
        const auto [lo, hi] = stripe_extent(to_polygons(stripe), angle);
        if (hi - lo < 0.99 * double(beam_n))
            continue; // clipped by the outline
        const Polylines polylines = support_interlock_fill_lines(ExPolygons{ stripe }, angle, line_w);
        // Count the long segments: the fill lines along the stripe. Links between them are short.
        size_t long_segments = 0;
        for (const Polyline &pl : polylines)
            for (size_t i = 1; i < pl.points.size(); ++ i) {
                const Vec2d d = (pl.points[i] - pl.points[i - 1]).cast<double>();
                if (d.norm() < scale_(3.))
                    continue;
                ++ long_segments;
                // Parallel to the stripe ...
                const double cross = std::abs(d.x() * std::sin(angle) - d.y() * std::cos(angle)) / d.norm();
                CHECK(cross < 1e-3);
                // ... half a line in from a stripe side, on the line grid.
                const double u = support_interlock_stripe_coordinate(pl.points[i], angle);
                const double k = (u - lo) / double(line_w) - 0.5;
                CHECK_THAT(k - std::round(k), WithinAbs(0., 0.01));
            }
        CHECK(long_segments == size_t(lines));
        // Every point stays inside the stripe.
        for (const Polyline &pl : polylines)
            for (const Point &pt : pl.points) {
                const double u = support_interlock_stripe_coordinate(pt, angle);
                CHECK(u > lo);
                CHECK(u < hi);
            }
        ++ checked;
    }
    CHECK(checked >= 2);
}
