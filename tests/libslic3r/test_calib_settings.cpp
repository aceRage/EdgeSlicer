// Calibration settings ported from OrcaSlicer (6A-7a / 6A-7b): values the calibration towers print with.
// These only touch calibration projects; normal slicing never reads them.
#include <catch2/catch.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/calib.hpp"

using namespace Slic3r;

TEST_CASE("Motion calibration towers print at the machine limits", "[Calib][MotionLimits]")
{
    DynamicPrintConfig printer = DynamicPrintConfig::full_print_config();

    SECTION("The slower axis and the extruding acceleration") {
        printer.set_key_value("machine_max_speed_x", new ConfigOptionFloats{500., 500.});
        printer.set_key_value("machine_max_speed_y", new ConfigOptionFloats{450., 500.});
        printer.set_key_value("machine_max_acceleration_extruding", new ConfigOptionFloats{12000., 10000.});
        const CalibMotionLimits limits = calib_machine_motion_limits(printer, 200., 20000.);
        CHECK(limits.speed == Approx(450.));
        CHECK(limits.acceleration == Approx(12000.));
    }
    SECTION("Unset limits keep the old fixed values") {
        printer.set_key_value("machine_max_speed_x", new ConfigOptionFloats{0.});
        printer.set_key_value("machine_max_speed_y", new ConfigOptionFloats{300.});
        printer.set_key_value("machine_max_acceleration_extruding", new ConfigOptionFloats{0.});
        const CalibMotionLimits limits = calib_machine_motion_limits(printer, 200., 2000.);
        CHECK(limits.speed == Approx(200.));
        CHECK(limits.acceleration == Approx(2000.));
    }
}

TEST_CASE("PA calibration batch values that look swapped are refused", "[Calib][PressureAdvance]")
{
    // Orca #11493
    CHECK(calib_pa_batch_values_plausible({}, {}));
    CHECK(calib_pa_batch_values_plausible({1000., 5000.}, {}));
    CHECK(calib_pa_batch_values_plausible({1000., 5000.}, {50., 200.}));
    CHECK_FALSE(calib_pa_batch_values_plausible({50., 200.}, {1000., 5000.}));
    CHECK_FALSE(calib_pa_batch_values_plausible({200.}, {200.}));
}
