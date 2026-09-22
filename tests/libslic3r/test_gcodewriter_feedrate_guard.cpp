#include <catch2/catch.hpp>

#include <cmath>
#include <cstdlib>
#include <limits>
#include <regex>
#include <string>
#include <vector>

#include "libslic3r/GCodeWriter.hpp"

using namespace Slic3r;

// Returns the value of the F word in a G-code line like "G1 F1234.5", or 0 if none.
static double feedrate_of(const std::string &gcode_line)
{
    std::smatch match;
    if (std::regex_search(gcode_line, match, std::regex(" F(-?[0-9.]+)")))
        return std::atof(match[1].str().c_str());
    return 0.;
}

// The writer must never emit F0, a negative F, or a NaN/inf F, even in Release
// builds where the debug asserts are compiled out. A bad F word does not error
// on Bambu printers - it silently wedges the motion planner indefinitely.
TEST_CASE("GCodeWriter refuses non-positive and non-finite feedrates", "[GCodeWriter]")
{
    GCodeWriter writer;

    SECTION("zero feedrate is replaced by a positive fallback") {
        const std::string line = writer.set_speed(0.0);
        REQUIRE_THAT(line, Catch::Contains("G1 F"));
        REQUIRE(feedrate_of(line) > 0.);
    }
    SECTION("negative feedrate is replaced by a positive fallback") {
        const std::string line = writer.set_speed(-100.0);
        REQUIRE_THAT(line, Catch::Contains("G1 F"));
        REQUIRE(feedrate_of(line) > 0.);
    }
    SECTION("NaN feedrate is replaced by a positive fallback") {
        const std::string line = writer.set_speed(std::numeric_limits<double>::quiet_NaN());
        REQUIRE_THAT(line, Catch::Contains("G1 F"));
        REQUIRE(feedrate_of(line) > 0.);
    }
    SECTION("infinite feedrate is replaced by a positive fallback") {
        const std::string line = writer.set_speed(std::numeric_limits<double>::infinity());
        REQUIRE(feedrate_of(line) > 0.);
        REQUIRE(std::isfinite(feedrate_of(line)));
    }
    SECTION("a bad feedrate falls back to the last valid emitted speed") {
        // No outer wall speed supplied, so the ladder drops to the last valid emitted speed.
        REQUIRE_THAT(writer.set_speed(1234.5), Catch::Equals("G1 F1234.5\n"));
        REQUIRE_THAT(writer.set_speed(0.0), Catch::Equals("G1 F1234.5\n"));
        REQUIRE_THAT(writer.set_speed(std::numeric_limits<double>::quiet_NaN()), Catch::Equals("G1 F1234.5\n"));
    }
    SECTION("consecutive calls keep working after a bad feedrate") {
        REQUIRE(feedrate_of(writer.set_speed(0.0)) > 0.);
        REQUIRE_THAT(writer.set_speed(2345.6), Catch::Equals("G1 F2345.6\n"));
        REQUIRE(feedrate_of(writer.set_speed(-5.0)) > 0.);
        REQUIRE_THAT(writer.set_speed(3000.0), Catch::Equals("G1 F3000\n"));
    }
    SECTION("normal positive speeds are emitted unchanged") {
        REQUIRE_THAT(writer.set_speed(99999.123), Catch::Equals("G1 F99999.123\n"));
        REQUIRE_THAT(writer.set_speed(1.0), Catch::Equals("G1 F1\n"));
        REQUIRE_THAT(writer.set_speed(203.200022), Catch::Equals("G1 F203.2\n"));
    }
}

