// Ultra (offset layers) - the gate for docs/superpowers/specs/2026-09-06-offset-layers-classic.md.
//
// "Offset layers" raises every odd-numbered wall by half a layer height so the walls of successive
// layers interlock. It shipped on the Arachne wall generator only; these cases lock the rule for
// both generators, and the OFF case is the hard gate:
//
//  * OFF: with offset_layers off, no ExtrusionPath anywhere in the perimeters may carry a z_offset
//    or an extrusion multiplier - i.e. the generator is the one that shipped.
//  * ON, classic and Arachne alike: odd walls carry z_offset 0.5 (half a layer), layer 1 carries
//    an extrusion multiplier of 1.5, the second-to-last layer 0.5 with NO raise, even walls stay
//    flat at multiplier 1, and within an island every flat wall is extruded before every raised
//    one.
//  * Thin walls and gap fill have no wall index and are never raised.

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
// tests/CLAUDE.md: floating point comparisons go through the matchers, never through Approx.
using Catch::Matchers::WithinAbs;

namespace {

// One wall of one island, with the two fields the feature writes on each of its paths.
struct WallRec
{
    int                inset_idx = -1;
    std::vector<float> z_offsets;
    std::vector<float> multipliers;
};

void gather_paths(const ExtrusionEntity *entity, WallRec &rec)
{
    if (entity->is_collection()) {
        for (const ExtrusionEntity *child : static_cast<const ExtrusionEntityCollection *>(entity)->entities)
            gather_paths(child, rec);
        return;
    }
    auto take = [&rec](const ExtrusionPaths &paths) {
        for (const ExtrusionPath &path : paths) {
            rec.z_offsets.push_back(path.z_offset);
            rec.multipliers.push_back(path.extrusion_multiplier);
        }
    };
    if (entity->is_loop()) {
        take(static_cast<const ExtrusionLoop *>(entity)->paths);
    } else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(entity)) {
        take(multi->paths);
    } else if (const auto *path = dynamic_cast<const ExtrusionPath *>(entity)) {
        rec.z_offsets.push_back(path->z_offset);
        rec.multipliers.push_back(path->extrusion_multiplier);
    }
}

// The perimeters of one LayerRegion, grouped the way the generator appends them: one collection per
// island, holding that island's walls in extrusion order. The wall index is read off the owning
// entity, which both generators set (the classic one only ever tags the paths of the walls it
// raises).
std::vector<std::vector<WallRec>> walls_by_island(const ExtrusionEntityCollection &perimeters)
{
    std::vector<std::vector<WallRec>> islands;
    for (const ExtrusionEntity *island : perimeters.entities) {
        std::vector<WallRec>       walls;
        ExtrusionEntitiesPtr       one{ const_cast<ExtrusionEntity *>(island) };
        const ExtrusionEntitiesPtr &members = island->is_collection() ?
            static_cast<const ExtrusionEntityCollection *>(island)->entities : one;
        for (const ExtrusionEntity *entity : members) {
            WallRec rec;
            rec.inset_idx = entity->inset_idx;
            gather_paths(entity, rec);
            if (! rec.z_offsets.empty())
                walls.push_back(std::move(rec));
        }
        if (! walls.empty())
            islands.push_back(std::move(walls));
    }
    return islands;
}

DynamicPrintConfig cube_config(const std::string &wall_generator, bool offset_layers)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "wall_generator",             wall_generator },
        { "offset_layers",              offset_layers ? "1" : "0" },
        { "wall_loops",                 "3" },
        { "layer_height",               "0.2" },
        { "initial_layer_print_height", "0.2" },
        // The feature's own prerequisites, so the configuration under test is one the GUI would
        // actually let a user reach.
        { "top_surface_line_width",     "0.42" },
        { "outer_wall_line_width",      "0.42" },
        { "spiral_mode",                "0" },
        // A top layer that drops to a single wall would leave nothing odd to check up there.
        { "only_one_wall_top",          "0" },
        { "sparse_infill_density",      "10%" },
        { "enable_support",             "0" },
    });
    return config;
}

// A 20 mm cube: 100 layers at 0.2 mm, three walls, and no geometry that could make the wall count
// wander from layer to layer.
void slice_cube(Slic3r::Print &print, Slic3r::Model &model, const DynamicPrintConfig &config)
{
    ModelObject *object = model.add_object();
    object->name = "cube20";
    object->add_volume(Slic3r::make_cube(20., 20., 20.));
    object->add_instance();
    object->ensure_on_bed();
    print.auto_assign_extruders(model.objects.front());
    print.apply(model, config);
    print.set_status_silent();
    print.process();
}

