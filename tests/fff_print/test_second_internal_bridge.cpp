// The second internal bridge layer (enable_extra_bridge_layer) turns what of a layer's solid and sparse infill lies
// on the internal bridges of the layer beneath into bridge. It changes what an area is filled as and nothing else:
// every layer has as much to fill with the option on as with it off.

#include <catch2/catch.hpp>

#include <cmath>
#include <map>
#include <vector>

#include "libslic3r/libslic3r.h"
#include "libslic3r/ClipperUtils.hpp"
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
