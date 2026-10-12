// Fan and cooling regressions: the CoolingBuffer fan markers and the FanMover post-pass.
// Batch 2B of the OrcaSlicer upstream port (tests/orca_port_plan.md, section 2B).
#include <catch2/catch.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
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

// The part-cooling fan value an M106/M107 line sets (0-255), or -1 for any other line, including
// M106 for another fan (P2 auxiliary, P3 exhaust).
int part_fan_value(const std::string &line)
{
    if (line.rfind("M107", 0) == 0)
        return 0;
    if (line.rfind("M106", 0) != 0)
        return -1;
    const std::string body = line.substr(0, line.find(';'));
    const size_t      p    = body.find(" P");
    if (p != std::string::npos && std::atoi(body.c_str() + p + 2) != 1)
        return -1;
    const size_t s = body.find(" S");
    return s == std::string::npos ? -1 : std::atoi(body.c_str() + s + 2);
}

// One extruding move, with the fan value in force while it prints.
struct FanAtExtrusion
{
    int         layer;   // counts ";TEST_LAYER" lines from the layer-change G-code, 0-based; -1 before the first
    std::string role;    // the last ";TYPE:" seen
    int         tool;    // the last "T<n>" seen, -1 before the first
    int         fan;     // 0-255, -1 before the first fan command
};

std::vector<FanAtExtrusion> fan_at_extrusions(const std::string &gcode)
{
    std::vector<FanAtExtrusion> out;
    std::istringstream          in(gcode);
    std::string                 line;
    int layer = -1, tool = -1, fan = -1;
    std::string role;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind(";TEST_LAYER", 0) == 0)
            ++layer;
        else if (line.rfind(";TYPE:", 0) == 0)
            role = line.substr(6);
        else if (line.size() > 1 && line[0] == 'T' && std::isdigit(static_cast<unsigned char>(line[1])))
            tool = std::atoi(line.c_str() + 1);
        else if (const int v = part_fan_value(line); v >= 0)
            fan = v;
        else if (line.rfind("G1 ", 0) == 0 || line.rfind("G2 ", 0) == 0 || line.rfind("G3 ", 0) == 0) {
            const std::string body = line.substr(0, line.find(';'));
            const size_t      e    = body.find(" E");
            if (e != std::string::npos && std::atof(body.c_str() + e + 2) > 0.)
                out.push_back({layer, role, tool, fan});
        }
    }
    return out;
}

// The part-cooling fan commands of `gcode`, in order, each with the layer it is issued in.
std::vector<std::pair<int, std::string>> fan_commands(const std::string &gcode)
{
    std::vector<std::pair<int, std::string>> out;
    std::istringstream                       in(gcode);
    std::string                              line;
    int                                      layer = -1;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind(";TEST_LAYER", 0) == 0)
            ++layer;
        else if (part_fan_value(line) >= 0)
            out.emplace_back(layer, line);
    }
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

namespace {

// A 20 mm cube printed as a single outer wall (no infill, no shells), so every layer starts and ends
// with the outer wall. overhang_fan_threshold 0% ("none") cools every outer wall with the overhang
// fan, so an overhang fan region is open at every layer change.
DynamicPrintConfig single_wall_overhang_fan_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"},
        {"wall_loops", 1},
        {"top_shell_layers", 0},
        {"bottom_shell_layers", 0},
        {"sparse_infill_density", "0%"},
        {"skirt_loops", 0},
        {"brim_type", "no_brim"},
        {"enable_overhang_bridge_fan", "1"},
        {"overhang_fan_threshold", "0%"},
        {"overhang_fan_speed", "100"},
        {"fan_min_speed", "20"},
        {"fan_max_speed", "20"},
        {"close_fan_the_first_x_layers", "1"},
        // Ramp the fan over the first ten layers: the layer fan speed changes on every one of them,
        // so CoolingBuffer writes it at every layer start.
        {"full_fan_speed_layer", "10"},
        {"slow_down_for_layer_cooling", "0"},
        {"machine_start_gcode", ""},
        {"layer_change_gcode", ";TEST_LAYER_Z=[layer_z]"},
    });
    return config;
}

