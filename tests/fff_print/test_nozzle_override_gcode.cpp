// Per-nozzle process P7 in the slicer and the G-code: a four-tool printer preset for 0.4 nozzles with T1-T4 set to
// 0.2 / 0.4 / 0.6 / 0.8 for the project prints each tool at its own size's widths, declares the sizes in the config
// block, refuses a layer the 0.2 cannot print, and slices byte-identically when no size is overridden.

#include <catch2/catch.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/NozzleOverride.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "test_data.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

void add_cube_per_filament(Model &model, int filaments)
{
    for (int filament = 1; filament <= filaments; ++filament) {
        ModelObject *object = model.add_object();
        object->name        = "cube-" + std::to_string(filament) + ".stl";
        object->add_volume(mesh(TestMesh::cube_20x20x20));
        object->add_instance()->set_offset(Vec3d(30. + 35. * filament, 60., 0.));
        object->ensure_on_bed();
        object->volumes.front()->config.set("extruder", filament);
    }
}

// "Snapmaker U1 (0.4 nozzle)"-like: four 0.4 tools, process widths in mm made for 0.4.
DynamicPrintConfig u1_04()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_filaments(4);
    config.set_deserialize_strict({
        { "nozzle_diameter", "0.4,0.4,0.4,0.4" },
        { "single_extruder_multi_material", "0" },
        { "printer_variant", "0.4" },
        { "enable_prime_tower", "0" },
        { "enable_arc_fitting", "0" },
        { "slow_down_for_layer_cooling", "0,0,0,0" },
        { "filament_max_volumetric_speed", "200,200,200,200" },
        { "nozzle_temperature", "210,210,210,210" },
        { "nozzle_temperature_initial_layer", "210,210,210,210" },
        { "wall_generator", "classic" },
        { "layer_height", "0.12" },
        { "initial_layer_print_height", "0.12" },
        { "line_width", "0.42" },
        { "outer_wall_line_width", "0.42" },
        { "inner_wall_line_width", "0.45" },
        { "sparse_infill_line_width", "0.45" },
        { "internal_solid_infill_line_width", "0.42" },
        { "top_surface_line_width", "0.42" },
        { "initial_layer_line_width", "0.5" },
        { "support_line_width", "0.42" },
        { "brim_type", "no_brim" },
        { "skirt_loops", "0" },
        { "layer_change_gcode", "G92 E0" }, // relative E needs it, or validate() refuses
    });
    return config;
}

std::string slice(const DynamicPrintConfig &config, std::string *error = nullptr)
{
    Print print;
    Model model;
    add_cube_per_filament(model, 4);
    print.apply(model, config);
    const StringObjectException err = print.validate();
    if (error != nullptr)
        *error = err.string;
    if (!err.string.empty())
        return std::string();
    print.set_status_silent();
    return Test::gcode(print);
}

// Per (tool, feature, first layer or not), the widths of the extruding moves. Plain string parsing (MSVC's std::regex
// has no multiline and is slow on a whole G-code file).
using Widths = std::map<std::tuple<int, std::string, bool>, std::map<double, int>>;

Widths widths_of(const std::string &gcode)
{
    Widths             out;
    std::istringstream in(gcode);
    std::string        line;
    int                tool = 0, layer = -1;
    double             width = 0.;
    std::string        type;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind(";LAYER_CHANGE", 0) == 0) {
            ++layer;
            continue;
        }
        if (line.size() >= 2 && line[0] == 'T' && std::isdigit((unsigned char) line[1])) {
            tool = std::atoi(line.c_str() + 1);
            continue;
        }
        if (line.rfind(";WIDTH:", 0) == 0) {
            width = std::atof(line.c_str() + 7);
            continue;
        }
        if (line.rfind(";TYPE:", 0) == 0) {
            type = line.substr(6);
            continue;
        }
        if (line.compare(0, 3, "G1 ") == 0 && line.find(" E") != std::string::npos && layer >= 0 && width > 0.)
            ++out[{ tool, type, layer == 0 }][std::round(width * 1000.) / 1000.];
    }
    return out;
}

double main_width(const Widths &w, int tool, const std::string &type, bool first_layer)
{
    const auto it = w.find({ tool, type, first_layer });
    if (it == w.end())
        return 0.;
    double best = 0.;
    int    most = -1;
    for (const auto &[width, count] : it->second)
        if (count > most) {
            most = count;
            best = width;
        }
    return best;
}

