// Port of OrcaSlicer #16857: Adaptive PA must read the speed of an extrusion that
// inherits the travel feedrate. CoolingBuffer drops a G1 F that repeats the F the
// preceding travel already set, so when travel speed equals wall speed the wall
// extrusion has no G1 F of its own. Without modal feedrate tracking, Adaptive PA
// falls back to the last standalone G1 F (the first layer's 30 mm/s) for the rest
// of the print.
//
// Edge keeps this as a separate file from test_adaptive_pa.cpp (in-band toolchange
// reset_marker determinism). Catch2 v2 headers; Test::slice + TestMesh cube.

#include <catch2/catch.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "libslic3r/PrintConfig.hpp"
#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

// The speeds, in mm/s, the adaptive pressure advance comments report for the extrusions above the first layer.
static std::vector<double> adaptive_pa_speeds_above_first_layer(const std::string &gcode)
{
    const std::string token = "; APA Speed Used: ";
    std::vector<double> speeds;
    int layer = 0;
    std::istringstream stream(gcode);
    for (std::string line; std::getline(stream, line);) {
        if (line.rfind(";LAYER_CHANGE", 0) == 0)
            ++layer;
        else if (layer > 1 && line.rfind(token, 0) == 0)
            speeds.push_back(std::stod(line.substr(token.size())));
    }
    return speeds;
}

// The cooling buffer drops a G1 F that repeats the feedrate the preceding travel set, so the wall
// extrusion then inherits the travel feedrate without a G1 F line of its own.
TEST_CASE("Adaptive pressure advance reads the speed of an extrusion that inherits the travel feedrate", "[AdaptivePressureAdvance][AdaptivePA]")
{
    const int travel_speed = GENERATE(120, 150);
    DYNAMIC_SECTION("travel speed " << travel_speed) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "gcode_flavor",                      "marlin2" },
            { "gcode_comments",                    1 },
            { "enable_pressure_advance",           "1" },
            { "adaptive_pressure_advance",         "1" },
            { "adaptive_pressure_advance_model",   "0.08,1,4000\n0.04,20,4000\n0.08,1,100000\n0.04,20,100000" },
            { "pressure_advance",                  "0.08" },
            { "wall_loops",                        1 },
            { "sparse_infill_density",             0 },
            { "top_shell_layers",                  0 },
            { "bottom_shell_layers",               0 },
            { "skirt_loops",                       0 },
            { "brim_type",                         "no_brim" },
            { "initial_layer_speed",               30 },
            // the wall speed must not be capped by the volumetric limit
            { "filament_max_volumetric_speed",     "200" },
            { "outer_wall_speed",                  150 },
            { "inner_wall_speed",                  150 },
            { "travel_speed",                      travel_speed },
            { "initial_layer_travel_speed",        travel_speed },
            { "slow_down_for_layer_cooling",       "0" },
            { "slow_down_layer_time",              "0" },
            { "enable_overhang_speed",             "0" },
            { "wipe",                              "0" },
            { "retraction_length",                 "0" },
        });
        const std::string gcode = slice({ TestMesh::cube_20x20x20 }, config);

        const std::vector<double> speeds = adaptive_pa_speeds_above_first_layer(gcode);
        REQUIRE(!speeds.empty());
        // every wall above the first layer prints at the wall speed, whatever the travel speed
        for (const double speed : speeds)
            CHECK_THAT(speed, Catch::Matchers::WithinAbs(150.0, 0.01));
    }
}

// Orca #17033: Print::validate names the filament and the invalid line, one warning per filament.
// Edge has no per-feature *_filament_id keys (Orca process options). The cube is printed with
// `filament` through the object extruder (1-based), which is what Print::extruders() already uses.
static std::vector<std::string> adaptive_pa_model_warnings(const std::vector<std::string> &models, int filament = 1)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    const unsigned int n      = static_cast<unsigned int>(models.size());
    config.set_num_extruders(n);
    config.set_num_filaments(n);
    config.set_deserialize_strict({
        // the default config fails validation on relative extrusion without a G92 E0 at each layer
        { "use_relative_e_distances", "0" },
    });
    config.option<ConfigOptionBools>("enable_pressure_advance")->values.assign(n, true);
    config.option<ConfigOptionBools>("adaptive_pressure_advance")->values.assign(n, true);
    config.option<ConfigOptionFloats>("filament_diameter")->values.assign(n, 1.75);
    config.option<ConfigOptionStrings>("adaptive_pressure_advance_model")->values = models;
    // A second filament would otherwise raise an unrelated prime-tower / wipe-tower check.
    config.option<ConfigOptionBool>("enable_prime_tower")->value = false;

    Print  print;
    Model  model;
    ModelObject *object = model.add_object();
    object->add_volume(mesh(TestMesh::cube_20x20x20));
    object->add_instance()->set_offset(Vec3d(80., 40., 0.));
    object->ensure_on_bed();
    object->config.set("extruder", filament);
    print.apply(model, config);

    std::vector<StringObjectException> warnings;
    REQUIRE(print.validate(&warnings).string.empty());
    std::vector<std::string> texts;
    for (const StringObjectException &warning : warnings)
        if (warning.opt_key == "adaptive_pressure_advance_model")
            texts.push_back(warning.string);
    return texts;
}

static std::vector<std::string> adaptive_pa_model_warnings(const std::string &model)
{
    return adaptive_pa_model_warnings(std::vector<std::string>{ model });
}

TEST_CASE("Slicing names the filament and the line of an invalid adaptive pressure advance model", "[AdaptivePressureAdvance]")
{
    // a period where the comma belongs leaves the second line with two values
    const std::vector<std::string> warnings = adaptive_pa_model_warnings("0.07,4.71,500\n0.04.7.53,1000\n0.035,9.41,1000");
    REQUIRE(warnings.size() == 1);
    CHECK(warnings.front().find("filament 1") != std::string::npos);
    CHECK(warnings.front().find("Line 2") != std::string::npos);
}

TEST_CASE("Slicing names the second filament when its adaptive pressure advance model is invalid", "[AdaptivePressureAdvance]")
{
    const std::vector<std::string> warnings = adaptive_pa_model_warnings(
        { "0.07,4.71,500\n0.035,9.41,1000", "0.07,4.71,500\n0.04.7.53,1000" }, 2);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings.front().find("filament 2") != std::string::npos);
    CHECK(warnings.front().find("Line 2") != std::string::npos);
}
