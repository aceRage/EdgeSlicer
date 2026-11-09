// Calibration G-code ported from OrcaSlicer 6A-7b (#14440 PA line, #14759 PA tower). Calibration projects only;
// a print with no calibration is untouched (the shared G-code gate covers that).
#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "libslic3r/Flow.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/calib.hpp"
#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

std::vector<std::string> lines_starting_with(const std::string &gcode, const std::string &prefix)
{
    std::vector<std::string> out;
    std::istringstream       in(gcode);
    std::string              line;
    while (std::getline(in, line))
        if (line.rfind(prefix, 0) == 0)
            out.push_back(line);
    return out;
}

std::string slice_cube(const DynamicPrintConfig &config, const Calib_Params *calib)
{
    Print print;
    Model model;
    init_print({TestMesh::cube_20x20x20}, print, model, config);
    if (calib != nullptr)
        print.set_calib_params(*calib);
    return gcode(print);
}

DynamicPrintConfig klipper_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"gcode_flavor", "klipper"}, {"skirt_loops", "0"}, {"brim_type", "no_brim"},
                                   {"layer_height", "0.2"}, {"initial_layer_print_height", "0.2"}, {"exclude_object", "1"}});
    return config;
}

double advance_of(const std::string &line)
{
    const std::string key = "ADVANCE=";
    return std::stod(line.substr(line.find(key) + key.size()));
}

struct Move
{
    double feedrate; // mm/min
    double e_per_mm;
};

// Extruding moves under a ";TYPE:<type>" tag: only the first one after each tag, or all of them. The PA line test prints
// no object, so its fast segments are the first move after each ";TYPE:Outer wall" and the box under its numbers is
// all of ";TYPE:Bottom surface".
std::vector<Move> extrusions_of_type(const std::string &gcode, const std::string &type, bool first_only)
{
    const std::string  tag = ";TYPE:" + type;
    std::vector<Move>  out;
    std::istringstream lines(gcode);
    std::string        line;
    bool               relative = false;
    bool               armed    = false;
    double             x = 0., y = 0., e = 0., f = 0.;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind(";TYPE:", 0) == 0) {
            armed = line == tag;
            continue;
        }
        if (line.rfind("M83", 0) == 0)
            relative = true;
        else if (line.rfind("M82", 0) == 0)
            relative = false;
        const bool is_move = line.rfind("G1 ", 0) == 0 || line.rfind("G0 ", 0) == 0;
        if (!is_move && line.rfind("G92 ", 0) != 0)
            continue;

        std::istringstream words(line.substr(0, line.find(';')));
        std::string        word;
        words >> word; // the command
        double nx = x, ny = y, ev = 0.;
        bool   has_e = false;
        while (words >> word) {
            const double v = std::stod(word.substr(1));
            switch (word[0]) {
            case 'X': nx = v; break;
            case 'Y': ny = v; break;
            case 'E': ev = v; has_e = true; break;
            case 'F': f = v; break;
            default: break;
            }
        }
        if (!is_move) { // G92
            if (has_e)
                e = ev;
            continue;
        }
        const double length = std::hypot(nx - x, ny - y);
        if (has_e && armed && length > 0.) {
            out.push_back({f, (relative ? ev : ev - e) / length});
            armed = !first_only;
        }
        if (has_e)
            e = relative ? e + ev : ev;
        x = nx;
        y = ny;
    }
    return out;
}

} // namespace

TEST_CASE("The PA tower steps pressure advance in equal bands from start to end", "[Calib][PressureAdvance][Slice]")
{
    // Orca #14759: the value came from print_z in millimetres (start + int(z) * step), so a 20 mm cube with
    // 0..0.02 in 0.002 steps went up to 0.04; it is now spread over the layers and ends at `end`.
    Calib_Params params;
    params.mode  = CalibMode::Calib_PA_Tower;
    params.start = 0.0;
    params.end   = 0.02;
    params.step  = 0.002;
    const std::vector<std::string> lines = lines_starting_with(slice_cube(klipper_config(), &params), "SET_PRESSURE_ADVANCE");
    REQUIRE(lines.size() >= 100); // one per layer

    std::set<double> values;
    double           last = -1.;
    for (const std::string &line : lines) {
        const double pa = advance_of(line);
        CHECK(pa >= last - 1e-9);
        last = pa;
        values.insert(std::round(pa * 10000.) / 10000.);
    }
    CHECK(advance_of(lines.front()) == Approx(0.0));
    CHECK(advance_of(lines.back()) == Approx(0.02));
    CHECK(values.size() == 11);
}

