#include <catch2/catch.hpp>

#include <libslic3r/OutlineObjects.hpp>
#include <libslic3r/BoundingBox.hpp>
#include <libslic3r/ClipperUtils.hpp>
#include <libslic3r/TriangleMesh.hpp>

#include <cmath>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

ExPolygon rect(double x0, double y0, double x1, double y1)
{
    ExPolygon e;
    e.contour.points = {Point::new_scale(x0, y0), Point::new_scale(x1, y0), Point::new_scale(x1, y1), Point::new_scale(x0, y1)};
    return e;
}

ExPolygon circle(double cx, double cy, double r, int segments = 180)
{
    ExPolygon e;
    for (int i = 0; i < segments; ++i) {
        double a = 2. * PI * double(i) / double(segments);
        e.contour.points.push_back(Point::new_scale(cx + r * std::cos(a), cy + r * std::sin(a)));
    }
    return e;
}

double area_mm2(const ExPolygons &shape)
{
    double a = 0.;
    for (const ExPolygon &e : shape)
        a += e.area();
    return a * SCALING_FACTOR * SCALING_FACTOR;
}

Vec2d size_mm(const ExPolygons &shape)
{
    BoundingBox bb = get_extents(shape);
    return Vec2d(unscale<double>(bb.size().x()), unscale<double>(bb.size().y()));
}

SilhouetteParams plain_params(double side)
{
    SilhouetteParams p;
    p.longest_side = side;
    p.close_gaps   = 0.;
    p.min_feature  = 0.;
    return p;
}

} // namespace

TEST_CASE("Outline prism is a closed solid of the right volume", "[OutlineObjects]")
{
    ExPolygon with_hole = rect(0., 0., 20., 10.);
    Polygon   hole      = rect(5., 2., 10., 8.).contour;
    hole.reverse();
    with_hole.holes.push_back(hole);

    indexed_triangle_set its = outline_prism({with_hole}, 1., 4.);
    REQUIRE_FALSE(its.indices.empty());
    REQUIRE(its_num_open_edges(its) == 0);
    REQUIRE_THAT(double(its_volume(its)), WithinRel((200. - 30.) * 3., 1e-4));

    BoundingBoxf3 bb = bounding_box(its);
    REQUIRE_THAT(bb.min.z(), WithinAbs(1., 1e-6));
    REQUIRE_THAT(bb.max.z(), WithinAbs(4., 1e-6));
    REQUIRE_THAT(bb.size().x(), WithinAbs(20., 1e-4));

    REQUIRE(outline_prism({}, 0., 1.).indices.empty());
    REQUIRE(outline_prism({with_hole}, 2., 2.).indices.empty());
}

TEST_CASE("Stacked bands are joined into one closed mesh", "[OutlineObjects]")
{
    std::vector<OutlineBand> bands;
    bands.push_back({{rect(0., 0., 20., 20.)}, 0., 2.});
    bands.push_back({{rect(5., 5., 15., 15.)}, 2., 5.});
    bool                 joined = false;
    indexed_triangle_set its    = join_outline_bands(bands, &joined);
    REQUIRE(joined);
    REQUIRE(its_num_open_edges(its) == 0);
    REQUIRE_THAT(double(its_volume(its)), WithinRel(400. * 2. + 100. * 3., 1e-4));
}

