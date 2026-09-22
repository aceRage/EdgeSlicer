#include <catch2/catch.hpp>

#include <cmath>
#include <cstdlib>
#include <limits>
#include <regex>
#include <string>

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
    SECTION("a bad feedrate falls back to the last valid emitted speed") {
        REQUIRE_THAT(writer.set_speed(1234.5), Catch::Equals("G1 F1234.5\n"));
        // The next bad call must reuse the last valid speed, not a default.
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

// Defense in depth: emit_axis('F', ...) is the single choke point for F words,
// so callers bypassing set_speed (travel moves, arcs, retracts, the pressure
// equalizer) still cannot push a bad feedrate to the printer.
TEST_CASE("GCodeFormatter clamps a bad F word at the formatter level", "[GCodeWriter]")
{
    SECTION("NaN feedrate emitted directly through the formatter is clamped") {
        GCodeG1Formatter valid;
        valid.emit_f(1500.0); // establish a last-valid feedrate on the stream
        GCodeG1Formatter bad;
        bad.emit_f(std::numeric_limits<double>::quiet_NaN());
        const std::string line = bad.string();
        REQUIRE(feedrate_of(line) > 0.);
    }
    SECTION("zero feedrate emitted directly through the formatter is clamped") {
        GCodeG1Formatter valid;
        valid.emit_f(1500.0);
        GCodeG2G3Formatter bad_ccw(true);
        bad_ccw.emit_ij(Vec2d(1.0, 1.0));
        bad_ccw.emit_f(0.0);
        const std::string line = bad_ccw.string();
        REQUIRE(feedrate_of(line) > 0.);
    }
    SECTION("a valid F word emitted through the formatter is unchanged") {
        GCodeG1Formatter w;
        w.emit_f(7200.0);
        REQUIRE_THAT(w.string(), Catch::Equals("G1 F7200\n"));
    }
}
