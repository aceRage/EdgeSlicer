// OrcaSlicer #17731: with staggered_inner_seams on, place_seam can walk forever at a hairpin
// inner corner (beta_angle near 0 blows depth into millions of mm; float spacing then stalls
// `while (depth > 0)`). Edge ports the hairpin beta-angle guard and fmod(depth, lap). This
// case is the upstream geometry, on Edge's ExtrusionLoop / Catch2 v2 APIs. Without the fix it
// fails (seam ~3.5 mm from the tip) or hangs; place_seam is run with a timeout so CI cannot hang.

#include <catch2/catch.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/GCode/SeamPlacer.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

struct PipelineFixture {
    Model              model;
    Print              print;
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();

    explicit PipelineFixture(float height = 0.2f)
    {
        auto mesh = its_make_cube(20, 20, height);
        config.set_deserialize_strict("seam_position", "back"); // Rear needs no visibility ray tracing.
        config.set_deserialize_strict("layer_height", "0.2");
        config.set_deserialize_strict("initial_layer_print_height", "0.2");
        config.set_deserialize_strict("outer_wall_line_width", "0.4");
        config.set_deserialize_strict("initial_layer_line_width", "0.4");
        config.set_deserialize_strict("wall_loops", "1");
        config.set_deserialize_strict("raft_layers", "0");
        Test::init_print({TriangleMesh(std::move(mesh))}, print, model, config);
    }

    PrintObject &prepare()
    {
        REQUIRE(print.objects().size() == 1);
        auto &object = *print.get_object(0);
        object.slice(); // Real layers/regions are sufficient: the case supplies its own perimeter loops.
        REQUIRE_FALSE(object.layers().empty());
        REQUIRE_FALSE(object.layers().front()->regions().empty());
        return object;
    }

    Points points_in_layer(const PrintObject &object, const std::vector<Vec2d> &xy) const
    {
        const auto       &volume    = *object.model_object()->volumes.front();
        const auto        minimum   = volume.mesh().bounding_box().min;
        const Transform3d transform = object.trafo_centered() * volume.get_matrix();
        Points            points;
        for (const auto &point : xy) {
            // add_volume centers the mesh; restore its local offset before applying the slicing transform.
            const Vec3d local  = minimum + Vec3d(point.x(), point.y(), 0.2);
            const Vec3d placed = transform * local;
            points.emplace_back(scale_(placed.x()), scale_(placed.y()));
        }
        return points;
    }
};

ExtrusionLoop make_loop(Points points, ExtrusionRole role, int inset_idx)
{
    REQUIRE(points.size() >= 3);
    points.push_back(points.front());
    ExtrusionPath path(role, 0.08, 0.4f, 0.2f);
    path.polyline = Polyline(std::move(points));
    ExtrusionPaths paths;
    paths.push_back(std::move(path));
    ExtrusionLoop loop(std::move(paths));
    loop.inset_idx = inset_idx;
    return loop;
}

LayerRegion &clear_first_layer(PrintObject &object)
{
    Layer &layer = *object.layers().front();
    for (LayerRegion *region : layer.regions())
        region->perimeters.clear();
    return *layer.get_region(0);
}

// place_seam on a worker so a hang cannot stall Catch2. Detach before FAIL: a joinable
// std::thread destructor would terminate, and an std::async future would join forever.
void place_seam_or_timeout(const SeamPlacer &placer, const Layer *layer, ExtrusionLoop &loop, float &overhang)
{
    std::atomic<bool> done{false};
    std::thread       worker([&] {
        placer.place_seam(layer, loop, Point(0, 0), overhang);
        done.store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            worker.detach();
            FAIL("place_seam hung on a hairpin inner seam");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    worker.join();
}

} // namespace

TEST_CASE("Staggered inner seam stays next to a seam on a hairpin corner", "[SeamPlacer][Regression]")
{
    PipelineFixture fixture; // One layer (0.2 mm cube).
    fixture.config.set_deserialize_strict("staggered_inner_seams", "1");
    fixture.print.apply(fixture.model, fixture.config);
    PrintObject &object = fixture.prepare();
    auto        &region = clear_first_layer(object);

    // A hole with an 8 mm slit into the material, 1 nm wide at its opening: the slit tip is the hole's rearmost point
    // and turns the wall back by a hair less than 180 degrees.
    const Point tip  = fixture.points_in_layer(object, {{10, 14}}).front();
    Points      hole = fixture.points_in_layer(object, {{10, 6}, {6, 6}, {6, 4}, {14, 4}, {14, 6}});
    hole.push_back(hole.front() + Point(1, 0));
    hole.push_back(tip);
    std::reverse(hole.begin(), hole.end()); // Holes run clockwise.
    ExtrusionLoop hole_loop = make_loop(hole, erExternalPerimeter, 0);
    hole_loop.set_loop_role(elrHole);
    // The inner wall sits 0.4 mm into the material and runs around the slit.
    ExtrusionLoop inner = make_loop(fixture.points_in_layer(object, {{9.6, 6.4}, {5.6, 6.4}, {5.6, 3.6}, {14.4, 3.6},
                                                                     {14.4, 6.4}, {10.4, 6.4}, {10.4, 14.4}, {9.6, 14.4}}),
                                    erPerimeter, 1);
    std::reverse(inner.paths.front().polyline.points.begin(), inner.paths.front().polyline.points.end());
    ExtrusionEntityCollection island;
    island.append(make_loop(fixture.points_in_layer(object, {{2, 2}, {18, 2}, {18, 18}, {2, 18}}), erExternalPerimeter, 0));
    island.append(std::move(hole_loop));
    island.append(inner);
    region.perimeters.append(std::move(island));

    SeamPlacer placer;
    placer.init(fixture.print, [] {});

    // The planned seam of the hole is the slit tip, a concave corner within a float step of a full reversal.
    const auto &layer_seams = placer.m_seam_per_object.at(&object).layers.front();
    const auto  hole_seam   = std::find_if(layer_seams.points.begin(), layer_seams.points.end(), [&](const auto &candidate) {
        return candidate.perimeter.seam_index < layer_seams.points.size() &&
               &layer_seams.points[candidate.perimeter.seam_index] == &candidate &&
               (unscale(tip).cast<float>() - candidate.position.head<2>()).norm() < 0.01f;
    });
    REQUIRE(hole_seam != layer_seams.points.end());
    REQUIRE(hole_seam->local_ccw_angle < -3.14159f);
    REQUIRE(hole_seam->local_ccw_angle > -static_cast<float>(PI)); // Short of a full reversal: cos(angle / 2) is positive.

    ExtrusionLoop placed   = inner;
    float         overhang = 0.0f;
    place_seam_or_timeout(placer, object.layers().front(), placed, overhang);
    // Staggering moves the inner seam about a line width along the wall, never around the loop at random.
    CHECK_THAT((unscale(placed.first_point()) - unscale(tip)).norm(), WithinAbs(0.0, 1.0));
}
