// Fan and cooling regressions: the CoolingBuffer fan markers and the FanMover post-pass.
// Batch 2B of the OrcaSlicer upstream port (tests/orca_port_plan.md, section 2B).
#include <catch2/catch.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
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

// With EDGE_TEST_DUMP_GCODE set to a directory, writes `gcode` there as <name>.gcode for inspection.
const std::string &dumped(const std::string &name, const std::string &gcode)
{
    if (const char *dir = std::getenv("EDGE_TEST_DUMP_GCODE"); dir != nullptr && *dir != 0) {
        std::ofstream out(std::string(dir) + "/" + name + ".gcode", std::ios::binary);
        out << gcode;
    }
    return gcode;
}

// `gcode` without its part-cooling fan command lines.
std::string without_fan_lines(const std::string &gcode)
{
    std::string        out;
    std::istringstream in(gcode);
    std::string        line;
    while (std::getline(in, line))
        if (part_fan_value(line) < 0)
            out += line + "\n";
    return out;
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
            // An extruding XY move (not a de-retraction, which carries E only).
            const std::string body = line.substr(0, line.find(';'));
            const size_t      e    = body.find(" E");
            const bool        xy   = body.find(" X") != std::string::npos || body.find(" Y") != std::string::npos;
            if (xy && e != std::string::npos && std::atof(body.c_str() + e + 2) > 0.)
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
        // The ironed layer takes long; keep its fan at the minimum rather than off.
        {"reduce_fan_stop_start_freq", "1"},
        {"close_fan_the_first_x_layers", "1"},
        {"full_fan_speed_layer", "0"},
        {"slow_down_for_layer_cooling", "0"},
        {"enable_overhang_bridge_fan", "0"},
        {"machine_start_gcode", ""},
        {"layer_change_gcode", ";TEST_LAYER_Z=[layer_z]"},
    });
    const auto moves = fan_at_extrusions(dumped("orca12595_ironing", slice({step}, config)));

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

namespace {

// A 30 mm sphere (overhangs of every degree) cooled with the overhang fan at 100% from a 25% overhang,
// a 20% layer fan and no layer-time slowdown.
DynamicPrintConfig sphere_overhang_fan_config(bool overhang_speed)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"layer_height", "0.3"},
        {"initial_layer_print_height", "0.3"},
        {"skirt_loops", 0},
        {"brim_type", "no_brim"},
        {"enable_arc_fitting", "0"},
        {"enable_overhang_bridge_fan", "1"},
        {"overhang_fan_threshold", "25%"},
        {"overhang_fan_speed", "100"},
        {"enable_overhang_speed", overhang_speed ? "1" : "0"},
        {"fan_min_speed", "20"},
        {"fan_max_speed", "20"},
        {"close_fan_the_first_x_layers", "1"},
        {"full_fan_speed_layer", "0"},
        {"slow_down_for_layer_cooling", "0"},
        {"machine_start_gcode", ""},
        {"layer_change_gcode", ";TEST_LAYER_Z=[layer_z]"},
    });
    return config;
}

std::vector<std::string> feedrate_words(const std::string &gcode)
{
    std::vector<std::string> out;
    std::istringstream       in(gcode);
    std::string              line;
    while (std::getline(in, line)) {
        const std::string body = line.substr(0, line.find(';'));
        std::istringstream words(body);
        std::string        word;
        while (words >> word)
            if (word.size() > 1 && word.front() == 'F')
                out.push_back(word);
    }
    return out;
}

} // namespace

