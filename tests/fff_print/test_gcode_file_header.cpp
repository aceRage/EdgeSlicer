// Package 6A-4 of the Orca upstream catch-up: file header G-code and the config block.
#include <catch2/catch.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Config.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_data.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace Slic3r;
using namespace Slic3r::Test;

// ---------------------------------------------------------------------------
// OrcaSlicer #12455: Skip G-code config block
// ---------------------------------------------------------------------------
TEST_CASE("gcode_skip_config_block is off by default", "[GCodeHeader][ConfigBlock]")
{
    const DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    REQUIRE(config.has("gcode_skip_config_block"));
    CHECK_FALSE(config.opt_bool("gcode_skip_config_block"));

    const std::string gcode = slice({ TestMesh::cube_20x20x20 }, { { "gcode_comments", "1" } });
    CHECK(gcode.find("; CONFIG_BLOCK_START") != std::string::npos);
    CHECK(gcode.find("; CONFIG_BLOCK_END") != std::string::npos);
    // The new option is itself listed in the block.
    CHECK(gcode.find("; gcode_skip_config_block = 0") != std::string::npos);
}

// gcode_skip_config_block suppresses the resolved-settings block while leaving the
// header and executable blocks intact.
TEST_CASE("gcode_skip_config_block omits the resolved-settings comment block", "[GCodeHeader][ConfigBlock]")
{
    const std::string gcode = slice({ TestMesh::cube_20x20x20 }, {
        { "gcode_skip_config_block", "1" },
        { "gcode_comments",          "1" },
    });
    CHECK(gcode.find("; CONFIG_BLOCK_START")     == std::string::npos);
    CHECK(gcode.find("; CONFIG_BLOCK_END")       == std::string::npos);
    CHECK(gcode.find("; layer_height =")         == std::string::npos);
    CHECK(gcode.find("; sparse_infill_density =") == std::string::npos);
    CHECK(gcode.find("; HEADER_BLOCK_START")     != std::string::npos);
    CHECK(gcode.find("; EXECUTABLE_BLOCK_START") != std::string::npos);
    // Edge: the non-BBL trailer keeps its statistics after the (skipped) config block.
    CHECK(gcode.find("; total layers count =")   != std::string::npos);
}

// ---------------------------------------------------------------------------
// OrcaSlicer #12186 / #13063: file_start_gcode and the print-time placeholders
// ---------------------------------------------------------------------------
TEST_CASE("file_start_gcode is empty by default and writes nothing", "[GCodeHeader][FileStartGCode]")
{
    const DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    REQUIRE(config.has("file_start_gcode"));
    CHECK(config.opt_string("file_start_gcode").empty());

    const std::string gcode = slice({ TestMesh::cube_20x20x20 }, { { "gcode_comments", "1" } });
    // Without a file header the G-code starts with the header block, as before.
    CHECK(gcode.rfind("; HEADER_BLOCK_START", 0) == 0);
}

TEST_CASE("file_start_gcode is written first with the print-time placeholders resolved", "[GCodeHeader][FileStartGCode]")
{
    const std::string gcode = slice({ TestMesh::cube_20x20x20 }, {
        { "gcode_comments", "1" },
        { "layer_height", "0.2" },
        { "file_start_gcode", ";HDR total={print_time_total_sec} split={print_time_day}:{print_time_hour}:{print_time_minute}:{print_time_sec} len={used_filament_length} lh={layer_height}" },
    });
    REQUIRE(gcode.rfind(";HDR total=", 0) == 0);
    const std::string first = gcode.substr(0, gcode.find('\n'));
    // Every reserved tag was replaced by the post-processor.
    CHECK(first.find('@') == std::string::npos);
    CHECK(gcode.find("@PRINT_TIME") == std::string::npos);
    CHECK(gcode.find("@USED_FILAMENT_LENGTH@") == std::string::npos);

    double total = 0., len = 0., lh = 0.;
    int    d = -1, h = -1, m = -1, s = -1;
    REQUIRE(std::sscanf(first.c_str(), ";HDR total=%lf split=%d:%d:%d:%d len=%lf lh=%lf", &total, &d, &h, &m, &s, &len, &lh) == 7);
    CHECK(total > 0.);
    CHECK(d == 0);
    CHECK(h >= 0);
    CHECK((m >= 0 && m < 60));
    CHECK((s >= 0 && s < 60));
    // The components split the whole seconds of the total (which is printed rounded to 0.01 s).
    CHECK(std::abs(d * 86400 + h * 3600 + m * 60 + s - int(total)) <= 1);
    CHECK(len > 0.);
    CHECK(lh == Approx(0.2));
    // The regular header still follows.
    CHECK(gcode.find("; HEADER_BLOCK_START") != std::string::npos);
}

