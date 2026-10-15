// A fuzzy skin point distance of 0 used to divide every wall segment forever: an endless loop
// that ran the slicer out of memory (OrcaSlicer #11069, fixed by #11915's 0.01 mm minimum). The
// minimum stops the GUI and the CLI's validate() from taking 0, but a config can still reach the
// slicer with it (an old project, a script, a Print::apply without validate()), so the slicer
// itself must not hang: region_config_from_model_volume switches such a region's fuzzy skin off.
//
// The painted case is the one that used to slip through: painted fuzzy skin regions start from the
// object's region config and turn "None" into "All". The guard now switches to Disabled
// (Orca #10312), which painting leaves alone.
//
// If one of these cases regresses the test does not fail, it hangs and eats memory - which is the
// bug. Each case slices a 20 mm cube in a few hundred milliseconds when it is fixed.

#include <catch2/catch.hpp>

#include <string>
#include <vector>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

void zero_pd_collect_loops(const ExtrusionEntity *entity, std::vector<const ExtrusionLoop *> &out)
{
    if (const auto *coll = dynamic_cast<const ExtrusionEntityCollection *>(entity)) {
        for (const ExtrusionEntity *child : coll->entities)
            zero_pd_collect_loops(child, out);
    } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(entity))
        out.push_back(loop);
}

// Points on the largest wall loop of the layer half way up: a plain 20 mm cube wall has a handful
// (its corners plus the seam), a fuzzed one hundreds.
size_t zero_pd_outer_wall_points(const Print &print)
{
    const PrintObject *object = print.objects().front();
    REQUIRE(object->layer_count() > 10);
    const Layer *layer = object->get_layer(int(object->layer_count() / 2));
    std::vector<const ExtrusionLoop *> loops;
    for (const LayerRegion *region : layer->regions())
        for (const ExtrusionEntity *entity : region->perimeters.entities)
            zero_pd_collect_loops(entity, loops);
    size_t best = 0;
    double best_len = -1.;
    for (const ExtrusionLoop *loop : loops) {
        const double len = loop->length();
        if (len > best_len) {
            best_len = len;
            best     = loop->as_polyline().points.size();
        }
    }
    return best;
}

size_t zero_pd_slice_cube(const char *fuzzy_skin, const char *point_distance, bool painted)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height",               "0.2" },
        { "initial_layer_print_height", "0.2" },
        { "wall_loops",                 "2" },
        { "enable_support",             "0" },
        { "enable_arc_fitting",         "0" },
        { "wall_generator",             "classic" },
        { "fuzzy_skin_noise_type",      "classic" },
        { "fuzzy_skin_thickness",       "0.3" },
        { "fuzzy_skin_first_layer",     "1" },
    });
    // Past validate(): set_deserialize_strict does not range check.
    config.set_deserialize_strict({ { "fuzzy_skin", fuzzy_skin }, { "fuzzy_skin_point_distance", point_distance } });

    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "cube";
    ModelVolume *volume = object->add_volume(mesh(TestMesh::cube_20x20x20));
    object->add_instance();
    object->ensure_on_bed();
    if (painted) {
        TriangleSelector selector(volume->mesh());
        for (int f = 0; f < int(volume->mesh().its.indices.size()); ++f)
            selector.set_facet(f, EnforcerBlockerType::ENFORCER);
        volume->fuzzy_skin_facets.set(selector);
        REQUIRE(volume->is_fuzzy_skin_painted());
    }

    Print print;
    print.auto_assign_extruders(model.objects.front());
    print.apply(model, config);
    print.set_status_silent();
    print.process();
    return zero_pd_outer_wall_points(print);
}

} // namespace

TEST_CASE("Fuzzy skin with a valid point distance fuzzes, painted or not", "[FuzzySkin][ConfigRange]")
{
    // The controls: they show the measure tells a fuzzed wall from a plain one.
    CHECK(zero_pd_slice_cube("disabled_fuzzy", "0.8", false) < 20);
    CHECK(zero_pd_slice_cube("external", "0.8", false) > 50);
    CHECK(zero_pd_slice_cube("none", "0.8", true) > 50);
    // Disabled ignores painting (Orca #10312).
    CHECK(zero_pd_slice_cube("disabled_fuzzy", "0.8", true) < 20);
}

TEST_CASE("A zero fuzzy skin point distance slices without fuzz instead of hanging", "[FuzzySkin][ConfigRange]")
{
    CHECK(zero_pd_slice_cube("external", "0", false) < 20);
    CHECK(zero_pd_slice_cube("allwalls", "0", false) < 20);
    // The out-of-memory case: painted fuzzy skin over a "None" object.
    CHECK(zero_pd_slice_cube("none", "0", true) < 20);
    CHECK(zero_pd_slice_cube("none", "0.005", true) < 20);
}