TEST_CASE("Silhouette is scaled, centered and has Y up", "[OutlineObjects]")
{
    // An "L": the foot points to +X at the bottom of the image (large Y, as rows go down)
    ExPolygons level = union_ex(ExPolygons{rect(0., 0., 10., 40.), rect(0., 30., 20., 40.)});

    SECTION("size and center")
    {
        Silhouette s = make_silhouette({level}, plain_params(80.));
        REQUIRE(s.shape.size() == 1);
        REQUIRE(s.warnings.empty());
        REQUIRE_THAT(s.height, WithinAbs(80., 0.05));
        REQUIRE_THAT(s.width, WithinAbs(40., 0.05));
        BoundingBox bb = get_extents(s.shape);
        REQUIRE(std::abs(unscale<double>(bb.center().x())) < 0.01);
        REQUIRE(std::abs(unscale<double>(bb.center().y())) < 0.01);
        REQUIRE_THAT(area_mm2(s.shape), WithinRel((400. + 100.) * 4., 1e-3));
    }

    SECTION("the foot of the L is at the bottom right, mirrored it is at the bottom left")
    {
        Silhouette s = make_silhouette({level}, plain_params(80.));
        // the foot fills the bottom right, above it is nothing
        REQUIRE(s.shape.front().contains(Point::new_scale(15., -35.)));
        REQUIRE_FALSE(s.shape.front().contains(Point::new_scale(15., 35.)));

        SilhouetteParams p = plain_params(80.);
        p.mirror           = true;
        Silhouette m       = make_silhouette({level}, p);
        REQUIRE(m.shape.size() == 1);
        // the stem moved from the left to the right
        REQUIRE(m.shape.front().contains(Point::new_scale(15., 35.)));
        REQUIRE_FALSE(m.shape.front().contains(Point::new_scale(-15., 35.)));
        REQUIRE(s.shape.front().contains(Point::new_scale(-15., 35.)));
        REQUIRE(m.shape.front().contour.is_counter_clockwise());
        REQUIRE_THAT(area_mm2(m.shape), WithinRel(area_mm2(s.shape), 1e-6));
    }
}

TEST_CASE("Silhouette clean-up", "[OutlineObjects]")
{
    SECTION("holes are filled on request")
    {
        ExPolygon ring = circle(0., 0., 20.);
        Polygon   hole = circle(0., 0., 10.).contour;
        hole.reverse();
        ring.holes.push_back(hole);

        SilhouetteParams p = plain_params(40.);
        Silhouette       filled = make_silhouette({{ring}}, p);
        REQUIRE(filled.shape.size() == 1);
        REQUIRE(filled.shape.front().holes.empty());

        p.fill_holes   = false;
        Silhouette kept = make_silhouette({{ring}}, p);
        REQUIRE(kept.shape.size() == 1);
        REQUIRE(kept.shape.front().holes.size() == 1);
    }

    SECTION("thin parts are removed and reported")
    {
        // 30 x 30 square with a 0.6 mm wide and 20 mm long arm
        ExPolygons level = union_ex(ExPolygons{rect(0., 0., 30., 30.), rect(30., 14.7, 50., 15.3)});
        SilhouetteParams p = plain_params(30.);
        p.min_feature      = 1.2;
        Silhouette s       = make_silhouette({level}, p);
        REQUIRE(s.shape.size() == 1);
        REQUIRE_THAT(s.width, WithinAbs(30., 0.05));
        REQUIRE_THAT(s.height, WithinAbs(30., 0.05));
        REQUIRE(area_mm2(s.removed) > 5.);
        REQUIRE_FALSE(s.warnings.empty());
        // corners stay sharp
        REQUIRE_THAT(area_mm2(s.shape), WithinRel(900., 2e-3));
    }

    SECTION("near parts are joined, far parts are separate islands")
    {
        ExPolygons two = {rect(0., 0., 10., 10.), rect(10.2, 0., 20., 10.)};
        SilhouetteParams p = plain_params(20.);
        p.close_gaps       = 0.5;
        Silhouette joined  = make_silhouette({two}, p);
        REQUIRE(joined.islands == 1);

        ExPolygons far = {rect(0., 0., 10., 10.), rect(15., 0., 20., 5.)};
        Silhouette largest = make_silhouette({far}, plain_params(40.));
        REQUIRE(largest.islands == 2);
        REQUIRE(largest.shape.size() == 1);
        REQUIRE_FALSE(largest.warnings.empty());
        // only the large square is left and it has the wanted size
        REQUIRE_THAT(largest.width, WithinAbs(40., 0.1));
        REQUIRE_THAT(largest.height, WithinAbs(40., 0.1));

        SilhouetteParams all = plain_params(40.);
        all.largest_only     = false;
        Silhouette both      = make_silhouette({far}, all);
        REQUIRE(both.shape.size() == 2);
        REQUIRE_THAT(both.width, WithinAbs(40., 0.1));
    }

    SECTION("empty input does not crash")
    {
        Silhouette none = make_silhouette({}, SilhouetteParams());
        REQUIRE(none.empty());
        REQUIRE_FALSE(none.warnings.empty());

        // everything is thinner than the minimum
        SilhouetteParams p = plain_params(50.);
        p.min_feature      = 5.;
        Silhouette thin    = make_silhouette({{rect(0., 0., 50., 0.5)}}, p);
        REQUIRE(thin.empty());
        REQUIRE_FALSE(thin.warnings.empty());
    }
}

