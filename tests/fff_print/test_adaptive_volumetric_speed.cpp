// Orca #12688 / Bambu Studio 02.09.01.70: adaptive volumetric speed. With filament_adaptive_volumetric_speed on,
// the flow cap is the smaller of filament_max_volumetric_speed and the fit of layer height and line width
// (volumetric_speed_coefficients). Off (the default, and every stock preset), the G-code does not change.
//
// Catch2 v2 headers; Test::slice + TestMesh cube. The fits below are constants (only f != 0), so the
// expected flow does not depend on the line width and layer height the slicer picks.

#include <catch2/catch.hpp>

#include <cmath>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

struct WallFlows
{
    std::set<int> feedrates;     // mm/min
    double        min_flow{1e9}; // mm3/s of the outer wall extrusions above the first layers
    double        max_flow{0.};
};

// Outer wall extrusions above the first layers: their feedrates and volumetric flows.
WallFlows outer_wall_flows(bool adaptive, const std::string &coefficients, double max_volumetric_speed)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"outer_wall_speed",              "200"},
        {"filament_max_volumetric_speed", std::to_string(max_volumetric_speed)},
        {"slow_down_layer_time",          "0"},
        {"slow_down_layers",              "0"},
        {"skirt_loops",                   "0"},
        {"brim_type",                     "no_brim"},
        {"gcode_flavor",                  "marlin"},
        {"use_relative_e_distances",      "1"},
        {"enable_pressure_advance",       "0"},
    });
    config.option<ConfigOptionBoolsNullable>("filament_adaptive_volumetric_speed", true)->values = { adaptive ? (unsigned char) 1 : (unsigned char) 0 };
    config.option<ConfigOptionStrings>("volumetric_speed_coefficients", true)->values            = { coefficients };
    const double filament_area = PI * 0.25 * config.option<ConfigOptionFloats>("filament_diameter")->get_at(0) *
                                 config.option<ConfigOptionFloats>("filament_diameter")->get_at(0);

    Print print;
    Model model;
    init_print({TestMesh::cube_20x20x20}, print, model, config);
    print.is_BBL_printer() = false;
    const std::string gcode = Test::gcode(print);

    WallFlows   out;
    int         layer      = 0;
    bool        outer_wall = false;
    double      feedrate   = 0.;
    double      x = 0., y = 0.;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&](GCodeReader &, const GCodeReader::GCodeLine &line) {
        const std::string &raw = line.raw();
        if (raw.rfind(";LAYER_CHANGE", 0) == 0)
            ++layer;
        else if (raw.rfind(";TYPE:", 0) == 0)
            outer_wall = raw.rfind(";TYPE:Outer wall", 0) == 0;
        if (!line.cmd_is("G1") && !line.cmd_is("G0"))
            return;
        if (line.has_f())
            feedrate = line.f();
        const double nx = line.has_x() ? line.x() : x;
        const double ny = line.has_y() ? line.y() : y;
        const double length = std::hypot(nx - x, ny - y);
        x = nx;
        y = ny;
        if (outer_wall && layer > 3 && line.has_e() && line.e() > 0. && length > 1.) {
            out.feedrates.insert(int(std::lround(feedrate)));
            const double flow = line.e() * filament_area / length * feedrate / 60.;
            out.min_flow      = std::min(out.min_flow, flow);
            out.max_flow      = std::max(out.max_flow, flow);
        }
    });
    return out;
}

} // namespace

TEST_CASE("Adaptive volumetric speed caps the flow at the fit", "[OrcaPackage5][AdaptiveVolumetricSpeed][GCode]")
{
    // Baseline: no fit at all. The outer wall runs at its 200 mm/s (about 17 mm3/s, far below the 200 cap).
    const WallFlows baseline = outer_wall_flows(false, "", 200.);
    REQUIRE(baseline.feedrates == std::set<int>{200 * 60});

    // Off: a fit that is stored but not enabled changes nothing. (Unusable fits with the flag on are covered
    // by the ResolvedFilamentFlow case in test_bambu_parity_orca_package5.cpp.)
    CHECK(outer_wall_flows(false, "0 0 0 0 0 5", 200.).feedrates == baseline.feedrates);

    // On, constant fit of 5 mm3/s below the cap: the flow is the fit.
    const WallFlows below = outer_wall_flows(true, "0 0 0 0 0 5", 200.);
    CHECK(below.feedrates != baseline.feedrates);
    CHECK(below.max_flow == Approx(5.).epsilon(0.03));
    CHECK(below.min_flow == Approx(5.).epsilon(0.03));

    // On, fit above the filament maximum: the maximum still wins.
    CHECK(outer_wall_flows(true, "0 0 0 0 0 500", 8.).max_flow == Approx(8.).epsilon(0.03));
}
