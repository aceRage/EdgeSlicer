// Time estimator tests for the batch 2F ports (OrcaSlicer #14573 filament change time, #15066 firmware retract
// feedrate, #15304 / #15308 junction deviation planning). Ported from upstream's tests/fff_print/test_gcode_timing.cpp
// (Catch2 v3 there) to this tree's Catch2 v2.

#include <catch2/catch.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <cmath>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

constexpr size_t NORMAL = static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal);

// Writes `gcode` to a temporary file and runs the processor over it with `config` applied. The G-code has no
// producer marker, so process_file() keeps the applied config.
void run_processor(GCodeProcessor& proc, const FullPrintConfig& config, const char* gcode)
{
    // reserved_tag() selects between two tag tables based on this shared static, and other tests in the binary
    // mutate it -- pin it so our "; FEATURE:" role tags are parsed deterministically regardless of test order.
    GCodeProcessor::s_IsBBLPrinter = true;
    const boost::filesystem::path path = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("edge_gcode_timing_%%%%%%%%.gcode");
    {
        boost::nowide::ofstream os(path.string(), std::ios::binary);
        os << gcode;
    }
    proc.apply_config(config);
    proc.process_file(path.string());
    boost::filesystem::remove(path);
}

// Estimated time per extrusion role, grouped the way the preview builds the feature-type legend: the sum of
// MoveVertex.time over EMoveType::Extrude moves keyed by extrusion_role.
std::map<ExtrusionRole, double> role_times(const GCodeProcessorResult& r)
{
    std::map<ExtrusionRole, double> m;
    for (const auto& mv : r.moves)
        if (mv.type == EMoveType::Extrude)
            m[mv.extrusion_role] += mv.time[NORMAL];
    return m;
}

// Sum of estimated time attributed to tool-change moves.
double sum_tool_change_time(const GCodeProcessorResult& r)
{
    double t = 0.0;
    for (const auto& mv : r.moves)
        if (mv.type == EMoveType::Tool_change)
            t += mv.time[NORMAL];
    return t;
}

// ---- #14573: filament change time ----

FullPrintConfig make_config(double load_time, double unload_time, double tool_change_time)
{
    FullPrintConfig config; // default-initialized with the built-in defaults
    config.gcode_flavor.value = gcfMarlinFirmware;
    // Two filaments, both assigned to the same (single) extruder, so a T1 after T0 is a same-extruder
    // filament swap that costs unload + load time.
    config.filament_diameter.values = {1.75, 1.75};
    config.filament_map.values = {1, 1};
    config.machine_load_filament_time.value = load_time;
    config.machine_unload_filament_time.value = unload_time;
    config.machine_tool_change_time.value = tool_change_time;
    return config;
}

constexpr double LOAD = 10.0;
constexpr double UNLOAD = 5.0;

} // namespace

// This tree's process_T() starts on extruder 0 and charges nothing for T0 at the start, so the tests switch to
// T1 first (empty extruder: load time only) and back to T0 (unload + load).

