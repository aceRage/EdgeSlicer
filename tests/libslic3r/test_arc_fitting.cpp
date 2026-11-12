#include <catch2/catch.hpp>

#include <cmath>
#include <vector>

#include "libslic3r/ArcFitter.hpp"
#include "libslic3r/Circle.hpp"
#include "libslic3r/Polyline.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// The tolerance the walls are fitted with: the default `resolution`, 0.012 mm.
const double wall_tolerance = scale_(0.012);

// A point at `degrees` round a circle of `radius_mm` about the origin. The origin keeps the arithmetic of the
// cases below exact where they need it to be.
Point at(double degrees, double radius_mm)
{
    const double a = degrees * M_PI / 180.;
    return Point(coord_t(std::llround(scale_(radius_mm) * std::cos(a))), coord_t(std::llround(scale_(radius_mm) * std::sin(a))));
}

// One point every degree from -`below` to +`above` degrees, counter-clockwise, on a 10 mm circle about the
// origin. The lower points are the upper ones mirrored, so the path is the same on both sides of the x axis to
// the unit and the point due east of the centre has y == 0.
Points ring_through_east(int below, int above)
{
    Points out;
    for (int deg = -below; deg <= above; ++deg) {
        Point p = at(std::abs(deg), 10.);
        if (deg < 0)
            p.y() = -p.y();
        out.push_back(p);
    }
    return out;
}

size_t count_arcs(const std::vector<PathFittingData> &result)
{
    size_t n = 0;
    for (const PathFittingData &data : result)
        if (data.path_type == EMovePathType::Arc_move_ccw || data.path_type == EMovePathType::Arc_move_cw)
            ++n;
    return n;
}

} // namespace

TEST_CASE("A counter-clockwise arc may have its middle point due east of its centre", "[ArcFitting]")
{
    SECTION("three points, the middle one at angle 0")
    {
        // 2 degrees either side of east. The circle through them has its centre on the x axis exactly, so the
        // middle point is at an angle of exactly 0 from it.
        Points pts = {at(2., 10.), at(0., 10.), at(2., 10.)};
        pts.front().y() = -pts.front().y();
        REQUIRE(pts[1].y() == 0);

        // Clockwise the three points were always an arc.
        const Points back(pts.rbegin(), pts.rend());
        ArcSegment   cw;
        REQUIRE(ArcSegment::try_create_arc(back, cw, Polyline(back).length(), DEFAULT_SCALED_MAX_RADIUS, wall_tolerance));
        CHECK(cw.center.y() == 0);
        CHECK(cw.direction == ArcDirection::Arc_Dir_CW);
        CHECK_THAT(cw.angle_radians, WithinRel(-M_PI / 45., 1e-3));

        // Counter-clockwise they are the same arc the other way round.
        ArcSegment ccw;
        REQUIRE(ArcSegment::try_create_arc(pts, ccw, Polyline(pts).length(), DEFAULT_SCALED_MAX_RADIUS, wall_tolerance));
        CHECK(ccw.center == cw.center);
        CHECK(ccw.direction == ArcDirection::Arc_Dir_CCW);
        CHECK_THAT(ccw.angle_radians, WithinRel(M_PI / 45., 1e-3));
        CHECK_THAT(ccw.length, WithinRel(cw.length, 1e-12));
    }

    SECTION("a round wall is one arc, not two")
    {
        // From 60 degrees below east to 90 above, a point every degree. When the fitter has taken in the first
        // 121 points the arc runs from -60 to +60 degrees and its middle point is the one due east.
        const Points pts = ring_through_east(60, 90);
        REQUIRE(pts.size() == 151);
        REQUIRE(pts[60].y() == 0);

        std::vector<PathFittingData> result;
        ArcFitter::do_arc_fitting(pts, result, wall_tolerance);
        REQUIRE(result.size() == 1);
        REQUIRE(count_arcs(result) == 1);
        CHECK(result.front().path_type == EMovePathType::Arc_move_ccw);
        CHECK(result.front().start_point_index == 0);
        CHECK(result.front().end_point_index == pts.size() - 1);
        CHECK_THAT(result.front().arc_data.angle_radians, WithinAbs(150. * M_PI / 180., 1e-3));

        // Clockwise it was one arc before as well.
        Points back(pts.rbegin(), pts.rend());
        ArcFitter::do_arc_fitting(back, result, wall_tolerance);
        REQUIRE(result.size() == 1);
        CHECK(result.front().path_type == EMovePathType::Arc_move_cw);
    }
}

TEST_CASE("The circle through a path's own middle point is tried", "[ArcFitting]")
{
    // Five points 2 degrees apart on a 10 mm circle, the second 8 um outside it and the fourth 8 um inside, and
    // 10 um allowed. The first guess goes through the point halfway between those two and is flatter than the
    // circle; the circle through the second point leaves the fourth out, and the other way about. The circle
    // through the middle point is the only one with every point within reach.
    const Points pts       = {at(0., 10.), at(2., 10.008), at(4., 10.), at(6., 9.992), at(8., 10.)};
    const double tolerance = scale_(0.010);

    auto through = [&pts](size_t index) {
        Circle c;
        REQUIRE(Circle::try_create_circle(pts[0], pts[index], pts[4], DEFAULT_SCALED_MAX_RADIUS, c));
        return c;
    };
    double sum = 0.;
    REQUIRE_FALSE(through(1).get_deviation_sum_squared(pts, tolerance, sum));
    REQUIRE_FALSE(through(3).get_deviation_sum_squared(pts, tolerance, sum));
    REQUIRE(through(2).get_deviation_sum_squared(pts, tolerance, sum));

    Circle found;
    REQUIRE(Circle::try_create_circle(pts, DEFAULT_SCALED_MAX_RADIUS, tolerance, found));
    CHECK(found.center == through(2).center);
    CHECK_THAT(unscale<double>(found.radius), WithinAbs(10., 0.01));

    // And so the five points are one arc.
    std::vector<PathFittingData> result;
    ArcFitter::do_arc_fitting(pts, result, tolerance);
    REQUIRE(result.size() == 1);
    CHECK(result.front().path_type == EMovePathType::Arc_move_ccw);
    CHECK(result.front().end_point_index == 4);
}

