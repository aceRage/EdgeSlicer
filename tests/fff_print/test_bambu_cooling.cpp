// Bambu parity PR 4 (tests/review_missing_bambu_settings_2026-11-25.md, B2 and A4): the auxiliary fan's own
// early layers and ramp (Bambu Studio 02.09.01.70 GCodeEditor.cpp:363-376, 469-479) and the part fan's early
// start before overhangs (pre_start_fan_time, GCodeEditor.cpp:517-541).
#include <catch2/catch.hpp>

#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "libslic3r/GCodeWriter.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Format/BambuExport.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// The S value (0-255) of an "M106 P<index>" line, or -1 for any other line.
int indexed_fan_value(const std::string &line, int index)
{
    if (line.rfind("M106", 0) != 0)
        return -1;
    const std::string body = line.substr(0, line.find(';'));
    const size_t      p    = body.find(" P");
    if (p == std::string::npos || std::atoi(body.c_str() + p + 2) != index)
        return -1;
    const size_t s = body.find(" S");
    return s == std::string::npos ? -1 : std::atoi(body.c_str() + s + 2);
}

// The part-cooling fan value an M106 (no P, or P0/P1) / M107 line sets, or -1.
int part_fan_value(const std::string &line)
{
    if (line.rfind("M107", 0) == 0)
        return 0;
    if (line.rfind("M106", 0) != 0)
        return -1;
    const std::string body = line.substr(0, line.find(';'));
    const size_t      p    = body.find(" P");
    if (p != std::string::npos && std::atoi(body.c_str() + p + 2) > 1)
        return -1;
    const size_t s = body.find(" S");
    return s == std::string::npos ? -1 : std::atoi(body.c_str() + s + 2);
}

bool is_extrusion(const std::string &line)
{
    if (line.rfind("G1 ", 0) != 0 && line.rfind("G2 ", 0) != 0 && line.rfind("G3 ", 0) != 0)
        return false;
    const std::string body = line.substr(0, line.find(';'));
    const size_t      e    = body.find(" E");
    const bool        xy   = body.find(" X") != std::string::npos || body.find(" Y") != std::string::npos;
    return xy && e != std::string::npos && std::atof(body.c_str() + e + 2) > 0.;
}

// Per layer (counted by ";TEST_LAYER" lines), the value of fan P<index> in force at the layer's first extrusion
// (-1 before any command for that fan).
std::vector<int> aux_fan_per_layer(const std::string &gcode, int index)
{
    std::vector<int>   out;
    std::istringstream in(gcode);
    std::string        line;
    int                layer = -1, fan = -1, last = -2;
    while (std::getline(in, line)) {
        if (! line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind(";TEST_LAYER", 0) == 0)
            ++layer;
        else if (const int v = indexed_fan_value(line, index); v >= 0)
            fan = v;
        else if (layer >= 0 && layer != last && is_extrusion(line)) {
            out.push_back(fan);
            last = layer;
        }
    }
    return out;
}

// Extrusion moves printed with the part fan at `value`.
size_t extrusions_at_part_fan(const std::string &gcode, int value)
{
    std::istringstream in(gcode);
    std::string        line;
    int                fan = -1;
    size_t             n   = 0;
    while (std::getline(in, line)) {
        if (const int v = part_fan_value(line); v >= 0)
            fan = v;
        else if (is_extrusion(line) && fan == value)
            ++n;
    }
    return n;
}

size_t count_lines_starting(const std::string &gcode, const std::string &prefix)
{
    std::istringstream in(gcode);
    std::string        line;
    size_t             n = 0;
    while (std::getline(in, line))
        if (line.rfind(prefix, 0) == 0)
            ++n;
    return n;
}

// A cube with the part fan steady at 50 % and the auxiliary fan on at 70 %.
DynamicPrintConfig aux_fan_config(int close_fan)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"},
        {"skirt_loops", 0},
        {"brim_type", "no_brim"},
        {"auxiliary_fan", "1"},
        {"additional_cooling_fan_speed", "70"},
        {"reduce_fan_stop_start_freq", "1"},
        {"fan_min_speed", "50"},
        {"fan_max_speed", "50"},
        {"enable_overhang_bridge_fan", "0"},
        {"close_fan_the_first_x_layers", std::to_string(close_fan)},
        {"full_fan_speed_layer", "0"},
        {"slow_down_for_layer_cooling", "0"},
        {"machine_start_gcode", ""},
        {"layer_change_gcode", ";TEST_LAYER_Z=[layer_z]"},
    });
    return config;
}