TEST_CASE("Filament-change time is attributed to tool-change moves, not extrusion roles", "[GCodeTiming]")
{
    // Relative extrusion (M83) so every "E5" is a real 5mm extrusion move rather than a zero-delta travel. Two
    // real travels precede T1 so its delay is flushed cleanly. The extrusions after T1 span several roles; the
    // first pending block at T0 is an "Outer wall" move, so the old code folded the T0 delay into that role.
    // The per-role check below verifies EVERY role stays clean, not just one.
    const char* gcode =
        "M83\n"
        "; FEATURE: Outer wall\n"
        "G1 X10 Y10 Z0.2 F600\n"
        "G1 X0 Y0 F6000\n"
        "T1\n"
        "; FEATURE: Outer wall\n"
        "G1 X50 Y0 E5 F1800\n"
        "G1 X50 Y50 E5\n"
        "; FEATURE: Sparse infill\n"
        "G1 X0 Y50 E5\n"
        "G1 X0 Y0 E5\n"
        "T0\n"
        "; FEATURE: Inner wall\n"
        "G1 X50 Y0 E5\n"
        "G1 X50 Y50 E5\n";

    GCodeProcessor proc_zero;
    run_processor(proc_zero, make_config(0.0, 0.0, 0.0), gcode);
    const GCodeProcessorResult& r_zero = proc_zero.get_result();

    GCodeProcessor proc_delay;
    run_processor(proc_delay, make_config(LOAD, UNLOAD, 0.0), gcode);
    const GCodeProcessorResult& r_delay = proc_delay.get_result();

    // T1 loads (10), T0 unloads (5) and loads (10).
    const double delay = LOAD + UNLOAD + LOAD;

    // The delay must not inflate the time of ANY extrusion role. Compare the full per-role breakdown between the
    // zero-delay and delayed runs -- every role must match to within tolerance.
    const auto roles_zero  = role_times(r_zero);
    const auto roles_delay = role_times(r_delay);
    // Guard: the gcode must genuinely exercise multiple distinct roles, otherwise this check would silently
    // cover only one.
    REQUIRE(roles_zero.size() >= 3);
    REQUIRE(roles_zero.size() == roles_delay.size());
    for (const auto& [role, zero_time] : roles_zero) {
        INFO("extrusion role index = " << static_cast<int>(role));
        REQUIRE(roles_delay.count(role) == 1);
        REQUIRE_THAT(roles_delay.at(role), WithinAbs(zero_time, 1e-2));
    }

    // The delay must instead land on the tool-change moves, so per-move consumers (layer-time view, layer slider)
    // stay consistent.
    REQUIRE_THAT(sum_tool_change_time(r_delay), WithinAbs(delay, 1e-2));
    REQUIRE_THAT(sum_tool_change_time(r_zero), WithinAbs(0.0, 1e-9));

    // The role tables are rebuilt from the moves: no extrusion role gets the delay, and the sum of the roles still
    // adds up to the total.
    const auto& modes_zero  = r_zero.print_statistics.modes[NORMAL];
    const auto& modes_delay = r_delay.print_statistics.modes[NORMAL];
    for (const auto& [role, time] : modes_zero.roles_times) {
        auto it = std::find_if(modes_delay.roles_times.begin(), modes_delay.roles_times.end(),
                               [role = role](const auto& item) { return item.first == role; });
        REQUIRE(it != modes_delay.roles_times.end());
        if (role != erNone)
            REQUIRE_THAT(it->second, WithinAbs(time, 1e-2));
    }

    // Both tool changes occur on layer 1, so the delay must also be reflected in the first-layer time.
    REQUIRE(!modes_delay.layers_times.empty());
    REQUIRE_THAT(modes_delay.layers_times.front() - modes_zero.layers_times.front(), WithinAbs(delay, 1e-2));
}

TEST_CASE("Filament-change time is not dropped when few motion blocks precede the change", "[GCodeTiming]")
{
    // Only a single motion block precedes T1, so the old "fewer than two pending blocks" early-out discarded that
    // filament-change delay entirely, making the total print time inconsistent with the filament changes.
    const char* gcode =
        "; FEATURE: Outer wall\n"
        "G1 X10 Y10 Z0.2 F600\n"
        "T1\n"
        "G1 X50 Y0 E5 F1800\n"
        "G1 X50 Y50 E5\n"
        "T0\n"
        "G1 X0 Y50 E5\n"
        "G1 X0 Y0 E5\n";

    GCodeProcessor proc_zero;
    run_processor(proc_zero, make_config(0.0, 0.0, 0.0), gcode);

    GCodeProcessor proc_delay;
    run_processor(proc_delay, make_config(LOAD, UNLOAD, 0.0), gcode);

    // Every second of filament-change delay must be present in the total estimated print time.
    const double delay = LOAD + UNLOAD + LOAD;
    const double total_delta = proc_delay.get_time(PrintEstimatedStatistics::ETimeMode::Normal)
                             - proc_zero.get_time(PrintEstimatedStatistics::ETimeMode::Normal);
    REQUIRE_THAT(total_delta, WithinAbs(delay, 1e-2));
}

