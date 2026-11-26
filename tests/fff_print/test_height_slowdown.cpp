// Bambu Studio 02.09.01.70 parity, PR 1 "Bambu small parity":
//  - height slowdown (enable_height_slowdown + slowdown_*; BS GCode::slowDownByHeight, GCode.cpp:7513-7563),
//  - initial_layer_infill_line_width (BS PrintRegion.cpp:24-31).
#include <catch2/catch.hpp>

#include "test_data.hpp"

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/GCode.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/libslic3r.h"

#include <algorithm>
#include <string>

using namespace Slic3r;

namespace {

template<typename Fn>
void for_each_path(const ExtrusionEntityCollection &entities, Fn &&fn)
{
    for (const ExtrusionEntity *entity : entities.entities) {
        if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(entity))
            for_each_path(*collection, fn);
        else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(entity))
            for (const ExtrusionPath &path : loop->paths)
                fn(path);
        else if (const auto *multi_path = dynamic_cast<const ExtrusionMultiPath *>(entity))
            for (const ExtrusionPath &path : multi_path->paths)
                fn(path);
        else if (const auto *path = dynamic_cast<const ExtrusionPath *>(entity))
            fn(*path);
    }
}

bool infill_role(ExtrusionRole role)
{
    return role == erInternalInfill || role == erSolidInfill || role == erTopSolidInfill || role == erBottomSurface;
}

} // namespace

TEST_CASE("Height slowdown limits follow Bambu's slowDownByHeight", "[BambuParity][HeightSlowdown]")
{
    // A2L "0.20mm Steady": 150 -> 325 mm, 500 -> 500 mm/s, 8000 -> 2000 mm/s^2.
    SECTION("off, below the starting height, or with the heights the wrong way round: nothing is capped") {
        CHECK_FALSE(height_slowdown_limits(false, 0., 400., 1000., 500., 8000., 1000., 100.).active);
        CHECK_FALSE(height_slowdown_limits(true, 150., 325., 500., 500., 8000., 2000., 149.9).active);
        CHECK_FALSE(height_slowdown_limits(true, 200., 200., 500., 100., 8000., 2000., 250.).active);
        CHECK_FALSE(height_slowdown_limits(true, 300., 200., 500., 100., 8000., 2000., 250.).active);
    }
    SECTION("at the starting height: the starting values") {
        const HeightSlowdown l = height_slowdown_limits(true, 150., 325., 500., 500., 8000., 2000., 150.);
        CHECK(l.active);
        CHECK(l.max_speed == Approx(500.));
        CHECK(l.max_acc == Approx(8000.));
    }
    SECTION("linear between the two heights") {
        // Halfway from 10 to 30 mm: 200 -> 100 mm/s gives 150, 6000 -> 2000 gives 4000.
        const HeightSlowdown half = height_slowdown_limits(true, 10., 30., 200., 100., 6000., 2000., 20.);
        CHECK(half.active);
        CHECK(half.max_speed == Approx(150.));
        CHECK(half.max_acc == Approx(4000.));
        // A2L 0.20mm Standard (0 -> 225 mm, 8000 -> 1000 mm/s^2) at 45 mm: 8000 - 7000 * 0.2 = 6600.
        const HeightSlowdown a2l = height_slowdown_limits(true, 0., 225., 500., 500., 8000., 1000., 45.);
        CHECK(a2l.max_speed == Approx(500.));
        CHECK(a2l.max_acc == Approx(6600.));
    }
    SECTION("at and above the ending height: the ending values") {
        const HeightSlowdown at = height_slowdown_limits(true, 10., 30., 200., 100., 6000., 2000., 30.);
        CHECK(at.max_speed == Approx(100.));
        CHECK(at.max_acc == Approx(2000.));
        const HeightSlowdown above = height_slowdown_limits(true, 10., 30., 200., 100., 6000., 2000., 120.);
        CHECK(above.active);
        CHECK(above.max_speed == Approx(100.));
        CHECK(above.max_acc == Approx(2000.));
    }
    SECTION("the ending values may be higher than the starting ones (Bambu does not order them)") {
        const HeightSlowdown up = height_slowdown_limits(true, 0., 100., 100., 300., 1000., 3000., 50.);
        CHECK(up.max_speed == Approx(200.));
        CHECK(up.max_acc == Approx(2000.));
    }
}

