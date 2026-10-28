// OrcaSlicer #16334: the seam placer takes one candidate polygon per island from the island's outer
// wall. A fully overhanging outer loop carries no erExternalPerimeter path, so it used to be missed:
// - when another island came first on the layer, the overhanging island got no polygon at all;
// - when it came first, the layer-wide fallback merged its outer and inner loops into one polygon.
// The outer loop is now recognised by inset_idx 0, and the fallback applies per island collection.
// These are the upstream cases (test_seam_placer.cpp there), run directly on
// SeamPlacerImpl::extract_perimeter_polygons over a real sliced layer.

#include <catch2/catch.hpp>

#include <cmath>
#include <vector>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

namespace Slic3r { namespace SeamPlacerImpl {
Polygons extract_perimeter_polygons(const Layer *layer, std::vector<const LayerRegion *> &corresponding_regions_out,
                                    bool has_precise_seam_modifiers);
}} // namespace Slic3r::SeamPlacerImpl

using namespace Slic3r;

namespace {

struct SlicedLayer
{
    Model              model;
    Print              print;
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();

    SlicedLayer()
    {
        config.set_deserialize_strict({ { "layer_height", "0.2" }, { "initial_layer_print_height", "0.2" }, { "wall_loops", "1" } });
        Test::init_print({ TriangleMesh(its_make_cube(20, 20, 0.4)) }, print, model, config);
    }

    // The first layer of the sliced object, with its perimeters cleared so each case supplies its own loops.
    Layer &layer()
    {
        REQUIRE(print.objects().size() == 1);
        PrintObject &object = *print.get_object(0);
        object.slice();
        REQUIRE_FALSE(object.layers().empty());
        Layer &layer = *object.layers().front();
        REQUIRE_FALSE(layer.regions().empty());
        for (LayerRegion *region : layer.regions())
            region->perimeters.clear();
        return layer;
    }
};

// Axis-aligned rectangle (mm) as a closed loop of the given role, one path or one path per edge.
ExtrusionLoop make_loop(const BoundingBoxf &rect, ExtrusionRole role, int inset_idx, bool separate_paths = false)
{
    Points points{ Point::new_scale(rect.min.x(), rect.min.y()), Point::new_scale(rect.max.x(), rect.min.y()),
                   Point::new_scale(rect.max.x(), rect.max.y()), Point::new_scale(rect.min.x(), rect.max.y()) };
    points.push_back(points.front());
    ExtrusionPaths paths;
    if (separate_paths) {
        for (size_t i = 1; i < points.size(); ++i) {
            ExtrusionPath path(role, 0.08, 0.4f, 0.2f);
            path.polyline = Polyline(Points{ points[i - 1], points[i] });
            paths.push_back(std::move(path));
        }
    } else {
        ExtrusionPath path(role, 0.08, 0.4f, 0.2f);
        path.polyline = Polyline(std::move(points));
        paths.push_back(std::move(path));
    }
    ExtrusionLoop loop(std::move(paths));
    loop.inset_idx = inset_idx;
    return loop;
}

BoundingBoxf rect(double x0, double y0, double x1, double y1) { return BoundingBoxf(Vec2d(x0, y0), Vec2d(x1, y1)); }

const BoundingBoxf OUTER_A = rect(2, 2, 8, 8);
const BoundingBoxf INNER_A = rect(3.5, 3.5, 6.5, 6.5);
const BoundingBoxf OUTER_B = rect(12, 2, 18, 8);
const BoundingBoxf INNER_B = rect(13.5, 3.5, 16.5, 6.5);

// True if every point of the polygon lies on the outline of `r` (within 0.01 mm).
bool on_outline(const Polygon &polygon, const BoundingBoxf &r)
{
    if (polygon.points.empty())
        return false;
    for (const Point &p : polygon.points) {
        const Vec2d q = unscale(p);
        const bool  on_x = std::abs(q.x() - r.min.x()) < 0.01 || std::abs(q.x() - r.max.x()) < 0.01;
        const bool  on_y = std::abs(q.y() - r.min.y()) < 0.01 || std::abs(q.y() - r.max.y()) < 0.01;
        const bool  in_x = q.x() > r.min.x() - 0.01 && q.x() < r.max.x() + 0.01;
        const bool  in_y = q.y() > r.min.y() - 0.01 && q.y() < r.max.y() + 0.01;
        if (!((on_x && in_y) || (on_y && in_x)))
            return false;
    }
    return true;
}

Polygons extract(const Layer &layer)
{
    std::vector<const LayerRegion *> regions;
    Polygons polygons = SeamPlacerImpl::extract_perimeter_polygons(&layer, regions, false);
    CHECK(regions.size() == polygons.size());
    return polygons;
}

} // namespace

