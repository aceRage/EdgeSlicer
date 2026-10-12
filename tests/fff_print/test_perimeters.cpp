// Perimeter generator tests ported from upstream OrcaSlicer's tests/fff_print/test_perimeters.cpp
// (Catch2 v3 there, v2 here). Only the cases of the ported PRs are here; upstream's other cases test
// features this tree does not have yet.

#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

// ---------------------------------------------------------------------------------------------------
// Orca #15694: overhang detection must not count a lower-layer sliver the wall generator never prints.
// ---------------------------------------------------------------------------------------------------
namespace {

// The rib spans z=[0,5] and the slab z=[5,6], so this is the slab's first layer - the only one whose
// support comes from the rib rather than from the slab below it.
const double slab_first_layer_z = 5.2;

// Rib widths either side of what the wall generators can print. At a 0.4mm nozzle the classic generator
// builds nothing thinner than nozzle/3 = 0.133mm and Arachne drops anything below min_feature_size, 25%
// of the nozzle = 0.1mm. 0.08mm is under both thresholds, 0.3mm over both.
const double unprintable_rib = 0.08;
const double printable_rib   = 0.3;

// A 4x5mm anchor tower carrying a 20x5mm slab at z=[5,6], with a rib `rib_width` wide running the whole
// length of the slab beneath its y=0 edge; a `rib_width` of 0 leaves the rib out. Nothing else is under
// that edge, so whether the wall along it is an overhang rests entirely on the rib. Overhang detection
// grows the lower slices by half the nozzle diameter before it asks, which carries either rib past the
// 0.21mm from the slab edge to that wall - the unprintable one only fails to reach it once it is filtered
// out for being unprintable.
Print &slab_over_rib(Print &print, Model &model, double rib_width, const DynamicPrintConfig &config)
{
    ModelObject *object = model.add_object();
    object->name = "slab_over_rib.stl";
    object->add_volume(make_cube(4., 5., 6.), ModelVolumeType::MODEL_PART, false);
    if (rib_width > 0.) {
        TriangleMesh rib = make_cube(20., rib_width, 5.);
        rib.translate(4.f, 0.f, 0.f);
        object->add_volume(std::move(rib), ModelVolumeType::MODEL_PART, false);
    }
    TriangleMesh slab = make_cube(20., 5., 1.);
    slab.translate(4.f, 0.f, 5.f);
    object->add_volume(std::move(slab), ModelVolumeType::MODEL_PART, false);
    object->add_instance();
    object->ensure_on_bed();

    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    return print;
}

// Every setting the assertions below depend on, so none of them rests on a default. The wall line widths
// are pinned because the rib widths above are chosen against the distance from the slab edge to its outer
// wall.
DynamicPrintConfig printable_rib_config(const char *wall_generator, bool detect_thin_wall)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "wall_generator",                wall_generator },
        { "layer_height",                  0.2 },  // puts a layer boundary exactly on the top of the rib
        { "initial_layer_print_height",    0.2 },
        { "nozzle_diameter",               "0.4" },
        { "outer_wall_line_width",         0.42 },
        { "inner_wall_line_width",         0.45 },
        { "wall_loops",                    2 },
        { "detect_overhang_wall",          true },
        { "detect_thin_wall",              detect_thin_wall },
        { "min_feature_size",              "25%" },
        { "raft_layers",                   0 },
        // Anything that adds, drops or reorders walls would move length between the roles being counted.
        { "extra_perimeters_on_overhangs", false },
        { "overhang_reverse",              false },
        { "only_one_wall_top",             false },
        { "only_one_wall_first_layer",     false },
        { "unsupported_wall_last",         false },
        { "sparse_infill_density",         "15%" },
    });
    return config;
}

// Length of every path of `role` on the layer at `print_z`, loops and open extrusions alike.
double role_length_at(const Print &print, double print_z, ExtrusionRole role)
{
    double len = 0.;
    const auto add_entity = [&len, role](const ExtrusionEntity *entity, auto &&self) -> void {
        const auto add_paths = [&len, role](const ExtrusionPaths &paths) {
            for (const ExtrusionPath &path : paths)
                if (path.role() == role)
                    len += path.length();
        };
        if (const auto *coll = dynamic_cast<const ExtrusionEntityCollection*>(entity)) {
            for (const ExtrusionEntity *child : coll->entities)
                self(child, self);
        } else if (const auto *loop = dynamic_cast<const ExtrusionLoop*>(entity)) {
            add_paths(loop->paths);
        } else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath*>(entity)) {
            add_paths(multi->paths);
        } else if (const auto *path = dynamic_cast<const ExtrusionPath*>(entity)) {
            if (path->role() == role)
                len += path->length();
        }
    };

    for (const Layer *layer : print.objects().front()->layers()) {
        if (std::abs(layer->print_z - print_z) > EPSILON)
            continue;
        for (const LayerRegion *region : layer->regions())
            add_entity(&region->perimeters, add_entity);
    }
    return len;
}

double overhang_length_at(const Print &print, double print_z) { return role_length_at(print, print_z, erOverhangPerimeter); }

} // namespace

