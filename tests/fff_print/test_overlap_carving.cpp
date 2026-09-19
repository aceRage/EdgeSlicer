// Order-independent overlap carving (enable_order_independent_overlap_carving), ported from
// BambuStudio dba0b39d7 + d0d0fab7f. See tests/design_modifier_overlap.md §1.
//
// The scene both cases below slice is the one the upstream fix is about: ONE object holding two
// overlapping MODEL_PART volumes, the SMALL one first in ModelObject::volumes and the LARGE one
// second. In slices_to_regions() the carve loop walks the volumes in that order and subtracts each
// part's slice from every earlier overlapping one, so with the option off the large part (later)
// carves the small part (earlier) and takes the shared corner. With the option on the pair is
// resolved by bounding-box volume instead, so the small part keeps the corner wherever it sits in
// the list.
//
// The two parts carry different wall_loops, which is what puts them in different PrintRegions and
// so lets a layer's regions be told apart: each LayerRegion reports the config of the region it
// belongs to, and the area it owns at a given Z says which part won the overlap.

#include <catch2/catch.hpp>

#include <vector>

#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Surface.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;
using Catch::Matchers::WithinRel;

namespace {

// wall_loops of the region each part resolves to - the tag that says which part owns an area.
constexpr int SMALL_PART_WALLS = 5;
constexpr int LARGE_PART_WALLS = 2;

constexpr double LARGE_MM = 20.;
constexpr double SMALL_MM = 8.;

// One object, two overlapping normal parts:
//   volumes[0] - the SMALL 8 mm cube, sitting in the large cube's origin corner (overlapping it
//                completely, since it is wholly inside the large cube's footprint and height),
//   volumes[1] - the LARGE 20 mm cube at the origin.
// Deliberately small-first: that is the ordering the upstream bug erases the small part in.
void add_small_then_large(Slic3r::Model &model)
{
    ModelObject *object = model.add_object();
    object->name = "small_inside_large";

    ModelVolume *small_part = object->add_volume(Slic3r::make_cube(SMALL_MM, SMALL_MM, SMALL_MM));
    ModelVolume *large_part = object->add_volume(Slic3r::make_cube(LARGE_MM, LARGE_MM, LARGE_MM));

    REQUIRE(small_part->is_model_part());
    REQUIRE(large_part->is_model_part());

    // Distinct wall counts put the two parts in distinct PrintRegions.
    small_part->config.set_key_value("wall_loops", new ConfigOptionInt(SMALL_PART_WALLS));
    large_part->config.set_key_value("wall_loops", new ConfigOptionInt(LARGE_PART_WALLS));

    object->add_instance();
    object->ensure_on_bed();
}

struct CarveResult
{
    double small_area = 0.; // mm^2 owned by the 5-wall region on the probed layer
    double large_area = 0.; // mm^2 owned by the 2-wall region on the probed layer
};

// Slice the scene and measure, on a layer well inside both cubes, how much area each part's
// region ends up owning.
CarveResult slice_and_measure(bool order_independent)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"enable_order_independent_overlap_carving", order_independent ? "1" : "0"},
        {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"},
        // Keep the two parts in their own regions rather than letting a shared filament merge them.
        {"wall_loops", "2"},
    });

    Slic3r::Print print;
    Slic3r::Model model;
    add_small_then_large(model);
    print.auto_assign_extruders(model.objects.front());
    print.apply(model, config);
    print.set_status_silent();
    print.process();

    REQUIRE(! print.objects().empty());
    const PrintObject *object = print.objects().front();

    // A layer around z = 4 mm: inside the small cube (0..8 mm) and inside the large one (0..20 mm),
    // and far enough off the bed to be clear of first-layer special-casing.
    const Layer *probe = nullptr;
    for (const Layer *layer : object->layers())
        if (layer->print_z >= 4. && layer->print_z <= 4.5) {
            probe = layer;
            break;
        }
    REQUIRE(probe != nullptr);

    CarveResult out;
    for (const LayerRegion *region : probe->regions()) {
        double area = 0.;
        for (const Surface &surface : region->slices.surfaces)
            // area() is in scaled^2 units; unscale both dimensions to get mm^2.
            area += unscaled<double>(unscaled<double>(surface.expolygon.area()));
        if (region->region().config().wall_loops.value == SMALL_PART_WALLS)
            out.small_area += area;
        else if (region->region().config().wall_loops.value == LARGE_PART_WALLS)
            out.large_area += area;
    }
    return out;
}

} // namespace

// Off: today's behaviour, and the whole point of the default. The large part is listed later, so
// it carves the small part that precedes it and the small part loses the entire overlap. Since
// the small cube lies wholly inside the large one, that leaves it with no area at all - exactly
// the "body silently disappears" symptom the upstream commit describes.
TEST_CASE("overlap carving: with the option off the later, larger part wins the overlap", "[OverlapCarving]")
{
    const CarveResult off = slice_and_measure(false);

    // The small part, listed first, is carved away to nothing by the later large part.
    CHECK(off.small_area < 1.);
    // The large part keeps its whole 20x20 footprint.
    REQUIRE_THAT(off.large_area, WithinRel(LARGE_MM * LARGE_MM, 0.05));
}

// On: the pair is resolved by bounding-box volume, so the smaller part carves the larger one even
// though it is listed first. The small part keeps its 8x8 corner and the large part is left with
// the rest of its footprint.
TEST_CASE("overlap carving: with the option on the smaller part wins the overlap regardless of order", "[OverlapCarving]")
{
    const CarveResult on = slice_and_measure(true);

    // The small part survives with its own 8x8 footprint.
    REQUIRE_THAT(on.small_area, WithinRel(SMALL_MM * SMALL_MM, 0.05));
    // ... carved out of the large part, which keeps 20x20 minus that corner.
    REQUIRE_THAT(on.large_area, WithinRel(LARGE_MM * LARGE_MM - SMALL_MM * SMALL_MM, 0.05));
}

// The two together are the actual claim: flipping the option moves the shared corner from one
// part to the other, and the total area covered is the same either way (nothing is created or
// lost, only reassigned).
TEST_CASE("overlap carving: the option reassigns the overlap rather than changing the covered area", "[OverlapCarving]")
{
    const CarveResult off = slice_and_measure(false);
    const CarveResult on  = slice_and_measure(true);

    // The small part owns area only when the option is on.
    CHECK(on.small_area > off.small_area);
    // ... and the large part gives up exactly that much.
    CHECK(on.large_area < off.large_area);
    // Total covered area is unchanged: the 20x20 footprint, just split differently.
    REQUIRE_THAT(off.small_area + off.large_area, WithinRel(LARGE_MM * LARGE_MM, 0.05));
    REQUIRE_THAT(on.small_area + on.large_area, WithinRel(LARGE_MM * LARGE_MM, 0.05));
}
