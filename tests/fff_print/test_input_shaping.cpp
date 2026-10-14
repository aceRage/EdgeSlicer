// Input shaping (OrcaSlicer #10913 / #10962 / #11202 / #14079 and the Edge Klipper override):
// the writer's command per firmware flavour, the machine override at the start of the print
// (default off: nothing is written until a user turns it on), and the calibration towers.
#include <catch2/catch.hpp>

#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "libslic3r/GCodeWriter.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/calib.hpp"
#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;
using Catch::Matchers::WithinAbs;

namespace {

// GCodeWriter::full_gcode_comment is a process-wide switch that export sets from gcode_comments;
// keep it off while calling the writer directly, and put it back afterwards.
struct NoWriterComments
{
    bool saved = GCodeWriter::full_gcode_comment;
    NoWriterComments() { GCodeWriter::full_gcode_comment = false; }
    ~NoWriterComments() { GCodeWriter::full_gcode_comment = saved; }
};

GCodeWriter writer_for(GCodeFlavor flavor)
{
    GCodeWriter writer;
    writer.config.gcode_flavor.value = flavor;
    return writer;
}

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

// Every line that sets input shaping, whatever the flavour.
std::vector<std::string> input_shaping_lines(const std::string &gcode)
{
    std::vector<std::string> out = lines_starting_with(gcode, "M593");
    for (const std::string &line : lines_starting_with(gcode, "SET_INPUT_SHAPER"))
        out.push_back(line);
    return out;
}

DynamicPrintConfig printer_config(const char *flavor, std::initializer_list<ConfigBase::SetDeserializeItem> extra = {})
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"gcode_flavor", flavor}, {"skirt_loops", "0"}, {"brim_type", "no_brim"}});
    if (extra.size() > 0)
        config.set_deserialize_strict(extra);
    return config;
}

std::string slice_cube(const DynamicPrintConfig &config, bool bbl = false, const Calib_Params *calib = nullptr)
{
    Print print;
    Model model;
    init_print({TestMesh::cube_20x20x20}, print, model, config);
    print.is_BBL_printer() = bbl;
    if (calib != nullptr)
        print.set_calib_params(*calib);
    return gcode(print);
}

} // namespace

TEST_CASE("Input shaping commands per firmware flavour", "[GCodeWriter][InputShaping]")
{
    NoWriterComments no_comments;

    SECTION("Klipper") {
        GCodeWriter w = writer_for(gcfKlipper);
        CHECK(w.set_input_shaping('X', 0.1f, 52.5f, "MZV") == "SET_INPUT_SHAPER SHAPER_TYPE=MZV SHAPER_FREQ_X=52.50 DAMPING_RATIO_X=0.100\n");
        CHECK(w.set_input_shaping('Y', 0.f, 40.f, "") == "SET_INPUT_SHAPER SHAPER_FREQ_Y=40.00\n");
        CHECK(w.set_input_shaping('A', 0.f, 30.f, "Default") == "SET_INPUT_SHAPER SHAPER_FREQ_X=30.00 SHAPER_FREQ_Y=30.00\n");
        CHECK(w.set_input_shaping('X', 0.3f, 50.f, "Disable") ==
              "SET_INPUT_SHAPER SHAPER_FREQ_X=0.00 SHAPER_FREQ_Y=0.00 DAMPING_RATIO_X=0.000 DAMPING_RATIO_Y=0.000\n");
        // Nothing to set: nothing written.
        CHECK(w.set_input_shaping('A', 0.f, 0.f, "Default") == "");
    }
    SECTION("RepRapFirmware") {
        GCodeWriter w = writer_for(gcfRepRapFirmware);
        CHECK(w.set_input_shaping('X', 0.1f, 40.f, "ZVD") == "M593 P\"ZVD\" F40.00 S0.100\n");
        // DAA and Default name no type.
        CHECK(w.set_input_shaping('X', 0.f, 40.f, "DAA") == "M593 F40.00\n");
        CHECK(w.set_input_shaping('X', 0.f, 0.f, "Disable") == "M593 F0.00 S0.000\n");
        CHECK(w.set_input_shaping('A', 0.f, 0.f, "") == "");
    }
    SECTION("Marlin 2") {
        GCodeWriter w = writer_for(gcfMarlinFirmware);
        // Marlin has ZV only; the type is not written.
        CHECK(w.set_input_shaping('X', 0.15f, 35.f, "ZV") == "M593 X F35.00 D0.150\n");
        CHECK(w.set_input_shaping('Y', 0.1f, 0.f, "") == "M593 Y D0.100\n");
        CHECK(w.set_input_shaping('A', 0.f, 0.f, "Disable") == "M593 F0.00 D0.000\n");
        CHECK(w.set_input_shaping('A', 0.f, 0.f, "") == "");
    }
    SECTION("Marlin legacy (Bambu printers) and other flavours refuse") {
        CHECK_THROWS_AS(writer_for(gcfMarlinLegacy).set_input_shaping('X', 0.1f, 40.f, ""), std::runtime_error);
        CHECK_THROWS_AS(writer_for(gcfRepRapSprinter).set_input_shaping('X', 0.1f, 40.f, ""), std::runtime_error);
        CHECK_THROWS_AS(writer_for(gcfSmoothie).set_input_shaping('X', 0.1f, 40.f, ""), std::runtime_error);
    }
    SECTION("Invalid values refuse") {
        GCodeWriter w = writer_for(gcfKlipper);
        CHECK_THROWS_AS(w.set_input_shaping('X', 1.5f, 40.f, ""), std::runtime_error);
        CHECK_THROWS_AS(w.set_input_shaping('X', 0.1f, -1.f, ""), std::runtime_error);
        CHECK_THROWS_AS(w.set_input_shaping('Q', 0.1f, 40.f, ""), std::runtime_error);
    }
}