// Orca #14788: the overhang fan was only applied to walls when "Slow down for overhangs" was on,
// because _extrude only measured the overhang for the slowdown.
TEST_CASE("The overhang fan cools overhanging walls with the overhang slowdown off", "[Cooling][Orca14788]")
{
    const std::string gcode = slice({make_sphere(15., PI / 24.)}, sphere_overhang_fan_config(false));
    const auto        moves = fan_at_extrusions(gcode);
    size_t outer = 0, cooled = 0;
    for (const FanAtExtrusion &m : moves)
        if (m.role == "Outer wall" && m.layer > 1) {
            ++outer;
            if (m.fan == 255)
                ++cooled;
        }
    INFO(cooled << " of " << outer << " outer wall moves at the overhang fan speed");
    REQUIRE(outer > 0);
    CHECK(cooled > 0);
    CHECK(cooled < outer);

    // The fan transitions do not slow anything down: the walls keep their speed, as with the overhang
    // fan off.
    DynamicPrintConfig no_fan = sphere_overhang_fan_config(false);
    no_fan.set_deserialize_strict({{"enable_overhang_bridge_fan", "0"}});
    const std::string no_fan_gcode = slice({make_sphere(15., PI / 24.)}, no_fan);
    std::vector<std::string> with_fan_f = feedrate_words(gcode), without_fan_f = feedrate_words(no_fan_gcode);
    std::sort(with_fan_f.begin(), with_fan_f.end());
    with_fan_f.erase(std::unique(with_fan_f.begin(), with_fan_f.end()), with_fan_f.end());
    std::sort(without_fan_f.begin(), without_fan_f.end());
    without_fan_f.erase(std::unique(without_fan_f.begin(), without_fan_f.end()), without_fan_f.end());
    CHECK(with_fan_f == without_fan_f);
}

namespace {

// Two filaments with the same 20% layer fan: the first prints the walls, outer wall first, with every
// outer wall cooled by its overhang fan at 100%; the second prints the infill with no overhang fan.
// The tool change G-code is a comment, so the tool change carries the _FORCE_RESUME_FAN_SPEED marker.
std::string slice_two_filaments_overhang_fan(const char *change_gcode)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#0000FF"};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4};
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value = true;
    config.option<ConfigOptionBool>("enable_prime_tower")->value             = false;
    config.set_key_value("change_filament_gcode", new ConfigOptionString(change_gcode));
    config.set_deserialize_strict({
        {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"},
        {"wall_filament", 1},
        {"sparse_infill_filament", 2},
        {"solid_infill_filament", 2},
        {"wall_loops", 2},
        {"wall_sequence", "outer wall/inner wall"},
        {"skirt_loops", 0},
        {"brim_type", "no_brim"},
        {"enable_overhang_bridge_fan", "1,0"},
        {"overhang_fan_threshold", "0%,0%"},
        {"overhang_fan_speed", "100,100"},
        {"fan_min_speed", "20,20"},
        {"fan_max_speed", "20,20"},
        {"close_fan_the_first_x_layers", "1,1"},
        {"full_fan_speed_layer", "0,0"},
        {"slow_down_for_layer_cooling", "0,0"},
        {"machine_start_gcode", ""},
        {"layer_change_gcode", ";TEST_LAYER_Z=[layer_z]"},
    });
    return slice({TestMesh::cube_20x20x20}, config);
}

} // namespace

// Orca #14788: CoolingBuffer did not record the layer fan speed it wrote when an overhang fan region
// ended, so a tool change's _FORCE_RESUME_FAN_SPEED put the first filament's overhang fan speed back
// on, and the second filament printed at it.
TEST_CASE("The overhang fan of one filament does not leak into the next after a tool change", "[Cooling][Orca14788]")
{
    const auto moves = fan_at_extrusions(slice_two_filaments_overhang_fan(";tool change\n"));
    size_t second = 0, cooled_outer = 0;
    for (const FanAtExtrusion &m : moves) {
        if (m.layer < 2)
            continue;
        if (m.tool == 1) {
            ++second;
            INFO("layer " << m.layer << " " << m.role);
            CHECK(m.fan == 51); // 20%
        } else if (m.role == "Outer wall" && m.fan == 255)
            ++cooled_outer;
    }
    CHECK(second > 0);
    CHECK(cooled_outer > 0);
}