TEST_CASE("Height slowdown applies to walls, infill and gap fill only", "[BambuParity][HeightSlowdown]")
{
    for (ExtrusionRole role : { erPerimeter, erExternalPerimeter, erOverhangPerimeter, erOverSupportPerimeter, erInternalInfill,
                                erSolidInfill, erTopSolidInfill, erBottomSurface, erBottomSurfaceOverSupport, erIroning,
                                erBridgeInfill, erInternalBridgeInfill, erGapFill }) {
        INFO(ExtrusionEntity::role_to_string(role));
        CHECK(height_slowdown_role(role));
    }
    for (ExtrusionRole role : { erNone, erSkirt, erBrim, erSupportMaterial, erSupportMaterialInterface, erSupportTransition,
                                erWipeTower, erCustom, erMixed }) {
        INFO(ExtrusionEntity::role_to_string(role));
        CHECK_FALSE(height_slowdown_role(role));
    }
}

TEST_CASE("Height slowdown caps print speed and acceleration in the G-code", "[BambuParity][HeightSlowdown]")
{
    const bool enabled = GENERATE(true, false);
    INFO("enable_height_slowdown " << enabled);

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "gcode_flavor", "marlin2" },
        { "layer_height", "0.2" },
        { "initial_layer_print_height", "0.2" },
        { "outer_wall_speed", "200" },
        { "inner_wall_speed", "250" },
        { "sparse_infill_speed", "300" },
        { "internal_solid_infill_speed", "300" },
        { "top_surface_speed", "200" },
        { "gap_infill_speed", "200" },
        { "travel_speed", "400" },
        { "filament_max_volumetric_speed", "200" },
        { "slow_down_for_layer_cooling", "0" },
        { "slow_down_layers", "0" },
        { "default_acceleration", "10000" },
        { "outer_wall_acceleration", "8000" },
        { "inner_wall_acceleration", "10000" },
        { "travel_acceleration", "10000" },
        { "machine_max_acceleration_extruding", "20000,20000" },
        { "machine_max_acceleration_travel", "20000,20000" },
        { "enable_height_slowdown", enabled ? "1" : "0" },
        { "slowdown_start_height", "5" },
        { "slowdown_end_height", "15" },
        { "slowdown_start_speed", "150" },
        { "slowdown_end_speed", "60" },
        { "slowdown_start_acc", "6000" },
        { "slowdown_end_acc", "2000" },
    });
    const std::string gcode = Slic3r::Test::slice({ Slic3r::Test::TestMesh::cube_20x20x20 }, config);
    REQUIRE(!gcode.empty());

    const auto cap = [](double z, double start, double end) {
        return z >= 15. ? end : start + (end - start) * (z - 5.) / 10.;
    };
    double print_acc = 0.;
    double fastest_low = 0., fastest_high = 0., worst_speed_excess = 0., worst_acc_excess = 0.;
    double max_acc_low = 0.;
    size_t checked = 0;
    GCodeReader parser;
    parser.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        float value = 0.f;
        if (line.cmd_is("M204") && (line.has_value('P', value) || line.has_value('S', value)))
            print_acc = value;
        if (!line.extruding(self) || line.dist_XY(self) <= 0.)
            return;
        const double z     = self.z();
        const double speed = line.new_F(self) / 60.;
        if (z < 5. - EPSILON) {
            fastest_low = std::max(fastest_low, speed);
            max_acc_low = std::max(max_acc_low, print_acc);
            return;
        }
        ++checked;
        fastest_high       = std::max(fastest_high, speed);
        worst_speed_excess = std::max(worst_speed_excess, speed - cap(z, 150., 60.));
        worst_acc_excess   = std::max(worst_acc_excess, print_acc - cap(z, 6000., 2000.));
    });
    REQUIRE(checked > 100);
    // Below the starting height nothing is capped, either way.
    CHECK(fastest_low > 150.);
    CHECK(max_acc_low > 6000.);
    if (enabled) {
        // F is written to 3 decimals of mm/min; M204 values are rounded to whole mm/s^2.
        CHECK(worst_speed_excess < 0.01);
        CHECK(worst_acc_excess < 1.);
        CHECK(fastest_high <= 150. + 0.01);
    } else {
        CHECK(fastest_high > 150.);
        CHECK(worst_acc_excess > 1.);
    }
}

