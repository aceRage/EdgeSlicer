#include <catch2/catch.hpp>

#include <map>
#include <string>
#include <utility>
#include <vector>

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

TEST_CASE("Each extruder of a two-extruder printer has its own external spool", "[BambuExtruderMap]")
{
    // vir_slot 255 is the main (physical 0, right) extruder's holder, 254 the deputy's (physical 1,
    // left) - BambuStudio VIRTUAL_TRAY_MAIN_ID / VIRTUAL_TRAY_DEPUTY_ID.
    CHECK(external_spool_physical_extruder(255) == 0);
    CHECK(external_spool_physical_extruder(254) == 1);
    CHECK(external_spool_physical_extruder(0) == -1);
    CHECK(external_spool_physical_extruder(128) == -1);
    CHECK(external_spool_ams_id(0) == 255);
    CHECK(external_spool_ams_id(1) == 254);
    CHECK(external_spool_ams_id(2) == -1);
    // On the H2D / H2D Pro / H2C / X2D map the right external spool is the right extruder's.
    CHECK(physical_to_logical(H2_MAP, external_spool_physical_extruder(255)) == 1);
    CHECK(physical_to_logical(H2_MAP, external_spool_physical_extruder(254)) == 0);

    // People read Ext-L / Ext-R; the grouping keeps reading "Ext".
    CHECK(tray_display_name(254, 0) == "Ext-L");
    CHECK(tray_display_name(255, 0) == "Ext-R");
    CHECK(tray_display_name(128, 0) == "HT-A");
    CHECK(tray_display_name(1, 2) == "B3");
    CHECK(tray_name(255, 0) == "Ext");

    // The other-extruder check judges an external spool like an AMS slot: filament 2 sliced for
    // the right extruder may come from Ext-R (physical 0), not from Ext-L (physical 1).
    const std::vector<int> fil_map{ 1, 2 };
    CHECK(filaments_on_wrong_extruder(fil_map, H2_MAP, { { 1, external_spool_physical_extruder(255) } }).empty());
    CHECK(filaments_on_wrong_extruder(fil_map, H2_MAP, { { 1, external_spool_physical_extruder(254) } }) == std::vector<int>{ 1 });
}

// The printer report's AMS units as the sync reads them (DeviceManager amsList: ams id -> slot ids),
// keyed into the sidebar's AMS list the way Sidebar::build_filament_ams_list does (emplace: a
// second tray on a used key is dropped).
using AmsReport = std::vector<std::pair<std::string, std::vector<std::string>>>;

static std::map<int, std::string> ams_list_of(const AmsReport &report)
{
    std::map<int, std::string> list;
    for (const auto &ams : report)
        for (const std::string &slot : ams.second) {
            const AmsListEntry e = ams_list_entry(ams.first, slot);
            if (e.key >= 0)
                list.emplace(e.key, e.tray_name);
        }
    return list;
}

TEST_CASE("Sync AMS keeps an H2C's AMS HT next to its two AMS 2 Pro units", "[BambuExtruderMap]")
{
    // H2C, 2026-10-16: two AMS 2 Pro units on the right extruder (ams 0 and 1), an AMS HT on the
    // left one (ams 128). The old key, (first digit of the ams id) * 4 + slot, put the HT on
    // AMS 1 slot 1's key 4 and the sync lost it.
    const AmsReport h2c{ { "0", { "0", "1", "2", "3" } }, { "1", { "0", "1", "2", "3" } }, { "128", { "0" } } };
    const std::map<int, std::string> list = ams_list_of(h2c);
    REQUIRE(list.size() == 9);
    CHECK(list.at(4) == "B1");
    CHECK(list.at(7) == "B4");
    REQUIRE(list.count(128 * 4) == 1);
    CHECK(list.at(128 * 4) == "HT-A");
    // The HT comes after both four-slot units, as in BambuStudio.
    CHECK(list.rbegin()->second == "HT-A");
    // It is the key CalibrationPresetPage::sync_ams_info looks a tray up by: ams id * 4 + slot.
    CHECK(ams_list_entry("128", "0").key == 128 * 4);
    CHECK(ams_list_entry("1", "3").key == 7);

    // What the old key did with the same report: nine trays, eight entries, no HT.
    std::map<int, std::string> old;
    for (const auto &ams : h2c)
        for (const std::string &slot : ams.second)
            old.emplace((ams.first.front() - '0') * 4 + (slot.front() - '0'), ams.first + "/" + slot);
    CHECK(old.size() == 8);
    CHECK(old.at(4) == "1/0");
}

TEST_CASE("Sync AMS of an H2D with one AMS and an AMS HT is unchanged", "[BambuExtruderMap]")
{
    const AmsReport h2d{ { "0", { "0", "1", "2", "3" } }, { "128", { "0" } } };
    const std::map<int, std::string> list = ams_list_of(h2d);
    REQUIRE(list.size() == 5);
    CHECK(list.at(0) == "A1");
    CHECK(list.at(3) == "A4");
    CHECK(list.rbegin()->second == "HT-A");
    // Same order as before: the four AMS slots first, then the HT.
    std::vector<std::string> names;
    for (const auto &kv : list)
        names.push_back(kv.second);
    CHECK(names == std::vector<std::string>{ "A1", "A2", "A3", "A4", "HT-A" });
}

TEST_CASE("Sync AMS keys stay apart for four AMS units and several AMS HTs", "[BambuExtruderMap]")
{
    const AmsReport big{ { "0", { "0", "1", "2", "3" } },
                         { "1", { "0", "1", "2", "3" } },
                         { "2", { "0", "1", "2", "3" } },
                         { "3", { "0", "1", "2", "3" } },
                         { "128", { "0" } },
                         { "129", { "0" } },
                         { "135", { "0" } } };
    const std::map<int, std::string> list = ams_list_of(big);
    REQUIRE(list.size() == 19);
    CHECK(list.at(15) == "D4");
    CHECK(list.at(129 * 4) == "HT-B");
    CHECK(list.at(135 * 4) == "HT-H");
    // None of them is the external spool's key (VIRTUAL_TRAY_ID, 254).
    CHECK(list.count(254) == 0);
    // A report key that is not a number is skipped, not filed under some other tray.
    CHECK(ams_list_entry("", "0").key == -1);
    CHECK(ams_list_entry("x", "0").key == -1);
    CHECK(ams_list_entry("0", "").key == -1);
}