TEST_CASE("Back-to-back tool changes buffer then merge into one tool-change block", "[GCodeTiming]")
{
    // T1 is the very first line: the block queue is empty when its delay is synchronized, so with only the single
    // (artificial) tool-change block queued the delay can't be attributed yet and is buffered. T0 follows
    // immediately with no motion between; its synchronize now sees two tool-change blocks queued, so its own delay
    // joins the buffered T1 entry at application time, the two same-type entries merge into one, and the sum lands
    // entirely on the first tool-change block. The trailing travels leave both runs with >= 2 blocks so their
    // end-of-file flush is identical and cancels in every delta.
    const char* gcode =
        "T1\n"                     // first charged change (load only); empty queue -> buffers (Tool_change,10)
        "T0\n"                     // same-extruder swap (unload+load); merges with the buffered entry to (Tool_change,25)
        "G1 X10 Y0 Z0.2 F6000\n"   // travels: keep >= 2 blocks queued at EOF (flushed identically by both runs)
        "G1 X10 Y10\n"
        "G1 X0 Y10\n";

    GCodeProcessor proc_zero;
    run_processor(proc_zero, make_config(0.0, 0.0, 0.0), gcode);
    const GCodeProcessorResult& r_zero = proc_zero.get_result();

    GCodeProcessor proc_delay;
    run_processor(proc_delay, make_config(LOAD, UNLOAD, 0.0), gcode);
    const GCodeProcessorResult& r_delay = proc_delay.get_result();

    // T1 load 10 + T0 unload 5 + T0 load 10 = 25.
    const double delay = LOAD + UNLOAD + LOAD;

    // The whole buffered-then-merged delay must reach the total print time.
    const double total_delta = proc_delay.get_time(PrintEstimatedStatistics::ETimeMode::Normal)
                             - proc_zero.get_time(PrintEstimatedStatistics::ETimeMode::Normal);
    REQUIRE_THAT(total_delta, WithinAbs(delay, 1e-2));

    // ...and must land on the tool-change moves, not on any extrusion role.
    REQUIRE_THAT(sum_tool_change_time(r_delay), WithinAbs(25.0, 1e-2));
    REQUIRE_THAT(sum_tool_change_time(r_zero), WithinAbs(0.0, 1e-9));

    // Characterization (documents the current merge-collapse behavior, not a correctness requirement): the two
    // buffered same-type entries combine onto the FIRST artificial tool-change block; the second receives nothing.
    std::vector<double> tc;
    for (const auto& mv : r_delay.moves)
        if (mv.type == EMoveType::Tool_change)
            tc.push_back(mv.time[NORMAL]);
    REQUIRE(tc.size() >= 2);
    REQUIRE_THAT(tc[0], WithinAbs(25.0, 1e-2));
    REQUIRE_THAT(tc[1], WithinAbs(0.0, 1e-9));
}

