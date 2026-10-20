// Calibration G-code ported from OrcaSlicer 6A-7b (#14440 PA line, #14759 PA tower). Calibration projects only;
// a print with no calibration is untouched (the shared G-code gate covers that).
#include <catch2/catch.hpp>

#include <cmath>
#include <set>
#include <sstream>
#include <string>
#include <vector>

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
