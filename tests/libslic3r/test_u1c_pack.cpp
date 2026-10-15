#include <catch2/catch.hpp>

#include <algorithm>

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
