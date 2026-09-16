#include <catch2/catch.hpp>

#include "libslic3r/GCode/LayerTimeSpeedSmoothingFilter.hpp"
#include "libslic3r/LayerTimeSpeedSmoothing.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>
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

TEST_CASE("Layer time slowdown: frozen layers keep their time and still bound their neighbours", "[LayerTimeSpeedSmoothing]")
{
    // 30, 8, 3 s at 25% variation. Layer 1 is the one CoolingBuffer already stretched to an
    // 8 s cooling floor, so the apply stage hands it in as frozen.
    // Free:   layer 1 rises to 0.75 * 30 = 22.5 s, layer 2 to 0.75 * 22.5 = 16.875 s.
    // Frozen: layer 1 stays 8 s (its cap is its own time), layer 2 rises only to 0.75 * 8 = 6 s,
    //         so its speed factor is 3 / 6 = 0.5.
    const std::vector<double> times = {30.0, 8.0, 3.0};
    LayerTimeSlowdownParams   params;
    params.max_variation     = 0.25;
    params.max_slowdown      = 100.0;
    params.max_time_increase = 100.0;

    const auto free = solve_layer_time_slowdown(times, params, /*first_layer=*/0);
    REQUIRE_THAT(free.times[1], WithinAbs(22.5, 1e-6));
    REQUIRE_THAT(free.times[2], WithinAbs(16.875, 1e-6));

    const auto frozen = solve_layer_time_slowdown(times, params, /*first_layer=*/0, {false, true, false});
    require_not_shortened(times, frozen.times);
    require_speed_factors(frozen, times);
    REQUIRE_THAT(frozen.times[0], WithinAbs(30.0, 1e-9));
    REQUIRE_THAT(frozen.times[1], WithinAbs(8.0, 1e-9));
    REQUIRE_THAT(frozen.times[2], WithinAbs(6.0, 1e-6));
    REQUIRE_THAT(frozen.speed_factors[1], WithinAbs(1.0, 1e-9));
    REQUIRE_THAT(frozen.speed_factors[2], WithinAbs(0.5, 1e-6));
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

static std::string g1_x(double x, double e, int f)
{
    std::ostringstream oss;
    oss << "G1 X" << x << " E" << e << " F" << f << "\n";
    return oss.str();
}

static int feedrate_of(const std::string &gcode, const char *needle)
{
    const size_t line = gcode.find(needle);
    REQUIRE(line != std::string::npos);
    const size_t fpos = gcode.find(" F", line);
    REQUIRE(fpos != std::string::npos);
    return std::atoi(gcode.c_str() + fpos + 2);
}

static size_t count_of(const std::string &haystack, const std::string &needle)
{
    size_t n = 0;
    for (size_t pos = haystack.find(needle); pos != std::string::npos; pos = haystack.find(needle, pos + needle.size()))
        ++n;
    return n;
}

// The flushed output, split at each per-layer diagnostic comment (one body per buffered layer).
static std::vector<std::string> smoothed_layer_bodies(const std::string &out)
{
    static const char *marker = "; LAYER_TIME_SPEED_SMOOTH";
    std::vector<std::string> bodies;
    for (size_t pos = out.find(marker); pos != std::string::npos;) {
        const size_t next = out.find(marker, pos + 1);
        bodies.push_back(out.substr(pos, next == std::string::npos ? std::string::npos : next - pos));
        pos = next;
    }
    return bodies;
}

// repeats moves of 10 mm each at feedrate f (mm/min): the layer takes repeats * 10 / (f / 60) s.
static std::string infill_layer(int repeats, int f)
{
    std::string g = "G92 X0\n;TYPE:Sparse infill\n";
    for (int i = 0; i < repeats; ++i)
        g += g1_x(10. * (i + 1), 0.05, f);
    return g;
}

TEST_CASE("Layer time speed smoothing: Off is identity and does not buffer", "[LayerTimeSpeedSmoothing][GCode]")
{
    PrintConfig cfg;
    REQUIRE(cfg.layer_time_speed_smoothing.value == ltssmOff);

    LayerTimeSpeedSmoothingFilter filter(cfg);
    REQUIRE_FALSE(filter.enabled());

    const std::string gcode = "G1 X10 Y10 F3000\nG1 X20 E0.4 F1800\n";
    REQUIRE(filter.process_layer(std::string(gcode)) == gcode);
    REQUIRE(filter.process_layer(std::string(gcode), 0, false) == gcode);
    REQUIRE(filter.process_layer(std::string()) == "");
}

TEST_CASE("Layer time speed smoothing: spiral vase is pass-through", "[LayerTimeSpeedSmoothing][GCode]")
{
    PrintConfig cfg;
    cfg.layer_time_speed_smoothing.value = ltssmSpeedUpAll;
    cfg.spiral_mode.value                = true;

    LayerTimeSpeedSmoothingFilter filter(cfg);
    REQUIRE(filter.enabled());

    const std::string gcode = "G1 X10 Y10 F3000\nG1 X20 E0.4 F1800\n";
    REQUIRE(filter.process_layer(std::string(gcode), 2, false) == gcode);
    REQUIRE(filter.process_layer(std::string(gcode), 3, true) == gcode);
}

TEST_CASE("Layer time speed smoothing: enabled modes buffer until last layer", "[LayerTimeSpeedSmoothing][GCode]")
{
    PrintConfig cfg;
    cfg.layer_time_speed_smoothing.value = ltssmSpeedUpAll;
    cfg.filament_max_volumetric_speed.values = { 1000. };

    LayerTimeSpeedSmoothingFilter filter(cfg);
    REQUIRE(filter.enabled());

    const std::string l0 = ";TYPE:Sparse infill\n" + g1_x(10, 0.4, 1800);
    REQUIRE(filter.process_layer(std::string(l0), 0, false).empty());
    REQUIRE(filter.process_layer(std::string(l0), 1, false).empty());

    const std::string out = filter.process_layer(std::string(l0), 2, true);
    REQUIRE(out.find("LAYER_TIME_SPEED_SMOOTH") != std::string::npos);
    REQUIRE(out.find("mode=speed_up_all") != std::string::npos);
    REQUIRE(filter.process_layer(std::string(), 0, true).empty());
}

TEST_CASE("Layer time speed smoothing: diagnostic includes mode and a single-layer print keeps F", "[LayerTimeSpeedSmoothing][GCode]")
{
    const LayerTimeSpeedSmoothMode modes[] = {ltssmSpeedUpExcludeOuter, ltssmSpeedUpAll, ltssmSlowDown};
    for (LayerTimeSpeedSmoothMode mode : modes) {
        DYNAMIC_SECTION("mode " << int(mode))
        {
            PrintConfig cfg;
            cfg.layer_time_speed_smoothing.value         = mode;
            cfg.filament_max_volumetric_speed.values     = { 1000. };

            LayerTimeSpeedSmoothingFilter filter(cfg);
            REQUIRE(filter.enabled());

            const std::string gcode = "G1 X10 Y10 F3000\nG1 X20 E0.4 F1800\n";
            const std::string out   = filter.process_layer(std::string(gcode));

            REQUIRE(out.find(LayerTimeSpeedSmoothingFilter::mode_key(mode)) != std::string::npos);
            REQUIRE(out.find("factor=1.000") != std::string::npos);
            REQUIRE(out.find("t_raw=") != std::string::npos);
            REQUIRE(out.find("t_out=") != std::string::npos);
            REQUIRE(out.find("F3000") != std::string::npos);
            REQUIRE(out.find("F1800") != std::string::npos);
            REQUIRE(filter.process_layer(std::string()) == "");
        }
    }
}

TEST_CASE("Layer time speed smoothing: Mode B speeds up a long layer and leaves the first layer", "[LayerTimeSpeedSmoothing][GCode]")
{
    PrintConfig cfg;
    cfg.layer_time_speed_smoothing.value     = ltssmSpeedUpAll;
    cfg.layer_time_speed_max_variation.value = 25.;
    cfg.layer_time_speed_max_speedup.value   = 100.;
    cfg.filament_max_volumetric_speed.values = { 1000. };
    cfg.use_relative_e_distances.value       = true;
    cfg.slow_down_for_layer_cooling.values   = { false };
    cfg.slow_down_layer_time.values          = { 0. };
    cfg.slow_down_min_speed.values           = { 0. };

    LayerTimeSpeedSmoothingFilter filter(cfg);

    // Layer 0 is the first-layer band (untouched). Layers 1 and 2 are 10 mm vs 100 mm
    // at the same F, so layer 2 is 10x longer and is pulled down by the speed-up solver.
    auto make_layer = [](int repeats, int f) {
        std::string g = "G92 X0\n;TYPE:Sparse infill\n";
        for (int i = 0; i < repeats; ++i)
            g += g1_x(10. * (i + 1), 0.05, f);
        return g;
    };

    REQUIRE(filter.process_layer(make_layer(2, 1800), 0, false).empty());
    REQUIRE(filter.process_layer(make_layer(2, 1800), 1, false).empty());
    const std::string out = filter.process_layer(make_layer(20, 1800), 2, true);

    REQUIRE(out.find("mode=speed_up_all") != std::string::npos);

    size_t c0 = out.find("LAYER_TIME_SPEED_SMOOTH");
    size_t c1 = out.find("LAYER_TIME_SPEED_SMOOTH", c0 + 1);
    size_t c2 = out.find("LAYER_TIME_SPEED_SMOOTH", c1 + 1);
    REQUIRE(c2 != std::string::npos);
    const std::string first_body = out.substr(c0, c1 - c0);
    const std::string last_body  = out.substr(c2);
    REQUIRE(first_body.find("factor=1.000") != std::string::npos);
    REQUIRE(feedrate_of(first_body, "G1 X") == 1800);
    REQUIRE(feedrate_of(last_body, "G1 X") > 1800);
}

TEST_CASE("Layer time speed smoothing: Mode A does not rewrite outer-wall F", "[LayerTimeSpeedSmoothing][GCode]")
{
    PrintConfig cfg;
    cfg.layer_time_speed_smoothing.value     = ltssmSpeedUpExcludeOuter;
    cfg.layer_time_speed_max_variation.value = 25.;
    cfg.layer_time_speed_max_speedup.value   = 100.;
    cfg.filament_max_volumetric_speed.values = { 1000. };
    cfg.use_relative_e_distances.value       = true;
    cfg.slow_down_for_layer_cooling.values   = { false };
    cfg.slow_down_layer_time.values          = { 0. };
    cfg.slow_down_min_speed.values           = { 0. };

    LayerTimeSpeedSmoothingFilter filter(cfg);

    auto make_mixed = [](int infill_repeats) {
        std::string g = "G92 X0\n;TYPE:Sparse infill\n";
        for (int i = 0; i < infill_repeats; ++i)
            g += g1_x(10. * (i + 1), 0.05, 1800);
        g += ";TYPE:Outer wall\n";
        g += g1_x(10. * (infill_repeats + 1), 0.05, 1800);
        return g;
    };

    REQUIRE(filter.process_layer(make_mixed(2), 0, false).empty());
    REQUIRE(filter.process_layer(make_mixed(2), 1, false).empty());
    const std::string out = filter.process_layer(make_mixed(20), 2, true);

    const size_t last_comment = out.rfind("LAYER_TIME_SPEED_SMOOTH");
    const std::string last_body = out.substr(last_comment);
    REQUIRE(last_body.find("TYPE:Outer wall") != std::string::npos);
    REQUIRE(feedrate_of(last_body, "TYPE:Outer wall") == 1800);
    REQUIRE(feedrate_of(last_body, "TYPE:Sparse infill") > 1800);
}

TEST_CASE("Layer time speed smoothing: never speeds up overhang, bridge, ironing, top solid or support", "[LayerTimeSpeedSmoothing][GCode]")
{
    PrintConfig cfg;
    cfg.layer_time_speed_smoothing.value     = ltssmSpeedUpAll;
    cfg.layer_time_speed_max_variation.value = 25.;
    cfg.layer_time_speed_max_speedup.value   = 100.;
    cfg.filament_max_volumetric_speed.values = { 1000. };
    cfg.use_relative_e_distances.value       = true;
    cfg.slow_down_for_layer_cooling.values   = { false };
    cfg.slow_down_layer_time.values          = { 0. };
    cfg.slow_down_min_speed.values           = { 0. };

    const char *roles[] = {"Overhang wall", "Bridge", "Internal Bridge", "Ironing", "Top surface",
                           "Support", "Support interface", "Support transition"};
    for (const char *role : roles) {
        DYNAMIC_SECTION(role)
        {
            LayerTimeSpeedSmoothingFilter filter(cfg);
            auto make_layer = [role](int repeats) {
                std::string g = std::string("G92 X0\n;TYPE:") + role + "\n";
                for (int i = 0; i < repeats; ++i)
                    g += g1_x(10. * (i + 1), 0.05, 1800);
                return g;
            };
            REQUIRE(filter.process_layer(make_layer(2), 0, false).empty());
            REQUIRE(filter.process_layer(make_layer(2), 1, false).empty());
            const std::string out = filter.process_layer(make_layer(20), 2, true);
            const std::string last_body = out.substr(out.rfind("LAYER_TIME_SPEED_SMOOTH"));
            REQUIRE(feedrate_of(last_body, "G1 X") == 1800);
        }
    }
}

TEST_CASE("Layer time speed smoothing: speed-up skips lines already at min print speed", "[LayerTimeSpeedSmoothing][GCode]")
{
    PrintConfig cfg;
    cfg.layer_time_speed_smoothing.value     = ltssmSpeedUpAll;
    cfg.layer_time_speed_max_variation.value = 25.;
    cfg.layer_time_speed_max_speedup.value   = 100.;
    cfg.filament_max_volumetric_speed.values = { 1000. };
    cfg.use_relative_e_distances.value       = true;
    cfg.slow_down_for_layer_cooling.values   = { false };
    cfg.slow_down_layer_time.values          = { 0. };
    cfg.slow_down_min_speed.values           = { 30. }; // 1800 mm/min

    LayerTimeSpeedSmoothingFilter filter(cfg);
    auto make_layer = [](int repeats) {
        std::string g = "G92 X0\n;TYPE:Sparse infill\n";
        for (int i = 0; i < repeats; ++i)
            g += g1_x(10. * (i + 1), 0.05, 1800);
        return g;
    };
    REQUIRE(filter.process_layer(make_layer(2), 0, false).empty());
    REQUIRE(filter.process_layer(make_layer(2), 1, false).empty());
    const std::string out = filter.process_layer(make_layer(20), 2, true);
    const std::string last_body = out.substr(out.rfind("LAYER_TIME_SPEED_SMOOTH"));
    REQUIRE(feedrate_of(last_body, "G1 X") == 1800);
}

TEST_CASE("Layer time speed smoothing: volumetric clamp caps F_new", "[LayerTimeSpeedSmoothing][GCode]")
{
    PrintConfig cfg;
    cfg.layer_time_speed_smoothing.value     = ltssmSpeedUpAll;
    cfg.layer_time_speed_max_variation.value = 25.;
    cfg.layer_time_speed_max_speedup.value   = 1000.;
    cfg.filament_diameter.values             = { 1.75 };
    cfg.filament_max_volumetric_speed.values = { 2. };
    cfg.use_relative_e_distances.value       = true;
    cfg.slow_down_for_layer_cooling.values   = { false };
    cfg.slow_down_layer_time.values          = { 0. };

    LayerTimeSpeedSmoothingFilter filter(cfg);

    auto make_layer = [](int repeats, int f) {
        std::string g = "G92 X0\n;TYPE:Sparse infill\n";
        for (int i = 0; i < repeats; ++i)
            g += g1_x(10. * (i + 1), 0.4, f);
        return g;
    };

    REQUIRE(filter.process_layer(make_layer(2, 6000), 0, false).empty());
    REQUIRE(filter.process_layer(make_layer(2, 6000), 1, false).empty());
    const std::string out = filter.process_layer(make_layer(20, 6000), 2, true);

    const std::string last_body = out.substr(out.rfind("LAYER_TIME_SPEED_SMOOTH"));
    const int f_new = feedrate_of(last_body, "G1 X");
    // mm3_per_mm = 0.4 * pi*(1.75/2)^2 / 10 ≈ 0.0962; cap = 60*2/0.0962 ≈ 1247 mm/min.
    REQUIRE(f_new <= 1300);
    REQUIRE(f_new < 6000);
}

TEST_CASE("Layer time speed smoothing: cooling floor stops a speed-up", "[LayerTimeSpeedSmoothing][GCode]")
{
    PrintConfig cfg;
    cfg.layer_time_speed_smoothing.value     = ltssmSpeedUpAll;
    cfg.layer_time_speed_max_variation.value = 25.;
    cfg.layer_time_speed_max_speedup.value   = 100.;
    cfg.filament_max_volumetric_speed.values = { 1000. };
    cfg.use_relative_e_distances.value       = true;
    cfg.slow_down_for_layer_cooling.values   = { true };
    cfg.slow_down_layer_time.values          = { 1000. }; // well above any synthetic layer time

    LayerTimeSpeedSmoothingFilter filter(cfg);
    auto make_layer = [](int repeats) {
        std::string g = "G92 X0\n;TYPE:Sparse infill\n";
        for (int i = 0; i < repeats; ++i)
            g += g1_x(10. * (i + 1), 0.05, 1800);
        return g;
    };
    REQUIRE(filter.process_layer(make_layer(2), 0, false).empty());
    REQUIRE(filter.process_layer(make_layer(2), 1, false).empty());
    const std::string out = filter.process_layer(make_layer(20), 2, true);
    const std::string last_body = out.substr(out.rfind("LAYER_TIME_SPEED_SMOOTH"));
    REQUIRE(last_body.find("factor=1.000") != std::string::npos);
    REQUIRE(feedrate_of(last_body, "G1 X") == 1800);
}

TEST_CASE("Layer time speed smoothing: Mode C slows a short layer", "[LayerTimeSpeedSmoothing][GCode]")
{
    PrintConfig cfg;
    cfg.layer_time_speed_smoothing.value          = ltssmSlowDown;
    cfg.layer_time_speed_max_variation.value      = 25.;
    cfg.layer_time_speed_max_slowdown.value       = 200.;
    cfg.layer_time_speed_max_time_increase.value  = 100.;
    cfg.layer_time_speed_slowdown_scope.value     = ltssAll;
    cfg.filament_max_volumetric_speed.values      = { 1000. };
    cfg.use_relative_e_distances.value            = true;
    cfg.slow_down_for_layer_cooling.values        = { false };
    cfg.slow_down_layer_time.values               = { 0. };

    LayerTimeSpeedSmoothingFilter filter(cfg);
    auto make_layer = [](int repeats) {
        std::string g = "G92 X0\n;TYPE:Sparse infill\n";
        for (int i = 0; i < repeats; ++i)
            g += g1_x(10. * (i + 1), 0.05, 1800);
        return g;
    };
    REQUIRE(filter.process_layer(make_layer(20), 0, false).empty());
    REQUIRE(filter.process_layer(make_layer(2), 1, false).empty());
    const std::string out = filter.process_layer(make_layer(20), 2, true);

    REQUIRE(out.find("mode=slow_down") != std::string::npos);
    // Middle short layer (id 1) is lengthened: F drops below 1800.
    size_t c0 = out.find("LAYER_TIME_SPEED_SMOOTH");
    size_t c1 = out.find("LAYER_TIME_SPEED_SMOOTH", c0 + 1);
    size_t c2 = out.find("LAYER_TIME_SPEED_SMOOTH", c1 + 1);
    REQUIRE(c2 != std::string::npos);
    const std::string mid = out.substr(c1, c2 - c1);
    REQUIRE(feedrate_of(mid, "G1 X") < 1800);
}

TEST_CASE("Layer time speed smoothing: Mode C keeps a layer CoolingBuffer already slowed to the cooling floor", "[LayerTimeSpeedSmoothing][GCode]")
{
    PrintConfig cfg;
    cfg.layer_time_speed_smoothing.value          = ltssmSlowDown;
    cfg.layer_time_speed_max_variation.value      = 25.;
    cfg.layer_time_speed_max_slowdown.value       = 200.;
    cfg.layer_time_speed_max_time_increase.value  = 100.;
    cfg.layer_time_speed_slowdown_scope.value     = ltssAll;
    cfg.filament_max_volumetric_speed.values      = { 1000. };
    cfg.use_relative_e_distances.value            = true;
    cfg.slow_down_for_layer_cooling.values        = { true };
    cfg.slow_down_layer_time.values               = { 8. };
    cfg.slow_down_min_speed.values                = { 20. }; // 1200 mm/min

    // Layer 0 is the first-layer band. Layers 1 and 3: 100 x 10 mm at 2000 mm/min = 33.33 mm/s
    // -> 30 s each. Layer 2: 20 x 10 mm at 1500 mm/min = 25 mm/s -> 8.0 s, exactly the cooling
    // floor, and it is flagged as the layer CoolingBuffer slowed to get there.
    //
    // Frozen (cooling_slowed_down = true): layer 2 keeps factor 1, F1500, t_out 8.00.
    // Control (flag off): the 25% band lifts it to min(3 x 8, 0.75 x 30) = 22.5 s, factor
    // 8 / 22.5 = 0.356, F = 1500 x 0.356 = 533 -> floored at slow_down_min_speed 1200,
    // so t_out = 200 mm / 20 mm/s = 10.00 s. The control proves the freeze is what holds F.
    for (const bool slowed_by_cooling : {true, false}) {
        DYNAMIC_SECTION((slowed_by_cooling ? "flagged by CoolingBuffer" : "control: not flagged"))
        {
            LayerTimeSpeedSmoothingFilter filter(cfg);
            REQUIRE(filter.process_layer(infill_layer(2, 2000), 0, false).empty());
            REQUIRE(filter.process_layer(infill_layer(100, 2000), 1, false).empty());
            REQUIRE(filter.process_layer(infill_layer(20, 1500), 2, false, slowed_by_cooling).empty());
            const std::string out = filter.process_layer(infill_layer(100, 2000), 3, true);

            const std::vector<std::string> bodies = smoothed_layer_bodies(out);
            REQUIRE(bodies.size() == 4);
            REQUIRE(bodies[2].find("t_raw=8.00") != std::string::npos);
            if (slowed_by_cooling) {
                REQUIRE(bodies[2].find("factor=1.000") != std::string::npos);
                REQUIRE(bodies[2].find("t_out=8.00") != std::string::npos);
                REQUIRE(feedrate_of(bodies[2], "G1 X") == 1500);
                REQUIRE(count_of(bodies[2], "F1500") == 20);
            } else {
                REQUIRE(bodies[2].find("factor=0.356") != std::string::npos);
                REQUIRE(bodies[2].find("t_out=10.00") != std::string::npos);
                REQUIRE(feedrate_of(bodies[2], "G1 X") == 1200);
                REQUIRE(count_of(bodies[2], "F1200") == 20);
            }
            // The 30 s neighbours are long layers: Mode C never shortens them.
            REQUIRE(bodies[1].find("factor=1.000") != std::string::npos);
            REQUIRE(bodies[3].find("factor=1.000") != std::string::npos);
            REQUIRE(feedrate_of(bodies[1], "G1 X") == 2000);
            REQUIRE(feedrate_of(bodies[3], "G1 X") == 2000);
        }
    }
}

TEST_CASE("Layer time speed smoothing: a slowdown never pushes a line below slow_down_min_speed", "[LayerTimeSpeedSmoothing][GCode]")
{
    PrintConfig cfg;
    cfg.layer_time_speed_smoothing.value          = ltssmSlowDown;
    cfg.layer_time_speed_max_variation.value      = 25.;
    cfg.layer_time_speed_max_slowdown.value       = 200.; // a layer may take up to 3x its time
    cfg.layer_time_speed_max_time_increase.value  = 100.;
    cfg.layer_time_speed_slowdown_scope.value     = ltssAll;
    cfg.filament_max_volumetric_speed.values      = { 1000. };
    cfg.use_relative_e_distances.value            = true;
    cfg.slow_down_for_layer_cooling.values        = { false };
    cfg.slow_down_layer_time.values               = { 0. };
    cfg.slow_down_min_speed.values                = { 20. }; // 1200 mm/min

    // Layers 1 and 3: 30 s (100 x 10 mm at 33.33 mm/s). Layer 2: 10 x 10 mm at 3000 mm/min
    // = 50 mm/s -> 2.0 s, plus one 10 mm move at 900 mm/min = 15 mm/s -> 0.667 s, so 2.667 s.
    // The band would lift it to 0.75 x 30 = 22.5 s but max_slowdown caps it at 3 x 2.667 = 8.0 s:
    // factor = 2.667 / 8 = 0.333. The 3000 lines would become 1000 mm/min, below the 1200 floor,
    // so they are written as F1200; the 900 line is already below the floor and is left alone.
    // t_out = 100 mm / 20 mm/s + 0.667 s = 5.67 s.
    LayerTimeSpeedSmoothingFilter filter(cfg);
    REQUIRE(filter.process_layer(infill_layer(2, 3000), 0, false).empty());
    REQUIRE(filter.process_layer(infill_layer(100, 2000), 1, false).empty());
    REQUIRE(filter.process_layer(infill_layer(10, 3000) + g1_x(110, 0.05, 900), 2, false).empty());
    const std::string out = filter.process_layer(infill_layer(100, 2000), 3, true);

    const std::vector<std::string> bodies = smoothed_layer_bodies(out);
    REQUIRE(bodies.size() == 4);
    REQUIRE(bodies[2].find("factor=0.333") != std::string::npos);
    REQUIRE(bodies[2].find("t_out=5.67") != std::string::npos);
    REQUIRE(feedrate_of(bodies[2], "G1 X") == 1200);
    REQUIRE(count_of(bodies[2], "F1200") == 10);
    REQUIRE(count_of(bodies[2], "F900") == 1);
    REQUIRE(count_of(bodies[2], "F1000") == 0);
}

TEST_CASE("Layer time speed smoothing format_comment includes mode", "[LayerTimeSpeedSmoothing][GCode]")
{
    const std::string comment = LayerTimeSpeedSmoothingFilter::format_comment(ltssmSpeedUpExcludeOuter, 1.0, 0.0, 0.0);
    REQUIRE(comment == "; LAYER_TIME_SPEED_SMOOTH mode=speed_up_exclude_outer factor=1.000 t_raw=0.00 t_out=0.00\n");
}