TEST_CASE("An arc found by going the other way round has that arc's angle", "[ArcFitting]")
{
    // Nine points on a 10 mm circle. The first six are within 40 nm of one another where the path begins, at
    // 30 degrees, the fifth of them 40 nm short of the start; the last three go on counter-clockwise 2 degrees
    // at a time, to 36 degrees. The middle point by count is the one short of the start, which says "clockwise,
    // the long way round" (354 degrees). The path is as long as the 6 degrees, so the arc is taken the other
    // way round. It takes points this close together to get here: ArcFitter itself never passes such a path.
    const double r    = scale_(10.);
    const double step = 40. / r * 180. / M_PI; // 40 units along the circle, in degrees
    const Points pts  = {at(30., 10.),       at(30. + step, 10.), at(30., 10.), at(30. + step, 10.), at(30. - step, 10.),
                         at(30. + step, 10.), at(32., 10.),       at(34., 10.), at(36., 10.)};
    const double six_degrees = 6. * M_PI / 180.;

    ArcSegment arc;
    REQUIRE(ArcSegment::try_create_arc(pts, arc, Polyline(pts).length(), DEFAULT_SCALED_MAX_RADIUS, wall_tolerance));
    CHECK(arc.direction == ArcDirection::Arc_Dir_CCW);
    CHECK_THAT(arc.length, WithinRel(r * six_degrees, 1e-3));
    // The angle is that of the 6 degrees, and the length is the angle times the radius as everywhere else.
    CHECK_THAT(arc.angle_radians, WithinRel(six_degrees, 1e-3));
    CHECK_THAT(arc.length, WithinRel(std::abs(arc.angle_radians) * arc.radius, 1e-9));
    // What reads the angle: a point is abreast of the arc at 33 degrees and not at 200.
    CHECK(arc.is_point_inside(at(33., 10.)));
    CHECK_FALSE(arc.is_point_inside(at(200., 10.)));
    ArcSegment clipped = arc;
    CHECK_FALSE(clipped.clip_end(at(200., 12.)));

    // The mirror image, which ends up clockwise.
    Points mirrored = pts;
    for (Point &p : mirrored)
        p.x() = -p.x();
    ArcSegment cw;
    REQUIRE(ArcSegment::try_create_arc(mirrored, cw, Polyline(mirrored).length(), DEFAULT_SCALED_MAX_RADIUS, wall_tolerance));
    CHECK(cw.direction == ArcDirection::Arc_Dir_CW);
    CHECK_THAT(cw.angle_radians, WithinRel(-six_degrees, 1e-3));
    Point far_side = at(200., 10.);
    far_side.x()   = -far_side.x();
    CHECK_FALSE(cw.is_point_inside(far_side));
}

TEST_CASE("Two points in the same place have no foot of a perpendicular between them", "[ArcFitting]")
{
    SECTION("the function itself")
    {
        const Point p(scaled<coord_t>(10.), scaled<coord_t>(5.));
        const Point c(scaled<coord_t>(3.), scaled<coord_t>(4.));
        Point       out(123, 456);
        CHECK_FALSE(Circle::get_closest_perpendicular_point(p, p, c, out));
        CHECK(out == Point(123, 456));

        // A real segment is as before: the foot of the perpendicular from (5, 5) on the x axis from 0 to 10.
        REQUIRE(Circle::get_closest_perpendicular_point(Point(0, 0), Point(scaled<coord_t>(10.), 0),
                                                        Point(scaled<coord_t>(5.), scaled<coord_t>(5.)), out));
        CHECK(out == Point(scaled<coord_t>(5.), 0));
        // And off its end there is none.
        CHECK_FALSE(Circle::get_closest_perpendicular_point(Point(0, 0), Point(scaled<coord_t>(10.), 0),
                                                            Point(scaled<coord_t>(15.), scaled<coord_t>(5.)), out));
    }

    SECTION("a path with a point given twice is still within reach of its circle")
    {
        // A quarter circle about (50, 50) mm, a point every 5 degrees, with the point at 45 degrees given twice.
        Points pts;
        for (int deg = 0; deg <= 90; deg += 5) {
            pts.push_back(at(deg, 10.) + Point(scaled<coord_t>(50.), scaled<coord_t>(50.)));
            if (deg == 45)
                pts.push_back(pts.back());
        }
        Circle circle;
        REQUIRE(Circle::try_create_circle(pts.front(), pts[10], pts.back(), DEFAULT_SCALED_MAX_RADIUS, circle));
        CHECK_FALSE(circle.is_over_deviation(pts, wall_tolerance));
        double sum = 0.;
        CHECK(circle.get_deviation_sum_squared(pts, wall_tolerance, sum));
    }
}