TEST_CASE("Input shaping options default off and write nothing", "[InputShaping][Slice]")
{
    const DynamicPrintConfig defaults = DynamicPrintConfig::full_print_config();
    CHECK_FALSE(defaults.opt_bool("input_shaping_emit"));
    CHECK(defaults.opt_enum<InputShaperType>("input_shaping_type") == InputShaperType::Default);

    for (const char *flavor : {"marlin", "marlin2", "reprapfirmware", "klipper"}) {
        INFO("flavour " << flavor);
        CHECK(input_shaping_lines(slice_cube(printer_config(flavor))).empty());
    }
}

TEST_CASE("Machine input shaping override at the start of the print", "[InputShaping][Slice]")
{
    SECTION("Marlin 2: X and Y with the machine limits") {
        const std::string g = slice_cube(printer_config("marlin2", {{"input_shaping_emit", "1"}, {"input_shaping_type", "ZV"},
            {"input_shaping_freq_x", "40"}, {"input_shaping_freq_y", "35"}, {"input_shaping_damp_x", "0.1"}, {"input_shaping_damp_y", "0.15"}}));
        const std::vector<std::string> lines = input_shaping_lines(g);
        REQUIRE(lines.size() == 2);
        CHECK(lines[0] == "M593 X F40.00 D0.100");
        CHECK(lines[1] == "M593 Y F35.00 D0.150");
        // With the machine envelope, before the first layer.
        CHECK(g.find("M593 X") > g.find("M201 X"));
        CHECK(g.find("M593 X") < g.find("\nG1 "));
    }
    SECTION("Marlin 2 without machine limits in the G-code: nothing") {
        CHECK(input_shaping_lines(slice_cube(printer_config("marlin2", {{"input_shaping_emit", "1"},
            {"input_shaping_freq_x", "40"}, {"emit_machine_limits_to_gcode", "0"}}))).empty());
    }
    SECTION("Marlin 2, Disable") {
        const std::vector<std::string> lines = input_shaping_lines(slice_cube(printer_config("marlin2",
            {{"input_shaping_emit", "1"}, {"input_shaping_type", "Disable"}, {"input_shaping_freq_x", "40"}})));
        REQUIRE(lines.size() == 1);
        CHECK(lines[0] == "M593 F0.00 D0.000");
    }
    SECTION("RepRapFirmware: one command for both axes") {
        const std::vector<std::string> lines = input_shaping_lines(slice_cube(printer_config("reprapfirmware",
            {{"input_shaping_emit", "1"}, {"input_shaping_type", "MZV"}, {"input_shaping_freq_x", "42"}, {"input_shaping_damp_x", "0.1"}})));
        REQUIRE(lines.size() == 1);
        CHECK(lines[0] == "M593 P\"MZV\" F42.00 S0.100");
    }
    SECTION("Klipper (Edge): SET_INPUT_SHAPER whatever the machine limits setting says") {
        for (const char *limits : {"0", "1"}) {
            INFO("emit_machine_limits_to_gcode " << limits);
            const std::string g = slice_cube(printer_config("klipper", {{"input_shaping_emit", "1"}, {"input_shaping_type", "MZV"},
                {"input_shaping_freq_x", "48"}, {"input_shaping_freq_y", "44"}, {"input_shaping_damp_x", "0.1"}, {"input_shaping_damp_y", "0.12"},
                {"emit_machine_limits_to_gcode", limits}}));
            const std::vector<std::string> lines = input_shaping_lines(g);
            REQUIRE(lines.size() == 2);
            CHECK(lines[0] == "SET_INPUT_SHAPER SHAPER_TYPE=MZV SHAPER_FREQ_X=48.00 DAMPING_RATIO_X=0.100");
            CHECK(lines[1] == "SET_INPUT_SHAPER SHAPER_FREQ_Y=44.00 DAMPING_RATIO_Y=0.120");
            // Klipper still gets no machine limits.
            CHECK(lines_starting_with(g, "M201").empty());
            CHECK(g.find("SET_INPUT_SHAPER") < g.find("\nG1 "));
        }
    }
    SECTION("Klipper (Edge), Disable") {
        const std::vector<std::string> lines = input_shaping_lines(slice_cube(printer_config("klipper",
            {{"input_shaping_emit", "1"}, {"input_shaping_type", "Disable"}})));
        REQUIRE(lines.size() == 1);
        CHECK(lines[0] == "SET_INPUT_SHAPER SHAPER_FREQ_X=0.00 SHAPER_FREQ_Y=0.00 DAMPING_RATIO_X=0.000 DAMPING_RATIO_Y=0.000");
    }
    SECTION("Marlin legacy (the Bambu flavour): nothing, no error") {
        CHECK(input_shaping_lines(slice_cube(printer_config("marlin", {{"input_shaping_emit", "1"},
            {"input_shaping_freq_x", "40"}}))).empty());
    }
    SECTION("A Bambu printer never gets it, whatever its flavour") {
        for (const char *flavor : {"marlin2", "klipper"}) {
            INFO("flavour " << flavor);
            CHECK(input_shaping_lines(slice_cube(printer_config(flavor, {{"input_shaping_emit", "1"},
                {"input_shaping_freq_x", "40"}}), true)).empty());
        }
    }
}

