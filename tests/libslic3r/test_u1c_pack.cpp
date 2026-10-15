#include <catch2/catch.hpp>

#include <algorithm>

#include "libslic3r/FilamentSyncMatch.hpp"
#include "libslic3r/FlushVolCalc.hpp"

using namespace Slic3r;

// Orca #11778: the flush volume clamp is relaxed (800 -> 20000) so printers that need more
// purge keep what the user (or the auto-calculation plus a large minimum) asks for; the old
// cap stays only as the "typical" guidance in the flushing volume dialog.
TEST_CASE("Flush volume cap accepts volumes above the old 800 mm3 limit", "[FlushVol][U1C]")
{
    STATIC_REQUIRE(g_max_flush_volume == 20000);
    STATIC_REQUIRE(g_typical_max_flush_volume == 800);
    STATIC_REQUIRE(g_max_flush_volume > g_typical_max_flush_volume);
    // A user value above the old cap survives the clamp the dialog and Plater apply.
    const int user_value = 1200;
    CHECK(std::min(user_value, g_max_flush_volume) == user_value);
    // Extreme values typed in the UI are still clamped.
    CHECK(std::min(1000000, g_max_flush_volume) == 20000);
}

// Orca #13265 (closest-colour rule): a reported filament with no exact preset name maps to the
// same-vendor, same-type preset whose default colour is nearest in RGB.
TEST_CASE("Filament sync picks the preset with the closest default colour", "[FilamentSync][U1C]")
{
    using namespace FilamentSyncMatch;

    SECTION("Colour parsing accepts #RRGGBB and the printer's RRGGBBAA") {
        unsigned int rgb = 0;
        REQUIRE(parse_rgb("#FF8000", rgb));
        CHECK(rgb == 0xFF8000u);
        REQUIRE(parse_rgb("ff8000ff", rgb));
        CHECK(rgb == 0xFF8000u);
        REQUIRE(parse_rgb(" #00ff00 ", rgb));
        CHECK(rgb == 0x00FF00u);
        CHECK_FALSE(parse_rgb("", rgb));
        CHECK_FALSE(parse_rgb("#FFF", rgb));
        CHECK_FALSE(parse_rgb("#GG0000", rgb));
        CHECK_FALSE(parse_rgb("#FF0000ZZ", rgb));
    }

    SECTION("Distance is symmetric RGB") {
        CHECK(rgb_distance_sq(0x000000u, 0xFFFFFFu) == 3u * 255u * 255u);
        CHECK(rgb_distance_sq(0x102030u, 0x302010u) == rgb_distance_sq(0x302010u, 0x102030u));
        CHECK(rgb_distance_sq(0x123456u, 0x123456u) == 0u);
    }

    SECTION("Nearest candidate wins") {
        const std::vector<std::string> colours = {"#000000", "#FF0000", "#0000FF", "#FFFFFF"};
        CHECK(closest_colour_index("E01010FF", colours) == 1);   // reddish -> red
        CHECK(closest_colour_index("#1010C0", colours) == 2);    // bluish -> blue
        CHECK(closest_colour_index("#F0F0F0", colours) == 3);    // light grey -> white
        CHECK(closest_colour_index("#202020", colours) == 0);    // dark grey -> black
    }

    SECTION("Candidates without a colour count as black; ties keep the first") {
        CHECK(closest_colour_index("#101010", {"#FFFFFF", ""}) == 1);
        CHECK(closest_colour_index("#808080", {"#707070", "#909090"}) == 0);
    }

    SECTION("Nothing to match") {
        CHECK(closest_colour_index("#FF0000", {}) == -1);
        CHECK(closest_colour_index("not a colour", {"#FF0000"}) == -1);
    }
}
