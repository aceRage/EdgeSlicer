// The end-of-print unload the send dialog's checkbox asks for: END_UNLOAD_FILAMENT on the printer's
// SET_PRINT_PREFERENCES line.
//
// Two builders are under test, and they are the only two places any send path builds the flag:
//
//   end_unload_parameter(used)  - the parameter itself, used by the phone's LAN send (through
//                                SnapmakerLan::mapping_script, which knows its own mapping) and by
//                                the print-host queue.
//   with_end_unload(script)     - amends a task-config script that already carries
//                                SET_PRINT_USED_EXTRUDERS. This is what the desktop send does: the
//                                bundled Device page builds the script and hands it to the slicer
//                                over SSWCP, so the flag has to be added to the line already there.
//                                The firmware refuses a second SET_PRINT_PREFERENCES once the print
//                                has begun, which is why appending a new command is not an option.
//
// Nothing here touches a printer or the network.

#include <catch2/catch.hpp>

#include "slic3r/GUI/SnapmakerTaskConfig.hpp"

#include <string>
#include <vector>

using namespace Slic3r::GUI::SnapmakerLan;

TEST_CASE("end_unload_parameter names every toolhead, used or not", "[SnapmakerUnload]")
{
    // Four entries, always: the firmware zeroes the array and then copies what it is given, so an
    // explicit 0 is how a toolhead this job does not use is told not to unload.
    CHECK(end_unload_parameter({1, 2}) == "END_UNLOAD_FILAMENT=[0,1,1,0]");
    CHECK(end_unload_parameter({0}) == "END_UNLOAD_FILAMENT=[1,0,0,0]");
    CHECK(end_unload_parameter({0, 1, 2, 3}) == "END_UNLOAD_FILAMENT=[1,1,1,1]");
    CHECK(end_unload_parameter({}) == "END_UNLOAD_FILAMENT=[0,0,0,0]");
    // A Klipper parameter value ends at a space, and the firmware parses this with
    // ast.literal_eval: no spaces anywhere in the list.
    CHECK(end_unload_parameter({1, 2}).find(' ') == std::string::npos);
    // Out of range is ignored rather than lengthening the list past PHYSICAL_EXTRUDER_NUM.
    CHECK(end_unload_parameter({7}) == "END_UNLOAD_FILAMENT=[0,0,0,0]");
}

// ---- with_end_unload: the desktop send's builder ----------------------------
//
// The shape the bundled Device page produces, verbatim from its own composer
// (resources/web/flutter_web/main.dart.js): the extruder map, then the used extruders, then the
// preferences, one blob, no trailing newline after the last line.
static std::string page_script(const std::string& used = "1,2")
{
    return "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=0 MAP_EXTRUDER=1\n"
           "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=1 MAP_EXTRUDER=2\n"
           "SET_PRINT_USED_EXTRUDERS EXTRUDERS=" + used + "\n"
           "SET_PRINT_PREFERENCES BED_LEVEL=0 FLOW_CALIBRATE=0";
}

TEST_CASE("with_end_unload amends the page's own preferences line", "[SnapmakerUnload]")
{
    const std::string out = with_end_unload(page_script());
    CHECK(out == "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=0 MAP_EXTRUDER=1\n"
                 "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=1 MAP_EXTRUDER=2\n"
                 "SET_PRINT_USED_EXTRUDERS EXTRUDERS=1,2\n"
                 "SET_PRINT_PREFERENCES BED_LEVEL=0 FLOW_CALIBRATE=0 END_UNLOAD_FILAMENT=[0,1,1,0]");
    // Still one command: nothing was appended as a new line the firmware would refuse.
    CHECK(out.find("SET_PRINT_PREFERENCES") == out.rfind("SET_PRINT_PREFERENCES"));
}

TEST_CASE("with_end_unload agrees with the LAN path for the same job", "[SnapmakerUnload]")
{
    // The whole point of one builder: a desktop send and a phone send of the same plate to the same
    // toolheads must put the same characters on the wire.
    // The phone's half - mapping_script() - lives in SnapmakerLan.cpp, which pulls GUI_App in and
    // so cannot be linked into this test binary; test_u1_unload.py proves it end to end against the
    // mock printer instead. What is provable here is that the desktop's script ends in exactly the
    // parameter the phone's builder produces, because both compose it with end_unload_parameter().
    const std::string desktop = with_end_unload(page_script("1,2"));
    const std::string want    = end_unload_parameter({1, 2});
    CHECK(want == "END_UNLOAD_FILAMENT=[0,1,1,0]");
    CHECK(desktop.substr(desktop.size() - want.size()) == want);
}

TEST_CASE("with_end_unload reads the used toolheads off the script, not the mapping order", "[SnapmakerUnload]")
{
    CHECK(with_end_unload(page_script("0")).find("END_UNLOAD_FILAMENT=[1,0,0,0]") != std::string::npos);
    CHECK(with_end_unload(page_script("3,0")).find("END_UNLOAD_FILAMENT=[1,0,0,1]") != std::string::npos);
    CHECK(with_end_unload(page_script("0,1,2,3")).find("END_UNLOAD_FILAMENT=[1,1,1,1]") != std::string::npos);
}

TEST_CASE("with_end_unload leaves alone anything that is not a task-config script", "[SnapmakerUnload]")
{
    // The page sends plenty of other scripts through the same call. None of them may grow a
    // parameter, and a preferences line without a used-extruders line names no toolhead to unload.
    CHECK(with_end_unload("M104 S0") == "M104 S0");
    CHECK(with_end_unload("") == "");
    CHECK(with_end_unload("SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=0 SAVE=1") ==
          "SET_PRINT_FILAMENT_CONFIG CONFIG_EXTRUDER=0 SAVE=1");
    CHECK(with_end_unload("SET_PRINT_PREFERENCES BED_LEVEL=0") == "SET_PRINT_PREFERENCES BED_LEVEL=0");
    CHECK(with_end_unload("SET_PRINT_USED_EXTRUDERS EXTRUDERS=1,2") == "SET_PRINT_USED_EXTRUDERS EXTRUDERS=1,2");
}

TEST_CASE("with_end_unload never adds the flag twice", "[SnapmakerUnload]")
{
    const std::string once = with_end_unload(page_script());
    CHECK(with_end_unload(once) == once);
    // Nor over a page that one day starts sending it itself.
    const std::string theirs = page_script() + " END_UNLOAD_FILAMENT=[1,1,1,1]";
    CHECK(with_end_unload(theirs) == theirs);
}

TEST_CASE("with_end_unload copes with CRLF and a trailing newline", "[SnapmakerUnload]")
{
    const std::string crlf = "SET_PRINT_USED_EXTRUDERS EXTRUDERS=2\r\n"
                             "SET_PRINT_PREFERENCES BED_LEVEL=0\r\n";
    const std::string out  = with_end_unload(crlf);
    CHECK(out == "SET_PRINT_USED_EXTRUDERS EXTRUDERS=2\r\n"
                 "SET_PRINT_PREFERENCES BED_LEVEL=0 END_UNLOAD_FILAMENT=[0,0,1,0]\r\n");
}
