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

TEST_CASE("Traced levels follow the silhouette", "[OutlineObjects]")
{
    // two halves of a 20 x 10 image: left and right
    ExPolygons left  = {rect(0., 0., 10., 10.)};
    ExPolygons right = {rect(10., 0., 20., 10.)};

    SilhouetteParams p = plain_params(80.);
    Silhouette       s = make_silhouette({left, right}, p);
    REQUIRE(s.shape.size() == 1);
    REQUIRE_THAT(s.factor, WithinRel(4., 1e-3));

    ExPolygons l = to_silhouette_frame(left, s);
    ExPolygons r = to_silhouette_frame(right, s);
    REQUIRE_THAT(area_mm2(l), WithinRel(1600., 1e-3));
    REQUIRE_THAT(area_mm2(union_ex(l, r)), WithinRel(area_mm2(s.shape), 1e-3));
    REQUIRE(unscale<double>(get_extents(l).max.x()) < 0.01);
    REQUIRE(unscale<double>(get_extents(r).min.x()) > -0.01);

    p.mirror     = true;
    Silhouette m = make_silhouette({left, right}, p);
    REQUIRE(m.mirrored);
    REQUIRE(unscale<double>(get_extents(to_silhouette_frame(left, m)).min.x()) > -0.01);
}

TEST_CASE("Box parameters are limited to printable values", "[OutlineObjects]")
{
    OutlineBoxParams p;
    p.height           = 0.5;
    p.wall             = 0.;
    p.chamfer          = 5.;
    p.lip_height       = 100.;
    p.lid_thickness    = 0.1;
    p.decoration_depth = 1.;
    OutlineBoxParams s = sanitize(p);
    REQUIRE(s.wall >= 0.4);
    REQUIRE(s.height >= s.floor + 2.);
    REQUIRE(s.chamfer <= s.floor);
    REQUIRE(s.lip_height <= s.height - s.floor);
    REQUIRE(s.lid_thickness >= s.decoration_depth + 0.4);
}