TEST_CASE("print_time placeholders also resolve in the machine start G-code", "[GCodeHeader][FileStartGCode]")
{
    const std::string gcode = slice({ TestMesh::cube_20x20x20 }, {
        { "machine_start_gcode", ";START_T {print_time_total_sec} {used_filament_length}" },
    });
    const size_t pos = gcode.find(";START_T ");
    REQUIRE(pos != std::string::npos);
    const std::string line = gcode.substr(pos, gcode.find('\n', pos) - pos);
    CHECK(line.find('@') == std::string::npos);
}

// ---------------------------------------------------------------------------
// OrcaSlicer #15915: placeholders in the file header that need the export context
// ---------------------------------------------------------------------------
TEST_CASE("file_start_gcode can use placeholders that need the export context", "[GCodeHeader][FileStartGCode]")
{
    // initial_extruder, total_layer_count and the per-filament arrays are only set once the tool
    // ordering exists; before #15915 the header was expanded ahead of that and could crash or fail.
    std::string gcode;
    REQUIRE_NOTHROW(gcode = slice({ TestMesh::cube_20x20x20 }, {
        { "layer_height", "0.2" },
        { "initial_layer_print_height", "0.2" },
        { "file_start_gcode", ";HDR ext={initial_extruder} layers={total_layer_count} t={nozzle_temperature_initial_layer[initial_extruder]} ts={timestamp}" },
    }));
    REQUIRE(gcode.rfind(";HDR ext=0 layers=100 t=", 0) == 0);
    const std::string first = gcode.substr(0, gcode.find('\n'));
    CHECK(first.find("ts=") != std::string::npos);
    CHECK(first.find("{") == std::string::npos);
    // The regular header follows the file header directly.
    const size_t next = gcode.find('\n') + 1;
    CHECK(gcode.compare(next, 20, "; HEADER_BLOCK_START") == 0);
}

TEST_CASE("an invalid file_start_gcode placeholder is reported, not a crash", "[GCodeHeader][FileStartGCode]")
{
    CHECK_THROWS(slice({ TestMesh::cube_20x20x20 }, {
        { "file_start_gcode", ";HDR {this_is_not_a_variable}" },
    }));
}

// The move of the header block (#15915) must not change the file when no file header is set:
// same blocks in the same order.
TEST_CASE("Header, config and executable blocks keep their order without a file header", "[GCodeHeader]")
{
    const std::string gcode = slice({ TestMesh::cube_20x20x20 }, { { "gcode_comments", "1" } });
    const size_t header_start = gcode.find("; HEADER_BLOCK_START");
    const size_t header_end   = gcode.find("; HEADER_BLOCK_END");
    const size_t widths       = gcode.find("; external perimeters extrusion width");
    const size_t exec_start   = gcode.find("; EXECUTABLE_BLOCK_START");
    const size_t exec_end     = gcode.find("; EXECUTABLE_BLOCK_END");
    const size_t config_start = gcode.find("; CONFIG_BLOCK_START");
    REQUIRE(header_start == 0);
    REQUIRE(header_end != std::string::npos);
    REQUIRE(widths != std::string::npos);
    REQUIRE(exec_start != std::string::npos);
    CHECK(header_start < header_end);
    CHECK(header_end < widths);
    CHECK(widths < exec_start);
    CHECK(exec_start < exec_end);
    // Non-Bambu: the config block is the trailer.
    CHECK(exec_end < config_start);
}