std::string config_line(const std::string &gcode, const std::string &key)
{
    const std::string needle = "; " + key + " = ";
    const size_t      pos    = gcode.find(needle);
    if (pos == std::string::npos)
        return std::string();
    const size_t end = gcode.find('\n', pos);
    std::string  out = gcode.substr(pos + needle.size(), end == std::string::npos ? std::string::npos : end - pos - needle.size());
    if (!out.empty() && out.back() == '\r')
        out.pop_back();
    return out;
}

std::string without_generated_stamp(std::string gcode)
{
    if (const size_t stamp = gcode.find("; generated by "); stamp != std::string::npos) {
        const size_t end = gcode.find('\n', stamp);
        gcode.erase(stamp, end == std::string::npos ? std::string::npos : end - stamp);
    }
    return gcode;
}

std::string without_config_block(const std::string &gcode)
{
    const size_t pos = gcode.find("; CONFIG_BLOCK_START");
    return without_generated_stamp(pos == std::string::npos ? gcode : gcode.substr(0, pos));
}

} // namespace

TEST_CASE("U1 T1-T4 at 0.2 / 0.4 / 0.6 / 0.8: every tool prints at its own size's widths", "[NozzleOverride][GCode]")
{
    DynamicPrintConfig config = u1_04();
    config.set_deserialize_strict({ { "nozzle_diameter_override", "0.2,0.4,0.6,0.8" } });
    std::string error;
    const std::string gcode = slice(config, &error);
    INFO(error);
    REQUIRE(!gcode.empty());
    const Widths w = widths_of(gcode);
    const double outer[] = { 0.21, 0.42, 0.63, 0.84 };
    const double inner[] = { 0.225, 0.45, 0.675, 0.9 };
    const double first[] = { 0.25, 0.5, 0.75, 1.0 };
    for (int t = 0; t < 4; ++t) {
        INFO("tool T" << t);
        CHECK(main_width(w, t, "Outer wall", false) == Approx(outer[t]).margin(0.003));
        CHECK(main_width(w, t, "Inner wall", false) == Approx(inner[t]).margin(0.003));
        CHECK(main_width(w, t, "Sparse infill", false) == Approx(inner[t]).margin(0.003));
        CHECK(main_width(w, t, "Outer wall", true) == Approx(first[t]).margin(0.003));
    }
    // The config block declares the sizes the job was sliced for (the printer reads them) and the project's override.
    CHECK(config_line(gcode, "nozzle_diameter") == "0.2,0.4,0.6,0.8");
    CHECK(config_line(gcode, "nozzle_diameter_override") == "0.2,0.4,0.6,0.8");
}

TEST_CASE("No override, or an override of the preset's own sizes, slices exactly as before", "[NozzleOverride][GCode][Identity]")
{
    const DynamicPrintConfig plain = u1_04();
    const std::string        a     = slice(plain);
    REQUIRE(!a.empty());
    // The key at its unused value: byte-identical, config block included.
    DynamicPrintConfig unused = plain;
    unused.set_deserialize_strict({ { "nozzle_diameter_override", "0" } });
    CHECK(without_generated_stamp(slice(unused)) == without_generated_stamp(a));
    CHECK(config_line(a, "nozzle_diameter_override").empty());
    // The preset's own sizes named explicitly: the same moves.
    DynamicPrintConfig same = plain;
    same.set_deserialize_strict({ { "nozzle_diameter_override", "0.4,0.4,0.4,0.4" } });
    CHECK(without_config_block(slice(same)) == without_config_block(a));
}

TEST_CASE("The 0.2 tool limits the layer height", "[NozzleOverride][Validate]")
{
    DynamicPrintConfig config = u1_04();
    config.set_deserialize_strict({ { "nozzle_diameter_override", "0.2,0.4,0.6,0.8" }, { "layer_height", "0.24" } });
    std::string error;
    slice(config, &error);
    CHECK_FALSE(error.empty());
    // The same layer on the 0.4 preset without the override is fine.
    DynamicPrintConfig plain = u1_04();
    plain.set_deserialize_strict({ { "layer_height", "0.24" } });
    std::string plain_error;
    CHECK_FALSE(slice(plain, &plain_error).empty());
    CHECK(plain_error.empty());
}