TEST_CASE("Box of a square", "[OutlineObjects]")
{
    Silhouette s = make_silhouette({{rect(0., 0., 10., 10.)}}, plain_params(40.));
    REQUIRE(s.shape.size() == 1);

    OutlineBoxParams p;
    p.chamfer = 0.;

    SECTION("sections")
    {
        OutlineBoxSections sec = outline_box_sections(s.shape, {}, p);
        REQUIRE_THAT(area_mm2(sec.cavity), WithinRel(36.8 * 36.8, 1e-3));
        REQUIRE_THAT(area_mm2(sec.wall), WithinRel(1600. - 36.8 * 36.8, 1e-3));
        REQUIRE_THAT(area_mm2(sec.lid), WithinRel(1600. + 4. * 40. * 1.5 + PI * 1.5 * 1.5, 2e-3));
        REQUIRE_THAT(area_mm2(sec.lip), WithinRel(36.4 * 36.4 - 33.2 * 33.2, 1e-3));
        // the lip goes into the cavity with the clearance on every side
        REQUIRE(area_mm2(intersection_ex(sec.lip, sec.wall)) < 1e-6);
        REQUIRE(area_mm2(diff_ex(sec.lip, sec.cavity)) < 1e-6);
        REQUIRE(area_mm2(intersection_ex(sec.lip, diff_ex(sec.cavity, offset_ex(sec.cavity, -float(scale_(0.19)))))) < 1e-3);
        REQUIRE(sec.decoration.empty());

        p.lid_overhang = 0.;
        REQUIRE_THAT(area_mm2(outline_box_sections(s.shape, {}, p).lid), WithinRel(1600., 1e-6));
    }

    SECTION("meshes")
    {
        OutlineBoxResult res = make_outline_box(s.shape, {}, p);
        REQUIRE(res.is_valid());
        REQUIRE(res.warnings.empty());
        REQUIRE(res.base.parts.size() == 1);
        REQUIRE(res.lid.parts.size() == 1);

        const indexed_triangle_set &base = res.base.parts.front().mesh;
        REQUIRE(its_num_open_edges(base) == 0);
        REQUIRE_THAT(double(its_volume(base)), WithinRel(1600. * 1.2 + (1600. - 36.8 * 36.8) * (25. - 1.2), 1e-3));
        REQUIRE_THAT(bounding_box(base).min.z(), WithinAbs(0., 1e-6));
        REQUIRE_THAT(bounding_box(base).max.z(), WithinAbs(25., 1e-4));

        const indexed_triangle_set &lid = res.lid.parts.front().mesh;
        REQUIRE(its_num_open_edges(lid) == 0);
        double plate = 1600. + 4. * 40. * 1.5 + PI * 1.5 * 1.5;
        REQUIRE_THAT(double(its_volume(lid)), WithinRel(plate * 2. + (36.4 * 36.4 - 33.2 * 33.2) * 4., 2e-3));
        REQUIRE_THAT(bounding_box(lid).min.z(), WithinAbs(0., 1e-6));
        REQUIRE_THAT(bounding_box(lid).max.z(), WithinAbs(6., 1e-4));
        REQUIRE_THAT(bounding_box(lid).size().x(), WithinAbs(43., 0.01));
    }

    SECTION("chamfered bottom edge")
    {
        p.chamfer = 0.6;
        OutlineBoxResult res = make_outline_box(s.shape, {}, p);
        REQUIRE(res.is_valid());
        const indexed_triangle_set &base = res.base.parts.front().mesh;
        REQUIRE(its_num_open_edges(base) == 0);
        double plain = 1600. * 1.2 + (1600. - 36.8 * 36.8) * (25. - 1.2);
        REQUIRE(double(its_volume(base)) < plain);
        REQUIRE(double(its_volume(base)) > plain - 4. * 40. * 0.6 * 0.6);
        // the footprint is smaller than the box
        REQUIRE(bounding_box(base).size().x() < 40.01);
    }
}

TEST_CASE("Box lid with a picture", "[OutlineObjects]")
{
    // The right half of the image is one colour, a small square in the left half is another
    ExPolygons level_a = {rect(10., 0., 20., 20.)};
    ExPolygons level_b = {rect(2., 2., 6., 6.)};
    ExPolygons rest    = {rect(0., 0., 20., 20.)};

    Silhouette s = make_silhouette({rest}, plain_params(40.));
    std::vector<ExPolygons> detail = {to_silhouette_frame(level_a, s), to_silhouette_frame(level_b, s)};

    OutlineBoxParams   p;
    OutlineBoxSections sec = outline_box_sections(s.shape, detail, p);
    REQUIRE(sec.decoration.size() == 2);
    // the picture keeps the margin from the outline
    REQUIRE_THAT(area_mm2(sec.decoration[0]), WithinRel((20. - 0.8) * (40. - 1.6), 1e-3));
    REQUIRE_THAT(area_mm2(sec.decoration[1]), WithinRel(64., 1e-3));
    REQUIRE(area_mm2(intersection_ex(sec.decoration[0], sec.decoration[1])) < 1e-6);

    OutlineBoxResult res = make_outline_box(s.shape, detail, p);
    REQUIRE(res.is_valid());
    REQUIRE(res.lid.parts.size() == 3);
    REQUIRE(res.lid.parts[0].level == -1);
    REQUIRE(res.lid.parts[1].level == 0);
    REQUIRE(res.lid.parts[2].level == 1);

    double volume = 0.;
    for (const OutlinePart &part : res.lid.parts) {
        REQUIRE(its_num_open_edges(part.mesh) == 0);
        REQUIRE(its_volume(part.mesh) > 0.f);
        volume += double(its_volume(part.mesh));
    }
    // the picture is inlaid: body and colours together are the plain lid
    OutlineBoxParams plain = p;
    plain.decoration       = false;
    OutlineBoxResult plain_res = make_outline_box(s.shape, detail, plain);
    REQUIRE(plain_res.lid.parts.size() == 1);
    REQUIRE_THAT(volume, WithinRel(double(its_volume(plain_res.lid.parts.front().mesh)), 1e-3));

    // 0.6 mm deep, in the first layers
    REQUIRE_THAT(bounding_box(res.lid.parts[1].mesh).min.z(), WithinAbs(0., 1e-6));
    REQUIRE_THAT(bounding_box(res.lid.parts[1].mesh).max.z(), WithinAbs(0.6, 1e-5));
    // the lid is printed top face down: the right half of the image is on the left of the bed
    REQUIRE(bounding_box(res.lid.parts[1].mesh).max.x() < 0.01);
    REQUIRE(bounding_box(res.lid.parts[2].mesh).min.x() > -0.01);

    SECTION("levels which overlap are separated")
    {
        std::vector<ExPolygons> both = {detail[0], detail[0]};
        OutlineBoxSections      o    = outline_box_sections(s.shape, both, p);
        REQUIRE(o.decoration.size() == 2);
        REQUIRE(o.decoration[1].empty());
        REQUIRE(make_outline_box(s.shape, both, p).lid.parts.size() == 2);
    }
}