// EDGE: a role fan region open at a tool change on the same layer. Two cubes, one per filament, each a single
// outer wall cooled all round by its overhang fan (threshold 0%), so every layer has a tool change from one
// cooled outer wall straight into the other. The first filament's region stayed open across the tool change:
// the second filament's wall got no START of its own, and CoolingBuffer kept the old request open, so it
// printed at the layer speed (or at whatever the tool change G-code left the fan at).
TEST_CASE("An overhang fan region open at a tool change starts again for the new filament", "[Cooling]")
{
    const std::vector<std::pair<std::string, std::string>> tool_changes{
        {"comment", ";tool change\n"},                    // the T command follows, after _FORCE_RESUME_FAN_SPEED
        {"T then fan off", "T[next_extruder]\nM106 S0\n"}, // BBL-like: T inside, the fan set after it
    };
    for (const auto &[name, change_gcode] : tool_changes) {
        DYNAMIC_SECTION("tool change G-code: " << name) {
            DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
            config.set_num_extruders(2);
            config.set_num_filaments(2);
            config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
            config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#0000FF"};
            config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4};
            config.option<ConfigOptionBool>("single_extruder_multi_material")->value = true;
            config.option<ConfigOptionBool>("enable_prime_tower")->value             = false;
            config.set_key_value("change_filament_gcode", new ConfigOptionString(change_gcode));
            config.set_deserialize_strict({
                {"layer_height", "0.2"},
                {"initial_layer_print_height", "0.2"},
                {"wall_loops", 1},
                {"top_shell_layers", 0},
                {"bottom_shell_layers", 0},
                {"sparse_infill_density", "0%"},
                {"skirt_loops", 0},
                {"brim_type", "no_brim"},
                {"enable_overhang_bridge_fan", "1,1"},
                {"overhang_fan_threshold", "0%,0%"},
                {"overhang_fan_speed", "100,100"},
                {"fan_min_speed", "40,40"},
                {"fan_max_speed", "40,40"},
                {"close_fan_the_first_x_layers", "1,1"},
                {"full_fan_speed_layer", "0,0"},
                {"slow_down_for_layer_cooling", "0,0"},
                {"machine_start_gcode", ""},
                {"layer_change_gcode", ";TEST_LAYER_Z=[layer_z]"},
            });
            Print print;
            Model model;
            init_print({TestMesh::cube_20x20x20, TestMesh::cube_20x20x20}, print, model, config);
            REQUIRE(model.objects.size() == 2);
            model.objects[1]->volumes.front()->config.set("extruder", 2);
            print.apply(model, config);
            const auto moves = fan_at_extrusions(Test::gcode(print));

            size_t walls[2] = {0, 0};
            for (const FanAtExtrusion &m : moves)
                if (m.layer >= 2 && m.role == "Outer wall" && m.tool >= 0 && m.tool < 2) {
                    ++walls[m.tool];
                    INFO("layer " << m.layer << " T" << m.tool);
                    CHECK(m.fan == 255);
                }
            CHECK(walls[0] > 0);
            CHECK(walls[1] > 0);
        }
    }
}

// Upstream's test for Orca #15158 (tests/fff_print/test_cooling.cpp), on Catch2 v2 and our config API.
TEST_CASE("Overhang fan transitions do not depend on overhang speed", "[Cooling][Regression][Orca15158]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"bridge_speed", "2"},
        {"enable_arc_fitting", "0"},
        {"enable_overhang_bridge_fan", "1"},
        {"enable_overhang_speed", "0"},
        {"initial_layer_print_height", "0.3"},
        {"inner_wall_speed", "30"},
        {"layer_height", "0.3"},
        {"outer_wall_speed", "30"},
        {"overhang_1_4_speed", "30"},
        {"overhang_2_4_speed", "29"},
        {"overhang_3_4_speed", "6"},
        {"overhang_4_4_speed", "3"},
        {"slow_down_for_layer_cooling", "0"},
        {"slowdown_for_curled_perimeters", "0"},
        {"fan_max_speed", "20"},
        {"fan_min_speed", "20"},
        {"overhang_fan_speed", "100"},
        {"overhang_fan_threshold", "25%"},
        {"layer_change_gcode", ";TEST_LAYER_Z=[layer_z]"},
    });

    constexpr double sphere_radius = 50.0; // 100 mm diameter.
    const std::string without_speed_gcode = slice({make_sphere(sphere_radius, PI / 24.0)}, config);
    config.set_deserialize_strict({{"enable_overhang_speed", "1"}});
    const std::string with_speed_gcode = slice({make_sphere(sphere_radius, PI / 24.0)}, config);

    const auto without_speed_fan       = fan_commands(without_speed_gcode);
    const auto with_speed_fan          = fan_commands(with_speed_gcode);
    const auto without_speed_feedrates = feedrate_words(without_speed_gcode);
    const auto with_speed_feedrates    = feedrate_words(with_speed_gcode);

    REQUIRE_FALSE(without_speed_fan.empty());
    REQUIRE(std::any_of(without_speed_fan.begin(), without_speed_fan.end(),
                        [](const auto &command) { return command.second.find("S255") != std::string::npos; }));
    REQUIRE(with_speed_feedrates != without_speed_feedrates);
    CHECK(with_speed_fan == without_speed_fan);
}

