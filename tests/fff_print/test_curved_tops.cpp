// EdgeSlicer SPIKE: curved tops (src/libslic3r/CurvedTops.cpp).
//
//  * OFF (the default): no conformal path anywhere and the extruded Z never goes back down.
//  * ON, a 30 mm puck with a 1.2 mm spherical cap: conformal paths are appended to the top layer
//    and, after the last planar layer, the G-code extrudes below that layer's Z (the curved
//    layers follow the cap down to its rim).
//  * The pure helpers: head clearance, effective angle, first-layer gap flow.

#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "libslic3r/ContourZ.hpp"
#include "libslic3r/CurvedTops.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

// A cylinder of radius r and height h_cyl topped by a spherical cap of height h_cap.
TriangleMesh make_capped_puck(double r, double h_cyl, double h_cap, int rings = 24, int sectors = 96)
{
    const double rs = (r * r + h_cap * h_cap) / (2.0 * h_cap); // sphere radius of the cap
    auto cap_z = [&](double rr) { return h_cyl + std::sqrt(rs * rs - rr * rr) - (rs - h_cap); };

    indexed_triangle_set its;
    its.vertices.emplace_back(0.f, 0.f, float(cap_z(0.)));            // 0: top centre
    for (int i = 1; i <= rings; ++i) {
        const double rr = r * double(i) / double(rings);
        for (int j = 0; j < sectors; ++j) {
            const double a = 2.0 * M_PI * double(j) / double(sectors);
            its.vertices.emplace_back(float(rr * std::cos(a)), float(rr * std::sin(a)), float(cap_z(rr)));
        }
    }
    const int bottom0 = int(its.vertices.size());
    for (int j = 0; j < sectors; ++j) {
        const double a = 2.0 * M_PI * double(j) / double(sectors);
        its.vertices.emplace_back(float(r * std::cos(a)), float(r * std::sin(a)), 0.f);
    }
    const int bottom_centre = int(its.vertices.size());
    its.vertices.emplace_back(0.f, 0.f, 0.f);

    auto ring = [sectors](int i, int j) { return 1 + (i - 1) * sectors + (j % sectors); };
    for (int j = 0; j < sectors; ++j)
        its.indices.emplace_back(0, ring(1, j), ring(1, j + 1));
    for (int i = 1; i < rings; ++i)
        for (int j = 0; j < sectors; ++j) {
            its.indices.emplace_back(ring(i, j), ring(i + 1, j), ring(i + 1, j + 1));
            its.indices.emplace_back(ring(i, j), ring(i + 1, j + 1), ring(i, j + 1));
        }
    for (int j = 0; j < sectors; ++j) {
        const int b0 = bottom0 + j, b1 = bottom0 + (j + 1) % sectors;
        its.indices.emplace_back(b0, b1, ring(rings, j + 1));
        its.indices.emplace_back(b0, ring(rings, j + 1), ring(rings, j));
        its.indices.emplace_back(bottom_centre, b1, b0);
    }
    return TriangleMesh(std::move(its));
}

size_t count_curved_paths(const ExtrusionEntity *e)
{
    if (const auto *c = dynamic_cast<const ExtrusionEntityCollection *>(e)) {
        size_t n = 0;
        for (const ExtrusionEntity *child : c->entities)
            n += count_curved_paths(child);
        return n;
    }
    if (const auto *p = dynamic_cast<const ExtrusionPath *>(e))
        return p->z_contour != nullptr && p->z_contour->curved_top ? 1 : 0;
    return 0;
}

// How far an extruding move ever goes back below the highest Z extruded before it. Planar
// G-code only ever extrudes upwards, so this is 0; z-hops and travels do not count.
double extrusion_dip(const std::string &gcode)
{
    std::istringstream in(gcode);
    std::string        line;
    double z = 0., highest = -1e9, lowest = 0.;
    while (std::getline(in, line)) {
        if (line.rfind("G1 ", 0) != 0 && line.rfind("G0 ", 0) != 0)
            continue;
        const auto code = line.substr(0, line.find(';'));
        auto       word = [&code](char c, double &v) {
            const auto pos = code.find(std::string(" ") + c);
            if (pos == std::string::npos)
                return false;
            v = std::atof(code.c_str() + pos + 2);
            return true;
        };
        double v, e;
        if (word('Z', v))
            z = v;
        if (word('E', e) && e > 0. && (word('X', v) || word('Y', v))) {
            lowest  = std::min(lowest, z - highest);
            highest = std::max(highest, z);
        }
    }
    return lowest;
}

