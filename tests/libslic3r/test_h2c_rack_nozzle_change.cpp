#include <catch2/catch.hpp>

#include "libslic3r/GCode.hpp"
#include "libslic3r/MultiNozzleUtils.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

// The H2C (printer_model_id O1C2) carries one nozzle on the left extruder and a rack of up to six
// swappable nozzles on the right. A change between two filaments that sit on two different rack
// slots is a nozzle change *inside one extruder*: the extruder does not move, the nozzle does.
//
// Bambu Studio marks that transition, and only that transition, with an M632/M633 skippable block.
// Measured on the gold plate (tests/h2c_bbs_12mm_slice.gcode.3mf, plate 5, 210 nozzle changes):
//
//     OF2 NF4 ON2 NN1   56   right rack slot 2 -> slot 1   rack change,     M632/M633 present
//     OF4 NF2 ON1 NN2   56   right rack slot 1 -> slot 2   rack change,     M632/M633 present
//     OF2 NF1 ON2 NN0   49   right rack -> left extruder   extruder change, NO M632/M633
//     OF1 NF2 ON0 NN2   49   left extruder -> right rack   extruder change, NO M632/M633
//
// These cases pin (a) the block's exact command shape against that gold file and (b) the
// per-logical-nozzle arrays the H2C change_filament template indexes with current_nozzle_id /
// next_nozzle_id -- which is a different, longer array than nozzle_diameter (one entry per
// extruder), because an H2C has two extruders and up to seven logical nozzles.
//
// See docs/superpowers/specs/2026-09-07-h2c-rack-nozzle-change.md.

namespace {

MultiNozzleUtils::NozzleInfo make_nozzle(int group_id, int extruder_id, const char *diameter, NozzleVolumeType volume_type)
{
    MultiNozzleUtils::NozzleInfo n;
    n.diameter    = diameter;
    n.volume_type = volume_type;
    n.extruder_id = extruder_id;
    n.group_id    = group_id;
    return n;
}

// The gold plate's own nozzle table, straight out of its Metadata/slice_info.config:
//   <nozzle id="0" extruder_id="1"/>  <nozzle id="1" extruder_id="2"/>  <nozzle id="2" extruder_id="2"/>
// (that file's extruder_id is 1-based; NozzleInfo::extruder_id is the 0-based logical extruder).
// Filament 0 runs on the left nozzle, filaments 1 and 2 on the two right-rack slots.
std::optional<MultiNozzleUtils::LayeredNozzleGroupResult> make_h2c_rack_result()
{
    std::vector<MultiNozzleUtils::NozzleInfo> nozzles{
        make_nozzle(0, 0, "0.4", NozzleVolumeType::nvtStandard),
        make_nozzle(1, 1, "0.4", NozzleVolumeType::nvtStandard),
        make_nozzle(2, 1, "0.6", NozzleVolumeType::nvtHighFlow),
    };
    std::vector<int>          filament_nozzle_map{ 0, 1, 2 };
    std::vector<unsigned int> used_filaments{ 0, 1, 2 };
    return MultiNozzleUtils::LayeredNozzleGroupResult::create(filament_nozzle_map, nozzles, used_filaments);
}

} // namespace