// Orca #15158 with our #181 (Orca #15945 split): #181 cuts a wall where only the overhang fan switches, but
// before #15158 a wall whose speed did not change was printed in one piece and the fan transition was lost.
TEST_CASE("A fan-only overhang transition inside a wall reaches the G-code", "[Cooling][Orca15158][Orca15945]")
{
    DynamicPrintConfig config = sphere_overhang_fan_config(true);
    // Overhang slowdown on, but no overhang degree slows anything down.
    config.set_deserialize_strict({
        {"overhang_1_4_speed", "0"},
        {"overhang_2_4_speed", "0"},
        {"overhang_3_4_speed", "0"},
        {"overhang_4_4_speed", "0"},
        {"slowdown_for_curled_perimeters", "1"},
    });
    // A cube tilted by 30 degrees about X: its walls lean out (overhang) on one side only, so an outer
    // wall loop is cooled along that side and not along the others. (A sphere overhangs evenly all round.)
    TriangleMesh tilted = make_cube(20., 20., 20.);
    tilted.rotate_x(float(PI / 6.));
    const auto moves = fan_at_extrusions(dumped("orca15158_fan_only", slice({tilted}, config)));
    // Layers whose outer wall prints partly with and partly without the overhang fan.
    std::vector<int> cooled, uncooled;
    for (const FanAtExtrusion &m : moves)
        if (m.role == "Outer wall" && m.layer > 1)
            (m.fan == 255 ? cooled : uncooled).push_back(m.layer);
    std::sort(cooled.begin(), cooled.end());
    std::sort(uncooled.begin(), uncooled.end());
    std::vector<int> both;
    std::set_intersection(cooled.begin(), cooled.end(), uncooled.begin(), uncooled.end(), std::back_inserter(both));
    CHECK(!both.empty());
}

namespace {

// The G-code of each layer, split at the ";TEST_LAYER" lines, without the part-cooling fan commands.
std::vector<std::string> layers_without_fan(const std::string &gcode)
{
    std::vector<std::string> out(1);
    std::istringstream       in(gcode);
    std::string              line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind(";TEST_LAYER", 0) == 0)
            out.emplace_back();
        if (part_fan_value(line) < 0)
            out.back() += line + "\n";
    }
    return out;
}

} // namespace

// Orca #15158 printed every wall and bridge point by point whenever the overhang fan is on, which drops the
// wall's arcs (arc fitting) even where the fan never switches; Orca #16297 does that only for a path along
// which the fan switches, so a layer without an overhang prints the same G-code with the overhang fan on as
// with it off.
TEST_CASE("Walls without an overhang fan transition keep their arcs", "[Cooling][Orca15158]")
{
    DynamicPrintConfig config = sphere_overhang_fan_config(true);
    config.set_deserialize_strict({{"enable_arc_fitting", "1"}});
    const std::string with_fan = slice({make_sphere(15., PI / 24.)}, config);
    config.set_deserialize_strict({{"enable_overhang_bridge_fan", "0"}});
    const std::string without_fan = slice({make_sphere(15., PI / 24.)}, config);

    const std::vector<std::string> a = layers_without_fan(with_fan), b = layers_without_fan(without_fan);
    REQUIRE(a.size() == b.size());
    REQUIRE(a.size() > 90); // 30 mm at 0.3 mm, plus the text before the first layer
    size_t arcs = 0;
    // The upper half of the sphere: every wall rests on the larger layer below it.
    for (size_t layer = 55; layer + 5 < a.size(); ++layer) {
        INFO("layer " << layer);
        CHECK(a[layer] == b[layer]);
        if (a[layer].find("\nG2 ") != std::string::npos || a[layer].find("\nG3 ") != std::string::npos)
            ++arcs;
    }
    CHECK(arcs > 0);
}

