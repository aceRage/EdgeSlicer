// Calibration settings ported from OrcaSlicer (6A-7a / 6A-7b): values the calibration towers print with.
// These only touch calibration projects; normal slicing never reads them.
#include <catch2/catch.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/calib.hpp"
#include "libslic3r/GCode/AdaptivePAProcessor.hpp"

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

TEST_CASE("Stepped calibration values come in equal bands over the layers", "[Calib][Stepped]")
{
    // Orca #14759 (PA tower, VFA tower). Layers count from 0; layers 0 and 1 print the start value.
    SECTION("PA tower: 0 to 0.1 in 0.002 steps over 255 layers (51 one-millimetre bands at 0.2 mm)") {
        CHECK(calib_stepped_layer_value(0, 255, 0.f, 0.1f, 0.002f) == Approx(0.f));
        CHECK(calib_stepped_layer_value(1, 255, 0.f, 0.1f, 0.002f) == Approx(0.f));
        CHECK(calib_stepped_layer_value(4, 255, 0.f, 0.1f, 0.002f) == Approx(0.f));
        CHECK(calib_stepped_layer_value(5, 255, 0.f, 0.1f, 0.002f) == Approx(0.002f));
        CHECK(calib_stepped_layer_value(254, 255, 0.f, 0.1f, 0.002f) == Approx(0.1f));
    }
    SECTION("VFA tower: 40 to 200 mm/s in 10 mm/s steps, 25 layers per block") {
        const unsigned int layers = 17 * vfa_layers_per_block;
        CHECK(calib_stepped_layer_value(24, layers, 40.f, 200.f, 10.f) == Approx(40.f));
        CHECK(calib_stepped_layer_value(25, layers, 40.f, 200.f, 10.f) == Approx(50.f));
        CHECK(calib_stepped_layer_value(int(layers) - 1, layers, 40.f, 200.f, 10.f) == Approx(200.f));
    }
    SECTION("A falling range steps down and never overshoots") {
        for (int layer = 0; layer < 100; ++layer) {
            const float v = calib_stepped_layer_value(layer, 100, 100.f, 50.f, 10.f);
            CHECK(v <= 100.f);
            CHECK(v >= 50.f);
        }
        CHECK(calib_stepped_layer_value(99, 100, 100.f, 50.f, 10.f) == Approx(50.f));
    }
    SECTION("Step 0 interpolates") {
        CHECK(calib_stepped_layer_value(50, 101, 0.f, 1.f, 0.f) == Approx(0.5f));
    }
}

TEST_CASE("The VFA dialog lowers the layer height, then the end speed, to stay under the volumetric limit", "[Calib][VFA]")
{
    // Orca #14759. 0.4 mm nozzle, 0.45 mm outer wall, 12 mm³/s: about 147 mm/s at 0.2 mm, 236 mm/s at 0.12 mm,
    // 347 mm/s at 0.08 mm.
    const std::vector<double> profile_heights{0.08, 0.12, 0.16, 0.2, 0.28, 0.2};
    CHECK(calib_vfa_speed_limit(12., 0.45, 0.2, 0.4) == Approx(147.4).epsilon(0.01));

    SECTION("Reachable at the default layer height") {
        const CalibVfaPlan plan = calib_vfa_plan(40., 140., 10., 0.2, profile_heights, 0.08, 0.28, 12., 0.45, 0.4);
        CHECK(plan.result == CalibVfaPlan::Result::Fits);
        CHECK(plan.layer_height == Approx(0.2));
    }
    SECTION("The largest profile layer height that reaches the end speed") {
        const CalibVfaPlan plan = calib_vfa_plan(40., 200., 10., 0.2, profile_heights, 0.08, 0.28, 12., 0.45, 0.4);
        CHECK(plan.result == CalibVfaPlan::Result::LayerHeightReduced);
        CHECK(plan.layer_height == Approx(0.12));
        CHECK(plan.end_speed == Approx(200.));
    }
    SECTION("Too fast even at the smallest: the end speed comes down to a step") {
        const CalibVfaPlan plan = calib_vfa_plan(40., 400., 10., 0.2, profile_heights, 0.08, 0.28, 12., 0.45, 0.4);
        CHECK(plan.result == CalibVfaPlan::Result::EndSpeedLowered);
        CHECK(plan.layer_height == Approx(0.08));
        CHECK(plan.end_speed == Approx(340.));
    }
    SECTION("No usable profile layer heights") {
        const CalibVfaPlan plan = calib_vfa_plan(40., 400., 10., 0.2, {0.32}, 0.08, 0.28, 12., 0.45, 0.4);
        CHECK(plan.result == CalibVfaPlan::Result::NoCandidates);
    }
    SECTION("No volumetric limit set") {
        CHECK(calib_vfa_plan(40., 400., 10., 0.2, profile_heights, 0., 0., 0., 0.45, 0.4).result == CalibVfaPlan::Result::Fits);
    }
}


TEST_CASE("Adaptive pressure advance models are validated", "[Calib][AdaptivePA]")
{
    // Orca #14198
    using V = AdaptivePAProcessor;
    CHECK(V::validate_adaptive_pa_model("").empty());
    CHECK(V::validate_adaptive_pa_model("0.04,3.96,3000\n0.033,3.96,10000\n0.029,7.91,3000\n0.026,7.91,10000").empty());
    // Edge: blanks around values and blank or CRLF lines are fine; the interpolator reads them.
    CHECK(V::validate_adaptive_pa_model("0.04, 3.96, 3000\r\n\n 0.033 ,3.96,10000 ").empty());

    CHECK_FALSE(V::validate_adaptive_pa_model("0.04;3.96;3000").empty());
    CHECK_FALSE(V::validate_adaptive_pa_model("0.04,3.96").empty());
    CHECK_FALSE(V::validate_adaptive_pa_model("0.04,3.96,3000,1").empty());
    CHECK_FALSE(V::validate_adaptive_pa_model("-0.04,3.96,3000").empty());
    CHECK_FALSE(V::validate_adaptive_pa_model("2.5,3.96,3000").empty());     // PA must be < 2
    CHECK_FALSE(V::validate_adaptive_pa_model("0.04,0.03,3000").empty());    // flow must be > PA
    CHECK_FALSE(V::validate_adaptive_pa_model("0.04,3.96,3").empty());       // acceleration must be > flow
    CHECK_FALSE(V::validate_adaptive_pa_model("0,0,0\n0,0,0").empty());      // the profiles' placeholder
    CHECK_FALSE(V::validate_adaptive_pa_model("0.04,3 .96,3000").empty());   // not one number
    CHECK_FALSE(V::validate_adaptive_pa_model("0.04,,3000").empty());
    // The message names the line.
    CHECK(V::validate_adaptive_pa_model("0.04,3.96,3000\n0.04,3.96").find("2") != std::string::npos);
}
