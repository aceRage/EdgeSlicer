// Orca #14685 / Bambu Studio 02.09.01.70: the cooling filter accessory. The machine G-code placeholder
// cooling_filter_enabled carries the printer setting instead of a hard-coded false.
//
// Catch2 v2 headers; Test::slice + TestMesh cube.

#include <catch2/catch.hpp>

#include <string>

#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

// The cooling filter placeholder carries the printer setting into the machine G-code (it used to be a hard-coded false).
TEST_CASE("Machine start G-code sees cooling_filter_enabled", "[OrcaPackage5][CoolingFilter][GCode]")
{
    auto marker = [](bool enabled) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            {"cooling_filter_enabled", enabled ? "1" : "0"},
            {"skirt_loops",            "0"},
            {"brim_type",              "no_brim"},
            {"gcode_flavor",           "marlin"},
        });
        config.set_key_value("machine_start_gcode",
                             new ConfigOptionString("; COOLING_FILTER {if cooling_filter_enabled}on{else}off{endif}\n"));
        Print print;
        Model model;
        init_print({TestMesh::cube_20x20x20}, print, model, config);
        const std::string gcode = Test::gcode(print);
        const size_t      pos   = gcode.find("; COOLING_FILTER ");
        REQUIRE(pos != std::string::npos);
        return gcode.substr(pos + 17, gcode.find('\n', pos) - pos - 17);
    };
    CHECK(marker(false) == "off");
    CHECK(marker(true) == "on");
}