// A sliver the wall generator prints nothing for holds nothing up, so it cannot be what decides that the
// wall above it is not an overhang. The rib under the slab is the only thing that edge of the slab could
// rest on: below the threshold of the active generator the slab has to come out exactly as it does with
// no rib at all, and the last check is the control - a rib the generator does print anchors that wall,
// without which the first check would hold for want of any sensitivity to the rib.
TEST_CASE("A lower layer sliver too thin to print does not support the wall above it", "[Perimeters]")
{
    const char *wall_generator   = GENERATE("classic", "arachne");
    const bool  detect_thin_wall = GENERATE(true, false);
    CAPTURE(wall_generator, detect_thin_wall);

    auto overhang_for = [wall_generator, detect_thin_wall](double rib_width) {
        Print print;
        Model model;
        slab_over_rib(print, model, rib_width, printable_rib_config(wall_generator, detect_thin_wall));
        print.process();
        REQUIRE_FALSE(print.objects().empty());
        return overhang_length_at(print, slab_first_layer_z);
    };

    const double no_rib      = overhang_for(0.);
    const double unprintable = overhang_for(unprintable_rib);
    const double printable   = overhang_for(printable_rib);

    // Only where the slab meets the tower is it held up from below, so both of its 20mm walls overhang.
    REQUIRE(no_rib > scale_(30.));
    CHECK_THAT(unprintable, Catch::Matchers::WithinAbs(no_rib, scale_(1.)));
    // A rib that does get printed takes the 20mm outer wall running along it out of the overhangs.
    CHECK(printable < no_rib - scale_(15.));
}

// ---------------------------------------------------------------------------------------------------
// Orca #13891: no fuzzy skin on bridge-like overhang walls.
// ---------------------------------------------------------------------------------------------------
namespace {

// Every setting the fuzzy skin assertions below depend on.
DynamicPrintConfig fuzzy_skin_config(const char *wall_generator)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "wall_generator",             wall_generator },
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        // One wall, so every wall point along the long sides belongs to the fuzzed outer wall.
        { "wall_loops",                 1 },
        { "fuzzy_skin",                 "external" },
        { "fuzzy_skin_noise_type",      "classic" },
        { "fuzzy_skin_thickness",       0.3 },
        { "fuzzy_skin_point_distance",  0.8 },
    });
    return config;
}

// How far the wall points over the middle 60% of the layer's length stray across its width, worst side.
// A negative result means there is no layer at `print_z`.
double mid_span_wall_spread(const Print &print, double print_z)
{
    for (const Layer *layer : print.objects().front()->layers()) {
        if (std::abs(layer->print_z - print_z) > 1e-4)
            continue;
        const BoundingBox bbox  = get_extents(layer->lslices);
        const coord_t     x_min = bbox.min.x() + bbox.size().x() / 5;
        const coord_t     x_max = bbox.max.x() - bbox.size().x() / 5;
        Points            points;
        for (const LayerRegion *region : layer->regions())
            region->perimeters.collect_points(points);
        coord_t spread = 0;
        for (const bool south : { true, false }) {
            coord_t lo = bbox.max.y(), hi = bbox.min.y();
            for (const Point &p : points)
                if (p.x() > x_min && p.x() < x_max && (p.y() < bbox.center().y()) == south) {
                    lo = std::min(lo, p.y());
                    hi = std::max(hi, p.y());
                }
            spread = std::max(spread, hi - lo);
        }
        return unscale<double>(spread);
    }
    return -1.;
}

} // namespace

// TestMesh::bridge is a 50x10mm deck from z=5 to z=8 on two 5mm-wide pillars, leaving a 40mm span. The deck's
// first layer (print_z 5.2) crosses the span unsupported; the layers above it rest on the deck.
TEST_CASE("Fuzzy skin leaves the walls of a bridge smooth", "[Perimeters]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    CAPTURE(wall_generator);

    Print print;
    init_and_process_print({ TestMesh::bridge }, print, fuzzy_skin_config(wall_generator));
    REQUIRE_FALSE(print.objects().empty());

    // Control: one deck layer up the same walls rest on the deck, so they are fuzzed.
    CHECK(mid_span_wall_spread(print, 5.6) > 0.1);
    // Over the unsupported span the walls stay straight.
    const double bridged = mid_span_wall_spread(print, 5.2);
    CHECK(bridged >= 0.);
    CHECK(bridged < 0.001);
}

// One object: a 20x20x3mm block on the bed and a second one floating above it from z=5 to z=8. The layers in
// the gap are empty, so the floating block's first layer (print_z 5.2) has a layer below it with nothing
// printed on it; the layers above rest on the floating block.
TEST_CASE("Fuzzy skin leaves the walls over an empty layer smooth", "[Perimeters]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    CAPTURE(wall_generator);

    TriangleMesh mesh     = make_cube(20., 20., 3.);
    TriangleMesh floating = make_cube(20., 20., 3.);
    floating.translate(0.f, 0.f, 5.f);
    mesh.merge(floating);

    Print print;
    init_and_process_print({ mesh }, print, fuzzy_skin_config(wall_generator));
    REQUIRE_FALSE(print.objects().empty());

    // Control: one layer up the walls rest on the floating block, so they are fuzzed.
    CHECK(mid_span_wall_spread(print, 5.6) > 0.1);
    // Nothing is printed under the first floating layer, so its walls stay straight.
    const double floating_first_layer = mid_span_wall_spread(print, 5.2);
    CHECK(floating_first_layer >= 0.);
    CHECK(floating_first_layer < 0.001);
}