TEST_CASE("use_initial_layer_infill_width picks the infill roles only", "[BambuParity][InitialLayerInfillWidth]")
{
    CHECK(use_initial_layer_infill_width(frInfill, 0.6, false));
    CHECK(use_initial_layer_infill_width(frSolidInfill, 0.6, false));
    CHECK(use_initial_layer_infill_width(frTopSolidInfill, 0.6, false));
    CHECK_FALSE(use_initial_layer_infill_width(frExternalPerimeter, 0.6, false));
    CHECK_FALSE(use_initial_layer_infill_width(frPerimeter, 0.6, false));
    CHECK_FALSE(use_initial_layer_infill_width(frSupportMaterial, 0.6, false));
    // 0 = follow the initial layer line width.
    CHECK_FALSE(use_initial_layer_infill_width(frSolidInfill, 0., false));
    // A per-nozzle process's own first-layer width wins.
    CHECK_FALSE(use_initial_layer_infill_width(frSolidInfill, 0.6, true));
}

TEST_CASE("Initial layer infill line width sets the first layer's infill width only", "[BambuParity][InitialLayerInfillWidth]")
{
    const double infill_width = GENERATE(0., 0.6);
    INFO("initial_layer_infill_line_width " << infill_width);

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height", "0.2" },
        { "initial_layer_print_height", "0.2" },
        { "wall_generator", "classic" },
        { "initial_layer_line_width", "0.5" },
        { "line_width", "0.42" },
        { "outer_wall_line_width", "0.42" },
        { "inner_wall_line_width", "0.45" },
        { "sparse_infill_line_width", "0.45" },
        { "internal_solid_infill_line_width", "0.42" },
        { "top_surface_line_width", "0.42" },
        { "sparse_infill_density", "15%" },
        { "skirt_loops", "0" },
        { "brim_type", "no_brim" },
    });
    config.set("initial_layer_infill_line_width", infill_width);

    Model model;
    Print print;
    Slic3r::Test::init_print({ Slic3r::Test::TestMesh::cube_20x20x20 }, print, model, config);
    print.process();
    REQUIRE(print.objects().size() == 1);
    const PrintObject &object = *print.objects().front();
    REQUIRE(object.layers().size() > 2);

    const double expected_first_infill = infill_width > 0. ? infill_width : 0.5;
    size_t first_infill = 0, first_walls = 0, second_infill = 0;
    for (const LayerRegion *layerm : object.layers()[0]->regions()) {
        for_each_path(layerm->fills, [&](const ExtrusionPath &path) {
            if (!infill_role(path.role()))
                return;
            ++first_infill;
            CHECK(path.width == Approx(expected_first_infill).margin(0.01));
        });
        for_each_path(layerm->perimeters, [&](const ExtrusionPath &path) {
            if (!is_perimeter(path.role()))
                return;
            ++first_walls;
            CHECK(path.width == Approx(0.5).margin(0.01));
        });
    }
    for (const LayerRegion *layerm : object.layers()[1]->regions())
        for_each_path(layerm->fills, [&](const ExtrusionPath &path) {
            if (!infill_role(path.role()))
                return;
            ++second_infill;
            CHECK(path.width < 0.46);
        });
    CHECK(first_infill > 0);
    CHECK(first_walls > 0);
    CHECK(second_infill > 0);
}
