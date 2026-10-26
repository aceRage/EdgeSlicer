// Perimeter generator tests ported from upstream OrcaSlicer's tests/fff_print/test_perimeters.cpp
// (Catch2 v3 there, v2 here). Only the cases of the ported PRs are here; upstream's other cases test
// features this tree does not have yet.

#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Polygon.hpp"
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

// ---------------------------------------------------------------------------------------------------
// Orca #16421 (partial): Arachne odd-line order, shared-centre-line island, Outer-only brim.
// Classic is_thin_wall_hole / is_narrow_wall_hole is not ported (needs #12669/#16051).
// ---------------------------------------------------------------------------------------------------
namespace {

const double tube_radius = 11.5;

DynamicPrintConfig tube_config(const char *wall_generator, int wall_loops)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "wall_generator",             wall_generator },
        { "wall_loops",                 wall_loops },
        { "wall_sequence",              "inner wall/outer wall" },
        { "wall_direction",             "ccw" },
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        { "nozzle_diameter",            "0.4" },
        { "outer_wall_line_width",      0.5 },
        { "inner_wall_line_width",      0.5 },
        { "precise_outer_wall",         true },
        { "detect_thin_wall",           false },
        { "only_one_wall_top",          false },
        { "only_one_wall_first_layer",  false },
        { "brim_type",                  "no_brim" },
        { "detect_overhang_wall",       true },
        { "overhang_reverse",           false },
        { "overhang_reverse_threshold", "0%" },
    });
    return config;
}

Print &round_tube(Print &print, Model &model, const DynamicPrintConfig &config, double wall)
{
    ModelObject *object = model.add_object();
    object->name = "round_tube.stl";
    object->add_volume(make_cylinder(tube_radius, 2.), ModelVolumeType::MODEL_PART, false);
    object->add_volume(make_cylinder(tube_radius - wall, 2.), ModelVolumeType::NEGATIVE_VOLUME, false);
    object->add_instance();
    object->ensure_on_bed();

    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();
    return print;
}

enum class TubeSide { Contour, Hole, Centre };

struct TubeLoop {
    TubeSide side;
    int      inset;
    bool     ccw;
};

std::vector<std::vector<TubeLoop>> tube_loops(const Print &print, double wall)
{
    const double centre = tube_radius - 0.5 * wall;
    std::vector<std::vector<TubeLoop>> out;
    for (const Layer *layer : print.objects().front()->layers()) {
        const Vec2d axis = get_extents(layer->lslices).center().cast<double>();
        std::vector<TubeLoop> &loops = out.emplace_back();
        for (const LayerRegion *region : layer->regions())
            for (const ExtrusionEntity *entity : region->perimeters.flatten().entities) {
                if (! entity->is_loop())
                    continue;
                const ExtrusionLoop *loop    = static_cast<const ExtrusionLoop*>(entity);
                const Polygon        polygon = loop->polygon();
                double               radius  = 0.;
                for (const Point &p : polygon.points)
                    radius += unscale<double>((p.cast<double>() - axis).norm());
                radius /= double(polygon.points.size());
                const TubeSide side = radius > centre + 0.1 ? TubeSide::Contour : radius < centre - 0.1 ? TubeSide::Hole : TubeSide::Centre;
                loops.push_back({ side, loop->inset_idx, polygon.is_counter_clockwise() });
            }
    }
    return out;
}

std::vector<int> wall_sequence_insets(const std::string &wall_sequence, bool first_layer, int deepest)
{
    std::vector<int> insets;
    if (wall_sequence == "inner wall/outer wall" || (wall_sequence == "inner-outer-inner wall" && first_layer)) {
        for (int inset = deepest; inset >= 0; -- inset)
            insets.push_back(inset);
    } else if (wall_sequence == "outer wall/inner wall" || deepest < 2) {
        for (int inset = 0; inset <= deepest; ++ inset)
            insets.push_back(inset);
    } else {
        for (int inset = deepest; inset >= 2; -- inset)
            insets.push_back(inset);
        insets.push_back(0);
        insets.push_back(1);
    }
    return insets;
}

std::vector<int> side_insets(const std::vector<TubeLoop> &loops, TubeSide side)
{
    std::vector<int> insets;
    for (const TubeLoop &loop : loops)
        if (loop.side == side || loop.side == TubeSide::Centre)
            insets.push_back(loop.inset);
    return insets;
}

