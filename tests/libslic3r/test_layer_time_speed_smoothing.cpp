#include <catch2/catch.hpp>

#include "libslic3r/GCode/LayerTimeSpeedSmoothingFilter.hpp"
#include "libslic3r/LayerTimeSpeedSmoothing.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>
#include <vector>

using Catch::Matchers::WithinAbs;
using namespace Slic3r;

static void require_not_lengthened(const std::vector<double> &original, const std::vector<double> &solved)
{
    REQUIRE(solved.size() == original.size());
    for (size_t i = 0; i < original.size(); ++i)
        REQUIRE(solved[i] <= original[i] + 1e-9);
}

static void require_not_shortened(const std::vector<double> &original, const std::vector<double> &solved)
{
    REQUIRE(solved.size() == original.size());
    for (size_t i = 0; i < original.size(); ++i)
        REQUIRE(solved[i] >= original[i] - 1e-9);
}

static void require_variation_band(const std::vector<double> &times, double v, size_t first_layer)
{
    const double ratio = 1.0 - v;
    for (size_t i = first_layer + 1; i < times.size(); ++i) {
        REQUIRE(times[i]     >= times[i - 1] * ratio - 1e-6);
        REQUIRE(times[i - 1] >= times[i] * ratio - 1e-6);
    }
}

static void require_speed_factors(const LayerTimeSpeedSolveResult &result, const std::vector<double> &original)
{
    REQUIRE(result.speed_factors.size() == original.size());
    for (size_t i = 0; i < original.size(); ++i) {
        const double expected = (result.times[i] > 0.0) ? original[i] / result.times[i] : 1.0;
        REQUIRE_THAT(result.speed_factors[i], WithinAbs(expected, 1e-9));
    }
}

TEST_CASE("Layer time speed-up: long layers drop into the neighbour band", "[LayerTimeSpeedSmoothing]")
{
    // Dual of the classic slowdown example: 10, 100, 200, 50, 10 s at 20% variation.
    // Short layers stay put; long layers are pulled down to 1/0.8 of the shorter neighbour.
    const std::vector<double> times = {10.0, 100.0, 200.0, 50.0, 10.0};
    LayerTimeSpeedUpParams    params;
    params.max_variation = 0.20;
    params.max_speedup   = 100.0; // unconstrained by the per-layer floor

    const auto result = solve_layer_time_speed_up(times, params, /*first_layer=*/0);

    require_not_lengthened(times, result.times);
    require_variation_band(result.times, 0.20, 0);
    require_speed_factors(result, times);

    REQUIRE_THAT(result.times[0], WithinAbs(10.0, 1e-6));
    REQUIRE_THAT(result.times[1], WithinAbs(12.5, 1e-6));
    REQUIRE_THAT(result.times[2], WithinAbs(15.625, 1e-6));
    REQUIRE_THAT(result.times[3], WithinAbs(12.5, 1e-6));
    REQUIRE_THAT(result.times[4], WithinAbs(10.0, 1e-6));

    for (double factor : result.speed_factors) {
        REQUIRE(factor >= 1.0 - 1e-9);
        REQUIRE(factor <= 1.0 + params.max_speedup + 1e-9);
    }
}

TEST_CASE("Layer time speed-up: max_speedup floors how far a layer may drop", "[LayerTimeSpeedSmoothing]")
{
    const std::vector<double> times = {10.0, 100.0, 200.0, 50.0, 10.0};
    LayerTimeSpeedUpParams    params;
    params.max_variation = 0.20;
    params.max_speedup   = 1.0; // time may be halved

    const auto result = solve_layer_time_speed_up(times, params, /*first_layer=*/0);

    require_not_lengthened(times, result.times);
    require_speed_factors(result, times);

    // Floors: 5, 50, 100, 25, 5. The 10 s ends stay; the long layers stop at 2x.
    REQUIRE_THAT(result.times[0], WithinAbs(10.0, 1e-6));
    REQUIRE_THAT(result.times[1], WithinAbs(50.0, 1e-6));
    REQUIRE_THAT(result.times[2], WithinAbs(100.0, 1e-6));
    REQUIRE_THAT(result.times[3], WithinAbs(25.0, 1e-6));
    REQUIRE_THAT(result.times[4], WithinAbs(10.0, 1e-6));

    for (size_t i = 0; i < times.size(); ++i) {
        REQUIRE(result.speed_factors[i] >= 1.0 - 1e-9);
        REQUIRE(result.speed_factors[i] <= 2.0 + 1e-9);
        REQUIRE(result.times[i] >= times[i] / 2.0 - 1e-9);
    }
}

