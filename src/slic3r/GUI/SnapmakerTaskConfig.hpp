#pragma once

// The Snapmaker U1's print_task_config commands, as text.
//
// Deliberately free of wx, of GUI_App and of everything else in this directory: these are pure
// string builders, so the tests can link them without dragging the whole GUI in, and the two send
// paths that need them (the phone's LAN send in SnapmakerLan.cpp, the desktop's send through
// SSWCP.cpp) cannot drift apart by each writing their own.
//
// Why the flag travels here at all rather than in the G-code: the U1's firmware refuses
// SET_PRINT_PREFERENCES / SET_PRINT_TASK_PARAMETERS while print_stats.state is "printing" or
// "paused" (klippy/extras/print_task_config.py), so it has to reach the printer before the job
// starts - on the preferences line the print-start sequence already sends.

#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {
namespace SnapmakerLan {

// How many physical toolheads the print_task_config arrays hold on this printer family (the U1's
// firmware PHYSICAL_EXTRUDER_NUM). The firmware clamps a longer list and zeroes what it is not
// given, so sending exactly this many flags is both safe and complete.
constexpr int TOOLHEAD_COUNT = 4;

// The END_UNLOAD_FILAMENT=[..] parameter for a print that uses `used_toolheads`: a 1 for each of
// them and an explicit 0 for the rest, TOOLHEAD_COUNT entries, no spaces (a Klipper parameter value
// ends at a space, and the firmware parses the list with ast.literal_eval).
std::string end_unload_parameter(const std::vector<int>& used_toolheads);

// Append that parameter to the SET_PRINT_PREFERENCES line of a task-config script that already
// carries SET_PRINT_USED_EXTRUDERS (the desktop Device page builds exactly such a script and hands
// it to the slicer over SSWCP). The used toolheads are read off that line, so this stays one
// parameter on the existing command rather than a second one the firmware would refuse mid-print.
// Returns the script unchanged when it has no SET_PRINT_PREFERENCES line, when it already carries
// an END_UNLOAD_FILAMENT, or when no toolhead is named.
std::string with_end_unload(const std::string& script);

} // namespace SnapmakerLan
} // namespace GUI
} // namespace Slic3r
