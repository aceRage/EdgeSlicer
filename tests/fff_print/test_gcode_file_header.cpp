// Package 6A-4 of the Orca upstream catch-up: file header G-code and the config block.
#include <catch2/catch.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Config.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_data.hpp"

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