// Regression guard for the whole change: a feedrate that is valid must produce byte-for-byte the
// same output it did before the guard existed, whatever diagnostic state is hanging off the
// writer. If this drifts, the guard has started costing something on the valid path.
TEST_CASE("Valid feedrates are byte-identical regardless of guard state", "[GCodeWriter]")
{
    const std::vector<std::pair<double, std::string>> cases{
        {1.0,          "G1 F1\n"},
        {203.200022,   "G1 F203.2\n"},
        {1234.5,       "G1 F1234.5\n"},
        {2700.0,       "G1 F2700\n"},
        {3000.0,       "G1 F3000\n"},
        {7200.0,       "G1 F7200\n"},
        {99999.123,    "G1 F99999.123\n"},
    };

    SECTION("with no origin and no fallback configured") {
        GCodeWriter writer;
        for (const auto &c : cases)
            REQUIRE_THAT(writer.set_speed(c.first), Catch::Equals(c.second));
    }
    SECTION("with an origin and an outer wall fallback configured") {
        GCodeWriter writer;
        FeedrateOrigin origin;
        origin.setting     = "internal_bridge_speed";
        origin.role_name   = "Internal Bridge";
        origin.object_name = "regression.stl";
        origin.layer_id    = 7;
        writer.set_feedrate_origin(origin);
        writer.set_guard_fallback_speed(1800.);
        bool reported = false;
        writer.set_feedrate_guard_reporter([&reported](const FeedrateOrigin &, double, double) { reported = true; });
        for (const auto &c : cases)
            REQUIRE_THAT(writer.set_speed(c.first), Catch::Equals(c.second));
        // The reporter must never fire for a valid feedrate.
        REQUIRE_FALSE(reported);
    }
}

// Defect 1: the substitution must be user-visible, and must name the offending SETTING - not just
// the extrusion role. Nothing in the UI suggests any of these values should be zero, so a message
// that does not name the config key leaves the user with nothing to act on.
TEST_CASE("A refused feedrate is reported and names the offending setting", "[GCodeWriter]")
{
    GCodeWriter writer;

    struct Report {
        std::string setting, role, object;
        int         layer = -1;
        double      bad = 0., substituted = 0.;
        bool        fired = false;
    } report;

    writer.set_feedrate_guard_reporter([&report](const FeedrateOrigin &origin, double bad, double substituted) {
        report.fired       = true;
        report.setting     = origin.setting     != nullptr ? origin.setting : "";
        report.role        = origin.role_name   != nullptr ? origin.role_name : "";
        report.object      = origin.object_name != nullptr ? origin.object_name : "";
        report.layer       = origin.layer_id;
        report.bad         = bad;
        report.substituted = substituted;
    });

    SECTION("the reporter receives the exact config key that resolved badly") {
        FeedrateOrigin origin;
        origin.setting     = "internal_bridge_speed";
        origin.role_name   = "Internal Bridge";
        origin.object_name = "bridge_test.stl";
        origin.layer_id    = 42;
        writer.set_feedrate_origin(origin);
        writer.set_guard_fallback_speed(1800.);

        const std::string line = writer.set_speed(0.0);

        REQUIRE(report.fired);
        // The whole point of defect 1: the SETTING, not merely the role.
        REQUIRE(report.setting == "internal_bridge_speed");
        REQUIRE(report.role == "Internal Bridge");
        REQUIRE(report.object == "bridge_test.stl");
        REQUIRE(report.layer == 42);
        REQUIRE(report.bad == 0.);
        REQUIRE(report.substituted == 1800.);
        // ...and the emitted line still carries the substituted speed.
        REQUIRE(feedrate_of(line) == Approx(1800.));
    }

    SECTION("a different role reports its own setting") {
        FeedrateOrigin origin;
        origin.setting   = "sparse_infill_speed";
        origin.role_name = "Sparse infill";
        writer.set_feedrate_origin(origin);
        writer.set_guard_fallback_speed(1800.);
        writer.set_speed(std::numeric_limits<double>::quiet_NaN());
        REQUIRE(report.fired);
        REQUIRE(report.setting == "sparse_infill_speed");
    }

    SECTION("an unknown origin still reports, with no setting name") {
        writer.clear_feedrate_origin();
        writer.set_guard_fallback_speed(1800.);
        writer.set_speed(-1.0);
        REQUIRE(report.fired);
        REQUIRE(report.setting.empty());
    }

    SECTION("clearing the origin stops a later trip being blamed on the wrong setting") {
        FeedrateOrigin origin;
        origin.setting = "top_surface_speed";
        writer.set_feedrate_origin(origin);
        writer.clear_feedrate_origin();
        writer.set_speed(0.0);
        REQUIRE(report.fired);
        REQUIRE(report.setting.empty());
    }

    SECTION("a valid feedrate does not report at all") {
        FeedrateOrigin origin;
        origin.setting = "outer_wall_speed";
        writer.set_feedrate_origin(origin);
        writer.set_speed(2700.0);
        REQUIRE_FALSE(report.fired);
    }

    // GCode::_extrude absorbs a zero/denormal role speed into the volumetric ceiling BEFORE the
    // writer ever sees it (the "autospeed" fallback), so the writer guard cannot fire for that
    // path - yet it is precisely the path a mistyped setting takes, and it silently prints far
    // faster than the profile asks. _extrude therefore calls report_bad_feedrate() directly.
    // This covers that entry point: reporting must work without the guard being tripped.
    SECTION("a caller can report a substitution the writer guard never saw") {
        FeedrateOrigin origin;
        origin.setting   = "internal_bridge_speed";
        origin.role_name = "Internal Bridge";
        origin.layer_id  = 3;
        writer.set_feedrate_origin(origin);

        // The volumetric ceiling stood in for a zero role speed: a perfectly valid F, reported.
        writer.report_bad_feedrate(0., 9744.18);

        REQUIRE(report.fired);
        REQUIRE(report.setting == "internal_bridge_speed");
        REQUIRE(report.bad == 0.);
        REQUIRE(report.substituted == Approx(9744.18));
        // And emitting that substituted speed must still be byte-identical, guard untouched.
        REQUIRE_THAT(writer.set_speed(9744.18), Catch::Equals("G1 F9744.18\n"));
    }
}