// Fan values of the moves in `layer` with `role`.
std::vector<int> fans_of(const std::vector<FanAtExtrusion> &moves, int layer, const std::string &role)
{
    std::vector<int> out;
    for (const FanAtExtrusion &m : moves)
        if (m.layer == layer && m.role == role)
            out.push_back(m.fan);
    return out;
}

} // namespace

// Orca #13390: the overhang fan region of the outer wall stayed open across the layer change, so the
// new layer got no START marker and CoolingBuffer (which reads the markers one layer at a time)
// printed it at the layer fan speed.
TEST_CASE("A role fan region continuing across a layer change keeps its fan speed", "[Cooling][Orca13390]")
{
    const std::string gcode    = slice({TestMesh::cube_20x20x20}, single_wall_overhang_fan_config());
    const auto        moves    = fan_at_extrusions(gcode);
    const auto        commands = fan_commands(gcode);
    REQUIRE(!moves.empty());

    // On the ramp the overhang speed (100%) and the layer speed (20%) are scaled by the same factor,
    // so the outer wall must run at about five times the lowest speed set around it. Printed at the
    // layer speed, it would be at most twice that (the ratio of two consecutive ramp steps).
    for (int layer = 3; layer < 9; ++layer) {
        int lowest = 256;
        for (const auto &[l, cmd] : commands)
            if ((l == layer || l == layer - 1) && part_fan_value(cmd) > 0)
                lowest = std::min(lowest, part_fan_value(cmd));
        const std::vector<int> wall = fans_of(moves, layer, "Outer wall");
        INFO("layer " << layer << ", lowest fan set around it " << lowest);
        REQUIRE(!wall.empty());
        REQUIRE(lowest < 256);
        for (int fan : wall)
            CHECK(fan >= 3 * lowest);
    }
}

// Orca #12595: ironing is the last thing printed on an object's layer, so the IRONING_FAN_END marker
// is written with the first extrusion of the next layer. CoolingBuffer reads the fan requests one
// layer at a time and only honoured that END when its own layer had opened the ironing request, so
// the fan kept running at the ironing speed above an ironed step.
TEST_CASE("The fan returns to the layer speed after ironing", "[Cooling][Orca12595]")
{
    // A 20 x 20 x 10 mm step with a 10 x 10 x 10 mm tower on one corner: the step's top is ironed,
    // then the tower carries on for another 50 layers.
    TriangleMesh step  = make_cube(20., 20., 10.);
    TriangleMesh tower = make_cube(10., 10., 10.);
    tower.translate(0.f, 0.f, 10.f);
    step.merge(tower);

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"},
        {"skirt_loops", 0},
        {"brim_type", "no_brim"},
        {"ironing_type", "top"},
        {"ironing_fan_speed", "100"},
        {"fan_min_speed", "20"},
        {"fan_max_speed", "20"},
        {"close_fan_the_first_x_layers", "1"},
        {"full_fan_speed_layer", "0"},
        {"slow_down_for_layer_cooling", "0"},
        {"enable_overhang_bridge_fan", "0"},
        {"machine_start_gcode", ""},
        {"layer_change_gcode", ";TEST_LAYER_Z=[layer_z]"},
    });
    const auto moves = fan_at_extrusions(slice({step}, config));

    size_t checked = 0;
    for (size_t i = 1; i + 1 < moves.size(); ++i)
        if (moves[i].role == "Ironing" && moves[i + 1].role != "Ironing") {
            // The fan before the ironing started.
            size_t before = i;
            while (before > 0 && moves[before].role == "Ironing")
                --before;
            INFO("ironing ends in layer " << moves[i].layer << ", next move is " << moves[i + 1].role << " in layer "
                                          << moves[i + 1].layer);
            CHECK(moves[i].fan == 255);
            CHECK(moves[i + 1].fan == moves[before].fan);
            ++checked;
        }
    CHECK(checked > 0);
}
