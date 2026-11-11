#include <catch2/catch.hpp>

#include <cmath>
#include <cstdlib>
#include <limits>
#include <vector>

#include "libslic3r/libslic3r.h"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Tesselate.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace Slic3r {
// Defined in PrintObject.cpp.
Polygons create_polyholes(const Point center, const coord_t radius, const coord_t nozzle_diameter, bool multiple, int max_edges);
bool     polyhole_is_convex(const Polygon &hole);
} // namespace Slic3r

namespace {

// A ring of `sides` points about (cx, cy) mm, clockwise as a hole is held. `reach` gives each point's distance
// from the middle in mm.
template<typename Reach> Polygon hole_of(double cx, double cy, int sides, Reach reach)
{
    Polygon out;
    for (int i = 0; i < sides; ++i) {
        const double a = 2. * M_PI * i / sides;
        const double r = reach(i);
        out.points.emplace_back(scaled<coord_t>(cx + r * std::cos(a)), scaled<coord_t>(cy + r * std::sin(a)));
    }
    out.make_clockwise();
    return out;
}

Polygon round_hole(double cx, double cy, double radius, int sides = 64)
{
    return hole_of(cx, cy, sides, [radius](int) { return radius; });
}

// A plate 30 mm square about the origin with these holes through it.
ExPolygon plate_with(Polygons holes)
{
    ExPolygon out;
    const coord_t h = scaled<coord_t>(15.);
    out.contour.points = {{-h, -h}, {h, -h}, {h, h}, {-h, h}};
    out.holes = std::move(holes);
    return out;
}

// The shape as a solid from z0 to z1.
TriangleMesh extruded(const ExPolygon &shape, double z0, double z1)
{
    indexed_triangle_set its;
    auto add = [&its](const Vec3d &a, const Vec3d &b, const Vec3d &c) {
        const int n = int(its.vertices.size());
        its.vertices.emplace_back(a.cast<float>());
        its.vertices.emplace_back(b.cast<float>());
        its.vertices.emplace_back(c.cast<float>());
        its.indices.emplace_back(n, n + 1, n + 2);
    };
    const std::vector<Vec3d> top    = triangulate_expolygon_3d(shape, z1, NORMALS_UP);
    const std::vector<Vec3d> bottom = triangulate_expolygon_3d(shape, z0, NORMALS_DOWN);
    for (size_t i = 0; i + 2 < top.size(); i += 3)
        add(top[i], top[i + 1], top[i + 2]);
    for (size_t i = 0; i + 2 < bottom.size(); i += 3)
        add(bottom[i], bottom[i + 1], bottom[i + 2]);
    auto wall = [&add, z0, z1](const Polygon &ring) {
        for (size_t i = 0; i < ring.points.size(); ++i) {
            const Vec2d a = unscaled<double>(ring.points[i]);
            const Vec2d b = unscaled<double>(ring.points[(i + 1) % ring.points.size()]);
            add(Vec3d(a.x(), a.y(), z0), Vec3d(b.x(), b.y(), z0), Vec3d(b.x(), b.y(), z1));
            add(Vec3d(a.x(), a.y(), z0), Vec3d(b.x(), b.y(), z1), Vec3d(a.x(), a.y(), z1));
        }
    };
    wall(shape.contour);
    for (const Polygon &hole : shape.holes)
        wall(hole);
    its_merge_vertices(its);
    return TriangleMesh(its);
}

// How many points the hole nearest (x, y) mm from the middle of the plate has on each layer. Read from the layer's
// outline (lslices), where a polyhole is put as it is in the regions: the regions' own slices are cut up by
// surface type once the print has been processed.
std::vector<size_t> hole_points_by_layer(const Print &print, double x, double y)
{
    std::vector<size_t> out;
    for (const Layer *layer : print.objects().front()->layers()) {
        size_t best_points = 0;
        double best_dist   = std::numeric_limits<double>::max();
        for (const ExPolygon &island : layer->lslices) {
            const Point middle = get_extents(island.contour).center();
            for (const Polygon &hole : island.holes) {
                const Vec2d  at   = unscaled<double>(Point(get_extents(hole).center() - middle));
                const double dist = (at - Vec2d(x, y)).norm();
                if (dist < best_dist) {
                    best_dist   = dist;
                    best_points = hole.points.size();
                }
            }
        }
        // The hole asked for, not another one: within a millimetre of where it was drawn.
        out.push_back(best_dist < 1. ? best_points : 0);
    }
    return out;
}

const std::initializer_list<ConfigBase::SetDeserializeItem> polyhole_settings = {
    {"hole_to_polyhole", "1"},
    {"hole_to_polyhole_threshold", "0.05"},
    {"hole_to_polyhole_twisted", "0"},
    {"layer_height", "0.2"},
    {"initial_layer_print_height", "0.2"},
    {"elefant_foot_compensation", "0"},
    {"xy_hole_compensation", "0"},
    {"xy_contour_compensation", "0"},
    {"nozzle_diameter", "0.4"},
};

} // namespace