void slice_puck(Print &print, Model &model, bool curved_tops)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height",               "0.2" },
        { "initial_layer_print_height", "0.2" },
        { "sparse_infill_density",      "15%" },
        { "enable_support",             "0" },
        { "curved_tops_enabled",        curved_tops ? "1" : "0" },
        { "curved_tops_layers",         "3" },
        { "curved_tops_max_angle",      "15" },
    });
    Test::init_print(std::vector<TriangleMesh>{ make_capped_puck(15., 4., 1.2) }, print, model, config);
    print.process();
}

} // namespace

TEST_CASE("Curved tops: off by default leaves the slice planar", "[CurvedTops]")
{
    REQUIRE_FALSE(DynamicPrintConfig::full_print_config().opt_bool("curved_tops_enabled"));

    Print print;
    Model model;
    slice_puck(print, model, false);
    const PrintObject *object = print.objects().front();
    size_t curved = 0;
    for (const Layer *layer : object->layers())
        for (const LayerRegion *region : layer->regions())
            curved += count_curved_paths(&region->fills);
    CHECK(curved == 0);

    CHECK_THAT(extrusion_dip(Test::gcode(print)), WithinAbs(0., 1e-6));
}

TEST_CASE("Curved tops: a gentle cap is printed as conformal layers", "[CurvedTops]")
{
    Print print;
    Model model;
    slice_puck(print, model, true);
    const PrintObject *object = print.objects().front();

    size_t curved_top = 0, curved_elsewhere = 0;
    for (size_t i = 0; i < object->layers().size(); ++i)
        for (const LayerRegion *region : object->layers()[i]->regions())
            (i + 1 == object->layers().size() ? curved_top : curved_elsewhere) += count_curved_paths(&region->fills);
    CHECK(curved_elsewhere == 0);
    // Three layers, each at least a wall loop and some fill lines.
    CHECK(curved_top >= 6);

    // The cap rises 1.2 mm: each curved layer starts back down at the rim, below the apex of the
    // one before it, so the extrusion Z comes back down by about the cap's rise.
    const double dip = extrusion_dip(Test::gcode(print));
    CHECK(dip < -0.8);
    CHECK(dip > -(1.2 + 3 * 0.2 + 0.3));
}

TEST_CASE("Curved tops: head clearance and flow helpers", "[CurvedTops]")
{
    PrintConfig cfg;
    const CurvedTopsHeadClearance generic = curved_tops_head_clearance(cfg);
    cfg.printer_model.value = "Elegoo Centauri Carbon";
    const CurvedTopsHeadClearance cc1 = curved_tops_head_clearance(cfg);
    cfg.printer_model.value = "Elegoo Centauri Carbon 2";
    const CurvedTopsHeadClearance cc2 = curved_tops_head_clearance(cfg);

    CHECK_THAT(cc1.nozzle_height_mm, WithinAbs(2.0, 1e-9));
    CHECK_THAT(cc1.max_slope_deg, WithinAbs(20.0, 1e-9));
    CHECK_THAT(cc2.nozzle_height_mm, WithinAbs(generic.nozzle_height_mm, 1e-9));

    CHECK_THAT(curved_tops_effective_max_angle(15., cc1), WithinAbs(15., 1e-9));
    CHECK_THAT(curved_tops_effective_max_angle(35., cc1), WithinAbs(20., 1e-9));

    CHECK_THAT(curved_tops_gap_flow(0.2, 0.2), WithinAbs(1.0, 1e-9));
    CHECK_THAT(curved_tops_gap_flow(0.3, 0.2), WithinAbs(1.5, 1e-9));
    CHECK_THAT(curved_tops_gap_flow(0.1, 0.2), WithinAbs(1.0, 1e-9));
    CHECK_THAT(curved_tops_gap_flow(0.9, 0.2), WithinAbs(2.0, 1e-9));
}