// Upstream's test for Orca #16297 (tests/fff_print/test_cooling.cpp), on Catch2 v2 and our config API. The
// sphere's lower half overhangs all round, so its walls are cooled along their whole length: the path-wide
// marker carries that, and they keep their arcs.
TEST_CASE("Arc fitting remains active with overhang fan control", "[Cooling][Regression][Orca16297]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"enable_arc_fitting", "1"},
        {"enable_overhang_bridge_fan", "1"},
        {"enable_overhang_speed", "1"},
        {"initial_layer_print_height", "0.2"},
        {"layer_height", "0.2"},
        {"slow_down_for_layer_cooling", "0"},
        {"fan_max_speed", "20"},
        {"fan_min_speed", "20"},
        {"overhang_fan_speed", "100"},
        {"overhang_fan_threshold", "25%"},
        {"layer_change_gcode", ";TEST_LAYER=[layer_num]"},
    });

    const auto has_arc_after_first_layer = [](const std::string &gcode) {
        std::istringstream input(gcode);
        std::string        line;
        size_t             layers_seen = 0;
        while (std::getline(input, line)) {
            if (line.rfind(";TEST_LAYER=", 0) == 0)
                ++layers_seen;
            else if (layers_seen > 1 && (line.rfind("G2 ", 0) == 0 || line.rfind("G3 ", 0) == 0))
                return true;
        }
        return false;
    };
    const auto has_arc_with_overhang_fan = [](const std::string &gcode) {
        std::istringstream input(gcode);
        std::string        line;
        bool               overhang_fan_active = false;
        bool               outer_wall          = false;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.rfind("M106 S255", 0) == 0)
                overhang_fan_active = true;
            else if (line.rfind("M106 S0", 0) == 0 || line.rfind("M107", 0) == 0)
                overhang_fan_active = false;
            else if (line.rfind(";TYPE:", 0) == 0)
                outer_wall = line == ";TYPE:Outer wall";
            else if (overhang_fan_active && outer_wall && (line.rfind("G2 ", 0) == 0 || line.rfind("G3 ", 0) == 0))
                return true;
        }
        return false;
    };

    const std::string with_overhang_speed = slice({make_cylinder(25.0, 10.0, 2.0 * PI / 72.0)}, config);
    CHECK(has_arc_after_first_layer(with_overhang_speed));

    config.set_deserialize_strict({{"enable_overhang_speed", "0"}});
    const std::string without_overhang_speed = slice({make_cylinder(25.0, 10.0, 2.0 * PI / 72.0)}, config);
    CHECK(has_arc_after_first_layer(without_overhang_speed));

    const std::string uniform_overhang_gcode = slice({make_sphere(50.0, 2.0 * PI / 72.0)}, config);
    CHECK(has_arc_with_overhang_fan(uniform_overhang_gcode));
}

// EDGE, with Orca #15158's skipping of repeated fan commands: tool change G-code may set the fan itself.
// Here it switches the fan off after the tool change while the first filament's overhang fan region is
// still open, so the _FORCE_RESUME_FAN_SPEED after it does not resume the fan; the region's END then
// asks for the layer speed, which CoolingBuffer had already written at the T line. Skipping it as a repeat
// left the second filament printing with the fan off.
TEST_CASE("The fan is set again after tool change G-code that switches it off", "[Cooling][Orca15158]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#0000FF"};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4};
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value = true;
    config.option<ConfigOptionBool>("enable_prime_tower")->value             = false;
    config.set_key_value("change_filament_gcode", new ConfigOptionString("T[next_extruder]\nM106 S0\n"));
    config.set_deserialize_strict({
        {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"},
        {"wall_filament", 1},
        {"sparse_infill_filament", 2},
        {"solid_infill_filament", 2},
        {"wall_loops", 2},
        // The outer wall last, so its overhang fan region is open at the tool change.
        {"wall_sequence", "inner wall/outer wall"},
        {"skirt_loops", 0},
        {"brim_type", "no_brim"},
        {"enable_overhang_bridge_fan", "1,1"},
        {"overhang_fan_threshold", "0%,0%"},
        {"overhang_fan_speed", "100,100"},
        {"fan_min_speed", "20,20"},
        {"fan_max_speed", "20,20"},
        {"close_fan_the_first_x_layers", "1,1"},
        {"full_fan_speed_layer", "0,0"},
        {"slow_down_for_layer_cooling", "0,0"},
        {"machine_start_gcode", ""},
        {"layer_change_gcode", ";TEST_LAYER_Z=[layer_z]"},
    });
    const std::string gcode = slice({TestMesh::cube_20x20x20}, config);
    REQUIRE(gcode.find("M106 S0\n; custom gcode end") != std::string::npos);

    size_t checked = 0;
    for (const FanAtExtrusion &m : fan_at_extrusions(gcode))
        if (m.layer >= 2 && m.tool == 1 && m.role == "Sparse infill") {
            INFO("layer " << m.layer);
            CHECK(m.fan == 51); // 20%
            ++checked;
        }
    CHECK(checked > 0);
}

