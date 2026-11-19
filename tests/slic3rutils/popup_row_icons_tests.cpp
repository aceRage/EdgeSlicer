// Which icons a printer popup row draws (PopupRowIcons.hpp).
#include <catch2/catch.hpp>

#include "slic3r/GUI/PopupRowIcons.hpp"

using Slic3r::GUI::edit_icons_for_row;
using Slic3r::GUI::RowEditIcons;

TEST_CASE("Cloud printer rows: log-out always, pencil when online", "[PopupRowIcons]")
{
    CHECK(edit_icons_for_row(false, true, false, false) == RowEditIcons{true, true});
    CHECK(edit_icons_for_row(false, false, false, false) == RowEditIcons{false, true});
}

TEST_CASE("A LAN printer with a saved code keeps its pencil when locked or unreachable", "[PopupRowIcons]")
{
    // Online and free: both icons, as before.
    CHECK(edit_icons_for_row(true, true, true, true) == RowEditIcons{true, true});
    // LOCK row (the saved code was refused, printer not free): pencil only.
    CHECK(edit_icons_for_row(true, true, true, false) == RowEditIcons{true, false});
    // Unreachable: pencil only.
    CHECK(edit_icons_for_row(true, false, true, true) == RowEditIcons{true, false});
    CHECK(edit_icons_for_row(true, false, true, false) == RowEditIcons{true, false});
}

TEST_CASE("A LAN printer without a saved code has no icons", "[PopupRowIcons]")
{
    CHECK(edit_icons_for_row(true, true, false, true) == RowEditIcons{false, false});
    CHECK(edit_icons_for_row(true, false, false, false) == RowEditIcons{false, false});
}