std::string wall_order(const std::vector<TubeLoop> &loops)
{
    std::string out;
    for (const TubeLoop &loop : loops)
        out += std::string(out.empty() ? "" : " ") + (loop.side == TubeSide::Contour ? "c" : loop.side == TubeSide::Hole ? "h" : "m") +
               std::to_string(loop.inset);
    return out;
}

} // namespace

// Classic Inner/Outer/Inner is still asserted here for 3+ walls. The skipped #12669 thin/narrow
// hole reorder is not involved: each side is checked independently via side_insets().
TEST_CASE("Each side of a tube wall prints its walls in the wall sequence order", "[Perimeters]")
{
    const double wall = GENERATE(1.5, 2.0, 2.5, 3.0, 4.0);
    int wall_loops = 3;
    int arachne_walls = 3;
    int classic_walls = 3;
    if (wall == 1.5) { wall_loops = 3; arachne_walls = 2; classic_walls = 1; }
    else if (wall == 2.0) { wall_loops = 2; arachne_walls = 2; classic_walls = 2; }
    else if (wall == 2.5) { wall_loops = 3; arachne_walls = 3; classic_walls = 2; }
    else if (wall == 3.0) { wall_loops = 3; arachne_walls = 3; classic_walls = 3; }
    else { wall_loops = 4; arachne_walls = 4; classic_walls = 4; }
    const char       *wall_generator = GENERATE("classic", "arachne");
    const std::string wall_sequence  = GENERATE("inner wall/outer wall", "outer wall/inner wall", "inner-outer-inner wall");
    CAPTURE(wall, wall_loops, wall_generator, wall_sequence);

    DynamicPrintConfig config = tube_config(wall_generator, wall_loops);
    config.set_deserialize_strict("wall_sequence", wall_sequence);
    Print print;
    Model model;
    round_tube(print, model, config, wall);
    const std::vector<std::vector<TubeLoop>> layers = tube_loops(print, wall);
    REQUIRE(layers.size() == 10);

    const int walls = std::string(wall_generator) == "arachne" ? arachne_walls : classic_walls;
    for (size_t i = 0; i < layers.size(); ++ i) {
        for (const TubeSide side : { TubeSide::Contour, TubeSide::Hole }) {
            const bool contour_side = side == TubeSide::Contour;
            CAPTURE(i, contour_side);
            const std::vector<int> insets = side_insets(layers[i], side);
            REQUIRE(int(insets.size()) == walls);
            CHECK(insets == wall_sequence_insets(wall_sequence, i == 0, walls - 1));
        }
    }
}

TEST_CASE("Inner/Outer/Inner prints the hole of a narrow tube wall first and the middle last", "[Perimeters]")
{
    // Classic's is_narrow_wall_hole path is not ported (needs #12669/#16051). Arachne's shared
    // centre-line island is.
    const char *wall_generator = "arachne";
    CAPTURE(wall_generator);

    DynamicPrintConfig config = tube_config(wall_generator, 3);
    config.set_deserialize_strict("wall_sequence", "inner-outer-inner wall");
    Print print;
    Model model;
    round_tube(print, model, config, 1.5);
    const std::vector<std::vector<TubeLoop>> layers = tube_loops(print, 1.5);
    REQUIRE(layers.size() == 10);

    for (size_t i = 1; i < layers.size(); ++ i) {
        CAPTURE(i);
        CHECK(wall_order(layers[i]) == "h0 c0 m1");
    }
}

TEST_CASE("Tube walls including holes run in the wall direction", "[Perimeters]")
{
    // Edge make_counter_clockwise()s every loop (holes included) and does not reverse holes against
    // the wall direction (that needs #12669). Forced ccw skips reorient; forced cw clockwise()s all.
    const double      wall           = GENERATE(1.5, 2.0, 3.0);
    const char       *wall_generator = GENERATE("classic", "arachne");
    const std::string wall_direction = GENERATE("ccw", "cw");
    CAPTURE(wall, wall_generator, wall_direction);

    DynamicPrintConfig config = tube_config(wall_generator, 3);
    config.set_deserialize_strict("wall_direction", wall_direction);
    Print print;
    Model model;
    round_tube(print, model, config, wall);
    const std::vector<std::vector<TubeLoop>> layers = tube_loops(print, wall);
    REQUIRE(layers.size() == 10);

    const bool expect_ccw = wall_direction == "ccw";
    for (size_t i = 0; i < layers.size(); ++ i) {
        CAPTURE(i);
        REQUIRE(std::any_of(layers[i].begin(), layers[i].end(), [](const TubeLoop &loop) { return loop.side == TubeSide::Contour; }));
        REQUIRE(std::any_of(layers[i].begin(), layers[i].end(), [](const TubeLoop &loop) { return loop.side == TubeSide::Hole; }));
        for (const TubeLoop &loop : layers[i]) {
            CAPTURE(int(loop.side), loop.inset);
            CHECK(loop.ccw == expect_ccw);
        }
    }
}

