// Port of OrcaSlicer #14763 (tests/fff_print/test_multifilament.cpp): when only one of several configured
// filaments prints, the T command comes before the filament start G-code, and a single-extruder
// multi-material printer without a prime tower sets the temperature again after it.
#include <catch2/catch.hpp>

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"

#include "test_data.hpp"

#include <initializer_list>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {

// One nozzle and `filaments` filaments, as upstream's test helper of the same name.
DynamicPrintConfig multifilament_config(unsigned int filaments, std::initializer_list<ConfigBase::SetDeserializeItem> extra)
{
    static const char *palette[] = { "#FF0000", "#00FF00", "#0000FF", "#FFFF00" };
    std::string diameters, colours, flush;
    for (unsigned int i = 0; i < filaments; ++ i) {
        diameters += (i ? "," : "") + std::string("1.75");
        colours   += (i ? ";" : "") + std::string(palette[i % 4]);
        for (unsigned int j = 0; j < filaments; ++ j)
            flush += ((i || j) ? "," : "") + std::string(i == j ? "0" : "280");
    }
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({ { "nozzle_diameter", "0.4" }, { "filament_diameter", diameters } });
    config.set_num_filaments(filaments);
    const auto &defaults = FullPrintConfig::defaults();
    for (const char *key : { "filament_type", "filament_vendor", "filament_start_gcode", "nozzle_temperature", "nozzle_temperature_initial_layer" })
        static_cast<ConfigOptionVectorBase *>(config.option(key, true))->resize(filaments, defaults.option(key));
    config.set_deserialize_strict({ { "filament_colour", colours }, { "flush_volumes_matrix", flush } });
    config.set_deserialize_strict(extra);
    return config;
}

std::string two_temperatures(int temp) { return std::to_string(temp) + "," + std::to_string(temp); }

std::string slice_cube(const DynamicPrintConfig &config)
{
    return Slic3r::Test::slice({ TriangleMesh(make_cube(20., 20., 20.)) }, config);
}

// The commands from the first T0 up to the first layer change, T0 left out. Empty when there is no T0.
struct AfterT0
{
    bool                     found_t0 = false;
    std::vector<std::string> cmds;
    std::vector<int>         temperatures;
};

AfterT0 after_first_t0(const std::string &gcode)
{
    AfterT0     out;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&out](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (line.raw().find(";LAYER_CHANGE") != std::string::npos || line.raw().find("; CHANGE_LAYER") != std::string::npos) {
            if (out.found_t0)
                self.quit_parsing();
            return;
        }
        const std::string cmd(line.cmd());
        if (cmd.empty())
            return;
        if (! out.found_t0) {
            out.found_t0 = cmd == "T0";
            return;
        }
        out.cmds.push_back(cmd);
        float s = 0;
        if ((cmd == "M104" || cmd == "M109") && line.has_value('S', s))
            out.temperatures.push_back(int(s));
    });
    return out;
}

} // namespace

TEST_CASE("Single-extruder MM sets the temperature right after the T0 of a one-filament print", "[MultiFilament][Orca14763]")
{
    const int     expected_temp = 240;
    const AfterT0 after = after_first_t0(slice_cube(multifilament_config(2, {
        { "single_extruder_multi_material",   "1" },
        { "nozzle_temperature",               two_temperatures(expected_temp) },
        { "nozzle_temperature_initial_layer", two_temperatures(expected_temp) },
        { "machine_start_gcode",              "" },
        { "enable_prime_tower",               "0" },
    })));

    REQUIRE(after.found_t0);
    REQUIRE_FALSE(after.cmds.empty());
    REQUIRE(after.cmds.front() == "M104");
    REQUIRE_FALSE(after.temperatures.empty());
    REQUIRE(after.temperatures.front() == expected_temp);
}

// Edge: a toolchanger (the U1) gets the T command but no temperature of the slicer's own after it.
TEST_CASE("A multi-tool printer gets no temperature after the T0 of a one-filament print", "[MultiFilament][Orca14763]")
{
    const std::string gcode = slice_cube(multifilament_config(2, {
        { "single_extruder_multi_material",   "0" },
        { "nozzle_temperature",               two_temperatures(240) },
        { "nozzle_temperature_initial_layer", two_temperatures(240) },
        { "filament_start_gcode",             "M118 FILAMENT_START_MARK\n" },
        { "machine_start_gcode",              "" },
        { "enable_prime_tower",               "0" },
    }));
    const AfterT0 after = after_first_t0(gcode);

    REQUIRE(after.found_t0);
    CHECK(after.temperatures.empty());
    // The T command comes before the filament start G-code.
    const size_t t0   = gcode.find("\nT0");
    const size_t mark = gcode.find("M118 FILAMENT_START_MARK");
    REQUIRE(t0 != std::string::npos);
    REQUIRE(mark != std::string::npos);
    CHECK(t0 < mark);
}

// A print with one filament configured must not get a T command or a temperature after the start G-code.
TEST_CASE("A print with one filament configured gets no T command and no temperature after the start G-code", "[MultiFilament][Orca14763]")
{
    const std::string gcode = slice_cube(multifilament_config(1, {
        { "single_extruder_multi_material", "1" },
        { "machine_start_gcode",            "; START_GCODE_END\n" },
        { "enable_prime_tower",             "0" },
    }));

    const size_t start_pos = gcode.find("; START_GCODE_END");
    REQUIRE(start_pos != std::string::npos);
    size_t layer_pos = gcode.find(";LAYER_CHANGE", start_pos);
    if (layer_pos == std::string::npos)
        layer_pos = gcode.find("; CHANGE_LAYER", start_pos);
    REQUIRE(layer_pos != std::string::npos);

    const std::string between = gcode.substr(start_pos, layer_pos - start_pos);
    CHECK(between.find("M104") == std::string::npos);
    CHECK(between.find("M109") == std::string::npos);
    CHECK(between.find("\nT0") == std::string::npos);
}

// A temperature in the filament start G-code comes after the slicer's own and so wins.
TEST_CASE("A temperature in the filament start G-code wins on a one-filament single-extruder MM print", "[MultiFilament][Orca14763]")
{
    const int     slicer_temp = 240;
    const int     custom_temp = 255;
    const std::string start   = "M104 S" + std::to_string(custom_temp) + "\n";
    const AfterT0 after = after_first_t0(slice_cube(multifilament_config(2, {
        { "single_extruder_multi_material",   "1" },
        { "nozzle_temperature",               two_temperatures(slicer_temp) },
        { "nozzle_temperature_initial_layer", two_temperatures(slicer_temp) },
        { "filament_start_gcode",             start },
        { "machine_start_gcode",              "" },
        { "enable_prime_tower",               "0" },
    })));

    REQUIRE(after.found_t0);
    REQUIRE(after.temperatures.size() >= 2);
    CHECK(after.temperatures.front() == slicer_temp);
    CHECK(after.temperatures.back() == custom_temp);
}
