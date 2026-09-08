#include <catch2/catch.hpp>

#include <functional>
#include <vector>

// GCode.hpp first: ToolOrdering.hpp is not self-contained (it uses ExtrusionEntity/PrintObject
// without forward declaring them), the same way test_h2c_rack_nozzle_change.cpp includes it.
#include "libslic3r/GCode.hpp"
#include "libslic3r/GCode/ToolOrdering.hpp"
#include "libslic3r/MultiNozzleUtils.hpp"

using namespace Slic3r;

// H2 preload (preload_all_filaments). The option makes ToolOrdering put every filament the plate uses
// on LAYER 1's tool sequence, so the prime tower performs a real toolchange (normal change_filament_gcode,
// normal flush volume) for each of them before layer 2 - instead of paying that load at whatever layer
// each colour is first needed, which is where the mid-print minute-long stall and its visible line come
// from. preload_first_layer_tool_order() is that ordering rule on its own, so it can be pinned without
// building a Print.
//
// THE RULE:  [ preload-only filaments ] ++ [ layer 1's own sequence, unchanged ]
// Preload-only filaments are grouped by physical extruder (each extruder's loads consecutive), the group
// layer 1's first filament belongs to goes last among them, ascending filament id within a group. Layer
// 1's own order is never reordered, so the sequence still ENDS on the filament layer 1 really prints with.
//
// See docs/superpowers/specs/2026-09-07-h2-preload-filaments.md.

namespace {

MultiNozzleUtils::NozzleInfo make_nozzle(int group_id, int extruder_id, const char *diameter)
{
    MultiNozzleUtils::NozzleInfo n;
    n.diameter    = diameter;
    n.volume_type = NozzleVolumeType::nvtStandard;
    n.extruder_id = extruder_id;
    n.group_id    = group_id;
    return n;
}

// An H2D-shaped plate: three filaments, two physical extruders. Filament 0 sits on the left extruder,
// filaments 1 and 2 on the right one - exactly the case where the right extruder's second colour is the
// one that gets loaded mid-print today.
std::optional<MultiNozzleUtils::LayeredNozzleGroupResult> make_h2d_3_filament_result()
{
    std::vector<MultiNozzleUtils::NozzleInfo> nozzles{
        make_nozzle(0, 0, "0.4"),
        make_nozzle(1, 1, "0.4"),
    };
    std::vector<int>          filament_nozzle_map{ 0, 1, 1 };   // filament -> logical nozzle group
    std::vector<unsigned int> used_filaments{ 0, 1, 2 };
    return MultiNozzleUtils::LayeredNozzleGroupResult::create(filament_nozzle_map, nozzles, used_filaments);
}

std::function<int(unsigned int)> extruder_lookup(const MultiNozzleUtils::LayeredNozzleGroupResult &group)
{
    return [&group](unsigned int filament) -> int { return group.get_extruder_id((int) filament, 0); };
}

} // namespace

SCENARIO("H2 preload: layer 1 gets every filament the plate uses", "[H2PreloadFilaments]")
{
    auto group_opt = make_h2d_3_filament_result();
    REQUIRE(group_opt.has_value());
    const auto &group = *group_opt;
    // Sanity: the fixture really is a two-extruder grouping.
    REQUIRE(group.get_extruder_id(0, 0) == 0);
    REQUIRE(group.get_extruder_id(1, 0) == 1);
    REQUIRE(group.get_extruder_id(2, 0) == 1);

    const std::vector<unsigned int> used{ 0, 1, 2 };

    GIVEN("a 3-filament dual-nozzle plate whose layer 1 prints with one filament only")
    {
        // Layer 1 prints with filament 1 (right extruder). Filaments 0 and 2 are first needed later.
        const std::vector<unsigned int> layer0{ 1 };

        WHEN("the preload rule is applied")
        {
            const std::vector<unsigned int> out = preload_first_layer_tool_order(layer0, used, extruder_lookup(group));

            THEN("all three filaments are on layer 1, each exactly once")
            {
                REQUIRE(out.size() == 3);
                std::vector<unsigned int> sorted = out;
                std::sort(sorted.begin(), sorted.end());
                REQUIRE(sorted == used);
            }
            THEN("the sequence ends on the filament layer 1 really prints with")
            {
                REQUIRE(out.back() == layer0.back());
            }
            THEN("the preload-only filaments are grouped by extruder, layer 1's own extruder last")
            {
                // filament 0 is on extruder 0; filament 2 shares extruder 1 with layer 1's filament 1,
                // so its group is placed last among the pre-changes: 0, then 2, then the real 1.
                REQUIRE(out == std::vector<unsigned int>{ 0, 2, 1 });
            }
        }
    }

    GIVEN("a plate whose layer 1 already prints with two of the three filaments")
    {
        // Layer 1's own order is 2 then 0 (whatever first_layer_print_sequence / adhesion order decided).
        const std::vector<unsigned int> layer0{ 2, 0 };

        WHEN("the preload rule is applied")
        {
            const std::vector<unsigned int> out = preload_first_layer_tool_order(layer0, used, extruder_lookup(group));

            THEN("only the missing filament is prepended and layer 1's own order survives")
            {
                REQUIRE(out == std::vector<unsigned int>{ 1, 2, 0 });
                REQUIRE(out.back() == layer0.back());
            }
        }
    }

    GIVEN("a plate whose layer 1 already touches every filament")
    {
        const std::vector<unsigned int> layer0{ 1, 0, 2 };

        WHEN("the preload rule is applied")
        {
            const std::vector<unsigned int> out = preload_first_layer_tool_order(layer0, used, extruder_lookup(group));

            THEN("nothing changes - there is no toolchange to add")
            {
                REQUIRE(out == layer0);
            }
        }
    }

    GIVEN("no grouping result (the lookup is empty)")
    {
        const std::vector<unsigned int> layer0{ 1 };

        WHEN("the preload rule is applied")
        {
            const std::vector<unsigned int> out = preload_first_layer_tool_order(layer0, used, nullptr);

            THEN("the pre-changes are ascending by filament id and layer 1's filament is still last")
            {
                REQUIRE(out == std::vector<unsigned int>{ 0, 2, 1 });
            }
        }
    }

    GIVEN("a 5-filament plate spread over both extruders")
    {
        // filaments 0,1 on extruder 0; 2,3,4 on extruder 1. Layer 1 prints with 3 only.
        std::vector<MultiNozzleUtils::NozzleInfo> nozzles{ make_nozzle(0, 0, "0.4"), make_nozzle(1, 1, "0.4") };
        std::vector<int>                          map{ 0, 0, 1, 1, 1 };
        std::vector<unsigned int>                 five{ 0, 1, 2, 3, 4 };
        auto five_group = MultiNozzleUtils::LayeredNozzleGroupResult::create(map, nozzles, five);
        REQUIRE(five_group.has_value());

        const std::vector<unsigned int> layer0{ 3 };

        WHEN("the preload rule is applied")
        {
            const std::vector<unsigned int> out = preload_first_layer_tool_order(layer0, five, extruder_lookup(*five_group));

            THEN("each extruder's loads are consecutive and layer 1's extruder comes last")
            {
                // extruder 0's filaments (0,1) first, then extruder 1's remaining ones (2,4),
                // then layer 1's real filament 3. Four pre-changes, one per filament not on layer 1.
                REQUIRE(out == std::vector<unsigned int>{ 0, 1, 2, 4, 3 });
                REQUIRE(out.back() == 3);
                REQUIRE(out.size() == five.size());
            }
        }
    }
}
