#include <catch2/catch.hpp>
#include <test_utils.hpp>

#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/AABBTreeIndirect.hpp>

using namespace Slic3r;

TEST_CASE("Building a tree over a box, ray caster and closest query", "[AABBIndirect]")
{
    TriangleMesh tmesh = make_cube(1., 1., 1.);

    auto tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(tmesh.its.vertices, tmesh.its.indices);
    REQUIRE(! tree.empty());

    igl::Hit hit;
	bool intersected = AABBTreeIndirect::intersect_ray_first_hit(
		tmesh.its.vertices, tmesh.its.indices,
		tree,
		Vec3d(0.5, 0.5, -5.),
		Vec3d(0., 0., 1.),
		hit);

    REQUIRE(intersected);
    REQUIRE(hit.t == Approx(5.));

    std::vector<igl::Hit> hits;
	bool intersected2 = AABBTreeIndirect::intersect_ray_all_hits(
		tmesh.its.vertices, tmesh.its.indices,
		tree,
        Vec3d(0.3, 0.5, -5.),
		Vec3d(0., 0., 1.),
		hits);
    REQUIRE(intersected2);
    REQUIRE(hits.size() == 2);
    REQUIRE(hits.front().t == Approx(5.));
    REQUIRE(hits.back().t == Approx(6.));

    size_t hit_idx;
    Vec3d  closest_point;
    double squared_distance = AABBTreeIndirect::squared_distance_to_indexed_triangle_set(
		tmesh.its.vertices, tmesh.its.indices,
		tree,
        Vec3d(0.3, 0.5, -5.),
		hit_idx, closest_point);
    REQUIRE(squared_distance == Approx(5. * 5.));
    REQUIRE(closest_point.x() == Approx(0.3));
    REQUIRE(closest_point.y() == Approx(0.5));
    REQUIRE(closest_point.z() == Approx(0.));

    squared_distance = AABBTreeIndirect::squared_distance_to_indexed_triangle_set(
		tmesh.its.vertices, tmesh.its.indices,
		tree,
        Vec3d(0.3, 0.5, 5.),
		hit_idx, closest_point);
    REQUIRE(squared_distance == Approx(4. * 4.));
    REQUIRE(closest_point.x() == Approx(0.3));
    REQUIRE(closest_point.y() == Approx(0.5));
    REQUIRE(closest_point.z() == Approx(1.));
}


#include <libslic3r/AABBTreeLines.hpp>
#include <libslic3r/Line.hpp>
#include <algorithm>
#include <random>

// Bambu #12874: integer AlignedBox::squaredExteriorDistance() overflows past ~0.46 mm and disabled pruning.
// The extra double bound must not drop any line that is really within the radius.
TEST_CASE("AABB lines radius query matches brute force on a large layer", "[AABBIndirect][Regression]")
{
    std::mt19937 rng(12874);
    std::uniform_int_distribution<coord_t> pos(-scaled<coord_t>(100.), scaled<coord_t>(100.));
    std::uniform_int_distribution<coord_t> len(-scaled<coord_t>(2.), scaled<coord_t>(2.));
    Lines lines;
    for (int i = 0; i < 2000; ++ i) {
        Point a(pos(rng), pos(rng));
        lines.emplace_back(a, a + Point(len(rng), len(rng)));
    }
    auto tree = AABBTreeLines::build_aabb_tree_over_indexed_lines(lines);
    const coord_t radius = scaled<coord_t>(0.3);
    const coord_t r2     = radius * radius;
    for (int q = 0; q < 200; ++ q) {
        const Point p(pos(rng), pos(rng));
        std::vector<size_t> found = AABBTreeLines::all_lines_in_radius(lines, tree, p, r2);
        std::sort(found.begin(), found.end());
        std::vector<size_t> expected;
        for (size_t i = 0; i < lines.size(); ++ i)
            if (lines[i].distance_to_squared(p) < double(r2))
                expected.push_back(i);
        CHECK(found == expected);
    }
}