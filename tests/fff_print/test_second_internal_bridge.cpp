// The second internal bridge layer (enable_extra_bridge_layer) turns what of a layer's solid and sparse infill lies
// on the internal bridges of the layer beneath into bridge. It changes what an area is filled as and nothing else:
// every layer has as much to fill with the option on as with it off.

#include <catch2/catch.hpp>

#include <cmath>
#include <map>
#include <vector>

#include "libslic3r/libslic3r.h"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Surface.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// A block 40 x 20 mm whose top slopes from 2 mm at one end to 12 mm at the other (14 degrees): under the slope
// every layer has its solid shell a little further along than the layer beneath.
TriangleMesh wedge()
{
    indexed_triangle_set its = its_make_cube(40., 20., 1.);
    for (stl_vertex &v : its.vertices)
        if (v.z() > 0.5f)
            v.z() = 2.f + v.x() / 4.f;
    return TriangleMesh(its);
}

// Six steps, each 1.6 mm tall and 4 mm narrower all round than the one beneath: the top shells of every ledge lie on
// sparse infill and begin with an internal bridge.
TriangleMesh steps()
{
    TriangleMesh out;
    for (int step = 0; step < 6; ++step) {
        const double half = 26. - 4. * step;
        TriangleMesh box  = make_cube(2. * half, 2. * half, 1.6);
        box.translate(float(-half), float(-half), float(1.6 * step));
        out.merge(box);
    }
    return out;
}

struct LayerFill
{
    // mm2 by what it is to be filled as.
    std::map<SurfaceType, double> by_type;
    // mm2 of everything to fill, each place counted once.
    double                        covered = 0.;
};

std::vector<LayerFill> fill_of(const TriangleMesh &mesh, const char *extra_bridge_layer, const char *filter)
{
    Print print;
    init_and_process_print({mesh}, print, {
        {"enable_extra_bridge_layer", extra_bridge_layer},
        {"sparse_infill_pattern", "grid"},
        {"sparse_infill_density", "15%"},
        {"wall_loops", "2"},
        {"top_shell_layers", "4"},
        {"bottom_shell_layers", "3"},
        {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"},
        {"nozzle_diameter", "0.4"},
        {"wall_generator", "classic"},
        {"enable_support", "0"},
        {"dont_filter_internal_bridges", filter},
    });
    const double mm2 = SCALING_FACTOR * SCALING_FACTOR;
    std::vector<LayerFill> out;
    for (const Layer *layer : print.objects().front()->layers()) {
        LayerFill  fill;
        ExPolygons all;
        for (const LayerRegion *region : layer->regions())
            for (const Surface &surface : region->fill_surfaces.surfaces) {
                fill.by_type[surface.surface_type] += surface.expolygon.area() * mm2;
                all.push_back(surface.expolygon);
            }
        for (const ExPolygon &area : union_ex(all))
            fill.covered += area.area() * mm2;
        out.push_back(std::move(fill));
    }
    return out;
}

double of(const LayerFill &fill, SurfaceType type)
{
    const auto it = fill.by_type.find(type);
    return it == fill.by_type.end() ? 0. : it->second;
}

// Every layer has as much to fill with the second internal bridge layer as without it, and at least `least`
// layers have a second bridge on them.
void check_nothing_lost(const TriangleMesh &mesh, const char *filter, size_t least)
{
    const std::vector<LayerFill> off = fill_of(mesh, "disabled", filter);
    const std::vector<LayerFill> on  = fill_of(mesh, "internal_bridge_only", filter);
    REQUIRE(off.size() > 40);
    REQUIRE(on.size() == off.size());

    size_t layers_with_second_bridge = 0;
    for (size_t layer = 0; layer < off.size(); ++layer) {
        CAPTURE(layer, off[layer].covered, on[layer].covered);
        const double more_bridge = of(on[layer], stInternalBridge) - of(off[layer], stInternalBridge);
        if (more_bridge > 1.)
            ++layers_with_second_bridge;
        // No bridge is taken away.
        CHECK(more_bridge > -0.01);
        // What is to be filled is what it was. The safety offsets of the cuts are 10 nm wide, which over the
        // outlines of these parts is under a thousandth of a square millimetre.
        CHECK(std::abs(on[layer].covered - off[layer].covered) < 0.01);
        // And nothing is to be filled twice: the areas by type add up to what they added up to, but for the safety
        // offsets where two surfaces meet (some tens of nanometres along the 400 mm of a ledge's outline).
        double sum_on = 0., sum_off = 0.;
        for (const auto &[type, area] : on[layer].by_type)
            sum_on += area;
        for (const auto &[type, area] : off[layer].by_type)
            sum_off += area;
        CHECK(std::abs(sum_on - sum_off) < 0.05);
    }
    // The case only means something if there are second bridge layers on the part.
    CHECK(layers_with_second_bridge >= least);
}

} // namespace

