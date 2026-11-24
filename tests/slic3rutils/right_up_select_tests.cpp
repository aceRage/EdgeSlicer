#include <catch2/catch.hpp>

#include "slic3r/GUI/RightUpSelect.hpp"

using namespace Slic3r::GUI;

// OrcaSlicer #17432: releasing the right mouse button after a pan or rotate
// must not select the hovered volume. A click (ignore_right_up still false)
// still selects when a volume is under the cursor.

TEST_CASE("Right-up after a camera pan or rotate does not select the hovered volume", "[RightUpSelect]")
{
    const bool ignore_right_up = true;
    const bool hover_volume_empty = false;
    REQUIRE_FALSE(should_select_hovered_volume_on_right_up(ignore_right_up, hover_volume_empty));
}

TEST_CASE("Plain right-click on a volume still selects it", "[RightUpSelect]")
{
    const bool ignore_right_up = false;
    const bool hover_volume_empty = false;
    REQUIRE(should_select_hovered_volume_on_right_up(ignore_right_up, hover_volume_empty));
}

TEST_CASE("Right-up with no hovered volume does not select", "[RightUpSelect]")
{
    REQUIRE_FALSE(should_select_hovered_volume_on_right_up(false, true));
    REQUIRE_FALSE(should_select_hovered_volume_on_right_up(true, true));
}