// What an odd wall must look like on the given layer of an object with `layer_count` layers.
float expected_z_offset(int layer_id, int layer_count)
{
    return layer_id == layer_count - 2 ? 0.f : 0.5f;
}

float expected_multiplier(int layer_id, int layer_count)
{
    if (layer_id == 1)
        return 1.5f;
    if (layer_id == layer_count - 2)
        return 0.5f;
    return 1.f;
}

// The whole check, run against both generators so the two are held to one rule.
void check_offset_pattern(const std::string &wall_generator)
{
    Slic3r::Print  print;
    Slic3r::Model  model;
    slice_cube(print, model, cube_config(wall_generator, true));

    const PrintObject *object      = print.objects().front();
    const int          layer_count = int(object->layer_count());
    REQUIRE(layer_count > 4);

    int raised_walls = 0, flat_walls = 0, checked_layers = 0;
    for (const Layer *layer : object->layers()) {
        const int layer_id = int(layer->id());
        for (const LayerRegion *region : layer->regions()) {
            for (const std::vector<WallRec> &island : walls_by_island(region->perimeters)) {
                bool seen_raised = false;
                for (const WallRec &wall : island) {
                    const bool odd = wall.inset_idx > 0 && (wall.inset_idx % 2) == 1;
                    const float want_z    = odd ? expected_z_offset(layer_id, layer_count) : 0.f;
                    const float want_mult = odd ? expected_multiplier(layer_id, layer_count) : 1.f;
                    for (float z : wall.z_offsets)
                        CHECK_THAT(z, WithinAbs(want_z, 1e-6));
                    for (float m : wall.multipliers)
                        CHECK_THAT(m, WithinAbs(want_mult, 1e-6));
                    // Flat walls first: once a raised wall has been laid down, no flat one may
                    // follow it inside the same island.
                    if (odd && want_z > 0.f)
                        seen_raised = true;
                    else if (seen_raised && want_z == 0.f && wall.inset_idx >= 0)
                        FAIL("flat wall extruded after a raised one, layer " << layer_id);
                    if (odd)
                        ++raised_walls;
                    else
                        ++flat_walls;
                }
            }
        }
        ++checked_layers;
    }
    // The cube has three walls on every layer, so both groups have to be non-empty or the loop
    // above proved nothing.
    CHECK(checked_layers == layer_count);
    CHECK(raised_walls > 0);
    CHECK(flat_walls > raised_walls);

    // Gap fill and thin walls carry no wall index and are never raised.
    for (const Layer *layer : object->layers())
        for (const LayerRegion *region : layer->regions()) {
            WallRec rec;
            gather_paths(&region->thin_fills, rec);
            for (float z : rec.z_offsets)
                CHECK_THAT(z, WithinAbs(0.f, 1e-6));
            for (float m : rec.multipliers)
                CHECK_THAT(m, WithinAbs(1.f, 1e-6));
        }
}

// The hard gate for one generator: nothing in the feature may touch a print that has it off.
void check_off_mode(const std::string &wall_generator)
{
    Slic3r::Print print;
    Slic3r::Model model;
    slice_cube(print, model, cube_config(wall_generator, false));

    int paths = 0;
    for (const Layer *layer : print.objects().front()->layers())
        for (const LayerRegion *region : layer->regions()) {
            WallRec rec;
            gather_paths(&region->perimeters, rec);
            gather_paths(&region->thin_fills, rec);
            for (float z : rec.z_offsets)
                CHECK_THAT(z, WithinAbs(0.f, 1e-6));
            for (float m : rec.multipliers)
                CHECK_THAT(m, WithinAbs(1.f, 1e-6));
            paths += int(rec.z_offsets.size());
        }
    CHECK(paths > 0);
}

} // namespace

SCENARIO("Offset layers: off leaves every classic path flat", "[OffsetLayers]")
{
    check_off_mode("classic");
}

SCENARIO("Offset layers: off leaves every Arachne path flat", "[OffsetLayers]")
{
    check_off_mode("arachne");
}

// The port: the classic generator has to produce what Arachne produces.
SCENARIO("Offset layers: the classic wall generator raises odd walls", "[OffsetLayers]")
{
    check_offset_pattern("classic");
}

// The behaviour that shipped, locked so the shared helper cannot drift.
SCENARIO("Offset layers: the Arachne wall generator raises odd walls", "[OffsetLayers]")
{
    check_offset_pattern("arachne");
}
