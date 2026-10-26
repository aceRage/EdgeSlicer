#include <catch2/catch.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_data.hpp"

#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

// Custom G-code placeholder follow-ups from OrcaSlicer (#16348, #16597, #16680), ported for EdgeSlicer's
// custom G-code paths.

// Orca #16348: the final print statistics are known only after the whole G-code is generated, yet every
// custom G-code, the file header included, gets the same values as filename_format. Before the port
// {total_weight} and the other seven failed the slice ("Variable does not exist").
TEST_CASE("Print statistics in custom G-code match the final print statistics", "[Print][Placeholders][Orca16348]")
{
    const std::string gcode_key = GENERATE(as<std::string>{}, "file_start_gcode", "machine_start_gcode", "machine_end_gcode");
    // Bambu printers use another tag table and G-code layout.
    const bool is_bbl = GENERATE(false, true);
    CAPTURE(gcode_key, is_bbl);

    const std::vector<std::string> number_keys = { "total_weight", "used_filament", "total_cost", "total_wipe_tower_cost",
                                                   "total_wipe_tower_filament" };
    const std::vector<std::string> time_keys   = { "print_time", "normal_print_time", "silent_print_time" };
    std::string                    templ;
    for (const std::vector<std::string> *keys : { &number_keys, &time_keys })
        for (const std::string &key : *keys)
            templ += "; stat " + key + "={" + key + "}\n";

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { gcode_key,          templ },
        { "filament_density", "1.25" },
        { "filament_cost",    "20" },
        { "time_cost",        "2" },
    });
    Print print;
    print.is_BBL_printer() = is_bbl;
    Model model;
    init_print({ TestMesh::cube_20x20x20 }, print, model, config);

    std::string gcode;
    REQUIRE_NOTHROW(gcode = Slic3r::Test::gcode(print));

    // Match whole lines only: the config block repeats the template on a single line, and Bambu printers
    // write that block before the start G-code.
    const std::string lines    = "\n" + gcode;
    const auto        value_of = [&lines](const std::string &key) {
        const std::string prefix = "\n; stat " + key + "=";
        const size_t      start  = lines.find(prefix);
        REQUIRE(start != std::string::npos);
        const size_t begin = start + prefix.size();
        std::string  value = lines.substr(begin, lines.find('\n', begin) - begin);
        if (!value.empty() && value.back() == '\r')
            value.pop_back();
        return value;
    };
    // The same statistics filename_format is expanded with.
    DynamicConfig stats = print.print_statistics().config();
    // A 20mm cube with filament_density 1.25 weighs something, unlike the running extruded_weight_total in the header.
    CHECK(stats.opt_float("total_weight") > 0.);
    // EDGE: total_cost is the cost breakdown's total, filament plus time_cost per hour.
    CHECK(stats.opt_float("total_cost") > 0.);
    for (const std::string &key : number_keys) {
        CAPTURE(key);
        const std::string value = value_of(key);
        REQUIRE(value.find('@') == std::string::npos);
        // Written with two decimals.
        CHECK(std::stod(value) == Approx(stats.opt_float(key)).margin(0.006));
    }
    for (const std::string &key : time_keys) {
        CAPTURE(key);
        CHECK(value_of(key) == stats.opt_string(key));
    }
}

// Orca #16597: the tags are strings, so a calculation on one used to compute with the tag text
// ("{print_time_sec + 1}" wrote "@PRINT_TIME_SEC@1"). It now stops the slice with a message that says why.
TEST_CASE("Print statistics in custom G-code cannot be used in a calculation", "[Print][Placeholders][Orca16597]")
{
    const std::string key = GENERATE(as<std::string>{}, "total_weight", "used_filament", "total_cost", "print_time", "normal_print_time",
                                     "silent_print_time", "total_wipe_tower_cost", "total_wipe_tower_filament", "print_time_total_sec",
                                     "print_time_day", "print_time_hour", "print_time_minute", "print_time_sec", "used_filament_length");
    CAPTURE(key);
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({ { "file_start_gcode", "; {" + key + " + 1}\n" } });
    Print print;
    Model model;
    init_print({ TestMesh::cube_20x20x20 }, print, model, config);
    CHECK_THROWS_WITH(Slic3r::Test::gcode(print), Catch::Contains("known only after the G-code is generated"));
}

// Orca #16680: max_layer_z was set only after the layer change G-code was expanded, so [max_layer_z] stopped
// the slice there.
TEST_CASE("Layer change G-code gets max_layer_z", "[Print][Placeholders][Orca16680]")
{
    std::string gcode;
    REQUIRE_NOTHROW(gcode = Test::slice({ TestMesh::cube_20x20x20 }, {
                        { "layer_height",               0.2 },
                        { "initial_layer_print_height", 0.2 },
                        { "layer_change_gcode",         ";LC [layer_num] max [max_layer_z]" },
                    }));
    CHECK(gcode.find("\n;LC 0 max 0.2\n") != std::string::npos);
    CHECK(gcode.find("\n;LC 99 max 20\n") != std::string::npos);
}

