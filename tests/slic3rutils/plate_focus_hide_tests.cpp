#include <catch2/catch.hpp>

#include "slic3r/GUI/PlateFocusHide.hpp"

using namespace Slic3r::GUI;

// Ultra: "Hide other plates while moving". Only the exemption decision is unit-testable - the
// rendering itself is GL state - so that decision lives in PlateFocusHide.hpp as pure functions.

TEST_CASE("Only the current plate is exempt when nothing is selected", "[MoveHidePlates]")
{
    const std::set<int> no_selection;

    REQUIRE(plate_focus_should_hide_plate(1, 0, no_selection));
    REQUIRE(plate_focus_should_hide_plate(2, 0, no_selection));
    REQUIRE_FALSE(plate_focus_should_hide_plate(0, 0, no_selection));
}

TEST_CASE("The plate being worked on is never hidden", "[MoveHidePlates]")
{
    const std::set<int> selection{2};

    REQUIRE_FALSE(plate_focus_should_hide_plate(2, 2, selection));
    REQUIRE_FALSE(plate_focus_should_hide_plate(0, 0, selection));
    REQUIRE(plate_focus_should_hide_plate(1, 0, selection));
}

TEST_CASE("A selection spanning two plates exempts both", "[MoveHidePlates]")
{
    // Design 3.4: Ctrl-clicking an object on plate 0 and one on plate 2 must leave plate 2
    // visible even though plate 0 is the current plate - it holds something about to be moved.
    const std::set<int> selection{0, 2};

    REQUIRE(plate_focus_selection_spans_plates(selection));
    REQUIRE_FALSE(plate_focus_should_hide_plate(0, 0, selection));
    REQUIRE_FALSE(plate_focus_should_hide_plate(2, 0, selection));
    // An uninvolved plate still hides.
    REQUIRE(plate_focus_should_hide_plate(1, 0, selection));
    REQUIRE(plate_focus_should_hide_plate(3, 0, selection));
}

TEST_CASE("A single-plate selection does not span plates", "[MoveHidePlates]")
{
    REQUIRE_FALSE(plate_focus_selection_spans_plates(std::set<int>{}));
    REQUIRE_FALSE(plate_focus_selection_spans_plates(std::set<int>{1}));
    REQUIRE(plate_focus_selection_spans_plates(std::set<int>{1, 4}));
}

TEST_CASE("The exempt set is the current plate unioned with the selected plates", "[MoveHidePlates]")
{
    const std::set<int> expected_when_selection_elsewhere{0, 3};
    REQUIRE(plate_focus_exempt_plates(0, std::set<int>{3}) == expected_when_selection_elsewhere);

    const std::set<int> expected_when_selection_is_current{1};
    REQUIRE(plate_focus_exempt_plates(1, std::set<int>{1}) == expected_when_selection_is_current);

    // A negative current plate (no plate selected at all) contributes nothing.
    REQUIRE(plate_focus_exempt_plates(-1, std::set<int>{2}) == std::set<int>{2});
}

TEST_CASE("An instance no plate claims is never hidden", "[MoveHidePlates]")
{
    // owner == -1 means no plate owns the instance (it sits off every bed). There is no other
    // plate for it to "belong to", so hiding it would just make it vanish for no reason.
    const std::set<int> selection{0};

    REQUIRE_FALSE(plate_focus_should_hide_instance(-1, 0, selection));
    REQUIRE(plate_focus_should_hide_instance(1, 0, selection));
    REQUIRE_FALSE(plate_focus_should_hide_instance(0, 0, selection));
}

TEST_CASE("Switching the active plate re-targets which plate is exempt", "[MoveHidePlates]")
{
    // Design 3.6: the decision is recomputed per frame from the current plate index, so simply
    // changing that index flips which plate hides - there is no cached state to invalidate.
    const std::set<int> no_selection;

    REQUIRE(plate_focus_should_hide_plate(1, 0, no_selection));
    REQUIRE_FALSE(plate_focus_should_hide_plate(1, 1, no_selection));
    REQUIRE(plate_focus_should_hide_plate(0, 1, no_selection));
}