TEST_CASE("Trailing tool change at end of file is drained, not dropped", "[GCodeTiming]")
{
    // A tool change is the last line of the file, with only its single artificial block queued. Its delay is
    // buffered (fewer than two blocks) and there is no later motion to flush it, so only the finalization pass can
    // attribute it. Without the end-of-file drain the delay would be stranded in the buffer.
    const char* gcode =
        "G1 X10 Y0 Z0.2 F6000\n"   // three travels -> three blocks queued (no E, so no filament is selected)
        "G1 X10 Y10\n"
        "G1 X0 Y10\n"
        "G4 S0\n"                  // dwell with S present -> full flush; queue and buffer now empty
        "T1\n";                    // trailing change, nothing after: buffers (Tool_change,10), one block queued

    GCodeProcessor proc_zero;
    run_processor(proc_zero, make_config(0.0, 0.0, 0.0), gcode);
    const GCodeProcessorResult& r_zero = proc_zero.get_result();

    GCodeProcessor proc_delay;
    run_processor(proc_delay, make_config(LOAD, UNLOAD, 0.0), gcode);
    const GCodeProcessorResult& r_delay = proc_delay.get_result();

    // T1 is the first charged change on an empty extruder, so it costs the load time only. The zero run buffers
    // nothing and drops its artificial block, so the motion cancels and the delta is exactly the drained delay.
    const double total_delta = proc_delay.get_time(PrintEstimatedStatistics::ETimeMode::Normal)
                             - proc_zero.get_time(PrintEstimatedStatistics::ETimeMode::Normal);
    REQUIRE_THAT(total_delta, WithinAbs(LOAD, 1e-2));

    // The size-1 drain runs the body, so the delay lands on the artificial tool-change move.
    REQUIRE_THAT(sum_tool_change_time(r_delay), WithinAbs(LOAD, 1e-2));
    REQUIRE_THAT(sum_tool_change_time(r_zero), WithinAbs(0.0, 1e-9));
}

TEST_CASE("Carried-forward tool-change delay reaches the total without polluting roles", "[GCodeTiming]")
{
    // A wildcard dwell delay is buffered ahead of the tool-change delay, so when the blocks are next flushed the
    // dwell's (Noop) entry consumes the artificial tool-change block and the tool-change entry finds no matching
    // block and carries forward. It stays unmatched through the remaining extrusion moves and is only resolved at
    // finalization, where the end-of-file fold adds it to the machine total -- never to a move vertex, so it
    // cannot leak into an extrusion role's time.
    const char* gcode =
        "M83\n"
        "G4 S3\n"                        // empty queue -> buffers (Noop,3) [wildcard delay]
        "T1\n"                           // one block queued -> buffers (Tool_change,10) behind the dwell
        "; FEATURE: Inner wall\n"
        "G1 X20 Y0 Z0.2 E5 F1800\n"      // extrusion m1: queue is [artificial_TC, m1]
        "G4 S0\n"                        // flush: (Noop,3) consumes artificial_TC; (Tool_change,10) carries forward
        "G1 X20 Y20 E5\n"                // extrusion m2
        "G1 X0 Y20 E5\n";                // extrusion m3: at EOF queue is [m2, m3], buffer is [(Tool_change,10)]

    GCodeProcessor proc_zero;
    run_processor(proc_zero, make_config(0.0, 0.0, 0.0), gcode);
    const GCodeProcessorResult& r_zero = proc_zero.get_result();

    GCodeProcessor proc_delay;
    run_processor(proc_delay, make_config(LOAD, UNLOAD, 0.0), gcode);
    const GCodeProcessorResult& r_delay = proc_delay.get_result();

    // The stranded tool-change delay must be drained into the total, not dropped. The 3s dwell is identical in
    // both runs and cancels along with all motion, leaving exactly the delay.
    const double total_delta = proc_delay.get_time(PrintEstimatedStatistics::ETimeMode::Normal)
                             - proc_zero.get_time(PrintEstimatedStatistics::ETimeMode::Normal);
    REQUIRE_THAT(total_delta, WithinAbs(LOAD, 1e-2));

    // Pollution safety: the drained delay must NOT appear in any extrusion role. Every role's time must match
    // between the zero and delayed runs -- this is what the total-only fold buys.
    const auto rz = role_times(r_zero);
    const auto rd = role_times(r_delay);
    REQUIRE(rz.size() >= 1);
    REQUIRE(rz.size() == rd.size());
    for (const auto& [role, zero_time] : rz) {
        INFO("extrusion role index = " << static_cast<int>(role));
        REQUIRE(rd.count(role) == 1);
        REQUIRE_THAT(rd.at(role), WithinAbs(zero_time, 1e-2));
    }
}

// ---- #15066: firmware retraction ----

