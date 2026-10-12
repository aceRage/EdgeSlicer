// Fan and cooling regressions: the CoolingBuffer fan markers and the FanMover post-pass.
// Batch 2B of the OrcaSlicer upstream port (tests/orca_port_plan.md, section 2B).
#include <catch2/catch.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
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
    const auto moves = fan_at_extrusions(slice({make_sphere(15., PI / 24.)}, config));
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

// EDGE deviation from Orca #15158: upstream prints every wall and bridge point by point whenever the
// overhang fan is on, which drops the wall's arcs (arc fitting) even where the fan never switches. We do
// that only for a path along which the fan switches, so a layer without an overhang prints the same G-code
// with the overhang fan on as with it off.
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