TEST_CASE("Layer time speed-up: layers before first_layer are untouched and do not constrain", "[LayerTimeSpeedSmoothing]")
{
    const std::vector<double> times = {300.0, 10.0, 10.0, 10.0};
    LayerTimeSpeedUpParams    params;
    params.max_variation = 0.25;
    params.max_speedup   = 100.0;

    const auto result = solve_layer_time_speed_up(times, params, /*first_layer=*/1);

    REQUIRE_THAT(result.times[0], WithinAbs(300.0, 1e-9));
    REQUIRE_THAT(result.times[1], WithinAbs(10.0, 1e-9));
    REQUIRE_THAT(result.times[2], WithinAbs(10.0, 1e-9));
    REQUIRE_THAT(result.times[3], WithinAbs(10.0, 1e-9));
}

TEST_CASE("Layer time speed-up: a curve already inside the band is unchanged", "[LayerTimeSpeedSmoothing]")
{
    const std::vector<double> times = {10.0, 11.0, 12.0, 11.0, 10.0};
    LayerTimeSpeedUpParams    params;
    params.max_variation = 0.25;
    params.max_speedup   = 1.0;

    const auto result = solve_layer_time_speed_up(times, params, /*first_layer=*/0);
    for (size_t i = 0; i < times.size(); ++i)
        REQUIRE_THAT(result.times[i], WithinAbs(times[i], 1e-9));
}

TEST_CASE("Layer time speed-up: zero max_speedup is a no-op", "[LayerTimeSpeedSmoothing]")
{
    const std::vector<double> times = {10.0, 100.0, 10.0};
    LayerTimeSpeedUpParams    params;
    params.max_variation = 0.10;
    params.max_speedup   = 0.0;

    const auto result = solve_layer_time_speed_up(times, params, /*first_layer=*/0);
    for (size_t i = 0; i < times.size(); ++i) {
        REQUIRE_THAT(result.times[i], WithinAbs(times[i], 1e-9));
        REQUIRE_THAT(result.speed_factors[i], WithinAbs(1.0, 1e-9));
    }
}

TEST_CASE("Layer time slowdown: short layers rise into the neighbour band", "[LayerTimeSpeedSmoothing]")
{
    const std::vector<double> times = {10.0, 100.0, 200.0, 50.0, 10.0};
    LayerTimeSlowdownParams   params;
    params.max_variation     = 0.20;
    params.max_slowdown      = 100.0;
    params.max_time_increase = 100.0; // unconstrained budget

    const auto result = solve_layer_time_slowdown(times, params, /*first_layer=*/0);

    require_not_shortened(times, result.times);
    require_variation_band(result.times, 0.20, 0);
    require_speed_factors(result, times);

    REQUIRE_THAT(result.times[2], WithinAbs(200.0, 1e-6));
    REQUIRE_THAT(result.times[1], WithinAbs(160.0, 1e-6));
    REQUIRE_THAT(result.times[3], WithinAbs(160.0, 1e-6));
    REQUIRE_THAT(result.times[0], WithinAbs(128.0, 1e-6));
    REQUIRE_THAT(result.times[4], WithinAbs(128.0, 1e-6));

    for (double factor : result.speed_factors)
        REQUIRE(factor <= 1.0 + 1e-9);
}

TEST_CASE("Layer time slowdown: max_slowdown caps stop the ramp", "[LayerTimeSpeedSmoothing]")
{
    const std::vector<double> times = {6.0, 6.0, 6.0, 40.0, 40.0};
    LayerTimeSlowdownParams   params;
    params.max_variation     = 0.25;
    params.max_slowdown      = 2.0; // at most 3x
    params.max_time_increase = 100.0;

    const auto result = solve_layer_time_slowdown(times, params, /*first_layer=*/0);

    require_not_shortened(times, result.times);
    require_speed_factors(result, times);

    REQUIRE_THAT(result.times[2], WithinAbs(18.0, 1e-6));
    REQUIRE_THAT(result.times[1], WithinAbs(13.5, 1e-6));
    REQUIRE_THAT(result.times[0], WithinAbs(10.125, 1e-6));
    REQUIRE_THAT(result.times[3], WithinAbs(40.0, 1e-6));
    REQUIRE_THAT(result.times[4], WithinAbs(40.0, 1e-6));
}