TEST_CASE("A polyhole has three sides at the least whatever hole_to_polyhole_max_edges says", "[Polyholes]")
{
    // 4 * 5 * 0.4 / 0.4 = 20 sides for a hole of 5 mm radius and a 0.4 mm nozzle.
    const coord_t radius = scaled<coord_t>(5.);
    const coord_t nozzle = scaled<coord_t>(0.4);
    for (int max_edges : {2, 1, 0}) {
        DYNAMIC_SECTION("max_edges " << max_edges)
        {
            const Polygons made = create_polyholes(Point(0, 0), radius, nozzle, false, max_edges);
            REQUIRE(made.size() == 1);
            REQUIRE(made.front().size() == 3);
            // A triangle with its flats on the circle: corners twice the radius out, the hole's area inside it.
            for (const Point &corner : made.front().points)
                CHECK(std::abs(corner.cast<double>().norm() - 2. * radius) < 2.);
            CHECK(made.front().is_clockwise());
        }
    }
    // The limits that were in use stay as they were.
    CHECK(create_polyholes(Point(0, 0), radius, nozzle, false, 3).front().size() == 3);
    CHECK(create_polyholes(Point(0, 0), radius, nozzle, false, 8).front().size() == 8);
    CHECK(create_polyholes(Point(0, 0), radius, nozzle, false, 50).front().size() == 20);
}

TEST_CASE("A negative hole_to_polyhole_max_edges gives a polyhole of three sides", "[Polyholes][PolyholesNegativeEdges]")
{
    // Before the limit was held this was taken for a count of 2^64 - 4 sides.
    const Polygons made = create_polyholes(Point(0, 0), scaled<coord_t>(5.), scaled<coord_t>(0.4), true, -4);
    REQUIRE(made.size() == 5);
    for (const Polygon &polygon : made)
        CHECK(polygon.size() == 3);
}

TEST_CASE("A polyhole is drawn about its hole's middle to the unit", "[Polyholes]")
{
    // A hole 150 mm from the object's origin, its middle on coordinates a float cannot hold (it holds every
    // sixteenth unit there), 2 mm in radius: eight sides.
    const Point   center(coord_t(150000004), coord_t(-150000003));
    const coord_t radius = scaled<coord_t>(2.);
    const Polygons made  = create_polyholes(center, radius, scaled<coord_t>(0.4), false, 50);
    REQUIRE(made.size() == 1);
    const Polygon &polygon = made.front();
    REQUIRE(polygon.size() == 8);

    // The corners due east and west, and due north and south, are as far one side of the middle as the other.
    const BoundingBox box = get_extents(polygon);
    CHECK(box.center().x() == center.x());
    CHECK(box.center().y() == center.y());
    // And every corner is the same distance out, to the unit: the radius over cos(pi / 8).
    const double reach = double(radius) / std::cos(M_PI / 8.);
    for (const Point &corner : polygon.points) {
        CAPTURE(corner.x(), corner.y());
        CHECK(std::abs((corner - center).cast<double>().norm() - reach) < 1.5);
    }
}