TEST_CASE("Reverse on even reverses the tube walls of every other layer", "[Perimeters]")
{
    // Reverse on even only reorients when wall direction is Auto. Forced ccw/cw skip that path.
    const double      wall                  = GENERATE(1.5, 3.0);
    const char       *wall_generator        = GENERATE("classic", "arachne");
    const bool        reverse_internal_only = GENERATE(false, true);
    const std::string wall_direction        = GENERATE("auto", "ccw", "cw");
    CAPTURE(wall, wall_generator, reverse_internal_only, wall_direction);

    auto slice = [&](bool overhang_reverse) {
        DynamicPrintConfig config = tube_config(wall_generator, 3);
        config.set_deserialize_strict({
            { "wall_direction",                 wall_direction },
            { "overhang_reverse",               overhang_reverse },
            { "overhang_reverse_internal_only", reverse_internal_only },
        });
        Print print;
        Model model;
        round_tube(print, model, config, wall);
        return tube_loops(print, wall);
    };
    const std::vector<std::vector<TubeLoop>> plain    = slice(false);
    const std::vector<std::vector<TubeLoop>> reversed = slice(true);
    REQUIRE(plain.size() == 10);
    REQUIRE(reversed.size() == plain.size());

    const bool reverse_on_auto = wall_direction == "auto";
    for (size_t i = 0; i < plain.size(); ++ i) {
        CAPTURE(i);
        REQUIRE(reversed[i].size() == plain[i].size());
        for (size_t j = 0; j < plain[i].size(); ++ j) {
            CAPTURE(j);
            CHECK(reversed[i][j].side == plain[i][j].side);
            CHECK(reversed[i][j].inset == plain[i][j].inset);
            const bool flips = reverse_on_auto && i % 2 == 1 && (! reverse_internal_only || plain[i][j].inset > 0);
            CHECK(reversed[i][j].ccw == (plain[i][j].ccw != flips));
        }
    }
}

namespace {

DynamicPrintConfig brim_tube_config(const char *wall_generator, const std::string &wall_sequence, const char *brim_type, double brim_width)
{
    DynamicPrintConfig config = tube_config(wall_generator, 3);
    config.set_deserialize_strict({
        { "wall_sequence", wall_sequence },
        { "brim_type",     brim_type },
        { "brim_width",    brim_width },
    });
    return config;
}

} // namespace

TEST_CASE("A brim against the outer walls makes the first layer print them first", "[Perimeters]")
{
    // Edge has no btLeadingEdgeOnly. Only Outer only with a width above zero puts outer walls first.
    const char  *brim_type          = GENERATE("no_brim", "inner_only", "auto_brim", "outer_only", "outer_and_inner");
    const double brim_width         = GENERATE(0., 5.);
    const bool   outer_walls_first  = std::string(brim_type) == "outer_only" && brim_width > 0.;
    const char  *wall_generator     = GENERATE("classic", "arachne");
    const std::string wall_sequence = GENERATE("inner wall/outer wall", "inner-outer-inner wall");
    CAPTURE(brim_type, brim_width, wall_generator, wall_sequence);

    Print print;
    Model model;
    round_tube(print, model, brim_tube_config(wall_generator, wall_sequence, brim_type, brim_width), 3.);
    const std::vector<std::vector<TubeLoop>> layers = tube_loops(print, 3.);
    REQUIRE(layers.size() == 10);

    for (const size_t i : { size_t(0), size_t(1) }) {
        const bool first = outer_walls_first && i == 0;
        for (const TubeSide side : { TubeSide::Contour, TubeSide::Hole }) {
            const bool contour_side = side == TubeSide::Contour;
            CAPTURE(i, contour_side);
            const std::vector<int> insets = side_insets(layers[i], side);
            REQUIRE(insets.size() == 3);
            CHECK(insets == wall_sequence_insets(first ? "outer wall/inner wall" : wall_sequence, i == 0, 2));
        }
    }
}

