// Fan and cooling regressions: the CoolingBuffer fan markers and the FanMover post-pass.
// Batch 2B of the OrcaSlicer upstream port (tests/orca_port_plan.md, section 2B).
#include <catch2/catch.hpp>

#include <algorithm>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/GCode/FanMover.hpp"
#include "libslic3r/GCodeWriter.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// Runs `layers` through one FanMover, each layer flushed as GCode::process_layers() does, and
// returns the output of every layer.
std::vector<std::string> fan_mover_layers(float speedup_time, bool only_overhangs, float kickstart,
                                          const std::vector<std::string> &layers)
{
    GCodeWriter writer;
    writer.config.gcode_flavor.value = gcfMarlinFirmware;
    FanMover mover(writer, speedup_time, speedup_time > 0, true, only_overhangs, kickstart);
    std::vector<std::string> out;
    for (const std::string &layer : layers)
        out.emplace_back(mover.process_gcode(layer, true));
    return out;
}

} // namespace

// Orca #13336: a fan command still in the FanMover buffer when a layer is flushed was emitted
// without updating the speed FanMover believes the fan runs at. The next layer's command back to
// the layer fan speed then looked redundant and was dropped, leaving the fan at the previous
// layer's role-based speed (here a bridge at 90%).
TEST_CASE("FanMover keeps a fan command flushed at the end of a layer in its state", "[FanMover][Orca13336]")
{
    // Speed-up 2 s, only for overhangs: the bridge's fan increase is not moved earlier, it stays in
    // the buffer and is written by the end-of-layer flush.
    const std::vector<std::string> out = fan_mover_layers(2.f, true, 0.f, {
        "M106 S76\n"
        "G1 X0 Y0 F600\n"
        "G1 X50 E1\n"          // 5 s
        ";TYPE:Bridge\n"
        "M106 S229\n"
        "G1 X55 E0.1\n",       // 0.5 s, so the bridge fan is still buffered at the end of the layer
        ";TYPE:Outer wall\n"
        "M106 S76\n"
        "G1 X100 E1\n"         // 4.5 s, pushes the restore out of the buffer
        "G1 X100 Y45 E1\n",
    });
    REQUIRE(out.size() == 2);
    CHECK(out[0].find("M106 S229") != std::string::npos);
    // The restore to the layer speed survives.
    CHECK(out[1].find("M106 S76") != std::string::npos);
}
