#include <catch2/catch.hpp>

#include "libslic3r/BambuExtruderMap.hpp"

using namespace Slic3r::BambuExtruderMap;

// The H2C/H2D profile map: logical 0 (left) is physical 1, logical 1 (right) is physical 0.
static const std::vector<int> H2_MAP{ 1, 0 };

TEST_CASE("Bambu two-extruder logical/physical conversion", "[BambuExtruderMap]")
{
    CHECK(logical_to_physical(H2_MAP, 0) == 1);
    CHECK(logical_to_physical(H2_MAP, 1) == 0);
    CHECK(physical_to_logical(H2_MAP, 0) == 1);
    CHECK(physical_to_logical(H2_MAP, 1) == 0);

    // No map (single extruder profile default [0]) or out of range: identity.
    CHECK(logical_to_physical({}, 1) == 1);
    CHECK(physical_to_logical({}, 1) == 1);
    CHECK(physical_to_logical({ 0 }, 1) == 1);
}

TEST_CASE("Bambu tray names as the grouping expects them", "[BambuExtruderMap]")
{
    CHECK(tray_name(0, 0) == "A1");
    CHECK(tray_name(0, 3) == "A4");
    CHECK(tray_name(1, 1) == "B2");
    // An AMS HT (MQTT ams id 128+) is an ordinary AMS slot, not the external spool: calling it
    // "Ext" made the grouping treat the owner's left-side AMS HT spool as an external one.
    CHECK(tray_name(128, 0) == "HT-A");
    CHECK(tray_name(129, 0) == "HT-B");
    CHECK(tray_name(254, 0) == "Ext");
    CHECK(tray_name(255, 0) == "Ext");
    CHECK(is_ams_ht_id(128));
    CHECK_FALSE(is_ams_ht_id(3));
    CHECK_FALSE(is_external_spool_ams_id(128));
}

TEST_CASE("A filament mapped to an AMS on the other extruder is reported", "[BambuExtruderMap]")
{
    // The 2026-09-23 H2C job: ten filaments, 4/8/10 used, sliced all on the right rack
    // (filament_map 2 for every filament). The send mapped F4 -> AMS 0 slot 3 and F8 -> AMS 0
    // slot 1 (AMS 0 feeds physical extruder 0 = right) and F10 -> AMS HT 128 slot 0 (feeds
    // physical 1 = left). F10 cannot print as sliced.
    const std::vector<int> all_right(10, 2);
    const std::vector<MappedTray> sent{ { 3, 0 }, { 7, 0 }, { 9, 1 } };
    CHECK(filaments_on_wrong_extruder(all_right, H2_MAP, sent) == std::vector<int>{ 9 });

    // The intended slice (what Bambu Studio's manual grouping wrote): F10 left, F4/F8 right.
    std::vector<int> intended(10, 2);
    intended[9] = 1;
    CHECK(filaments_on_wrong_extruder(intended, H2_MAP, sent).empty());

    // The swapped AMS-aware slice (left and right AMS lists exchanged) put F4/F8 on the left
    // while their spools sit in the right-side AMS.
    std::vector<int> swapped(10, 2);
    swapped[3] = swapped[7] = swapped[9] = 1;
    CHECK(filaments_on_wrong_extruder(swapped, H2_MAP, sent) == std::vector<int>{ 3, 7 });

    // Unknown tray extruder or filament outside the map is not reported.
    CHECK(filaments_on_wrong_extruder(all_right, H2_MAP, { { 9, -1 }, { 42, 1 } }).empty());
}