TEST_CASE("Cookie cutter parameters are limited to printable values", "[OutlineObjects]")
{
    CookieCutterParams p;
    p.blade_thickness = 0.05;
    p.handle_width    = 0.;
    p.handle_height   = 30.;
    p.blade_height    = 2.;
    p.tip_width       = 9.;
    p.tip_height      = 50.;
    CookieCutterParams s = sanitize(p);
    REQUIRE(s.blade_thickness >= 0.3);
    REQUIRE(s.handle_width >= s.blade_thickness);
    REQUIRE(s.blade_height >= s.handle_height + 1.);
    REQUIRE(s.tip_width <= s.blade_thickness);
    REQUIRE(s.tip_height <= s.blade_height - s.handle_height);
}

TEST_CASE("Cookie cutter of a circle", "[OutlineObjects]")
{
    const double     r = 30.;
    Silhouette       s = make_silhouette({{circle(0., 0., 10., 360)}}, plain_params(2. * r));
    REQUIRE(s.shape.size() == 1);

    CookieCutterParams p;
    p.braces = CutterBraces::Never;

    SECTION("sections")
    {
        CookieCutterSections sec = cookie_cutter_sections(s.shape, p);
        REQUIRE_FALSE(sec.braces);
        REQUIRE_THAT(area_mm2(sec.blade), WithinRel(PI * (std::pow(r + 0.8, 2) - r * r), 1e-2));
        REQUIRE_THAT(area_mm2(sec.handle), WithinRel(PI * (std::pow(r + 4., 2) - r * r), 1e-2));
        REQUIRE_THAT(area_mm2(sec.tip), WithinRel(PI * (std::pow(r + 0.4, 2) - r * r), 1e-2));
        // the opening of the cutter is the silhouette: nothing of the blade is inside of it
        REQUIRE(area_mm2(intersection_ex(sec.blade, s.shape)) < 0.01);
        REQUIRE(area_mm2(intersection_ex(sec.handle, s.shape)) < 0.01);
        Vec2d size = size_mm(sec.handle);
        REQUIRE_THAT(size.x(), WithinAbs(2. * (r + 4.), 0.05));
    }

    SECTION("straight blade")
    {
        p.sharp_edge = false;
        OutlineObjectResult res = make_cookie_cutter(s.shape, p);
        REQUIRE(res.is_valid());
        REQUIRE(res.joined);
        REQUIRE(res.warnings.empty());
        REQUIRE(its_num_open_edges(res.mesh) == 0);
        double expected = PI * (std::pow(r + 4., 2) - r * r) * 3.5 + PI * (std::pow(r + 0.8, 2) - r * r) * (12.5 - 3.5);
        REQUIRE_THAT(double(its_volume(res.mesh)), WithinRel(expected, 0.01));
        BoundingBoxf3 bb = bounding_box(res.mesh);
        REQUIRE_THAT(bb.min.z(), WithinAbs(0., 1e-6));
        REQUIRE_THAT(bb.max.z(), WithinAbs(12.5, 1e-4));
        REQUIRE_THAT(bb.size().x(), WithinAbs(2. * (r + 4.), 0.05));
    }

    SECTION("sharp edge")
    {
        OutlineObjectResult res = make_cookie_cutter(s.shape, p);
        REQUIRE(res.is_valid());
        REQUIRE(res.joined);
        REQUIRE(its_num_open_edges(res.mesh) == 0);
        // the taper is a stair of 10 steps, its mean thickness is (0.8 + 0.4) / 2
        double ring = [r](double t) { return PI * (std::pow(r + t, 2) - r * r); }(0.6);
        double expected = PI * (std::pow(r + 4., 2) - r * r) * 3.5 + PI * (std::pow(r + 0.8, 2) - r * r) * (12.5 - 3.5 - 2.) + ring * 2.;
        REQUIRE_THAT(double(its_volume(res.mesh)), WithinRel(expected, 0.01));
        REQUIRE_THAT(bounding_box(res.mesh).max.z(), WithinAbs(12.5, 1e-4));
    }
}

