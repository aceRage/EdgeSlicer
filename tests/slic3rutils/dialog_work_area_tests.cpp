#include <catch2/catch.hpp>

#include "slic3r/GUI/DialogWorkArea.hpp"

using Slic3r::GUI::clamp_dialog_to_work_area;
using Slic3r::GUI::work_area_display_index;

TEST_CASE("GetFromWindow success keeps that display index", "[InputIpAddressDialog]")
{
    REQUIRE(work_area_display_index(0) == 0);
    REQUIRE(work_area_display_index(1) == 1);
    REQUIRE(work_area_display_index(3) == 3);
}

TEST_CASE("GetFromWindow wxNOT_FOUND falls back to the primary display", "[InputIpAddressDialog]")
{
    // wxNOT_FOUND is -1. Other negatives are treated the same way.
    REQUIRE(work_area_display_index(-1) == 0);
    REQUIRE(work_area_display_index(-2) == 0);
}

TEST_CASE("A dialog already inside the work area does not move", "[InputIpAddressDialog]")
{
    // Upstream check: maximized main window on 2560x1440 at 100%, dialog at (1057,324) 446x644.
    int x = 1057;
    int y = 324;
    clamp_dialog_to_work_area(x, y, 446, 644, 0, 0, 2560, 1440);
    REQUIRE(x == 1057);
    REQUIRE(y == 324);
}

TEST_CASE("A title bar pushed above the work area is clamped to the top", "[InputIpAddressDialog]")
{
    // Upstream check: CentreOnParent then y - 50 leaves the dialog at (577,-50).
    int x = 577;
    int y = -50;
    clamp_dialog_to_work_area(x, y, 446, 644, 0, 0, 2560, 1440);
    REQUIRE(x == 577);
    REQUIRE(y == 0);
}

TEST_CASE("A dialog hanging off the bottom is pulled up onto the work area", "[InputIpAddressDialog]")
{
    int x = 100;
    int y = 900;
    clamp_dialog_to_work_area(x, y, 446, 644, 0, 0, 1920, 1040);
    REQUIRE(x == 100);
    REQUIRE(y == 1040 - 644);
}

TEST_CASE("A dialog hanging off the right is pulled left onto the work area", "[InputIpAddressDialog]")
{
    int x = 1800;
    int y = 200;
    clamp_dialog_to_work_area(x, y, 446, 644, 0, 0, 1920, 1040);
    REQUIRE(x == 1920 - 446);
    REQUIRE(y == 200);
}

TEST_CASE("A dialog larger than the work area keeps its top-left on screen", "[InputIpAddressDialog]")
{
    int x = -40;
    int y = -80;
    clamp_dialog_to_work_area(x, y, 2000, 1200, 0, 0, 1920, 1040);
    REQUIRE(x == 0);
    REQUIRE(y == 0);
}

TEST_CASE("A secondary monitor work area is used, not the primary origin", "[InputIpAddressDialog]")
{
    const int work_x = 1920;
    const int work_y = 0;
    const int work_w = 1920;
    const int work_h = 1080;

    SECTION("already on the secondary display")
    {
        int x = 2200;
        int y = 200;
        clamp_dialog_to_work_area(x, y, 446, 644, work_x, work_y, work_w, work_h);
        REQUIRE(x == 2200);
        REQUIRE(y == 200);
    }

    SECTION("overshooting the secondary top after the 50 DIP shift")
    {
        int x = 2200;
        int y = -50;
        clamp_dialog_to_work_area(x, y, 446, 644, work_x, work_y, work_w, work_h);
        REQUIRE(x == 2200);
        REQUIRE(y == 0);
    }

    SECTION("spilling onto the primary from the secondary left edge")
    {
        int x = 1800;
        int y = 200;
        clamp_dialog_to_work_area(x, y, 446, 644, work_x, work_y, work_w, work_h);
        REQUIRE(x == 1920);
        REQUIRE(y == 200);
    }
}