TEST_CASE("The PA line calibration defines no exclude-object area", "[Calib][PressureAdvance][Slice]")
{
    // Orca #14440: the PA line test is a single pattern; Klipper got a bed-sized EXCLUDE_OBJECT_DEFINE for it.
    const DynamicPrintConfig config = klipper_config();
    CHECK_FALSE(lines_starting_with(slice_cube(config, nullptr), "EXCLUDE_OBJECT_DEFINE").empty());

    Calib_Params params;
    params.mode  = CalibMode::Calib_PA_Line;
    params.start = 0.0;
    params.end   = 0.04;
    params.step  = 0.002;
    const std::string g = slice_cube(config, &params);
    CHECK(lines_starting_with(g, "EXCLUDE_OBJECT_DEFINE").empty());
    // The pattern's fast segments are labelled as outer wall for the preview.
    CHECK_FALSE(lines_starting_with(g, ";TYPE:Custom").empty());
    CHECK_FALSE(lines_starting_with(g, ";TYPE:Outer wall").empty());
}

TEST_CASE("The PA line test prints with the speed and extrusion of the filament assigned to it", "[Calib][PressureAdvance][Slice]")
{
    // Orca #16882. Filament 1: 1.75 mm, flow ratio 1, up to 21 mm3/s. Filament 2: 2.85 mm, flow ratio 0.9, up to
    // 4 mm3/s, so its volumetric limit caps the fast lines well below filament 1's.
    for (const int filament : {1, 2}) {
        for (const bool print_numbers : {false, true}) {
            CAPTURE(filament, print_numbers);
            DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
            config.set_num_extruders(2);
            config.set_num_filaments(2);
            config.set_deserialize_strict({
                {"nozzle_diameter",              "0.4,0.4"},
                {"single_extruder_multi_material", "1"},
                {"filament_diameter",            "1.75,2.85"},
                {"filament_flow_ratio",          "1,0.9"},
                {"filament_max_volumetric_speed", "21,4"},
                {"print_flow_ratio",             "1"},
                {"outer_wall_speed",             "200"},
                {"initial_layer_print_height",   "0.2"},
                {"layer_height",                 "0.2"},
                {"enable_prime_tower",           "0"},
                {"skirt_loops",                  "0"},
                {"brim_type",                    "no_brim"},
                {"wall_filament",                std::to_string(filament)},
                {"sparse_infill_filament",       std::to_string(filament)},
                {"solid_infill_filament",        std::to_string(filament)},
            });
            config.option<ConfigOptionStrings>("filament_colour")->values = {"#FF0000", "#00FF00"};

            Calib_Params params;
            params.mode          = CalibMode::Calib_PA_Line;
            params.start         = 0.;
            params.end           = 0.02;
            params.step          = 0.002;
            params.print_numbers = print_numbers;
            const std::string       g        = slice_cube(config, &params);
            const std::vector<Move> segments = extrusions_of_type(g, "Outer wall", true);
            REQUIRE_FALSE(segments.empty());
            // The box under the numbers is drawn with the same line width and height as the PA lines.
            const std::vector<Move> box = extrusions_of_type(g, "Bottom surface", false);
            CHECK(box.empty() != print_numbers);

            // The test prints 1.5 nozzle-wide lines on the 0.2 mm first layer (CalibPressureAdvanceLine).
            const size_t idx        = size_t(filament - 1);
            const double mm3_per_mm = Flow(0.4 * 1.5, 0.2, 0.4).mm3_per_mm();
            const double max_vol    = config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->get_at(idx);
            const double diameter   = config.option<ConfigOptionFloats>("filament_diameter")->get_at(idx);
            const double flow_ratio = config.option<ConfigOptionFloats>("filament_flow_ratio")->get_at(idx);
            // Fast lines run at the outer wall speed (at least 100 mm/s), capped by the filament's volumetric speed.
            const double fast_speed = std::floor(std::min(std::max(100., 200.), max_vol / mm3_per_mm));
            const double e_per_mm   = mm3_per_mm * flow_ratio / (PI * diameter * diameter / 4.);
            for (const Move &move : segments) {
                CHECK(move.feedrate == Approx(fast_speed * 60.).margin(1.));
                CHECK(move.e_per_mm == Approx(e_per_mm).epsilon(0.005));
            }
            for (const Move &move : box)
                CHECK(move.e_per_mm == Approx(e_per_mm).epsilon(0.005));
        }
    }
}