SCENARIO("H2C rack: the nozzle-change block", "[H2CRackNozzleChange]")
{
    GIVEN("a change between two nozzles on the same extruder (a rack change)")
    {
        // Gold file, toolchange #21: filament 2 -> filament 4, rack slot 2 -> slot 1, PLA pre-cool 180,
        // right extruder == physical extruder 0 (physical_extruder_map = [1, 0]).
        const std::string gcode = format_nozzle_change_block(2, 4, 2, 1,
                                                             /*extruder_change*/ false,
                                                             /*dynamic_nozzle_map*/ false,
                                                             /*precool_temp*/ 180,
                                                             /*physical_extruder*/ 0);

        THEN("it is byte-for-byte the gold file's block")
        {
            const std::string expected = "; NOZZLE_CHANGE_START OF2 NF4 ON2 NN1\n"
                                         "M632 S4 M N\n"
                                         "M400\n"
                                         "M104 T0 S180 N0 ;Wipe tower nozzle change pre cooling\n"
                                         "M106 S255\n"
                                         "M633\n"
                                         "; NOZZLE_CHANGE_END OF2 NF4 ON2 NN1\n";
            REQUIRE(gcode == expected);
        }

        THEN("the interlock is balanced")
        {
            size_t n632 = 0, n633 = 0;
            for (size_t i = gcode.find("M632"); i != std::string::npos; i = gcode.find("M632", i + 1))
                ++n632;
            for (size_t i = gcode.find("M633"); i != std::string::npos; i = gcode.find("M633", i + 1))
                ++n633;
            REQUIRE(n632 == 1);
            REQUIRE(n633 == 1);
        }
    }

    GIVEN("a change between two extruders")
    {
        // Gold file: the 98 extruder changes carry the markers and nothing else - the machine is
        // already told about the extruder by the toolchange itself.
        const std::string gcode = format_nozzle_change_block(2, 1, 2, 0,
                                                             /*extruder_change*/ true,
                                                             /*dynamic_nozzle_map*/ false,
                                                             /*precool_temp*/ 180,
                                                             /*physical_extruder*/ 0);

        THEN("no M632/M633 interlock and no M104 are emitted")
        {
            REQUIRE(gcode == "; NOZZLE_CHANGE_START OF2 NF1 ON2 NN0\n"
                             "; NOZZLE_CHANGE_END OF2 NF1 ON2 NN0\n");
        }
    }

    GIVEN("a rack change with the pre-cool temperature disabled")
    {
        const std::string gcode = format_nozzle_change_block(2, 4, 2, 1, false, false, /*precool_temp*/ 0, 0);

        THEN("the block still notifies the firmware, without the temperature commands")
        {
            // BambuStudio WipeTower.cpp:3450 guards the M104 on precool_target_temp != 0 the same way.
            REQUIRE(gcode == "; NOZZLE_CHANGE_START OF2 NF4 ON2 NN1\n"
                             "M632 S4 M N\n"
                             "M633\n"
                             "; NOZZLE_CHANGE_END OF2 NF4 ON2 NN1\n");
        }
    }

    GIVEN("a machine with a dynamic nozzle map (a filament selector)")
    {
        const std::string gcode = format_nozzle_change_block(2, 4, 2, 1, false, /*dynamic_nozzle_map*/ true, 0, 0);

        THEN("M632 names the target nozzle with H")
        {
            // The static-map plates this fork produces write no H, which is why the same plate's
            // T<f>, M620 S<f>A and M620.6 all carry H-1.
            REQUIRE(gcode.find("M632 S4 H1 M N\n") != std::string::npos);
        }
    }

    GIVEN("no physical extruder for the target")
    {
        const std::string gcode = format_nozzle_change_block(2, 4, 2, 1, false, false, 180, /*physical_extruder*/ -1);

        THEN("M104 is emitted without a T argument")
        {
            REQUIRE(gcode.find("M104 S180 N0 ;Wipe tower nozzle change pre cooling\n") != std::string::npos);
            REQUIRE(gcode.find("M104 T") == std::string::npos);
        }
    }
}

SCENARIO("H2C rack: the per-logical-nozzle template arrays", "[H2CRackNozzleChange]")
{
    GIVEN("the gold plate's three-nozzle H2C grouping (1 left + 2 right-rack slots)")
    {
        auto result = make_h2c_rack_result();
        REQUIRE(result.has_value());

        WHEN("the change_filament template's arrays are built from it")
        {
            const auto diameters    = get_nozzle_diameters_by_nozzle_id(&result.value());
            const auto volume_types = get_nozzle_volume_types_by_nozzle_id(&result.value());

            THEN("they are indexed by logical nozzle, not by extruder")
            {
                // The shim this replaces used nozzle_diameter, which has one entry per extruder -
                // two for an H2C - so nozzle_diameter_at_nozzle_id[2] read out of range for the
                // second rack slot.
                REQUIRE(diameters.size() == 3);
                REQUIRE(volume_types.size() == 3);
            }

            THEN("each entry is that nozzle's own diameter and flow type")
            {
                REQUIRE(diameters[0] == Approx(0.4));
                REQUIRE(diameters[1] == Approx(0.4));
                REQUIRE(diameters[2] == Approx(0.6));
                REQUIRE(volume_types[0] == get_nozzle_volume_type_string(NozzleVolumeType::nvtStandard));
                REQUIRE(volume_types[1] == get_nozzle_volume_type_string(NozzleVolumeType::nvtStandard));
                REQUIRE(volume_types[2] == get_nozzle_volume_type_string(NozzleVolumeType::nvtHighFlow));
            }
        }

        WHEN("a transition is classified")
        {
            THEN("two filaments on the same extruder but different nozzles are a rack change")
            {
                REQUIRE(result->get_extruder_id(1) == result->get_extruder_id(2));
                REQUIRE(result->get_nozzle_id(1) != result->get_nozzle_id(2));
            }

            THEN("a filament on the other extruder is an extruder change")
            {
                REQUIRE(result->get_extruder_id(0) != result->get_extruder_id(2));
            }
        }
    }

    GIVEN("no grouping result at all (a single-nozzle machine)")
    {
        THEN("both arrays come back empty, so the caller keeps its per-extruder fallback")
        {
            REQUIRE(get_nozzle_diameters_by_nozzle_id(nullptr).empty());
            REQUIRE(get_nozzle_volume_types_by_nozzle_id(nullptr).empty());
        }
    }
}

SCENARIO("H2C rack: the rack gate", "[H2CRackNozzleChange]")
{
    GIVEN("a printer config")
    {
        PrintConfig config;

        THEN("no extruder_max_nozzle_count means no rack")
        {
            config.extruder_max_nozzle_count.values.clear();
            REQUIRE_FALSE(has_nozzle_rack(config));
        }

        THEN("the H2D's 1,1 is not a rack")
        {
            config.extruder_max_nozzle_count.values = { 1, 1 };
            REQUIRE_FALSE(has_nozzle_rack(config));
        }

        THEN("the H2C's 1,6 is a rack")
        {
            config.extruder_max_nozzle_count.values = { 1, 6 };
            REQUIRE(has_nozzle_rack(config));
        }
    }
}