// Orca #15158: FanMover (fan_speedup_time) wrote a fan command that fell in the last 10% of a move before
// the move rather than after it, almost a whole move too early.
TEST_CASE("FanMover writes a fan command near the end of a move after the move", "[FanMover][Orca15158]")
{
    // Speed-up 1 s. The fan increase comes 1.98 s after the start of a 1 s move followed by a 0.98 s
    // one, so it belongs 0.98 s into the first move: within its last 10%, too close to split.
    const std::vector<std::string> out = fan_mover_layers(1.f, false, 0.f, {
        "G1 X0 Y0 F600\n"
        "G1 X10 E1\n"      // 1 s
        "G1 X19.8 E1\n"    // 0.98 s
        "M106 S255\n"
        "G1 X30 E1\n",
    });
    REQUIRE(out.size() == 1);
    const size_t first_move = out[0].find("G1 X10 E1");
    const size_t fan        = out[0].find("M106 S255");
    const size_t next_move  = out[0].find("G1 X19.8 E1");
    REQUIRE(first_move != std::string::npos);
    REQUIRE(fan != std::string::npos);
    REQUIRE(next_move != std::string::npos);
    CHECK(first_move < fan);
    CHECK(fan < next_move);
}

// Our #170 (Orca #15904) brackets change_filament_gcode with "; custom gcode start" / "; custom gcode end" so
// that FanMover leaves it alone. FanMover only treated fan commands inside the span specially: a fan
// increase right after the tool change still reached back into it and split one of its moves, putting a
// waypoint computed from FanMover's own idea of the position into a hand-routed tool change.
TEST_CASE("FanMover never splits a move of tool change G-code", "[FanMover][GCode]")
{
    // Speed-up 2 s, after a 6 s tool change travel: the fan command belongs 4 s into the travel.
    const std::vector<std::string> out = fan_mover_layers(2.f, false, 0.f, {
        "G1 X0 Y0 F600\n"
        "M106 S51\n"
        "G1 X5 E1\n"
        "; custom gcode start\n"
        "G1 X65 F600\n"    // 6 s
        "; custom gcode end\n"
        "T1\n"
        "M106 S255\n"
        "G1 X70 E1\n",
    });
    REQUIRE(out.size() == 1);
    const std::string &g = out[0];
    INFO(g);
    const size_t start  = g.find("; custom gcode start\n");
    const size_t travel = g.find("\nG1 X65 F600\n");
    const size_t end    = g.find("; custom gcode end\n");
    REQUIRE(start != std::string::npos);
    REQUIRE(travel != std::string::npos);
    REQUIRE(end != std::string::npos);
    // The tool change keeps its moves as they came: no waypoint. (A fan command between them moves nothing.)
    CHECK(without_fan_lines(g.substr(start, end - start)) == "; custom gcode start\nG1 X65 F600\n");
    // The fan still comes early: before the travel rather than inside it.
    const size_t fan = g.find("M106 S255");
    REQUIRE(fan != std::string::npos);
    CHECK(fan < travel);
}

// Orca #15904 as merged upstream brackets the tool change with internal ";_FAN_MOVER_SKIP_START/END"
// markers instead, which its FanMover reads and drops. We keep our visible markers (above) but honour
// upstream's too, so G-code or a template written for upstream keeps its moves whole.
TEST_CASE("FanMover honours and drops upstream's tool change markers", "[FanMover][GCode][Orca15904]")
{
    const std::vector<std::string> out = fan_mover_layers(2.f, false, 0.f, {
        "G1 X0 Y0 F600\n"
        "M106 S51\n"
        "G1 X5 E1\n"
        ";_FAN_MOVER_SKIP_START\n"
        "G1 X65 F600\n"    // 6 s
        ";_FAN_MOVER_SKIP_END\n"
        "T1\n"
        "M106 S255\n"
        "G1 X70 E1\n",
    });
    REQUIRE(out.size() == 1);
    const std::string &g = out[0];
    INFO(g);
    // The markers are consumed.
    CHECK(g.find(";_FAN_MOVER_SKIP_") == std::string::npos);
    // The travel is not split: no waypoint between the move before it and the tool change.
    const size_t before = g.find("G1 X5 E1\n");
    const size_t travel = g.find("\nG1 X65 F600\n");
    const size_t tool   = g.find("\nT1\n");
    REQUIRE(before != std::string::npos);
    REQUIRE(travel != std::string::npos);
    REQUIRE(tool != std::string::npos);
    CHECK(without_fan_lines(g.substr(before, tool + 1 - before)) == "G1 X5 E1\nG1 X65 F600\n");
    // The fan still comes early: before the travel rather than inside it.
    const size_t fan = g.find("M106 S255");
    REQUIRE(fan != std::string::npos);
    CHECK(fan < travel);
}