// A sphere whose lower half has overhanging outer walls cooled at 100 %, the layer fan at 20 %.
DynamicPrintConfig sphere_overhang_config()
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
        {"enable_overhang_speed", "0"},
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

} // namespace

TEST_CASE("Auxiliary fan speed ramps as Bambu Studio computes it", "[BambuCooling]")
{
    // Below the initial layers: the initial-layer speed.
    CHECK(calc_aux_fan_speed(0, 70, 40, 1, 0) == 40);
    CHECK(calc_aux_fan_speed(2, 70, 0, 3, 0) == 0);
    // From the initial layers on, without a ramp: the target.
    CHECK(calc_aux_fan_speed(1, 70, 40, 1, 0) == 70);
    CHECK(calc_aux_fan_speed(3, 70, 0, 3, 2) == 70); // a ramp end below the initial layers is ignored
    // A ramp from layer 1 to layer 3: factor (layer + 1 - 1) / (3 - 1).
    CHECK(calc_aux_fan_speed(1, 70, 40, 1, 3) == 55);
    CHECK(calc_aux_fan_speed(2, 70, 40, 1, 3) == 70);
    // The PA-CF presets: initial 3 layers at 0, ramp to layer 2 ignored (2 < 3).
    CHECK(calc_aux_fan_speed(3, 50, 0, 3, 2) == 50);
}

TEST_CASE("An unset auxiliary fan layer count follows the part fan's", "[BambuCooling]")
{
    CHECK(aux_fan_initial_layers(-1, 3) == 3);
    CHECK(aux_fan_initial_layers(-1, 0) == 0);
    CHECK(aux_fan_initial_layers(1, 3) == 1);
    CHECK(aux_fan_initial_layers(0, 3) == 0);
    // Our default keeps presets that do not set it as before the port; Bambu's own presets all set it.
    const auto *def = print_config_def.get("close_additional_fan_first_x_layers");
    REQUIRE(def != nullptr);
    CHECK(def->get_default_value<ConfigOptionInts>()->get_at(0) == -1);
    CHECK(print_config_def.get("first_x_layer_fan_speed")->get_default_value<ConfigOptionFloats>()->get_at(0) == 0.);
    CHECK(print_config_def.get("pre_start_fan_time")->get_default_value<ConfigOptionFloats>()->get_at(0) == 0.);
}

TEST_CASE("Without its own settings the auxiliary fan stays off as long as the part fan", "[BambuCooling]")
{
    const std::string      gcode = slice({TestMesh::cube_20x20x20}, aux_fan_config(3));
    const std::vector<int> aux   = aux_fan_per_layer(gcode, 2);
    REQUIRE(aux.size() > 5);
    CHECK(aux[0] == 0);
    CHECK(aux[2] == 0);
    CHECK(aux[3] == 178); // 70 %
    CHECK(aux[5] == 178);
}

TEST_CASE("The auxiliary fan runs its own early layer speed", "[BambuCooling]")
{
    DynamicPrintConfig config = aux_fan_config(3);
    config.set_deserialize_strict({{"close_additional_fan_first_x_layers", "1"}, {"first_x_layer_fan_speed", "40"}});
    const std::vector<int> aux = aux_fan_per_layer(slice({TestMesh::cube_20x20x20}, config), 2);
    REQUIRE(aux.size() > 3);
    CHECK(aux[0] == 102); // 40 % on the first layer, while the part fan is still off
    CHECK(aux[1] == 178); // then the auxiliary fan speed, independent of the part fan's 3 layers
    CHECK(aux[3] == 178);
}