TEST_CASE("Box of difficult shapes", "[OutlineObjects]")
{
    SECTION("no shape")
    {
        OutlineBoxResult res = make_outline_box({}, {}, OutlineBoxParams());
        REQUIRE_FALSE(res.is_valid());
        REQUIRE_FALSE(res.error.empty());
    }

    SECTION("too thin for a cavity")
    {
        Silhouette       s   = make_silhouette({{rect(0., 0., 40., 3.)}}, plain_params(40.));
        OutlineBoxResult res = make_outline_box(s.shape, {}, OutlineBoxParams());
        REQUIRE(res.is_valid());
        REQUIRE(res.warnings.size() >= 2); // solid base, no lip
        REQUIRE(its_num_open_edges(res.base.parts.front().mesh) == 0);
        REQUIRE(its_num_open_edges(res.lid.parts.front().mesh) == 0);
    }

    SECTION("star")
    {
        ExPolygon star;
        for (int i = 0; i < 10; ++i) {
            double a = PI * double(i) / 5.;
            double r = i % 2 == 0 ? 30. : 12.;
            star.contour.points.push_back(Point::new_scale(r * std::cos(a), r * std::sin(a)));
        }
        Silhouette       s   = make_silhouette({{star}}, SilhouetteParams());
        OutlineBoxResult res = make_outline_box(s.shape, {}, OutlineBoxParams());
        REQUIRE(res.is_valid());
        REQUIRE(its_num_open_edges(res.base.parts.front().mesh) == 0);
        REQUIRE(its_num_open_edges(res.lid.parts.front().mesh) == 0);
        REQUIRE(its_volume(res.base.parts.front().mesh) > 0.f);
    }
}

TEST_CASE("Largest inscribed circle", "[OutlineObjects]")
{
    double radius = 0.;
    Vec2d  c      = largest_inscribed_circle({circle(5., -3., 20.)}, &radius);
    REQUIRE_THAT(c.x(), WithinAbs(5., 0.3));
    REQUIRE_THAT(c.y(), WithinAbs(-3., 0.3));
    REQUIRE_THAT(radius, WithinAbs(20., 0.2));

    // an "L": a 40 x 40 square with a 10 mm wide arm to the right
    ExPolygons l = union_ex(ExPolygons{rect(0., 0., 40., 40.), rect(40., 0., 90., 10.)});
    c            = largest_inscribed_circle(l, &radius);
    REQUIRE_THAT(radius, WithinAbs(20., 0.5));
    REQUIRE_THAT(c.x(), WithinAbs(20., 1.5));
    REQUIRE_THAT(c.y(), WithinAbs(20., 1.5));

    REQUIRE_THAT(largest_inscribed_circle({}, &radius).x(), WithinAbs(0., 1e-9));
    REQUIRE_THAT(radius, WithinAbs(0., 1e-9));
}

