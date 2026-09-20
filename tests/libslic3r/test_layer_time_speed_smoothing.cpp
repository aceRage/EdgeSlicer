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

    // Layers 0 and 1: 2 x 10 mm at 1800 mm/min = 30 mm/s -> 0.667 s. Layer 2: one sparse-infill
    // control move plus 20 moves of the protected role, all 10 mm at 30 mm/s -> 21 x 0.333 = 7.0 s.
    // The 25% band would pull layer 2 to 0.667 / 0.75 = 0.889 s, but max_speedup 100% floors it
    // at 7.0 / 2 = 3.5 s, so the layer factor is exactly 2.000. The control move is eligible and
    // must be written as F3600; the protected moves, in the same layer under the same factor,
    // must stay F1800. Without the control the test could not tell "protected" from "the layer
    // was never sped up at all".
    const char *roles[] = {"Overhang wall", "Bridge", "Internal Bridge", "Ironing", "Top surface",
                           "Support", "Support interface", "Support transition"};
    for (const char *role : roles) {
        DYNAMIC_SECTION(role)
        {
            LayerTimeSpeedSmoothingFilter filter(cfg);
            auto make_layer = [role](int repeats) {
                std::string g = "G92 X0\n;TYPE:Sparse infill\n" + g1_x(10, 0.05, 1800);
                g += std::string(";TYPE:") + role + "\n";
                for (int i = 0; i < repeats; ++i)
                    g += g1_x(10. * (i + 2), 0.05, 1800);
                return g;
            };
            REQUIRE(filter.process_layer(infill_layer(2, 1800), 0, false).empty());
            REQUIRE(filter.process_layer(infill_layer(2, 1800), 1, false).empty());
            const std::string out = filter.process_layer(make_layer(20), 2, true);

            const std::vector<std::string> bodies = smoothed_layer_bodies(out);
            REQUIRE(bodies.size() == 3);
            REQUIRE(bodies[2].find("factor=2.000") != std::string::npos);
            REQUIRE(feedrate_of(bodies[2], "TYPE:Sparse infill") == 3600);
            REQUIRE(feedrate_of(bodies[2], std::string(";TYPE:" + std::string(role)).c_str()) == 1800);
            REQUIRE(count_of(bodies[2], "F3600") == 1);
            REQUIRE(count_of(bodies[2], "F1800") == 20);
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
    // Control (flag off): the 25% band targets min(3 x 8, 0.75 x 30) = 22.5 s. Every line in
    // this layer is eligible and floors at slow_down_min_speed (1200) for any factor <=
    // 1200/1500 = 0.8, so t_out(f) is flat at 200mm/(1200/60) = 10.00 s for every f in the
    // slowdown search range [f_cap, 1] with f_cap = 1/(1+max_slowdown) = 1/3. 10.00 s never
    // reaches the 22.5 s target, so the bisection reports the target unreachable and returns
    // f_cap itself: factor = 1/3 = 0.333, t_out = 10.00 (same G-code as the old direct-factor
    // 0.356 would have produced, since both floor at F1200 - only the *reported* factor number
    // changes). The control proves the freeze is what holds F.
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
                REQUIRE(bodies[2].find("factor=0.333") != std::string::npos);
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
    const std::string comment = LayerTimeSpeedSmoothingFilter::format_comment(ltssmSpeedUpExcludeOuter, 1.0, 0.0, 0.0, 0.0);
    REQUIRE(comment == "; LAYER_TIME_SPEED_SMOOTH mode=speed_up_exclude_outer factor=1.000 t_raw=0.00 t_out=0.00 t_target=0.00\n");
}

// ---------------------------------------------------------------------------------------------
// Defect 1: the prime/wipe tower (;TYPE:Prime tower -> erWipeTower) must never be retimed, in
// any mode. Before the fix, is_speedup_protected() did not name erWipeTower, so Mode A/B would
// speed the tower up along with everything else (the owner's real print: layer 2, factor 2.392,
// tower F9600 -> F22960); Mode C had no protection for it at all.
// ---------------------------------------------------------------------------------------------

TEST_CASE("Layer time speed smoothing: the prime tower is never sped up (Mode B)", "[LayerTimeSpeedSmoothing][GCode][Tower]")
{
    PrintConfig cfg;
    cfg.layer_time_speed_smoothing.value     = ltssmSpeedUpAll;
    cfg.layer_time_speed_max_variation.value = 25.;
    cfg.layer_time_speed_max_speedup.value   = 100.; // f_cap = 2.0
    cfg.filament_max_volumetric_speed.values = { 1000. };
    cfg.use_relative_e_distances.value       = true;
    cfg.slow_down_for_layer_cooling.values   = { false };
    cfg.slow_down_layer_time.values          = { 0. };
    cfg.slow_down_min_speed.values           = { 0. };

    // Layers 0 (first-layer band, id 0) and 1 (id 1, the constraining neighbour): 12 x 10 mm at
    // 1800 mm/min = 30 mm/s -> 12 x 0.33333 = 4.0 s each. Layer 2 (id 2): 24 x 10 mm at
    // 1800 mm/min sparse infill (eligible, 24 x 0.33333 = 8.0 s) plus 2 x 10 mm tower moves at
    // 600 mm/min = 10 mm/s -> 1.0 s each, 2.0 s total (ineligible: erWipeTower). Raw = 10.0 s.
    //
    // The pure S2 solver sees only layer times [4.0, 4.0, 10.0] and knows nothing of roles: at
    // 25% variation the band pulls layer 2 to neighbour_upper(4.0, 0.75) = 4.0 / 0.75 = 5.3333 s
    // (the max_speedup floor of 10.0 / 2.0 = 5.0 s does not bind, since 5.3333 > 5.0), so
    // target = 16/3 s.
    //
    // Defect 2's bisection then finds the per-line factor f on the eligible 8.0 s alone so that
    // t_out(f) = t_tower + t_infill/f = 2.0 + 8.0/f hits that target exactly:
    //   2.0 + 8.0/f = 16/3  =>  8.0/f = 10/3  =>  f = 2.4
    // NOTE: f_cap here is 2.0 (max_speedup 100%), and 2.4 > 2.0, so the target is in fact
    // unreachable at the cap; solve_line_factor_for_target returns f_cap = 2.0 and the true
    // (higher) t_out is reported. See the arithmetic asserted below.
    auto make_layer = []() {
        std::string g = "G92 X0\n;TYPE:Sparse infill\n";
        for (int i = 0; i < 12; ++i)
            g += g1_x(10. * (i + 1), 0.05, 1800);
        return g;
    };
    auto make_mixed_layer = []() {
        std::string g = "G92 X0\n;TYPE:Sparse infill\n";
        for (int i = 0; i < 24; ++i)
            g += g1_x(10. * (i + 1), 0.05, 1800);
        g += ";TYPE:Prime tower\n";
        g += g1_x(250, 0.05, 600);
        g += g1_x(260, 0.05, 600);
        return g;
    };

    LayerTimeSpeedSmoothingFilter filter(cfg);
    REQUIRE(filter.process_layer(make_layer(), 0, false).empty());
    REQUIRE(filter.process_layer(make_layer(), 1, false).empty());
    const std::string out = filter.process_layer(make_mixed_layer(), 2, true);

    const std::vector<std::string> bodies = smoothed_layer_bodies(out);
    REQUIRE(bodies.size() == 3);
    // f_cap (2.0) cannot reach the naive 16/3 s target (2.0 + 8.0/2.0 = 6.0 s > 5.3333 s), so the
    // bisection reports the cap itself and the true (higher) t_out.
    REQUIRE(bodies[2].find("factor=2.000") != std::string::npos);
    REQUIRE(bodies[2].find("t_raw=10.00") != std::string::npos);
    REQUIRE(bodies[2].find("t_out=6.00") != std::string::npos);
    // The tower keeps its own F, no matter the layer factor.
    REQUIRE(feedrate_of(bodies[2], "TYPE:Prime tower") == 600);
    REQUIRE(count_of(bodies[2], "F600") == 2);
    // The infill doubled: 1800 * 2.0 = 3600.
    REQUIRE(feedrate_of(bodies[2], "TYPE:Sparse infill") == 3600);
    REQUIRE(count_of(bodies[2], "F3600") == 24);
}

TEST_CASE("Layer time speed smoothing: the prime tower is never slowed down (Mode C)", "[LayerTimeSpeedSmoothing][GCode][Tower]")
{
    PrintConfig cfg;
    cfg.layer_time_speed_smoothing.value         = ltssmSlowDown;
    cfg.layer_time_speed_max_variation.value     = 25.;
    cfg.layer_time_speed_max_slowdown.value      = 200.; // f_cap = 1/3
    cfg.layer_time_speed_max_time_increase.value = 100.;
    cfg.layer_time_speed_slowdown_scope.value    = ltssAll;
    cfg.filament_max_volumetric_speed.values     = { 1000. };
    cfg.use_relative_e_distances.value           = true;
    cfg.slow_down_for_layer_cooling.values       = { false };
    cfg.slow_down_layer_time.values              = { 0. };
    cfg.slow_down_min_speed.values                = { 0. };

    // Layers 1 and 3 (ids 1, 3): 30 x 10 mm at 1800 mm/min -> 30 x 0.33333 = 10.0 s each, the
    // long neighbours. Layer 2 (id 2, short): 9 x 10 mm sparse infill at 1800 mm/min (eligible,
    // 9 x 0.33333 = 3.0 s) plus 1 x 10 mm tower move at 400 mm/min = 6.6667 mm/s -> 1.5 s
    // (ineligible: erWipeTower). Raw = 4.5 s.
    //
    // The pure solver sees layer times [_, 10.0, 4.5, 10.0] (layer 0 is the first-layer band)
    // and targets neighbour_lower(10.0, 0.75) = 7.5 s for layer 2.
    //
    // Defect 2's bisection finds f on the eligible 3.0 s so t_out(f) = 1.5 + 3.0/f = 7.5:
    //   3.0/f = 6.0  =>  f = 0.5 (inside f_cap = 1/3 .. 1, so reachable).
    auto make_long_layer = []() {
        std::string g = "G92 X0\n;TYPE:Sparse infill\n";
        for (int i = 0; i < 30; ++i)
            g += g1_x(10. * (i + 1), 0.05, 1800);
        return g;
    };
    auto make_short_mixed_layer = []() {
        // g1_x takes an ABSOLUTE X target, so each move must advance by only 10 mm from the
        // previous one: 9 infill moves reach X=90, so the tower move targets X=100 (a 10 mm
        // move), not some far-off absolute X (which would silently make it a much longer move).
        std::string g = "G92 X0\n;TYPE:Sparse infill\n";
        for (int i = 0; i < 9; ++i)
            g += g1_x(10. * (i + 1), 0.05, 1800);
        g += ";TYPE:Prime tower\n";
        g += g1_x(100, 0.05, 400);
        return g;
    };

    LayerTimeSpeedSmoothingFilter filter(cfg);
    REQUIRE(filter.process_layer(make_long_layer(), 0, false).empty());
    REQUIRE(filter.process_layer(make_long_layer(), 1, false).empty());
    REQUIRE(filter.process_layer(make_short_mixed_layer(), 2, false).empty());
    const std::string out = filter.process_layer(make_long_layer(), 3, true);

    const std::vector<std::string> bodies = smoothed_layer_bodies(out);
    REQUIRE(bodies.size() == 4);
    REQUIRE(bodies[2].find("t_raw=4.50") != std::string::npos);
    REQUIRE(bodies[2].find("factor=0.500") != std::string::npos);
    REQUIRE(bodies[2].find("t_out=7.50") != std::string::npos);
    REQUIRE(bodies[2].find("t_target=7.50") != std::string::npos);
    // The tower keeps its own F even though the layer as a whole is lengthened.
    REQUIRE(feedrate_of(bodies[2], "TYPE:Prime tower") == 400);
    REQUIRE(count_of(bodies[2], "F400") == 1);
    // The infill is halved: 1800 * 0.5 = 900.
    REQUIRE(feedrate_of(bodies[2], "TYPE:Sparse infill") == 900);
    REQUIRE(count_of(bodies[2], "F900") == 9);
}

TEST_CASE("Layer time speed smoothing: the prime tower is never touched in Mode A either", "[LayerTimeSpeedSmoothing][GCode][Tower]")
{
    // Mode A (exclude outer walls) shares is_speedup_protected() / is_tower_role() with Mode B;
    // this only confirms the tower guard is not accidentally specific to ltssmSpeedUpAll.
    PrintConfig cfg;
    cfg.layer_time_speed_smoothing.value     = ltssmSpeedUpExcludeOuter;
    cfg.layer_time_speed_max_variation.value = 25.;
    cfg.layer_time_speed_max_speedup.value   = 100.;
    cfg.filament_max_volumetric_speed.values = { 1000. };
    cfg.use_relative_e_distances.value       = true;
    cfg.slow_down_for_layer_cooling.values   = { false };
    cfg.slow_down_layer_time.values          = { 0. };
    cfg.slow_down_min_speed.values           = { 0. };

    auto make_layer = []() {
        std::string g = "G92 X0\n;TYPE:Sparse infill\n";
        for (int i = 0; i < 12; ++i)
            g += g1_x(10. * (i + 1), 0.05, 1800);
        return g;
    };
    auto make_mixed_layer = []() {
        std::string g = "G92 X0\n;TYPE:Sparse infill\n";
        for (int i = 0; i < 24; ++i)
            g += g1_x(10. * (i + 1), 0.05, 1800);
        g += ";TYPE:Prime tower\n";
        g += g1_x(250, 0.05, 600);
        g += g1_x(260, 0.05, 600);
        return g;
    };

    LayerTimeSpeedSmoothingFilter filter(cfg);
    REQUIRE(filter.process_layer(make_layer(), 0, false).empty());
    REQUIRE(filter.process_layer(make_layer(), 1, false).empty());
    const std::string out = filter.process_layer(make_mixed_layer(), 2, true);

    const std::vector<std::string> bodies = smoothed_layer_bodies(out);
    REQUIRE(bodies.size() == 3);
    REQUIRE(feedrate_of(bodies[2], "TYPE:Prime tower") == 600);
    REQUIRE(count_of(bodies[2], "F600") == 2);
    REQUIRE(feedrate_of(bodies[2], "TYPE:Sparse infill") > 1800);
}

// ---------------------------------------------------------------------------------------------
// Defect 2: the layer factor the S2 solvers compute assumes the whole layer scales uniformly,
// but only eligible lines are ever rewritten and scaled_feedrate() caps them further. The apply
// stage must bisect a per-line factor that reaches the solver's target on the eligible lines
// alone, accounting for what the ineligible lines and the caps hold fixed.
// ---------------------------------------------------------------------------------------------

TEST_CASE("Layer time speed smoothing: eligible-only bisection reaches the target when half the layer is protected",
          "[LayerTimeSpeedSmoothing][GCode][BisectTarget]")
{
    // Hand-checkable arithmetic (spec item a/b combined): normalize the layer to 1.0 s with
    // exactly half its time protected. Halving the whole layer (target = 0.5) with a factor of
    // 3 applied to only the eligible half gives t = 0.5 + 0.5/3 = 0.6667, i.e. 66.7% of the
    // original remains even though the naive per-layer factor implied by "halve the layer" is
    // 2 - because half the time cannot be touched. Scaled by 8x to real seconds:
    //   protected (Support role, ineligible) = 4.0 s, eligible (Sparse infill) = 4.0 s, raw = 8.0 s.
    // Neighbour layer 1 is 3.0 s, so at 25% variation the band asks for
    // neighbour_upper(3.0, 0.75) = 3.0 / 0.75 = 4.0 s for layer 2 (the max_speedup floor of
    // 8.0 / 3.0 = 2.6667 s does not bind, since 4.0 > 2.6667), i.e. target = raw / 2 = 4.0 s -
    // "halve the layer".
    //
    // solve_line_factor_for_target must search up to f_cap = 1 + max_speedup = 3.0 for the
    // smallest f with t_out(f) = 4.0 (protected) + 4.0/f <= 4.0 s. That equation has no finite
    // solution (t_out(f) -> 4.0 only as f -> infinity), so f_cap itself is the answer:
    //   t_out(3.0) = 4.0 + 4.0/3.0 = 16/3 = 5.3333 s (the target is unreachable by design; the
    // header comment on the filter documents that the cap can leave the band violated).
    PrintConfig cfg;
    cfg.layer_time_speed_smoothing.value     = ltssmSpeedUpAll;
    cfg.layer_time_speed_max_variation.value = 25.;
    cfg.layer_time_speed_max_speedup.value   = 200.; // f_cap = 3.0
    cfg.filament_max_volumetric_speed.values = { 1000. };
    cfg.use_relative_e_distances.value       = true;
    cfg.slow_down_for_layer_cooling.values   = { false };
    cfg.slow_down_layer_time.values          = { 0. };
    cfg.slow_down_min_speed.values           = { 0. };

    auto make_neighbour = []() {
        std::string g = "G92 X0\n;TYPE:Sparse infill\n";
        for (int i = 0; i < 9; ++i)
            g += g1_x(10. * (i + 1), 0.05, 1800);
        return g;
    };
    auto make_half_protected_layer = []() {
        std::string g = "G92 X0\n;TYPE:Sparse infill\n";
        for (int i = 0; i < 12; ++i)
            g += g1_x(10. * (i + 1), 0.05, 1800);
        g += ";TYPE:Support\n";
        for (int i = 0; i < 12; ++i)
            g += g1_x(10. * (i + 13), 0.05, 1800);
        return g;
    };

    LayerTimeSpeedSmoothingFilter filter(cfg);
    REQUIRE(filter.process_layer(make_neighbour(), 0, false).empty());
    REQUIRE(filter.process_layer(make_neighbour(), 1, false).empty());
    const std::string out = filter.process_layer(make_half_protected_layer(), 2, true);

    const std::vector<std::string> bodies = smoothed_layer_bodies(out);
    REQUIRE(bodies.size() == 3);
    REQUIRE(bodies[2].find("t_raw=8.00") != std::string::npos);
    REQUIRE(bodies[2].find("t_target=4.00") != std::string::npos);
    // Bisection stops at the cap: it cannot reach the 4.0 s target (protected lines alone hold
    // 4.0 s fixed), so it reports f_cap = 3.0 and the true, higher t_out.
    REQUIRE(bodies[2].find("factor=3.000") != std::string::npos);
    REQUIRE(bodies[2].find("t_out=5.33") != std::string::npos);
    // Support (protected) keeps F1800; sparse infill (eligible) is tripled to F5400.
    REQUIRE(feedrate_of(bodies[2], "TYPE:Support") == 1800);
    REQUIRE(count_of(bodies[2], "F1800") == 12);
    REQUIRE(feedrate_of(bodies[2], "TYPE:Sparse infill") == 5400);
    REQUIRE(count_of(bodies[2], "F5400") == 12);
}

TEST_CASE("Layer time speed smoothing: eligible-only bisection reaches an in-range target exactly",
          "[LayerTimeSpeedSmoothing][GCode][BisectTarget]")
{
    // Same shape as the previous test but with headroom (max_speedup 900% => f_cap = 10.0) so
    // the target IS reachable: this is the "band actually gets reached" half of Defect 2, not
    // just the "cap makes it unreachable" half.
    //
    // Layer 2: protected (Support) 4.0 s, eligible (Sparse infill) 4.0 s, raw 8.0 s. Neighbour
    // (layer 1) 3.0 s => target = neighbour_upper(3.0, 0.75) = 4.0 s (same as above; the floor
    // 8.0 / 10.0 = 0.8 s does not bind). Solve 4.0 + 4.0/f = 4.0 is still not exactly reachable
    // at finite f (see previous test) UNLESS the protected share is smaller. Use a lighter
    // protected share instead: protected = 1.0 s, eligible = 7.0 s, raw = 8.0 s, same target
    // 4.0 s: 1.0 + 7.0/f = 4.0 => 7.0/f = 3.0 => f = 7/3 = 2.33333, comfortably under f_cap 10.0.
    PrintConfig cfg;
    cfg.layer_time_speed_smoothing.value     = ltssmSpeedUpAll;
    cfg.layer_time_speed_max_variation.value = 25.;
    cfg.layer_time_speed_max_speedup.value   = 900.; // f_cap = 10.0, well above what is needed
    cfg.filament_max_volumetric_speed.values = { 1000. };
    cfg.use_relative_e_distances.value       = true;
    cfg.slow_down_for_layer_cooling.values   = { false };
    cfg.slow_down_layer_time.values          = { 0. };
    cfg.slow_down_min_speed.values           = { 0. };

    auto make_neighbour = []() {
        std::string g = "G92 X0\n;TYPE:Sparse infill\n";
        for (int i = 0; i < 9; ++i)
            g += g1_x(10. * (i + 1), 0.05, 1800);
        return g;
    };
    auto make_light_protected_layer = []() {
        // 3 x 10 mm Support (protected) at 1800 mm/min = 3 x 0.33333 = 1.0 s.
        std::string g = "G92 X0\n;TYPE:Support\n";
        for (int i = 0; i < 3; ++i)
            g += g1_x(10. * (i + 1), 0.05, 1800);
        // 21 x 10 mm Sparse infill (eligible) at 1800 mm/min = 21 x 0.33333 = 7.0 s.
        g += ";TYPE:Sparse infill\n";
        for (int i = 0; i < 21; ++i)
            g += g1_x(10. * (i + 4), 0.05, 1800);
        return g;
    };

    LayerTimeSpeedSmoothingFilter filter(cfg);
    REQUIRE(filter.process_layer(make_neighbour(), 0, false).empty());
    REQUIRE(filter.process_layer(make_neighbour(), 1, false).empty());
    const std::string out = filter.process_layer(make_light_protected_layer(), 2, true);

    const std::vector<std::string> bodies = smoothed_layer_bodies(out);
    REQUIRE(bodies.size() == 3);
    REQUIRE(bodies[2].find("t_raw=8.00") != std::string::npos);
    REQUIRE(bodies[2].find("t_target=4.00") != std::string::npos);
    REQUIRE(bodies[2].find("factor=2.333") != std::string::npos);
    // Reached exactly: t_out == t_target (within the .2f comment precision).
    REQUIRE(bodies[2].find("t_out=4.00") != std::string::npos);
    REQUIRE(feedrate_of(bodies[2], "TYPE:Support") == 1800);
    REQUIRE(count_of(bodies[2], "F1800") == 3);
    // 1800 * 7/3 = 4200.
    REQUIRE(feedrate_of(bodies[2], "TYPE:Sparse infill") == 4200);
    REQUIRE(count_of(bodies[2], "F4200") == 21);
}