TEST_CASE("The auxiliary fan ramps to its speed", "[BambuCooling]")
{
    DynamicPrintConfig config = aux_fan_config(1);
    config.set_deserialize_strict({{"close_additional_fan_first_x_layers", "1"}, {"first_x_layer_fan_speed", "40"},
                                   {"additional_fan_full_speed_layer", "3"}});
    const std::vector<int> aux = aux_fan_per_layer(slice({TestMesh::cube_20x20x20}, config), 2);
    REQUIRE(aux.size() > 3);
    CHECK(aux[0] == 102); // 40 %
    CHECK(aux[1] == 140); // 55 %
    CHECK(aux[2] == 178); // 70 %
    CHECK(aux[3] == 178);
}

TEST_CASE("Custom G-code reads the auxiliary fan settings", "[BambuCooling]")
{
    DynamicPrintConfig config = aux_fan_config(3);
    config.set_deserialize_strict({
        {"first_x_layer_fan_speed", "40"},
        {"additional_fan_full_speed_layer", "4"},
        {"layer_change_gcode", ";TEST_LAYER_Z=[layer_z]\n;AUX close=[close_additional_fan_first_x_layers[0]] "
                               "speed=[first_x_layer_fan_speed[0]] full=[additional_fan_full_speed_layer[0]]"},
    });
    const std::string gcode = slice({TestMesh::cube_20x20x20}, config);
    // -1 reaches the templates as the part fan's count.
    CHECK(gcode.find(";AUX close=3 speed=40 full=4") != std::string::npos);
    CHECK(gcode.find("close=-1") == std::string::npos);
}

TEST_CASE("Bambu 3MF export writes the part fan's count for an unset auxiliary fan count", "[BambuCooling]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"close_fan_the_first_x_layers", "3"}, {"close_additional_fan_first_x_layers", "-1"}});
    BambuExport::Context      ctx = BambuExport::Context::from_project(config);
    BambuExport::Report       report;
    const BambuExport::Config out = BambuExport::convert_project(config, ctx, report);
    const auto it = out.find("close_additional_fan_first_x_layers");
    REQUIRE(it != out.end());
    REQUIRE(! it->second.values.empty());
    CHECK(it->second.values.front() == "3");
}

TEST_CASE("The part fan starts before an overhang by the pre-start time", "[BambuCooling]")
{
    DynamicPrintConfig without = sphere_overhang_config();
    DynamicPrintConfig with    = sphere_overhang_config();
    with.set_deserialize_strict({{"pre_start_fan_time", "2"}});
    const std::string gcode_without = slice({make_sphere(15., PI / 24.)}, without);
    const std::string gcode_with    = slice({make_sphere(15., PI / 24.)}, with);
    const size_t      cooled_without = extrusions_at_part_fan(gcode_without, 255);
    const size_t      cooled_with    = extrusions_at_part_fan(gcode_with, 255);
    INFO(cooled_without << " moves at the overhang fan speed without the pre-start, " << cooled_with << " with it");
    REQUIRE(cooled_without > 0);
    CHECK(cooled_with > cooled_without);
    // Not the whole print: only moves within 2 s before an overhang start early.
    CHECK(extrusions_at_part_fan(gcode_with, 51) > 0);
}

TEST_CASE("The pre-start time is unused while the printer moves fan commands itself", "[BambuCooling]")
{
    // fan_speedup_time (FanMover) already moves the overhang fan commands earlier: pre_start_fan_time stays out.
    DynamicPrintConfig without = sphere_overhang_config();
    without.set_deserialize_strict({{"fan_speedup_time", "0.5"}, {"fan_speedup_overhangs", "1"}});
    DynamicPrintConfig with = without;
    with.set_deserialize_strict({{"pre_start_fan_time", "2"}});
    CHECK(slice({make_sphere(15., PI / 24.)}, with) == slice({make_sphere(15., PI / 24.)}, without));

    // Without the overhang fan there is nothing to start early.
    DynamicPrintConfig no_overhang_fan = sphere_overhang_config();
    no_overhang_fan.set_deserialize_strict({{"enable_overhang_bridge_fan", "0"}});
    DynamicPrintConfig no_overhang_fan_pre = no_overhang_fan;
    no_overhang_fan_pre.set_deserialize_strict({{"pre_start_fan_time", "2"}});
    CHECK(slice({make_sphere(15., PI / 24.)}, no_overhang_fan_pre) == slice({make_sphere(15., PI / 24.)}, no_overhang_fan));
}