TEST_CASE("A second internal bridge layer leaves every layer as much to fill as there was", "[ExtraBridgeLayer]")
{
    SECTION("under a sloping top, with no filter on internal bridges")
    {
        // With the filter the strips under a slope are too narrow to be bridges at all.
        check_nothing_lost(wedge(), "nofilter", 20);
    }
    SECTION("on the ledges of a stepped part, with the filter as shipped")
    {
        check_nothing_lost(steps(), "disabled", 5);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Bridge over infill (PrintObject::bridge_over_infill): errors the Rust port turned up.

namespace {

// A short run of the outline of a bridged area that the direction vote samples `votes` times, at y = 0 from x = x0 on,
// with an anchor line under it running at `degrees`. The vote takes the run's first edge only (the polygon's two other
// edges are too short to be sampled, the last never is), and the runs lie 100 mm apart, so each run's samples all go
// to its own anchor. `votes` is 2 or more.
void add_run(Polygons &bridged, Lines &anchors, double x0, int votes, double degrees)
{
    const double length = 2. * votes - 1.; // sampled ceil(length / 2) times; a run under 2 mm is not sampled
    bridged.emplace_back(Points{Point::new_scale(x0, 0.), Point::new_scale(x0 + length, 0.), Point::new_scale(x0 + length, 0.01)});
    const double a = Geometry::deg2rad(degrees);
    const Vec2d  mid{x0 + 0.5 * length, -1.};
    const Vec2d  half{30. * std::cos(a), 30. * std::sin(a)};
    anchors.emplace_back(Point::new_scale(mid - half), Point::new_scale(mid + half));
}

// An undirected direction in degrees, from 0 to 180.
double undirected_degrees(double radians)
{
    double deg = std::fmod(Geometry::rad2deg(radians), 180.);
    return deg < 0. ? deg + 180. : deg;
}

} // namespace

// The vote on a bridge's direction counts each sample as square to its nearest anchor, a direction from 0.5 PI to
// 1.5 PI, and pools the votes within 0.1 PI of each other. A direction just above 0.5 PI and one just below 1.5 PI are
// near each other as lines; a window reaching below 0.5 PI took in the votes near 1.5 PI, but the test for a window
// reaching above 1.5 PI asked for one starting above it, which none does, so those were never pooled with the votes
// near 0.5 PI. Here anchors at 5, 170 and 155 degrees vote 95, 260 and 245 degrees: the window around 260 holds all
// nine votes, but was scored as five, and the window around 95 (four at 95 and two at 260) won with six.
TEST_CASE("The bridge direction vote pools the votes across both ends of its range", "[BridgeOverInfill][Regression]")
{
    Polygons bridged;
    Lines    anchors;
    add_run(bridged, anchors, 0., 4, 5.);
    add_run(bridged, anchors, 100., 2, 170.);
    add_run(bridged, anchors, 200., 3, 155.);
    // Each anchor's own vote, as the function works it out from the scaled line.
    const auto vote = [](const Line &l) {
        double a = l.orientation();
        if (a > PI)
            a -= PI;
        return Geometry::rad2deg(a + 0.5 * PI);
    };
    const double v5 = vote(anchors[0]), v170 = vote(anchors[1]), v155 = vote(anchors[2]);
    // All nine votes, the ones near 95 degrees taken round to near 275.
    const double expected = undirected_degrees(Geometry::deg2rad((4. * (v5 + 180.) + 2. * v170 + 3. * v155) / 9.));
    // What it chose before: the window around 95 degrees with six votes.
    const double before = undirected_degrees(Geometry::deg2rad((4. * v5 + 2. * (v170 - 180.)) / 6.));
    REQUIRE(std::abs(expected - before) > 5.);

    const double got = undirected_degrees(determine_bridging_angle(bridged, anchors));
    INFO("chose " << got << " degrees; all the votes pooled give " << expected << ", the low end's window " << before);
    CHECK(std::abs(got - expected) < 0.01);
}

// The same anchors mirrored (each direction d becomes 180 - d) must give the mirrored direction: the vote has no
// reason to favour either end of its range. Before the fix it favoured the low end only.
TEST_CASE("The bridge direction vote gives the mirrored direction for mirrored anchors", "[BridgeOverInfill][Regression]")
{
    const std::vector<std::pair<int, double>> runs{{4, 5.}, {2, 170.}, {3, 155.}};
    Polygons bridged, bridged_mirrored;
    Lines    anchors, anchors_mirrored;
    double   x0 = 0.;
    for (const auto &[votes, degrees] : runs) {
        add_run(bridged, anchors, x0, votes, degrees);
        add_run(bridged_mirrored, anchors_mirrored, x0, votes, 180. - degrees);
        x0 += 100.;
    }
    const double got      = undirected_degrees(determine_bridging_angle(bridged, anchors));
    const double mirrored = undirected_degrees(determine_bridging_angle(bridged_mirrored, anchors_mirrored));
    INFO("direction " << got << " degrees, mirrored " << mirrored);
    // Within the rounding of the anchors' ends to whole nanometres.
    const double diff = std::abs(undirected_degrees(Geometry::deg2rad(got + mirrored)));
    CHECK(std::min(diff, 180. - diff) < 0.01);
}

namespace {

// A 30 x 30 mm block with a sealed 12 x 12 mm cavity in the middle. With 0.12 mm layers, two bottom and three top
// shell layers, the layers over the cavity's footprint run, from the cavity's ceiling up: a bridge over the cavity,
// one internal solid layer, two sparse, and the three of the top shell. The first layer of the top shell lies on
// sparse infill and becomes an internal bridge where the sparse infill under it is deep enough for a thick bridge.
TriangleMesh block_with_cavity(int cavity_top_layer, int layers)
{
    constexpr double lh = 0.12;
    indexed_triangle_set block  = its_make_cube(30., 30., lh * layers);
    indexed_triangle_set cavity = its_make_cube(12., 12., 1.2);
    for (stl_vertex &v : cavity.vertices)
        v += stl_vertex(9.f, 9.f, float(lh * cavity_top_layer - 1.2));
    // Turned inside out, a hole.
    for (stl_triangle_vertex_indices &f : cavity.indices)
        std::swap(f[1], f[2]);
    its_merge(block, cavity);
    return TriangleMesh(block);
}

} // namespace

// A thick bridge sinks about 0.9 of its thread's height into the layers beneath, and bridges over infill only where
// those layers are all sparse infill. The depth handed to the lambda that gathers them is 0.9 of the thread already,
// and the lambda took 0.9 of it again: it looked 0.81 of a thread deep. With a 0.4 mm thread that is 0.324 mm
// instead of 0.36 mm, two layers of 0.12 mm instead of three, so a solid layer three layers down went unseen and the
// top shell over the cavity was laid as a bridge sinking onto it.
TEST_CASE("Bridge over infill looks 0.9 of a thick bridge deep for solid layers", "[BridgeOverInfill][Regression]")
{
    // The cavity's ceiling at layer 20 (z = 2.4): layer 21 bridges the cavity, 22 is solid, 23 and 24 sparse, 25 to
    // 27 the top shell.
    const int    cavity_top = 20, layers = 27, first_top_shell = 24; // 0-based index of layer 25
    Print        print;
    init_and_process_print({block_with_cavity(cavity_top, layers)}, print, {
        {"layer_height", "0.12"},
        {"initial_layer_print_height", "0.12"},
        {"nozzle_diameter", "0.4"},
        {"bridge_flow", "1"},
        {"top_shell_layers", "3"},
        {"top_shell_thickness", "0"},
        {"bottom_shell_layers", "2"},
        {"bottom_shell_thickness", "0"},
        {"sparse_infill_pattern", "grid"},
        {"sparse_infill_density", "15%"},
        {"wall_loops", "2"},
        {"wall_generator", "classic"},
        {"enable_support", "0"},
    });
    const PrintObject &object = *print.objects().front();
    REQUIRE(object.layer_count() == size_t(layers));

    const double     mm2    = SCALING_FACTOR * SCALING_FACTOR;
    // The layers are in the object's own coordinates, centred on it.
    const Polygons   middle = {Polygon::new_scale({{-4., -4.}, {4., -4.}, {4., 4.}, {-4., 4.}})};
    // mm2 of the 8 x 8 mm in the middle of the cavity's footprint, and of the whole layer, filled as `type`.
    const auto fill_of = [&](size_t layer_idx, SurfaceType type, bool in_middle) {
        double out = 0.;
        for (const LayerRegion *region : object.get_layer(int(layer_idx))->regions())
            for (const Surface &surface : region->fill_surfaces.surfaces)
                if (surface.surface_type == type)
                    out += in_middle ? area(intersection(to_polygons(surface.expolygon), middle)) * mm2 : surface.expolygon.area() * mm2;
        return out;
    };
    // The layers are what the comment above says they are, over the middle.
    CHECK(fill_of(first_top_shell - 3, stInternalSolid, true) > 60.);
    CHECK(fill_of(first_top_shell - 2, stInternal, true) > 60.);
    CHECK(fill_of(first_top_shell - 1, stInternal, true) > 60.);

    const double bridge_middle = fill_of(first_top_shell, stInternalBridge, true);
    const double bridge_all    = fill_of(first_top_shell, stInternalBridge, false);
    INFO("internal bridge on the first top shell layer: " << bridge_all << " mm2, " << bridge_middle << " mm2 of it over the cavity");
    // Around the cavity the sparse infill runs all the way down: a bridge there.
    CHECK(bridge_all > 100.);
    // Over the cavity a solid layer lies 0.36 mm down: no bridge.
    CHECK(bridge_middle < 1.);
}