TEST_CASE("Input shaping frequency calibration steps the frequency across layers", "[InputShaping][Calib]")
{
    Calib_Params params;
    params.mode        = CalibMode::Calib_Input_shaping_freq;
    params.start       = 0.;  // damping: firmware value
    params.freqStartX  = 20.;
    params.freqEndX    = 60.;
    params.freqStartY  = 20.;
    params.freqEndY    = 60.;
    params.shaper_type = "MZV";

    const std::string g = slice_cube(printer_config("klipper"), false, &params);
    CHECK(lines_starting_with(g, "SET_VELOCITY_LIMIT MINIMUM_CRUISE_RATIO=0").size() == 1);
    const std::vector<std::string> lines = lines_starting_with(g, "SET_INPUT_SHAPER");
    // The type is set once (second layer, damping 0: the firmware's), then one frequency per layer,
    // from the start up to (and clamped at) the end.
    std::vector<double> freqs;
    size_t              type_lines = 0;
    for (const std::string &line : lines) {
        if (line == "SET_INPUT_SHAPER SHAPER_TYPE=MZV") {
            ++type_lines;
            continue;
        }
        const size_t at = line.find("SHAPER_FREQ_X=");
        REQUIRE(at != std::string::npos);
        CHECK(line.find("SHAPER_FREQ_Y=") != std::string::npos);
        freqs.push_back(std::stod(line.substr(at + 14)));
    }
    CHECK(type_lines == 1);
    REQUIRE(freqs.size() > 3);
    CHECK_THAT(freqs.front(), WithinAbs(20., 1e-6));
    CHECK_THAT(freqs.back(), WithinAbs(60., 1e-6));
    for (size_t i = 1; i < freqs.size(); ++i)
        CHECK(freqs[i] >= freqs[i - 1]);
}

TEST_CASE("Input shaping calibration refuses a Marlin legacy printer", "[InputShaping][Calib]")
{
    Calib_Params params;
    params.mode       = CalibMode::Calib_Input_shaping_freq;
    params.freqStartX = params.freqStartY = 20.;
    params.freqEndX   = params.freqEndY   = 60.;
    CHECK_THROWS(slice_cube(printer_config("marlin"), false, &params));
}

TEST_CASE("Cornering calibration steps junction deviation or jerk", "[InputShaping][Calib]")
{
    Calib_Params params;
    params.mode  = CalibMode::Calib_Cornering;
    params.start = 1.;
    params.end   = 15.;

    SECTION("Klipper: square corner velocity") {
        const std::string g = slice_cube(printer_config("klipper", {{"machine_max_jerk_x", "20"}, {"machine_max_jerk_y", "20"}}), false, &params);
        const std::vector<std::string> lines = lines_starting_with(g, "SET_VELOCITY_LIMIT SQUARE_CORNER_VELOCITY=");
        REQUIRE(lines.size() > 3);
        CHECK(lines.back() == "SET_VELOCITY_LIMIT SQUARE_CORNER_VELOCITY=15");
        CHECK(input_shaping_lines(g).empty());
    }
    SECTION("Marlin 2 with a machine junction deviation: M205 J") {
        Calib_Params jd = params;
        jd.start = 0.;
        jd.end   = 0.25;
        const std::string g = slice_cube(printer_config("marlin2", {{"machine_max_junction_deviation", "0.3"}}), false, &jd);
        const std::vector<std::string> lines = lines_starting_with(g, "M205 J");
        // The machine envelope's own M205 J0.300, then one per layer up to the end value.
        REQUIRE(lines.size() > 3);
        CHECK(lines.front() == "M205 J0.300");
        CHECK(lines.back() == "M205 J0.250");
    }
}
