#include <catch2/catch.hpp>

#include "libslic3r/CliPresetSlots.hpp"

#include <string>
#include <vector>

using namespace Slic3r;

// Regression for the CLI crash of 2026-10-29: a project saved with 2 filaments, sliced with
// 4 --filament-presets, kept filament_count at 2, so the 4th preset wrote
// different_settings_to_system[4] / inherits_group[4] past the end of 4-entry vectors
// (heap corruption, intermittent segfault). With 3 presets it overwrote the printer slot.

TEST_CASE("CLI filament slot count grows to the presets passed", "[CLI][Config]")
{
    CHECK(cli_filament_slot_count(2, 4) == 4);
    CHECK(cli_filament_slot_count(4, 2) == 4);
    CHECK(cli_filament_slot_count(0, 3) == 3);
    CHECK(cli_filament_slot_count(2, 0) == 2);
    CHECK(cli_filament_slot_count(0, 0) == 0);
}

TEST_CASE("CLI preset slots keep the printer last when they grow", "[CLI][Config]")
{
    SECTION("2-filament project sliced with 4 presets")
    {
        // process, filament 1, filament 2, printer
        std::vector<std::string> slots{"process", "fil1", "fil2", "printer"};
        const int filament_count = cli_filament_slot_count(2, 4);
        resize_cli_preset_slots(slots, filament_count);
        REQUIRE(slots.size() == 6);
        CHECK(slots[0] == "process");
        CHECK(slots[1] == "fil1");
        CHECK(slots[2] == "fil2");
        CHECK(slots[3].empty());
        CHECK(slots[4].empty());
        CHECK(slots[5] == "printer");
        // Every filament index the CLI loop writes (1..filament_count) is in range,
        // and none of them is the printer slot.
        for (int filament_index = 1; filament_index <= filament_count; ++filament_index) {
            CAPTURE(filament_index);
            CHECK(size_t(filament_index) < slots.size() - 1);
        }
    }
    SECTION("empty vector from a bare 3MF")
    {
        std::vector<std::string> slots;
        resize_cli_preset_slots(slots, 4);
        REQUIRE(slots.size() == 6);
        for (const std::string &s : slots)
            CHECK(s.empty());
    }
    SECTION("same size is left alone")
    {
        std::vector<std::string> slots{"process", "fil1", "fil2", "printer"};
        resize_cli_preset_slots(slots, 2);
        CHECK(slots == std::vector<std::string>{"process", "fil1", "fil2", "printer"});
    }
    SECTION("shrinking keeps the old plain resize")
    {
        std::vector<std::string> slots{"process", "fil1", "fil2", "fil3", "fil4", "printer"};
        resize_cli_preset_slots(slots, 2);
        CHECK(slots == std::vector<std::string>{"process", "fil1", "fil2", "fil3"});
    }
}
