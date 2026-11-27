// Bambu Studio 02.09.01.70 parity, PR 2 "Auto infill retraction": reduce_infill_retraction_mode (Disabled / Auto /
// Enabled) and filament_metal_stickiness (BS GCode.cpp:8466-8482). Auto skips the retraction of a travel inside the
// infill only for a filament of None/Low metal stickiness (PLA); PETG and the other High filaments keep it.
#include <catch2/catch.hpp>

#include "test_data.hpp"

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/libslic3r.h"

#include <string>

using namespace Slic3r;

namespace {

// Number of retractions (a run of consecutive E-negative moves counts once).
size_t count_retractions(const std::string &mode, const std::string &stickiness)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height", "0.3" },
        { "initial_layer_print_height", "0.3" },
        { "wall_loops", "1" },
        { "top_shell_layers", "0" },
        { "bottom_shell_layers", "1" },
        { "sparse_infill_density", "15%" },
        { "sparse_infill_pattern", "rectilinear" },
        { "infill_anchor_max", "0" },
        { "retraction_length", "0.8" },
        { "retraction_minimum_travel", "1" },
        { "wipe", "0" },
        { "z_hop", "0" },
        { "enable_prime_tower", "0" },
        { "skirt_loops", "0" },
        { "reduce_infill_retraction_mode", mode },
        { "filament_metal_stickiness", stickiness },
    });
    const std::string gcode = Slic3r::Test::slice({ Slic3r::Test::TestMesh::cube_20x20x20 }, config);
    REQUIRE(!gcode.empty());
    size_t      retractions = 0;
    bool        in_retract  = false;
    GCodeReader parser;
    parser.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (!(line.cmd_is("G1") || line.cmd_is("G0") || line.cmd_is("G2") || line.cmd_is("G3")))
            return;
        const bool retracting = line.retracting(self);
        if (retracting && !in_retract)
            ++retractions;
        if (line.dist_E(self) != 0.f || line.dist_XY(self) > 0.f)
            in_retract = retracting;
    });
    return retractions;
}

} // namespace

TEST_CASE("Auto reduce infill retraction follows the filament's metal stickiness", "[BambuParity][InfillRetraction]")
{
    const size_t disabled = count_retractions("Disabled", "None");
    const size_t enabled  = count_retractions("Enabled", "High");
    INFO("Disabled " << disabled << ", Enabled " << enabled);
    // The cube's sparse infill has unanchored lines, so skipping the infill-travel retraction shows.
    REQUIRE(enabled < disabled);

    // Enabled skips whatever the filament; Disabled always retracts.
    CHECK(count_retractions("Enabled", "None") == enabled);
    CHECK(count_retractions("Disabled", "High") == disabled);
    // Auto: None (untested) and Low skip like Enabled; Medium and High (PETG, PA, TPU ...) retract like Disabled.
    CHECK(count_retractions("Auto", "None") == enabled);
    CHECK(count_retractions("Auto", "Low") == enabled);
    CHECK(count_retractions("Auto", "Medium") == disabled);
    CHECK(count_retractions("Auto", "High") == disabled);
}

TEST_CASE("An old config's reduce_infill_retraction = 1 slices as Enabled", "[BambuParity][InfillRetraction]")
{
    // The legacy bool goes through PrintConfigDef::handle_legacy: 1 -> Enabled, whatever the stickiness.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict("reduce_infill_retraction", "1");
    CHECK(config.opt_enum<ReduceInfillRetractionMode>("reduce_infill_retraction_mode") == rirEnabled);
    config.set_deserialize_strict("reduce_infill_retraction", "0");
    CHECK(config.opt_enum<ReduceInfillRetractionMode>("reduce_infill_retraction_mode") == rirDisabled);
}