TEST_CASE("Layer time slowdown: total time budget relaxes the variation", "[LayerTimeSpeedSmoothing]")
{
    const std::vector<double> times = {5.0, 5.0, 5.0, 5.0, 50.0, 5.0, 5.0, 5.0, 5.0};
    LayerTimeSlowdownParams   params;
    params.max_variation     = 0.25;
    params.max_slowdown      = 100.0;
    params.max_time_increase = 0.20;

    LayerTimeSlowdownParams unlimited = params;
    unlimited.max_time_increase       = 100.0;
    const auto free = solve_layer_time_slowdown(times, unlimited, /*first_layer=*/0);
    REQUIRE_THAT(free.effective_max_variation, WithinAbs(0.25, 1e-9));

    double base = 0.0, extra_free = 0.0;
    for (size_t i = 0; i < times.size(); ++i) {
        base += times[i];
        extra_free += free.times[i] - times[i];
    }
    REQUIRE(extra_free > 0.5 * base);

    const auto limited = solve_layer_time_slowdown(times, params, /*first_layer=*/0);
    REQUIRE(limited.effective_max_variation > 0.25);
    REQUIRE(limited.effective_max_variation <= 1.0 + 1e-9);

    double extra_limited = 0.0;
    for (size_t i = 0; i < times.size(); ++i) {
        REQUIRE(limited.times[i] >= times[i] - 1e-9);
        extra_limited += limited.times[i] - times[i];
    }
    REQUIRE(extra_limited <= 0.20 * base + 1e-2);
    require_variation_band(limited.times, limited.effective_max_variation, 0);
}

TEST_CASE("Layer time slowdown: layers before first_layer are untouched and do not constrain", "[LayerTimeSpeedSmoothing]")
{
    const std::vector<double> times = {300.0, 10.0, 10.0, 10.0};
    LayerTimeSlowdownParams   params;
    params.max_variation     = 0.25;
    params.max_slowdown      = 10.0;
    params.max_time_increase = 10.0;

    const auto result = solve_layer_time_slowdown(times, params, /*first_layer=*/1);
    REQUIRE_THAT(result.times[0], WithinAbs(300.0, 1e-9));
    REQUIRE_THAT(result.times[1], WithinAbs(10.0, 1e-9));
    REQUIRE_THAT(result.times[2], WithinAbs(10.0, 1e-9));
    REQUIRE_THAT(result.times[3], WithinAbs(10.0, 1e-9));
}

TEST_CASE("Layer time slowdown: zero max_slowdown is a no-op", "[LayerTimeSpeedSmoothing]")
{
    const std::vector<double> times = {10.0, 100.0, 10.0};
    LayerTimeSlowdownParams   params;
    params.max_variation     = 0.10;
    params.max_slowdown      = 0.0;
    params.max_time_increase = 1.0;

    const auto result = solve_layer_time_slowdown(times, params, /*first_layer=*/0);
    for (size_t i = 0; i < times.size(); ++i) {
        REQUIRE_THAT(result.times[i], WithinAbs(times[i], 1e-9));
        REQUIRE_THAT(result.speed_factors[i], WithinAbs(1.0, 1e-9));
    }
}