TEST_CASE("Firmware retraction does not change the modal feedrate", "[GCodeTiming][FirmwareRetraction]")
{
    // G10 / G11 are emulated as G1 moves with the retraction speed. That feedrate is not the one the next
    // G1 without an F inherits: before the fix every extrusion after a G10 / G11 was previewed (and timed) at the
    // retraction speed.
    FullPrintConfig config = make_config(0.0, 0.0, 0.0);
    config.retraction_length.values   = {2.0, 2.0};
    config.retraction_speed.values    = {30.0, 30.0};
    config.deretraction_speed.values  = {30.0, 30.0};
    config.retract_restart_extra.values = {0.0, 0.0};

    const char* gcode =
        "M83\n"
        "G1 X10 Y0 Z0.2 F6000\n"
        "G1 X20 Y0 E1 F6000\n"
        "G10\n"
        "G1 X30 Y0 E1\n"      // no F: the modal 100 mm/s
        "G11\n"
        "G1 X40 Y0 E1\n";     // no F: the modal 100 mm/s

    GCodeProcessor proc;
    run_processor(proc, config, gcode);

    size_t extrusions = 0, retractions = 0;
    for (const auto& mv : proc.get_result().moves) {
        if (mv.actual_speed_point)
            continue; // the planner's acceleration / deceleration points are copies of a move, not moves
        if (mv.type == EMoveType::Extrude) {
            ++extrusions;
            INFO("extrusion " << extrusions);
            REQUIRE_THAT(mv.feedrate, WithinAbs(100.0, 1e-3));
        } else if (mv.type == EMoveType::Retract || mv.type == EMoveType::Unretract) {
            ++retractions;
            // the retraction moves themselves keep the retraction speed
            REQUIRE_THAT(mv.feedrate, WithinAbs(30.0, 1e-3));
        }
    }
    REQUIRE(extrusions == 3);
    REQUIRE(retractions == 2);
}