TEST_CASE("Changing the brim updates the wall order of the first layer", "[Perimeters]")
{
    const bool   widen          = GENERATE(false, true);
    const char  *from_type      = "outer_only";
    const double from_width     = widen ? 0. : 5.;
    const char  *to_type        = "outer_only";
    const double to_width       = widen ? 5. : 0.;
    const char  *wall_generator = GENERATE("classic", "arachne");
    CAPTURE(from_width, to_width, wall_generator);

    const std::string wall_sequence = "inner wall/outer wall";
    Print print;
    Model model;
    round_tube(print, model, brim_tube_config(wall_generator, wall_sequence, from_type, from_width), 3.);
    const std::string before = wall_order(tube_loops(print, 3.).front());
    print.apply(model, brim_tube_config(wall_generator, wall_sequence, to_type, to_width));
    print.process();
    const std::string after = wall_order(tube_loops(print, 3.).front());

    Print fresh_print;
    Model fresh_model;
    round_tube(fresh_print, fresh_model, brim_tube_config(wall_generator, wall_sequence, to_type, to_width), 3.);
    const std::string fresh = wall_order(tube_loops(fresh_print, 3.).front());

    REQUIRE(before != fresh);
    CHECK(after == fresh);
}

// ---------------------------------------------------------------------------------------------------
// Orca #14296 / #14929: top surface expansion and the one-wall options gated on their shells.
// ---------------------------------------------------------------------------------------------------
namespace one_wall_top_14929 {

// The layer at this Z is the last one of the base, so its top surface is the ledge.
const double ledge_z = 5.0;

// The first layer, at initial_layer_print_height.
const double first_layer_z = 0.2;

// TestMesh::step scaled 3x in X/Y: a 60x60x5 base carrying a 54x54 column up to z=10, leaving a 3mm
// top ledge around a feature that keeps rising. That is the geometry both only_one_wall_top and the
// top surface expansion act on. The ledge has to stay wider than the wall band plus two top-infill
// lines, or the expansion discards it as a sliver and the tests below assert nothing.
TriangleMesh step_with_ledge()
{
    TriangleMesh m = Slic3r::Test::mesh(TestMesh::step);
    m.scale(Vec3f(3.f, 3.f, 1.f));
    return m;
}

// Every setting the assertions depend on, so none of them rests on a default.
DynamicPrintConfig base_config(const char *wall_generator)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "wall_generator",             wall_generator },
        { "layer_height",               0.2 },  // puts a layer boundary exactly on ledge_z
        { "initial_layer_print_height", 0.2 },
        { "wall_loops",                 3 },
        { "sparse_infill_density",      "15%" },
        { "top_shell_layers",           3 },
        { "bottom_shell_layers",        3 },
        { "top_surface_density",        "100%" },
        { "top_surface_expansion",      0.0 },
        { "only_one_wall_top",          false },
        { "only_one_wall_first_layer",  false },
        // Do not let the one-wall threshold discard the 3mm ledge before the feature sees it.
        { "min_width_top_surface",      0.0 },
    });
    return config;
}

double collection_length(const ExtrusionEntityCollection &coll)
{
    double len = 0.;
    for (const ExtrusionEntity *entity : coll.flatten().entities)
        if (! entity->is_collection())
            len += entity->length();
    return len;
}

// Extruded length per layer. Two slices are compared through this rather than through their G-code,
// because the G-code carries a config block that differs whenever any setting differs.
struct SliceLengths {
    std::vector<double> perimeters;
    std::vector<double> fills;
};

SliceLengths slice_lengths(const Print &print)
{
    SliceLengths out;
    for (const Layer *layer : print.objects().front()->layers()) {
        double perimeters = 0., fills = 0.;
        for (const LayerRegion *region : layer->regions()) {
            perimeters += collection_length(region->perimeters);
            fills      += collection_length(region->fills);
        }
        out.perimeters.push_back(perimeters);
        out.fills.push_back(fills);
    }
    return out;
}

double perimeter_length_at(const Print &print, double print_z)
{
    for (const Layer *layer : print.objects().front()->layers())
        if (std::abs(layer->print_z - print_z) < 1e-4) {
            double len = 0.;
            for (const LayerRegion *region : layer->regions())
                len += collection_length(region->perimeters);
            return len;
        }
    return 0.;
}

// Largest per-layer difference between two series; a negative result means they are not comparable.
double max_difference(const std::vector<double> &a, const std::vector<double> &b)
{
    if (a.size() != b.size() || a.empty())
        return -1.;
    double worst = 0.;
    for (size_t i = 0; i < a.size(); ++ i)
        worst = std::max(worst, std::abs(a[i] - b[i]));
    return worst;
}

} // namespace one_wall_top_14929

