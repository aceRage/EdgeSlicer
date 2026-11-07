// The printer list popup's always-drawn icons on macOS (MacPopupHover.hpp).
//
// The rows of that popup get no hover from macOS 13.1 on, so their pencil and log-out icons must be
// drawn all the time there. The test this replaced missed every x.0 release after 13.

#include <catch2/catch.hpp>

#include "slic3r/GUI/MacPopupHover.hpp"

using Slic3r::GUI::macos_popup_icons_always_drawn;

TEST_CASE("Popup row icons are always drawn from macOS 13.1 on", "[MacPopupHover]")
{
    CHECK_FALSE(macos_popup_icons_always_drawn(12, 7));
    CHECK_FALSE(macos_popup_icons_always_drawn(13, 0));
    CHECK(macos_popup_icons_always_drawn(13, 1));
    CHECK(macos_popup_icons_always_drawn(13, 6));
    // The x.0 releases the old "minor >= 1" test missed.
    CHECK(macos_popup_icons_always_drawn(14, 0));
    CHECK(macos_popup_icons_always_drawn(15, 0));
    CHECK(macos_popup_icons_always_drawn(26, 0));
    CHECK(macos_popup_icons_always_drawn(27, 0));
}