// ---- #15304 / #15308 (and #12417 / #12440): junction deviation planning ----
//
// Junction planning decides the speeds the "actual speed" / "actual flow" preview shows. Per-axis jerk limits a
// corner by the largest single-axis component of the velocity change, allowing sqrt(2) more speed on a diagonal
// than on an axis -- a four-lobed ripple around every circle. Klipper and Marlin 2 with M205 J plan with junction
// deviation instead, which sees only the corner angle.
namespace {

// One acceleration everywhere and axis limits far above it, so only the junction model under test can slow a
// corner down.
FullPrintConfig make_junction_config(GCodeFlavor flavor, double corner_velocity, double junction_deviation)
{
    FullPrintConfig config;
    config.gcode_flavor.value = flavor;
    config.filament_diameter.values = {1.75};
    config.filament_map.values = {1};

    const std::vector<double> accel   = {1000.0, 1000.0};
    const std::vector<double> axis    = {20000.0, 20000.0};
    const std::vector<double> speed   = {500.0, 500.0};
    config.machine_max_acceleration_extruding.values = accel;
    config.machine_max_acceleration_travel.values    = accel;
    config.machine_max_acceleration_retracting.values = accel;
    config.machine_max_acceleration_x.values = axis;
    config.machine_max_acceleration_y.values = axis;
    config.machine_max_acceleration_z.values = axis;
    config.machine_max_acceleration_e.values = axis;
    config.machine_max_speed_x.values = speed;
    config.machine_max_speed_y.values = speed;
    config.machine_max_speed_z.values = speed;
    config.machine_max_speed_e.values = speed;
    // Klipper reads this as the square corner velocity, Marlin as classic jerk.
    config.machine_max_jerk_x.values = {corner_velocity, corner_velocity};
    config.machine_max_jerk_y.values = {corner_velocity, corner_velocity};
    config.machine_max_jerk_z.values = {corner_velocity, corner_velocity};
    // Kept out of the way so it never binds in the classic-jerk comparisons.
    config.machine_max_jerk_e.values = {100.0, 100.0};
    config.machine_max_junction_deviation.values = {junction_deviation, junction_deviation};
    config.machine_min_extruding_rate.values = {0.0, 0.0};
    config.machine_min_travel_rate.values    = {0.0, 0.0};
    return config;
}

constexpr double junction_x = 60.0;
constexpr double junction_y = 60.0;

// Two 40mm moves meeting at (junction_x, junction_y) with the given turn, rotated by `orientation`. 40mm is long
// enough to reach the commanded 150mm/s and brake back to any corner speed these tests produce. `e_per_mm` of zero
// makes them travels, which keeps the junction vector purely geometric as the formulas below assume.
std::string corner_gcode(double turn_deg, double orientation_deg, double e_per_mm = 0.0)
{
    const double len   = 40.0;
    const double a_in  = orientation_deg * M_PI / 180.0;
    const double a_out = (orientation_deg + turn_deg) * M_PI / 180.0;
    std::ostringstream extrude;
    if (e_per_mm > 0.0)
        extrude << std::fixed << std::setprecision(4) << " E" << len * e_per_mm;

    std::ostringstream os;
    os << std::fixed << std::setprecision(4)
       << "M83\n"
       << "G1 Z0.2 F1200\n"
       << "G1 X" << junction_x - len * std::cos(a_in)  << " Y" << junction_y - len * std::sin(a_in)  << " F6000\n"
       << "G1 X" << junction_x                         << " Y" << junction_y   << extrude.str()      << " F9000\n"
       << "G1 X" << junction_x + len * std::cos(a_out) << " Y" << junction_y + len * std::sin(a_out)
       << extrude.str() << " F9000\n";
    return os.str();
}

// Speed allowed through the corner: the vertex ending the incoming move carries that block's exit speed, and the
// vertices the actual-speed pass inserts are all strictly interior.
double corner_speed(const GCodeProcessorResult& r)
{
    for (const auto& mv : r.moves)
        if ((mv.type == EMoveType::Travel || mv.type == EMoveType::Extrude) &&
            std::abs(mv.position.x() - junction_x) < 1e-3 &&
            std::abs(mv.position.y() - junction_y) < 1e-3)
            return mv.actual_feedrate;
    return -1.0;
}

double planned_corner_speed(GCodeFlavor flavor, double corner_velocity, double junction_deviation,
                            double turn_deg, double orientation_deg = 0.0, double e_per_mm = 0.0,
                            const std::string& prefix = std::string())
{
    GCodeProcessor proc;
    run_processor(proc, make_junction_config(flavor, corner_velocity, junction_deviation),
                  (prefix + corner_gcode(turn_deg, orientation_deg, e_per_mm)).c_str());
    return corner_speed(proc.get_result());
}

} // namespace

TEST_CASE("Klipper corners are planned with junction deviation derived from the square corner velocity",
          "[GCodeTiming][JunctionDeviation]")
{
    // jd = scv^2 * (sqrt(2) - 1) / max_accel, then v^2 = jd * accel * sin(t/2) / (1 - sin(t/2)).
    // The acceleration cancels: the corner speed depends only on the scv and the angle.
    const double scv = 5.0;

    SECTION("a right angle is taken at exactly the square corner velocity") {
        // sin(t/2) = sqrt(0.5) at 90 degrees, so v == scv -- the definition of the square corner velocity, and
        // what makes the mapping above the right one.
        REQUIRE_THAT(planned_corner_speed(gcfKlipper, scv, 0.0, 90.0), Catch::Matchers::WithinRel(scv, 0.02));
    }

    SECTION("a shallow corner is taken far faster than the per-axis jerk model allows") {
        // 6 degrees: sin(t/2) = cos(3 deg), so v = 5 * sqrt((sqrt(2) - 1) * 728.68) = 86.9mm/s. Per-axis jerk
        // ignores the angle and caps the velocity *change* (2v*sin(3 deg)), giving 47.8mm/s.
        const double jd_speed   = planned_corner_speed(gcfKlipper, scv, 0.0, 6.0);
        const double jerk_speed = planned_corner_speed(gcfMarlinLegacy, scv, 0.0, 6.0);
        REQUIRE_THAT(jd_speed, Catch::Matchers::WithinRel(86.87, 0.02));
        REQUIRE_THAT(jerk_speed, Catch::Matchers::WithinRel(47.75, 0.02));
    }
}

