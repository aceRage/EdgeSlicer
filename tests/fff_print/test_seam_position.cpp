// Seam Left/Right - the gate for docs/superpowers/specs/2026-09-06-seam-left-right.md.
//
// "Back" (spRear) pulls the seam of every loop toward the largest Y. Auxiliary part-cooling fans
// usually sit on the side of the machine rather than behind it, so Left and Right do the same thing
// along X: Left prefers the smallest X, Right the largest. All three are one code path in
// SeamComparator, parameterised by an axis and a sign, so this file holds them to the same rule:
//
//  * On a cylinder - a shape with exactly one extreme point per direction and no sharp corner to
//    distract the comparator - back lands at max Y, left at min X, right at max X.
//  * The seam is in the same place on every layer (that is the alignment doing its job), so the
//    spread along the chosen axis is a fraction of a millimetre.
//  * Left and Right are mirrors: their seams sit at opposite ends of the same X extent.

#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
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
// tests/CLAUDE.md: floating point comparisons go through the matchers, never through Approx.
using Catch::Matchers::WithinAbs;

namespace {

// Where the seams ended up, in unscaled object coordinates. The object is centred on the origin, so
// these are signed offsets from its middle - which is what "toward that side of the bed" means once
// the instance transform is undone.
struct SeamCloud
{
    std::vector<Vec2d> points;

    double min_x() const { return extreme(0, false); }
    double max_x() const { return extreme(0, true); }
    double min_y() const { return extreme(1, false); }
    double max_y() const { return extreme(1, true); }

    double spread(int axis) const { return extreme(axis, true) - extreme(axis, false); }

private:
    double extreme(int axis, bool largest) const
    {
        double best = largest ? -std::numeric_limits<double>::max() : std::numeric_limits<double>::max();
        for (const Vec2d &p : points)
            best = largest ? std::max(best, p[axis]) : std::min(best, p[axis]);
        return best;
    }
};

void collect_outer_loops(const ExtrusionEntity *entity, std::vector<const ExtrusionLoop *> &out)
{
    if (entity->is_collection()) {
        for (const ExtrusionEntity *child : static_cast<const ExtrusionEntityCollection *>(entity)->entities)
            collect_outer_loops(child, out);
        return;
    }
    if (entity->is_loop() && entity->role() == erExternalPerimeter)
        out.push_back(static_cast<const ExtrusionLoop *>(entity));
}

DynamicPrintConfig cylinder_config(const std::string &seam_position)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "seam_position",              seam_position },
        { "wall_loops",                 "2" },
        { "layer_height",               "0.2" },
        { "initial_layer_print_height", "0.2" },
        // A plain tube: nothing on the inside for the seam logic to trip over, and it slices fast.
        { "top_shell_layers",           "0" },
        { "bottom_shell_layers",        "0" },
        { "sparse_infill_density",      "0%" },
        { "enable_support",             "0" },
        { "spiral_mode",                "0" },
        { "staggered_inner_seams",      "0" },
    });
    return config;
}

// Slice a 20 mm diameter, 10 mm tall cylinder and ask the SeamPlacer where the seam of every outer
// wall loop would go. Going through place_seam rather than the exported G-code keeps seam_gap, the
// scarf joint and the travel moves out of the measurement.
SeamCloud seams_for(const std::string &seam_position)
{
    Slic3r::Print print;
    Slic3r::Model model;

    DynamicPrintConfig config = cylinder_config(seam_position);

    ModelObject *object = model.add_object();
    object->name = "cylinder20";
    object->add_volume(Slic3r::make_cylinder(10., 10.));
    object->add_instance();
    object->ensure_on_bed();
    print.auto_assign_extruders(model.objects.front());
    print.apply(model, config);
    print.set_status_silent();
    print.process();

    SeamPlacer placer;
    placer.init(print, []() {});

    SeamCloud cloud;
    for (const PrintObject *po : print.objects()) {
        const size_t raft_layers = po->slicing_parameters().raft_layers();
        for (const Layer *layer : po->layers()) {
            if (layer->id() < raft_layers)
                continue;
            for (const LayerRegion *region : layer->regions()) {
                std::vector<const ExtrusionLoop *> loops;
                collect_outer_loops(&region->perimeters, loops);
                for (const ExtrusionLoop *source : loops) {
                    ExtrusionLoop loop     = *source;
                    float         overhang = 0.f;
                    placer.place_seam(layer, loop, Point(0, 0), overhang);
                    cloud.points.push_back(unscale(loop.first_point()));
                }
            }
        }
    }
    return cloud;
}