TEST_CASE("Layer time speed smoothing: process config defaults", "[LayerTimeSpeedSmoothing][Config]")
{
    const FullPrintConfig &defaults = FullPrintConfig::defaults();

    REQUIRE(defaults.layer_time_speed_smoothing.value == ltssmOff);
    REQUIRE_THAT(defaults.layer_time_speed_max_variation.value, WithinAbs(25.0, 1e-9));
    REQUIRE_THAT(defaults.layer_time_speed_max_speedup.value, WithinAbs(100.0, 1e-9));
    REQUIRE_THAT(defaults.layer_time_speed_max_slowdown.value, WithinAbs(200.0, 1e-9));
    REQUIRE_THAT(defaults.layer_time_speed_max_time_increase.value, WithinAbs(20.0, 1e-9));
    REQUIRE(defaults.layer_time_speed_slowdown_scope.value == ltssExcludeOuterWalls);

    REQUIRE(is_layer_time_speed_up(ltssmSpeedUpExcludeOuter));
    REQUIRE(is_layer_time_speed_up(ltssmSpeedUpAll));
    REQUIRE_FALSE(is_layer_time_speed_up(ltssmOff));
    REQUIRE_FALSE(is_layer_time_speed_up(ltssmSlowDown));
    REQUIRE(is_layer_time_slowdown(ltssmSlowDown));
    REQUIRE_FALSE(is_layer_time_slowdown(ltssmSpeedUpAll));

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    REQUIRE(config.has("layer_time_speed_smoothing"));
    REQUIRE(config.has("layer_time_speed_max_variation"));
    REQUIRE(config.has("layer_time_speed_max_speedup"));
    REQUIRE(config.has("layer_time_speed_max_slowdown"));
    REQUIRE(config.has("layer_time_speed_max_time_increase"));
    REQUIRE(config.has("layer_time_speed_slowdown_scope"));

    REQUIRE(config.opt_enum<LayerTimeSpeedSmoothMode>("layer_time_speed_smoothing") == ltssmOff);
    config.set_deserialize_strict("layer_time_speed_smoothing", "speed_up_exclude_outer");
    REQUIRE(config.opt_enum<LayerTimeSpeedSmoothMode>("layer_time_speed_smoothing") == ltssmSpeedUpExcludeOuter);
    config.set_deserialize_strict("layer_time_speed_smoothing", "speed_up_all");
    REQUIRE(config.opt_enum<LayerTimeSpeedSmoothMode>("layer_time_speed_smoothing") == ltssmSpeedUpAll);
    config.set_deserialize_strict("layer_time_speed_smoothing", "slow_down");
    REQUIRE(config.opt_enum<LayerTimeSpeedSmoothMode>("layer_time_speed_smoothing") == ltssmSlowDown);
    config.set_deserialize_strict("layer_time_speed_slowdown_scope", "all");
    REQUIRE(config.opt_enum<LayerTimeSlowdownScope>("layer_time_speed_slowdown_scope") == ltssAll);
}

TEST_CASE("Layer time speed smoothing keys are process preset options", "[LayerTimeSpeedSmoothing][Config]")
{
    // Changing a steps_gcode key must not force a geometry reslice. Print::invalidate_state_by_config_options
    // is protected; we only assert the keys exist on the process preset list so they survive save/load.
    const auto &keys = Preset::print_options();
    const char *wanted[] = {
        "layer_time_speed_smoothing",
        "layer_time_speed_max_variation",
        "layer_time_speed_max_speedup",
        "layer_time_speed_max_slowdown",
        "layer_time_speed_max_time_increase",
        "layer_time_speed_slowdown_scope",
    };
    for (const char *key : wanted) {
        const bool found = std::find(keys.begin(), keys.end(), key) != keys.end();
        REQUIRE(found);
    }
}

TEST_CASE("Layer time speed smoothing S3 stub: Off is identity", "[LayerTimeSpeedSmoothing][GCode]")
{
    PrintConfig cfg;
    REQUIRE(cfg.layer_time_speed_smoothing.value == ltssmOff);

    LayerTimeSpeedSmoothingFilter filter(cfg);
    REQUIRE_FALSE(filter.enabled());

    const std::string gcode = "G1 X10 Y10 F3000\nG1 X20 E0.4 F1800\n";
    REQUIRE(filter.process_layer(std::string(gcode)) == gcode);
    REQUIRE(filter.process_layer(std::string()) == "");
}

TEST_CASE("Layer time speed smoothing S3 stub: enabled modes emit factor=1 and do not rewrite F", "[LayerTimeSpeedSmoothing][GCode]")
{
    const LayerTimeSpeedSmoothMode modes[] = {ltssmSpeedUpExcludeOuter, ltssmSpeedUpAll, ltssmSlowDown};
    for (LayerTimeSpeedSmoothMode mode : modes) {
        DYNAMIC_SECTION("mode " << int(mode))
        {
            PrintConfig cfg;
            cfg.layer_time_speed_smoothing.value = mode;

            LayerTimeSpeedSmoothingFilter filter(cfg);
            REQUIRE(filter.enabled());

            const std::string gcode = "G1 X10 Y10 F3000\nG1 X20 E0.4 F1800\n";
            const std::string out   = filter.process_layer(std::string(gcode));

            const std::string comment = LayerTimeSpeedSmoothingFilter::format_comment(1.0, 0.0, 0.0);
            REQUIRE(comment == "; LAYER_TIME_SPEED_SMOOTH factor=1.000 t_raw=0.00 t_out=0.00\n");
            REQUIRE(out == comment + gcode);
            REQUIRE(out.find("F3000") != std::string::npos);
            REQUIRE(out.find("F1800") != std::string::npos);
            REQUIRE(filter.process_layer(std::string()) == "");
        }
    }
}
