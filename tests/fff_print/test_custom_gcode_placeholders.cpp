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