TEST_CASE("Clicker parameters are limited to printable values", "[OutlineObjects]")
{
    ClickerParams p;
    p.body_height   = 2.;
    p.chamfer       = 9.;
    p.cap_thickness = 0.1;
    ClickerParams s = sanitize(p);
    REQUIRE(s.body_height >= s.floor + MX_DEPTH + MX_PLATE);
    REQUIRE(s.chamfer <= s.floor);
    REQUIRE(s.cap_thickness >= s.decoration_depth + 0.4);
}

TEST_CASE("Clicker of a square", "[OutlineObjects]")
{
    Silhouette s = make_silhouette({{rect(0., 0., 10., 10.)}}, plain_params(40.));
    ClickerParams p;
    p.chamfer = 0.;

    SECTION("sections")
    {
        ClickerSections sec = clicker_sections(s.shape, {}, p);
        REQUIRE(sec.fits);
        REQUIRE_THAT(sec.switch_position.x(), WithinAbs(0., 0.5));
        REQUIRE_THAT(sec.switch_position.y(), WithinAbs(0., 0.5));
        REQUIRE_THAT(sec.inscribed_radius, WithinAbs(20., 0.3));
        REQUIRE_THAT(area_mm2(sec.pocket), WithinRel(MX_POCKET * MX_POCKET, 1e-6));
        // the opening is 14 mm plus the fit on every side
        REQUIRE_THAT(area_mm2(sec.opening), WithinRel(14.2 * 14.2, 1e-6));
        REQUIRE_THAT(area_mm2(sec.cap), WithinRel(1600. + 4. * 40. * 1.5 + PI * 1.5 * 1.5, 2e-3));
        // post: a cylinder without the cross of the stem
        double cross = 2. * 4.2 * 1.4 - 1.4 * 1.4;
        REQUIRE_THAT(area_mm2(sec.post), WithinRel(PI * 2.8 * 2.8 - cross, 0.01));

        p.switch_offset = Vec2d(5., 0.);
        REQUIRE_THAT(clicker_sections(s.shape, {}, p).switch_position.x() - sec.switch_position.x(), WithinAbs(5., 1e-6));
        REQUIRE(clicker_sections(s.shape, {}, p).fits);
        // too close to the edge
        p.switch_offset = Vec2d(12., 0.);
        REQUIRE_FALSE(clicker_sections(s.shape, {}, p).fits);
        REQUIRE_FALSE(make_clicker(s.shape, {}, p).is_valid());
    }

    SECTION("meshes")
    {
        ClickerResult res = make_clicker(s.shape, {}, p);
        REQUIRE(res.is_valid());
        REQUIRE(res.warnings.empty());
        REQUIRE(res.body.parts.size() == 1);
        REQUIRE(res.cap.parts.size() == 1);

        const indexed_triangle_set &body = res.body.parts.front().mesh;
        REQUIRE(its_num_open_edges(body) == 0);
        // solid but for the room of the switch
        double expected = 1600. * 11. - MX_POCKET * MX_POCKET * (11. - 1.2 - MX_PLATE) - 14.2 * 14.2 * MX_PLATE;
        REQUIRE_THAT(double(its_volume(body)), WithinRel(expected, 1e-3));
        REQUIRE_THAT(bounding_box(body).max.z(), WithinAbs(11., 1e-4));

        const indexed_triangle_set &cap = res.cap.parts.front().mesh;
        REQUIRE(its_num_open_edges(cap) == 0);
        double plate = 1600. + 4. * 40. * 1.5 + PI * 1.5 * 1.5;
        double cross = 2. * 4.2 * 1.4 - 1.4 * 1.4;
        REQUIRE_THAT(double(its_volume(cap)), WithinRel(plate * 2. + (PI * 2.8 * 2.8 - cross) * MX_POST_HEIGHT, 3e-3));
        REQUIRE_THAT(bounding_box(cap).max.z(), WithinAbs(2. + MX_POST_HEIGHT, 1e-4));
    }

    SECTION("too small for a switch")
    {
        Silhouette    small = make_silhouette({{rect(0., 0., 10., 10.)}}, plain_params(16.));
        ClickerResult res   = make_clicker(small.shape, {}, p);
        REQUIRE_FALSE(res.is_valid());
        REQUIRE_FALSE(res.error.empty());
        ClickerSections sec = clicker_sections(small.shape, {}, p);
        REQUIRE(sec.needed_radius > sec.inscribed_radius);
    }

    SECTION("chamfer and key ring")
    {
        p.chamfer       = 0.6;
        p.keyring       = true;
        p.keyring_angle = 0.;
        ClickerSections sec = clicker_sections(s.shape, {}, p);
        // the tab sticks out to the right and has a hole
        REQUIRE(unscale<double>(get_extents(sec.body).max.x()) > 24.);
        REQUIRE_THAT(unscale<double>(get_extents(sec.body).min.x()), WithinAbs(-20., 0.01));
        size_t holes = 0;
        for (const ExPolygon &e : sec.body)
            holes += e.holes.size();
        REQUIRE(holes == 1);

        ClickerResult res = make_clicker(s.shape, {}, p);
        REQUIRE(res.is_valid());
        const indexed_triangle_set &body = res.body.parts.front().mesh;
        REQUIRE(its_num_open_edges(body) == 0);
        REQUIRE(bounding_box(body).max.x() > 24.);
        REQUIRE_THAT(bounding_box(body).max.z(), WithinAbs(11., 1e-4));
    }
}