// The expansion only retypes area as top solid infill, so it can do nothing where there is no top
// fill to begin with: zero top shell layers retypes the top surfaces as internal, and a top surface
// density of 0% leaves the top layer with walls only. The last section is the control - the same
// expansion on the same model does change the slice once a top fill exists - without which the two
// equality checks above it would hold for an unrelated reason.
TEST_CASE("Top surface expansion only acts where there is a top fill", "[Perimeters][TopSurfaceExpansion]")
{
    using namespace one_wall_top_14929;
    const char *wall_generator = GENERATE("classic", "arachne");
    CAPTURE(wall_generator);

    auto lengths_for = [wall_generator](int top_shell_layers, const char *top_surface_density, double expansion) {
        DynamicPrintConfig config = base_config(wall_generator);
        config.set_deserialize_strict({
            { "top_shell_layers",      top_shell_layers },
            { "top_surface_density",   top_surface_density },
            { "top_surface_expansion", expansion },
        });
        Print print;
        init_and_process_print({ step_with_ledge() }, print, config);
        REQUIRE_FALSE(print.objects().empty());
        return slice_lengths(print);
    };

    SECTION("no top shell layers") {
        const SliceLengths off = lengths_for(0, "100%", 0.0);
        const SliceLengths on  = lengths_for(0, "100%", 2.0);
        REQUIRE(off.perimeters.size() == on.perimeters.size());
        CHECK_THAT(max_difference(off.perimeters, on.perimeters), Catch::Matchers::WithinAbs(0., 1.0));
        CHECK_THAT(max_difference(off.fills,      on.fills),      Catch::Matchers::WithinAbs(0., 1.0));
    }

    SECTION("zero top surface density") {
        const SliceLengths off = lengths_for(3, "0%", 0.0);
        const SliceLengths on  = lengths_for(3, "0%", 2.0);
        REQUIRE(off.perimeters.size() == on.perimeters.size());
        CHECK_THAT(max_difference(off.perimeters, on.perimeters), Catch::Matchers::WithinAbs(0., 1.0));
        CHECK_THAT(max_difference(off.fills,      on.fills),      Catch::Matchers::WithinAbs(0., 1.0));
    }

    SECTION("with a top fill the same expansion does change the slice") {
        const SliceLengths off = lengths_for(3, "100%", 0.0);
        const SliceLengths on  = lengths_for(3, "100%", 2.0);
        REQUIRE(off.fills.size() == on.fills.size());
        CHECK(max_difference(off.fills, on.fills) > scale_(0.5));
    }
}

// With no top shell the top surfaces are retyped as internal, so the top surface density has nothing
// left to control: there is no top fill, and only_one_wall_top - the one route from the density to the
// perimeters - is itself switched off for want of a top surface to act on.
TEST_CASE("Top surface density does not affect a slice without a top shell", "[Perimeters][TopSurfaceExpansion]")
{
    using namespace one_wall_top_14929;
    const char *wall_generator = GENERATE("classic", "arachne");
    CAPTURE(wall_generator);

    auto lengths_for = [wall_generator](const char *top_surface_density) {
        DynamicPrintConfig config = base_config(wall_generator);
        config.set_deserialize_strict({
            { "top_shell_layers",    0 },
            { "only_one_wall_top",   true },
            { "top_surface_density", top_surface_density },
        });
        Print print;
        init_and_process_print({ step_with_ledge() }, print, config);
        REQUIRE_FALSE(print.objects().empty());
        return slice_lengths(print);
    };

    const SliceLengths solid = lengths_for("100%");
    const SliceLengths none  = lengths_for("0%");
    REQUIRE(solid.perimeters.size() == none.perimeters.size());
    CHECK_THAT(max_difference(solid.perimeters, none.perimeters), Catch::Matchers::WithinAbs(0., 1.0));
    CHECK_THAT(max_difference(solid.fills,      none.fills),      Catch::Matchers::WithinAbs(0., 1.0));
}