TEST_CASE("Junction deviation limits a corner by its angle alone, not by its orientation",
          "[GCodeTiming][JunctionDeviation]")
{
    // The four-lobed ripple on circular walls is per-axis jerk being anisotropic: a velocity change lying on an
    // axis gets sqrt(2) less headroom than the same change on the diagonal.
    const double scv  = 5.0;
    const double turn = 6.0;

    SECTION("Klipper plans both orientations identically") {
        const double on_axis  = planned_corner_speed(gcfKlipper, scv, 0.0, turn, 0.0);
        const double diagonal = planned_corner_speed(gcfKlipper, scv, 0.0, turn, 45.0);
        REQUIRE(on_axis > 0.0);
        REQUIRE_THAT(diagonal, Catch::Matchers::WithinRel(on_axis, 0.02));
    }

    SECTION("the classic jerk model keeps its orientation dependence") {
        const double on_axis  = planned_corner_speed(gcfMarlinLegacy, scv, 0.0, turn, 0.0);
        const double diagonal = planned_corner_speed(gcfMarlinLegacy, scv, 0.0, turn, 45.0);
        REQUIRE(on_axis > 0.0);
        REQUIRE(diagonal / on_axis > 1.2);
    }
}

TEST_CASE("Junction deviation is only used where the firmware actually plans with it",
          "[GCodeTiming][JunctionDeviation]")
{
    const double jerk = 5.0;

    SECTION("Marlin 2 with M205 J disabled keeps the classic jerk planning") {
        // machine_max_junction_deviation == 0 is how a Marlin 2 printer says it runs classic jerk.
        const double classic = planned_corner_speed(gcfMarlinLegacy, jerk, 0.0, 90.0);
        REQUIRE(classic > 0.0);
        REQUIRE_THAT(planned_corner_speed(gcfMarlinFirmware, jerk, 0.0, 90.0),
                     Catch::Matchers::WithinRel(classic, 1e-4));
    }

    SECTION("Marlin 2 with M205 J enabled switches to junction deviation") {
        // sqrt(1000 * 0.05 * 2.4142136) = 11.0mm/s, independent of the jerk values it no longer reads.
        REQUIRE_THAT(planned_corner_speed(gcfMarlinFirmware, jerk, 0.05, 90.0),
                     Catch::Matchers::WithinRel(10.99, 0.02));
    }

    SECTION("machines without junction deviation are untouched by the jerk values it would ignore") {
        // A flavor that never enters the junction deviation path must ignore the setting entirely.
        const double without = planned_corner_speed(gcfMarlinLegacy, jerk, 0.0, 90.0);
        REQUIRE_THAT(planned_corner_speed(gcfMarlinLegacy, jerk, 0.05, 90.0),
                     Catch::Matchers::WithinRel(without, 1e-4));
    }
}