TEST_CASE("Clicker cap with a picture", "[OutlineObjects]")
{
    ExPolygons level_a = {rect(10., 0., 20., 20.)};
    Silhouette s       = make_silhouette({{rect(0., 0., 20., 20.)}}, plain_params(40.));
    std::vector<ExPolygons> detail = {to_silhouette_frame(level_a, s)};

    ClickerResult res = make_clicker(s.shape, detail, ClickerParams());
    REQUIRE(res.is_valid());
    REQUIRE(res.cap.parts.size() == 2);
    REQUIRE(res.cap.parts[1].level == 0);
    for (const OutlinePart &part : res.cap.parts)
        REQUIRE(its_num_open_edges(part.mesh) == 0);
    // printed top face down: the right half of the image is on the left of the bed
    REQUIRE(bounding_box(res.cap.parts[1].mesh).max.x() < 0.01);
    REQUIRE_THAT(bounding_box(res.cap.parts[1].mesh).max.z(), WithinAbs(0.6, 1e-5));
    // the body has no picture
    REQUIRE(res.body.parts.size() == 1);
}

TEST_CASE("Clicker fit test", "[OutlineObjects]")
{
    std::vector<OutlineObject> objects = make_clicker_fit_test(ClickerParams());
    REQUIRE(objects.size() == 2);
    for (const OutlineObject &object : objects) {
        REQUIRE(object.parts.size() == 1);
        REQUIRE(its_num_open_edges(object.parts.front().mesh) == 0);
        REQUIRE(its_volume(object.parts.front().mesh) > 0.f);
        REQUIRE_THAT(bounding_box(object.parts.front().mesh).min.z(), WithinAbs(0., 1e-6));
    }
    // three sockets side by side
    REQUIRE_THAT(bounding_box(objects[0].parts.front().mesh).size().x(), WithinAbs(66., 0.01));
    // three separate stubs
    REQUIRE(its_split(objects[1].parts.front().mesh).size() == 3);
}