// On the ledge layer the inner walls are given up to the top fill, so that layer loses wall length.
// The handover needs a top fill that reaches the freed space: at a top surface density of 0% there is
// no top fill at all, and without top_surface_expansion the fill never grows over the walls. Either
// way the feature still runs, through the original generation, which keeps the inner walls up to the
// top boundary - putting that layer back between the plain and the one-wall slice.
TEST_CASE("Only one wall on top surfaces drops inner walls only where a top fill replaces them", "[Perimeters][TopSurfaceExpansion]")
{
    using namespace one_wall_top_14929;
    const char *wall_generator = GENERATE("classic", "arachne");
    CAPTURE(wall_generator);

    auto ledge_perimeters_for = [wall_generator](bool only_one_wall_top, const char *top_surface_density, double expansion) {
        DynamicPrintConfig config = base_config(wall_generator);
        config.set_deserialize_strict({
            { "only_one_wall_top",     only_one_wall_top },
            { "top_surface_density",   top_surface_density },
            { "top_surface_expansion", expansion },
        });
        Print print;
        init_and_process_print({ step_with_ledge() }, print, config);
        REQUIRE_FALSE(print.objects().empty());
        return perimeter_length_at(print, ledge_z);
    };

    const double plain              = ledge_perimeters_for(false, "100%", 2.0);
    const double one_wall           = ledge_perimeters_for(true,  "100%", 2.0);
    const double one_wall_no_fill   = ledge_perimeters_for(true,  "0%",   2.0);
    const double one_wall_no_expand = ledge_perimeters_for(true,  "100%", 0.0);

    REQUIRE(plain > 0.);
    CHECK(one_wall < plain);
    // Both fall back to the original generation, which cuts the walls back to the top boundary but not past it.
    CHECK(one_wall_no_fill > one_wall);
    CHECK(one_wall_no_fill < plain);
    CHECK(one_wall_no_expand > one_wall);
    CHECK(one_wall_no_expand < plain);
}

// The bottom counterpart: the first layer is thinned to a single wall only where a bottom shell fills the
// space behind it. With no bottom shell layers the bottom surfaces are retyped as internal, so that wall
// would ring sparse infill on the bed - the option is switched off instead, and the GUI hides it in that
// state so a profile that left it enabled cannot act behind a hidden checkbox.
TEST_CASE("Only one wall on the first layer needs a bottom shell", "[Perimeters][TopSurfaceExpansion]")
{
    using namespace one_wall_top_14929;
    const char *wall_generator = GENERATE("classic", "arachne");
    CAPTURE(wall_generator);

    auto first_layer_perimeters_for = [wall_generator](bool only_one_wall_first_layer, int bottom_shell_layers) {
        DynamicPrintConfig config = base_config(wall_generator);
        config.set_deserialize_strict({
            { "only_one_wall_first_layer", only_one_wall_first_layer },
            { "bottom_shell_layers",       bottom_shell_layers },
        });
        Print print;
        init_and_process_print({ step_with_ledge() }, print, config);
        REQUIRE_FALSE(print.objects().empty());
        return perimeter_length_at(print, first_layer_z);
    };

    const double plain             = first_layer_perimeters_for(false, 3);
    const double one_wall          = first_layer_perimeters_for(true,  3);
    // Both at zero bottom shell layers, so everything else that setting changes cancels out between them.
    const double plain_no_shell    = first_layer_perimeters_for(false, 0);
    const double one_wall_no_shell = first_layer_perimeters_for(true,  0);

    REQUIRE(plain > 0.);
    CHECK(one_wall < plain);
    // No bottom shell: the option is inert, down to the same walls an unchecked box gives.
    CHECK_THAT(one_wall_no_shell, Catch::Matchers::WithinAbs(plain_no_shell, 1.0));
}

// EdgeSlicer: the inward / outward expansion directions are upstream code with no upstream test. Inward
// only fills the holes of the top surface, so it can never add more top fill than both directions do.
TEST_CASE("Top surface expansion inward adds no more top fill than inward and outward", "[Perimeters][TopSurfaceExpansion]")
{
    using namespace one_wall_top_14929;
    auto ledge_fill_for = [](const char *direction) {
        DynamicPrintConfig config = base_config("arachne");
        config.set_deserialize_strict({
            { "top_surface_expansion",           2.0 },
            { "top_surface_expansion_direction", direction },
        });
        Print print;
        init_and_process_print({ step_with_ledge() }, print, config);
        REQUIRE_FALSE(print.objects().empty());
        double fills = 0.;
        for (const Layer *layer : print.objects().front()->layers())
            if (std::abs(layer->print_z - ledge_z) < 1e-4)
                for (const LayerRegion *region : layer->regions())
                    fills += collection_length(region->fills);
        return fills;
    };
    const double both   = ledge_fill_for("inward_and_outward");
    const double inward = ledge_fill_for("inward");
    REQUIRE(both > 0.);
    CHECK(inward > 0.);
    // Fill lengths are not strictly monotone in area (line spacing is re-fitted per region), hence the 1%.
    CHECK(inward <= both * 1.01);
}