TEST_CASE("Cookie cutter braces", "[OutlineObjects]")
{
    CookieCutterParams p;

    Silhouette small = make_silhouette({{rect(0., 0., 10., 10.)}}, plain_params(60.));
    Silhouette large = make_silhouette({{rect(0., 0., 10., 10.)}}, plain_params(120.));

    REQUIRE_FALSE(cookie_cutter_sections(small.shape, p).braces);
    CookieCutterSections with = cookie_cutter_sections(large.shape, p);
    REQUIRE(with.braces);
    // the braces cross the opening
    REQUIRE(area_mm2(intersection_ex(with.handle, large.shape)) > 100.);
    // one part: the bars are joined with the flange
    REQUIRE(with.handle.size() == 1);

    p.braces = CutterBraces::Never;
    REQUIRE_FALSE(cookie_cutter_sections(large.shape, p).braces);
    p.braces = CutterBraces::Always;
    REQUIRE(cookie_cutter_sections(small.shape, p).braces);

    OutlineObjectResult res = make_cookie_cutter(large.shape, CookieCutterParams());
    REQUIRE(res.is_valid());
    REQUIRE(res.joined);
    REQUIRE(its_num_open_edges(res.mesh) == 0);
}

TEST_CASE("Cookie cutter of difficult shapes", "[OutlineObjects]")
{
    SECTION("no shape")
    {
        OutlineObjectResult res = make_cookie_cutter({}, CookieCutterParams());
        REQUIRE_FALSE(res.is_valid());
        REQUIRE_FALSE(res.error.empty());
    }

    SECTION("two islands and a hole")
    {
        ExPolygon ring = circle(0., 0., 20.);
        Polygon   hole = circle(0., 0., 10.).contour;
        hole.reverse();
        ring.holes.push_back(hole);
        ExPolygons level = {ring, rect(30., -10., 50., 10.)};

        SilhouetteParams sp = plain_params(70.);
        sp.fill_holes       = false;
        sp.largest_only     = false;
        Silhouette s        = make_silhouette({level}, sp);
        REQUIRE(s.shape.size() == 2);

        OutlineObjectResult res = make_cookie_cutter(s.shape, CookieCutterParams());
        REQUIRE(res.is_valid());
        REQUIRE(res.joined);
        REQUIRE(its_num_open_edges(res.mesh) == 0);
        REQUIRE(its_volume(res.mesh) > 0.f);
    }

    SECTION("star with sharp points")
    {
        ExPolygon star;
        for (int i = 0; i < 10; ++i) {
            double a = PI * double(i) / 5.;
            double r = i % 2 == 0 ? 30. : 12.;
            star.contour.points.push_back(Point::new_scale(r * std::cos(a), r * std::sin(a)));
        }
        Silhouette          s   = make_silhouette({{star}}, SilhouetteParams());
        REQUIRE(s.shape.size() == 1);
        OutlineObjectResult res = make_cookie_cutter(s.shape, CookieCutterParams());
        REQUIRE(res.is_valid());
        REQUIRE(res.joined);
        REQUIRE(its_num_open_edges(res.mesh) == 0);
    }
}