TEST_CASE("A hole with a dent in it is not convex", "[Polyholes]")
{
    // 64 points on a circle of 3 mm, clockwise.
    CHECK(polyhole_is_convex(round_hole(50., 50., 3.)));

    // One point set in toward the middle. By 0.005 mm it is still outside the line through its neighbours
    // (which passes 0.0144 mm inside the circle), and the hole is convex; by 0.018 mm it is inside that line.
    auto with_point_at = [](double reach) { return hole_of(50., 50., 64, [reach](int i) { return i == 10 ? reach : 3.; }); };
    CHECK(polyhole_is_convex(with_point_at(2.995)));
    CHECK_FALSE(polyhole_is_convex(with_point_at(2.982)));
    CHECK_FALSE(polyhole_is_convex(with_point_at(2.94)));

    // A point on a flat that rounding has left two units to the wrong side of it is no dent; one 0.001 mm to
    // the wrong side is.
    Polygon square;
    square.points = {{0, 0}, {0, 10000000}, {5000000, 10000000}, {10000000, 10000000}, {10000000, 0}};
    REQUIRE(square.is_clockwise());
    CHECK(polyhole_is_convex(square));
    square.points[2].y() -= 2;
    CHECK(polyhole_is_convex(square));
    square.points[2].y() -= 1000;
    CHECK_FALSE(polyhole_is_convex(square));
}

TEST_CASE("A round hole is made a polyhole and one with a dent within the margin is left as drawn", "[Polyholes]")
{
    // A plate 3 mm thick with two holes of 3 mm radius in 64 sides. One is round. The other has one point set in
    // by 0.06 mm: every point is still within the margin of 0.05 mm of one distance, but the hole is not convex.
    const Polygon dented = hole_of(7., 0., 64, [](int i) { return i == 10 ? 2.94 : 3.; });

    Print print;
    init_and_process_print({extruded(plate_with({round_hole(-7., 0., 3.), dented}), 0., 3.)}, print, polyhole_settings);
    REQUIRE(print.objects().front()->layers().size() == 15);

    // 4 * 3 * 0.4 / 0.4 = 12 sides.
    const std::vector<size_t> round = hole_points_by_layer(print, -7., 0.);
    const std::vector<size_t> dent  = hole_points_by_layer(print, 7., 0.);
    for (size_t layer = 0; layer < round.size(); ++layer) {
        CAPTURE(layer);
        CHECK(round[layer] == 12);
        CHECK(dent[layer] > 12);
    }
}

TEST_CASE("A hole that is round again further up is made a polyhole there too", "[Polyholes]")
{
    // Three slabs of 2 mm on one another: a round hole of 2.5 mm radius through the first and the third, and a
    // square one in the same place through the second. The round hole of the third slab is, to the unit, the
    // hole of the first.
    const Polygon round = round_hole(0., 0., 2.5);
    Polygon       square;
    const coord_t h = scaled<coord_t>(2.5);
    square.points   = {{-h, -h}, {-h, h}, {h, h}, {h, -h}};
    REQUIRE(square.is_clockwise());

    TriangleMesh mesh = extruded(plate_with({round}), 0., 2.);
    mesh.merge(extruded(plate_with({square}), 2., 4.));
    mesh.merge(extruded(plate_with({round}), 4., 6.));

    Print print;
    init_and_process_print({mesh}, print, polyhole_settings);
    const std::vector<size_t> points = hole_points_by_layer(print, 0., 0.);
    REQUIRE(points.size() == 30);

    // 4 * 2.5 * 0.4 / 0.4 = 10 sides.
    for (size_t layer = 0; layer < points.size(); ++layer) {
        CAPTURE(layer);
        if (layer >= 10 && layer < 20)
            CHECK(points[layer] == 4);
        else
            CHECK(points[layer] == 10);
    }
}