// The outer wall of a 20 mm cylinder runs at roughly 9.8 mm from the axis. These bounds only need to
// tell one side of the tube from the other three, so they are deliberately slack.
constexpr double on_the_far_side = 8.0; // a seam pushed all the way to one side clears this
constexpr double near_the_middle = 3.0; // ... and the other coordinate stays near zero

} // namespace

SCENARIO("Seam position Back, Left and Right each pull the seam to their own side", "[Seam]")
{
    GIVEN("a cylinder sliced with seam_position = back")
    {
        SeamCloud cloud = seams_for("back");
        THEN("there is a seam to look at on every layer")
        {
            REQUIRE(cloud.points.size() >= 40);
        }
        THEN("every seam sits at the back of the tube")
        {
            REQUIRE(cloud.min_y() > on_the_far_side);
            REQUIRE(std::abs(cloud.min_x()) < near_the_middle);
            REQUIRE(std::abs(cloud.max_x()) < near_the_middle);
        }
        THEN("the seams line up from layer to layer")
        {
            REQUIRE(cloud.spread(1) < 0.5);
        }
    }

    GIVEN("a cylinder sliced with seam_position = left")
    {
        SeamCloud cloud = seams_for("left");
        THEN("there is a seam to look at on every layer")
        {
            REQUIRE(cloud.points.size() >= 40);
        }
        THEN("every seam sits at the left of the tube")
        {
            REQUIRE(cloud.max_x() < -on_the_far_side);
            REQUIRE(std::abs(cloud.min_y()) < near_the_middle);
            REQUIRE(std::abs(cloud.max_y()) < near_the_middle);
        }
        THEN("the seams line up from layer to layer")
        {
            REQUIRE(cloud.spread(0) < 0.5);
        }
    }

    GIVEN("a cylinder sliced with seam_position = right")
    {
        SeamCloud cloud = seams_for("right");
        THEN("there is a seam to look at on every layer")
        {
            REQUIRE(cloud.points.size() >= 40);
        }
        THEN("every seam sits at the right of the tube")
        {
            REQUIRE(cloud.min_x() > on_the_far_side);
            REQUIRE(std::abs(cloud.min_y()) < near_the_middle);
            REQUIRE(std::abs(cloud.max_y()) < near_the_middle);
        }
        THEN("the seams line up from layer to layer")
        {
            REQUIRE(cloud.spread(0) < 0.5);
        }
    }

    GIVEN("the same cylinder sliced left and right")
    {
        SeamCloud left  = seams_for("left");
        SeamCloud right = seams_for("right");
        THEN("the two are mirrors of each other across the axis")
        {
            REQUIRE_THAT(left.min_x(), WithinAbs(-right.max_x(), 0.5));
            REQUIRE_THAT(left.max_x(), WithinAbs(-right.min_x(), 0.5));
        }
    }
}

SCENARIO("The Left and Right seam keys survive a round trip through the config", "[Seam]")
{
    GIVEN("a print config")
    {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        THEN("left and right deserialise to the new enum values")
        {
            config.set_deserialize_strict({ { "seam_position", "left" } });
            REQUIRE(config.opt_enum<SeamPosition>("seam_position") == spLeft);
            REQUIRE(config.opt_serialize("seam_position") == "left");

            config.set_deserialize_strict({ { "seam_position", "right" } });
            REQUIRE(config.opt_enum<SeamPosition>("seam_position") == spRight);
            REQUIRE(config.opt_serialize("seam_position") == "right");
        }
        THEN("the values that shipped before keep their numbers, so old projects still load")
        {
            REQUIRE(int(spNearest) == 0);
            REQUIRE(int(spAligned) == 1);
            REQUIRE(int(spAlignedBack) == 2);
            REQUIRE(int(spRear) == 3);
            REQUIRE(int(spRandom) == 4);
            REQUIRE(int(spLeft) == 5);
            REQUIRE(int(spRight) == 6);
        }
    }
}