// Defect 2: the fallback ladder. Outer wall speed first (the owner's requirement - typically far
// below the old 600 mm/min constant in these profiles, and too slow is recoverable where too fast
// ruins a print), then the last valid emitted speed, then the last-resort constant.
TEST_CASE("The feedrate guard fallback ladder prefers the outer wall speed", "[GCodeWriter]")
{
    SECTION("rung 1: the resolved outer wall speed is used when valid") {
        GCodeWriter writer;
        writer.set_speed(9000.0);              // a much faster last-valid speed is available...
        writer.set_guard_fallback_speed(1500.); // ...but outer wall speed must win.
        REQUIRE(feedrate_of(writer.set_speed(0.0)) == Approx(1500.));
    }
    SECTION("rung 1 is preferred even though it is slower than the old 600 mm/min default") {
        GCodeWriter writer;
        writer.set_guard_fallback_speed(300.); // 5 mm/s outer wall - slow, but certified printable
        REQUIRE(feedrate_of(writer.set_speed(0.0)) == Approx(300.));
    }
    SECTION("rung 2: an unusable outer wall speed falls through to the last valid speed") {
        GCodeWriter writer;
        REQUIRE_THAT(writer.set_speed(2400.0), Catch::Equals("G1 F2400\n"));
        writer.set_guard_fallback_speed(0.); // outer_wall_speed itself resolved to zero
        REQUIRE(feedrate_of(writer.set_speed(0.0)) == Approx(2400.));
    }
    SECTION("rung 2: a negative or NaN outer wall speed is rejected, not substituted") {
        GCodeWriter writer;
        writer.set_speed(2400.0);
        writer.set_guard_fallback_speed(-50.);
        REQUIRE(feedrate_of(writer.set_speed(0.0)) == Approx(2400.));
        writer.set_guard_fallback_speed(std::numeric_limits<double>::quiet_NaN());
        REQUIRE(feedrate_of(writer.set_speed(0.0)) == Approx(2400.));
    }
    SECTION("rung 3: the last-resort constant when nothing else is available") {
        GCodeWriter writer;
        // A fresh writer's m_current_speed is a valid default, so drive the ladder directly.
        writer.set_guard_fallback_speed(0.);
        const double f = writer.resolve_guard_fallback();
        REQUIRE(std::isfinite(f));
        REQUIRE(f > 0.);
    }
    SECTION("the ladder never yields a non-positive or non-finite speed") {
        for (double bad : {0., -1., -1e9, std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity()}) {
            GCodeWriter writer;
            writer.set_guard_fallback_speed(bad);
            const double f = writer.resolve_guard_fallback();
            REQUIRE(std::isfinite(f));
            REQUIRE(f > 0.);
        }
    }
}