// The kickstart path: a fan increase is kicked at 100% and its own speed is put back once the kick time
// has run out, which may fall inside the next move. That move must not be split when it is tool change
// G-code either.
TEST_CASE("FanMover's kickstart never splits a move of tool change G-code", "[FanMover][GCode]")
{
    // No speed-up, kickstart 3 s: a raise from 0 to 49% kicks for 1.47 s, which ends 1.47 s into the
    // 6 s tool change travel that follows.
    const std::vector<std::string> out = fan_mover_layers(0.f, false, 3.f, {
        "G1 X0 Y0 F600\n"
        "M106 S127\n"
        "; custom gcode start\n"
        "G1 X60 F600\n"    // 6 s
        "; custom gcode end\n"
        "G1 X70 E1\n",
    });
    REQUIRE(out.size() == 1);
    const std::string &g = out[0];
    INFO(g);
    const size_t start = g.find("; custom gcode start\n");
    const size_t end   = g.find("; custom gcode end\n");
    REQUIRE(start != std::string::npos);
    REQUIRE(end != std::string::npos);
    // The kick happened, and the travel stayed whole.
    CHECK(g.find("M106 S255") != std::string::npos);
    CHECK(without_fan_lines(g.substr(start, end - start)) == "; custom gcode start\nG1 X60 F600\n");
}

// The same with GCode generating the tool change: FanMover and the fan commands CoolingBuffer writes
// around the bracketed change_filament_gcode leave its moves intact.
TEST_CASE("Tool change G-code stays intact with the fan speed-up and the overhang fan", "[FanMover][GCode][Cooling]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#0000FF"};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4};
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value = true;
    config.option<ConfigOptionBool>("enable_prime_tower")->value             = false;
    // A long hand-routed travel in the tool change, in relative coordinates FanMover cannot follow.
    // (Different feed rates: CoolingBuffer drops an F that repeats the one in force, also in custom G-code.)
    config.set_key_value("change_filament_gcode", new ConfigOptionString("G91\nG1 X40 F600\nG1 Y40 F700\nG90\n"));
    config.set_deserialize_strict({
        {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"},
        {"wall_filament", 1},
        {"sparse_infill_filament", 2},
        {"solid_infill_filament", 2},
        {"skirt_loops", 0},
        {"brim_type", "no_brim"},
        {"fan_speedup_time", "2"},
        {"fan_kickstart", "0"},
        {"fan_speedup_overhangs", "0"},
        {"enable_overhang_bridge_fan", "1,1"},
        {"overhang_fan_threshold", "0%,0%"},
        {"overhang_fan_speed", "100,100"},
        // Different layer fans, so the tool change itself raises or lowers the fan.
        {"fan_min_speed", "20,60"},
        {"fan_max_speed", "20,60"},
        {"close_fan_the_first_x_layers", "1,1"},
        {"full_fan_speed_layer", "0,0"},
        {"slow_down_for_layer_cooling", "0,0"},
        {"machine_start_gcode", ""},
    });
    const std::string gcode = slice({TestMesh::cube_20x20x20}, config);

    size_t changes = 0;
    for (size_t start = gcode.find("; custom gcode start\n"); start != std::string::npos;
         start = gcode.find("; custom gcode start\n", start + 1)) {
        const size_t end = gcode.find("; custom gcode end", start);
        REQUIRE(end != std::string::npos);
        // Without fan commands and the print progress (M73) the time estimate writes between any lines.
        std::string body;
        {
            std::istringstream in(without_fan_lines(gcode.substr(start, end - start)));
            std::string        line;
            while (std::getline(in, line))
                if (line.rfind("M73 ", 0) != 0)
                    body += line + "\n";
        }
        INFO(body);
        CHECK(body == "; custom gcode start\nG91\nG1 X40 F600\nG1 Y40 F700\nG90\n");
        ++changes;
    }
    CHECK(changes > 10);
}