TEST_CASE("Fully overhanging outer loop is its own seam perimeter regardless of island order", "[SeamPlacer][Regression]")
{
    const bool overhang_first = GENERATE(false, true);
    CAPTURE(overhang_first);
    SlicedLayer  sliced;
    Layer       &layer  = sliced.layer();
    LayerRegion &region = *layer.get_region(0);

    // Island A: ordinary outer wall. Island B: the outer wall is entirely overhang but keeps inset_idx 0,
    // followed by an inner wall that is overhang as well.
    ExtrusionEntityCollection island_a, island_b;
    island_a.append(make_loop(OUTER_A, erExternalPerimeter, 0));
    island_b.append(make_loop(OUTER_B, erOverhangPerimeter, 0));
    island_b.append(make_loop(INNER_B, erOverhangPerimeter, 1));
    if (overhang_first) {
        region.perimeters.append(std::move(island_b));
        region.perimeters.append(std::move(island_a));
    } else {
        region.perimeters.append(std::move(island_a));
        region.perimeters.append(std::move(island_b));
    }

    const Polygons polygons = extract(layer);
    // Exactly the two outer loops; the inner loop is not merged into either of them.
    REQUIRE(polygons.size() == 2);
    const Polygon &a = overhang_first ? polygons[1] : polygons[0];
    const Polygon &b = overhang_first ? polygons[0] : polygons[1];
    CHECK(on_outline(a, OUTER_A));
    CHECK(on_outline(b, OUTER_B));
}

TEST_CASE("External role selects the outer loop when inset_idx is unknown", "[SeamPlacer][Regression]")
{
    SlicedLayer  sliced;
    Layer       &layer = sliced.layer();
    // Thin-wall style loops carry the external role without a stored depth.
    ExtrusionEntityCollection island;
    island.append(make_loop(OUTER_A, erExternalPerimeter, -1));
    island.append(make_loop(INNER_A, erPerimeter, -1));
    layer.get_region(0)->perimeters.append(std::move(island));

    const Polygons polygons = extract(layer);
    REQUIRE(polygons.size() == 1);
    CHECK(on_outline(polygons.front(), OUTER_A));
}

TEST_CASE("A collection without a recognizable outer loop gets its own fallback polygon", "[SeamPlacer][Regression]")
{
    SlicedLayer  sliced;
    Layer       &layer = sliced.layer();
    // The second collection has neither an external path nor a stored depth (for example a loop loaded
    // from the slicing cache). It must not be skipped just because an earlier collection added polygons.
    ExtrusionEntityCollection island_a, island_b;
    island_a.append(make_loop(OUTER_A, erExternalPerimeter, 0));
    island_b.append(make_loop(OUTER_B, erOverhangPerimeter, -1));
    layer.get_region(0)->perimeters.append(std::move(island_a));
    layer.get_region(0)->perimeters.append(std::move(island_b));

    const Polygons polygons = extract(layer);
    REQUIRE(polygons.size() == 2);
    CHECK(on_outline(polygons[0], OUTER_A));
    CHECK(on_outline(polygons[1], OUTER_B));
}

TEST_CASE("Mixed overhang and external outer loop is taken once", "[SeamPlacer][Regression]")
{
    // Whether or not the generator stored the depth, the external path alone identifies the loop.
    const int outer_inset_idx = GENERATE(0, -1);
    CAPTURE(outer_inset_idx);
    SlicedLayer  sliced;
    Layer       &layer = sliced.layer();

    ExtrusionLoop mixed = make_loop(OUTER_A, erExternalPerimeter, outer_inset_idx, true);
    mixed.paths.front().set_extrusion_role(erOverhangPerimeter);
    ExtrusionEntityCollection island;
    island.append(std::move(mixed));
    island.append(make_loop(INNER_A, erPerimeter, outer_inset_idx == 0 ? 1 : -1));
    layer.get_region(0)->perimeters.append(std::move(island));

    const Polygons polygons = extract(layer);
    REQUIRE(polygons.size() == 1);
    CHECK(on_outline(polygons.front(), OUTER_A));
}

// Edge: islands whose outer wall already has external paths keep exactly the polygons they had before.
TEST_CASE("Ordinary islands keep one outer polygon each", "[SeamPlacer]")
{
    SlicedLayer  sliced;
    Layer       &layer = sliced.layer();
    ExtrusionEntityCollection island_a, island_b;
    island_a.append(make_loop(OUTER_A, erExternalPerimeter, 0));
    island_a.append(make_loop(INNER_A, erPerimeter, 1));
    island_b.append(make_loop(OUTER_B, erExternalPerimeter, 0));
    island_b.append(make_loop(INNER_B, erPerimeter, 1));
    layer.get_region(0)->perimeters.append(std::move(island_a));
    layer.get_region(0)->perimeters.append(std::move(island_b));

    const Polygons polygons = extract(layer);
    REQUIRE(polygons.size() == 2);
    CHECK(on_outline(polygons[0], OUTER_A));
    CHECK(on_outline(polygons[1], OUTER_B));
}