TEST_CASE("M205 J in the G-code sets the junction deviation the estimator plans with",
          "[GCodeTiming][JunctionDeviation]")
{
    // #12440: the estimator follows the firmware's M205 J, not only the machine value of the printer profile.
    const double jerk = 5.0;
    const double classic = planned_corner_speed(gcfMarlinFirmware, jerk, 0.0, 90.0);
    REQUIRE(classic > 0.0);

    // The printer profile says classic jerk (machine junction deviation 0); the G-code switches Marlin 2 to junction
    // deviation 0.05: sqrt(1000 * 0.05 * 2.4142136) = 11.0mm/s.
    REQUIRE_THAT(planned_corner_speed(gcfMarlinFirmware, jerk, 0.0, 90.0, 0.0, 0.0, "M205 J0.05\n"),
                 Catch::Matchers::WithinRel(10.99, 0.02));

    // M205 J0 puts it back on classic jerk.
    REQUIRE_THAT(planned_corner_speed(gcfMarlinFirmware, jerk, 0.05, 90.0, 0.0, 0.0, "M205 J0\n"),
                 Catch::Matchers::WithinRel(classic, 1e-4));
}
TEST_CASE("How fast a corner is taken does not depend on how much is extruded through it",
          "[GCodeTiming][JunctionDeviation]")
{
    // The junction cosine is taken over XYZE, so the direction vectors have to be unit length or the E term makes
    // the two paths look more parallel than they are and the corner comes out too fast, the more so the higher the
    // flow. Marlin normalizes over XYZE on any extruding move (planner.cpp, esteps > 0) and Klipper leaves E out of
    // the cosine altogether (toolhead.py::Move.calc_junction); on both, this corner is planned by its geometry alone.
    const double scv  = 5.0;
    const double turn = 6.0;
    const double geometric = planned_corner_speed(gcfKlipper, scv, 0.0, turn);
    REQUIRE(geometric > 0.0);

    // 0.029mm/mm is an ordinary 0.42 x 0.2 line on 1.75mm filament; 0.1 is a fat large-nozzle one. Unnormalized
    // these came out at 94.4 and 150.0mm/s against a geometric 86.9.
    for (double e_per_mm : {0.029, 0.1}) {
        INFO("e_per_mm = " << e_per_mm);
        REQUIRE_THAT(planned_corner_speed(gcfKlipper, scv, 0.0, turn, 0.0, e_per_mm),
                     Catch::Matchers::WithinRel(geometric, 0.02));
    }

    SECTION("and the same holds on Marlin 2") {
        const double marlin = planned_corner_speed(gcfMarlinFirmware, scv, 0.05, turn);
        REQUIRE(marlin > 0.0);
        REQUIRE_THAT(planned_corner_speed(gcfMarlinFirmware, scv, 0.05, turn, 0.0, 0.029),
                     Catch::Matchers::WithinRel(marlin, 0.02));
    }
}

// ---- upstream 403292cb63: end G-code waits ----

TEST_CASE("Timed waits of the machine end G-code are not part of the estimate", "[GCodeTiming][EndGCode]")
{
    // Air filtration / timelapse / sound waits (M400 S, M400 P) after the machine end G-code marker are post-print.
    // Since #14573 a wait with no motion behind it is buffered and counted, so they have to be skipped on purpose.
    auto gcode = [](bool marker) {
        std::string s =
            "M83\n"
            "; FEATURE: Outer wall\n"
            "G1 X10 Y0 Z0.2 F6000\n"
            "G1 X50 Y0 E5 F1800\n"
            "G1 X50 Y50 E5\n"
            "G1 X0 Y50 E5\n";
        if (marker)
            s += "; MACHINE_END_GCODE_START\n";
        s +=
            "G1 Z10 F900\n"
            "G1 Z12\n"
            "M400 S180\n"          // wait while the nozzle is idle
            "G1 Z20\n"
            "G1 Z22\n"
            "M400 P5000\n";
        return s;
    };

    GCodeProcessor with_marker, without_marker;
    run_processor(with_marker, make_config(0.0, 0.0, 0.0), gcode(true).c_str());
    run_processor(without_marker, make_config(0.0, 0.0, 0.0), gcode(false).c_str());

    const double t_with    = with_marker.get_time(PrintEstimatedStatistics::ETimeMode::Normal);
    const double t_without = without_marker.get_time(PrintEstimatedStatistics::ETimeMode::Normal);
    REQUIRE(t_with > 0.0);
    // Without the marker both waits count (180 + 5 s).
    REQUIRE_THAT(t_without - t_with, WithinAbs(185.0, 1e-1));
}