// Defense in depth: emit_axis('F', ...) is the single choke point for F words, so callers
// bypassing set_speed (travel moves, arcs, retracts, the pressure equalizer) still cannot push a
// bad feedrate to the printer.
TEST_CASE("GCodeFormatter clamps a bad F word at the formatter level", "[GCodeWriter]")
{
    SECTION("NaN feedrate emitted directly through the formatter is clamped") {
        GCodeG1Formatter bad;
        bad.set_fallback_feedrate(1500.0);
        bad.emit_f(std::numeric_limits<double>::quiet_NaN());
        REQUIRE(feedrate_of(bad.string()) == Approx(1500.));
    }
    SECTION("zero feedrate emitted directly through the formatter is clamped") {
        GCodeG2G3Formatter bad_ccw(true);
        bad_ccw.set_fallback_feedrate(1500.0);
        bad_ccw.emit_ij(Vec2d(1.0, 1.0));
        bad_ccw.emit_f(0.0);
        REQUIRE(feedrate_of(bad_ccw.string()) == Approx(1500.));
    }
    SECTION("with no fallback seeded it still clamps to the last-resort constant") {
        GCodeG1Formatter bad;
        bad.emit_f(0.0);
        REQUIRE(feedrate_of(bad.string()) == Approx(GCodeFormatter::GUARD_LAST_RESORT_FEEDRATE));
    }
    SECTION("a valid F word emitted through the formatter is unchanged") {
        GCodeG1Formatter w;
        w.emit_f(7200.0);
        REQUIRE_THAT(w.string(), Catch::Equals("G1 F7200\n"));
    }
}

// Defect 3 / the latent data race: the fallback used to live in a mutable static shared by every
// formatter instance. G-code emission is single-threaded today (the pipeline's generator stage is
// serial_in_order), so it was not a live race - but it would have become one silently. Formatters
// must now be fully independent: nothing one instance sees can leak into another.
TEST_CASE("Formatter feedrate guard state is per-instance, not shared", "[GCodeWriter]")
{
    SECTION("a valid feedrate on one formatter does not seed another") {
        GCodeG1Formatter a;
        a.emit_f(9000.0);           // would have populated the old shared static
        REQUIRE_THAT(a.string(), Catch::Equals("G1 F9000\n"));

        GCodeG1Formatter b;         // independent instance, nothing seeded
        REQUIRE(b.fallback_feedrate() == 0.);
        b.emit_f(0.0);
        // Falls to the last-resort constant, NOT to a's 9000 - no cross-instance leakage.
        REQUIRE(feedrate_of(b.string()) == Approx(GCodeFormatter::GUARD_LAST_RESORT_FEEDRATE));
    }
    SECTION("two formatters keep independent fallbacks") {
        GCodeG1Formatter a, b;
        a.set_fallback_feedrate(1200.0);
        b.set_fallback_feedrate(3600.0);
        REQUIRE(a.fallback_feedrate() == Approx(1200.));
        REQUIRE(b.fallback_feedrate() == Approx(3600.));
        a.emit_f(0.0);
        b.emit_f(0.0);
        REQUIRE(feedrate_of(a.string()) == Approx(1200.));
        REQUIRE(feedrate_of(b.string()) == Approx(3600.));
    }
    SECTION("an invalid seed is rejected so the ladder cannot be poisoned") {
        GCodeG1Formatter w;
        w.set_fallback_feedrate(std::numeric_limits<double>::quiet_NaN());
        REQUIRE(w.fallback_feedrate() == 0.);
        w.set_fallback_feedrate(-10.0);
        REQUIRE(w.fallback_feedrate() == 0.);
    }
}