// Orca #16680: filament start and end G-code get the same placeholders on every tool change path. Bambu
// printers change tools in the wipe tower (WipeTowerIntegration::append_tcr), whose filament end G-code had
// no placeholders of its own ([filament_extruder_id] stopped the slice) and whose filament start G-code had
// no layer placeholders. Other printers change tools through GCode::set_extruder().
TEST_CASE("Filament start and end G-code get the same placeholders on every tool change path", "[Print][Placeholders][Orca16680]")
{
    const bool bbl = GENERATE(false, true);
    CAPTURE(bbl);

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("filament_diameter")->values = { 1.75, 1.75 };
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = { 0.4, 0.4 };
    config.option<ConfigOptionStrings>("filament_colour")->values  = { "#FF0000", "#0000FF" };
    config.option<ConfigOptionBool>("enable_prime_tower")->value   = true;
    config.option<ConfigOptionBool>("enable_support")->value       = false;
    config.option<ConfigOptionBool>("spiral_mode")->value          = false;
    config.option<ConfigOptionFloats>("wipe_tower_x")->values      = { 15. };
    config.option<ConfigOptionFloats>("wipe_tower_y")->values      = { 15. };
    config.option<ConfigOptionFloat>("prime_tower_width")->value   = 35.;
    config.set_deserialize_strict({ { "brim_type", "no_brim" },
                                    { "skirt_loops", "0" },
                                    { "layer_height", "0.2" },
                                    { "initial_layer_print_height", "0.2" } });
    const std::string start = "; filament start [filament_extruder_id] [layer_num] [layer_z] [max_layer_z]";
    const std::string end   = "; filament end [filament_extruder_id] [layer_num] [layer_z] [max_layer_z]";
    config.set_key_value("filament_start_gcode", new ConfigOptionStrings({ start, start }));
    config.set_key_value("filament_end_gcode", new ConfigOptionStrings({ end, end }));

    Print        print;
    Model        model;
    ModelObject *first = model.add_object();
    first->name        = "cube-a.stl";
    first->add_volume(mesh(TestMesh::cube_20x20x20));
    first->add_instance()->set_offset(Vec3d(80., 40., 0.));
    first->ensure_on_bed();
    ModelObject *second = model.add_object();
    second->name        = "cube-b.stl";
    second->add_volume(mesh(TestMesh::cube_20x20x20));
    second->add_instance()->set_offset(Vec3d(120., 40., 0.));
    second->ensure_on_bed();
    second->volumes.front()->config.set("extruder", 2);
    print.apply(model, config);
    print.is_BBL_printer() = bbl;
    REQUIRE(print.has_wipe_tower());

    std::string gcode;
    REQUIRE_NOTHROW(gcode = Test::gcode(print));
    // Loading filament 2 (index 1) on the first layer, and unloading it on a later layer.
    CHECK(gcode.find("\n; filament start 1 0 ") != std::string::npos);
    CHECK(gcode.find("\n; filament end 1 ") != std::string::npos);
}

// Orca #16680: time_lapse_gcode on printers other than Bambu's got only the layer placeholders, so
// has_timelapse_safe_pos was always false and timelapse_pos_x / _y were 0 there.
TEST_CASE("Timelapse G-code gets the snapshot placeholders on bed slingers and other printers", "[Print][Placeholders][Orca16680]")
{
    const std::string structure = GENERATE(as<std::string>{}, "i3", "corexy");
    CAPTURE(structure);
    std::string gcode;
    REQUIRE_NOTHROW(gcode = Test::slice({ TestMesh::cube_20x20x20 }, {
                        { "printer_structure",         structure },
                        { "printable_area",            "0x0,200x0,200x200,0x200" },
                        { "bed_exclude_area",          "0x0" },
                        { "extruder_clearance_radius", 40 },
                        { "timelapse_type",            "0" },
                        { "time_lapse_gcode",  "; Timelapse {if has_timelapse_safe_pos}safe{else}none{endif} [timelapse_pos_x] "
                                               "[clear_to_x0] [curr_physical_extruder_id]" },
                    }));
    CHECK(gcode.find("\n; Timelapse safe ") != std::string::npos);
}

// The first filament start G-code on a Bambu printer runs before the first layer (GCode::set_extruder() with
// no filament loaded yet). Upstream had to add the layer placeholders there; ours already had them.
TEST_CASE("The first filament start G-code on Bambu Lab printers gets the layer placeholders", "[Print][Placeholders][Orca16680]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("filament_start_gcode", new ConfigOptionStrings({ "; Start [layer_num] [layer_z] [max_layer_z]" }));
    Print print;
    print.is_BBL_printer() = true;
    Model model;
    init_print({ TestMesh::cube_20x20x20 }, print, model, config);

    std::string gcode;
    REQUIRE_NOTHROW(gcode = Slic3r::Test::gcode(print));
    CHECK(gcode.find("\n; Start -1 ") != std::string::npos);
}
