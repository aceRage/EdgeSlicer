#include <catch2/catch.hpp>

#include "slic3r/GUI/ConfigManipulation.hpp"

using namespace Slic3r::GUI;
using Check = ConfigManipulation::LayerHeightCheck;

// The decision behind the Adjust / Ignore dialogs for Layer height and Initial layer height. The dialogs
// themselves are wx and are in the GUI hand test; this is the part that picks which one (if any) appears.
TEST_CASE("Layer height limits: which dialog an initial layer height gets", "[LayerHeightCheck]")
{
    const double min_limit = 0.08, max_limit = 0.28;

    SECTION("inside the limits, and exactly on them") {
        CHECK(ConfigManipulation::classify_layer_height(0.2, min_limit, max_limit) == Check::InRange);
        CHECK(ConfigManipulation::classify_layer_height(0.08, min_limit, max_limit) == Check::InRange);
        CHECK(ConfigManipulation::classify_layer_height(0.28, min_limit, max_limit) == Check::InRange);
    }
    SECTION("above the maximum and below the minimum") {
        CHECK(ConfigManipulation::classify_layer_height(0.32, min_limit, max_limit) == Check::TooHigh);
        CHECK(ConfigManipulation::classify_layer_height(0.05, min_limit, max_limit) == Check::TooLow);
    }
    SECTION("zero goes to the minimum without a question") {
        CHECK(ConfigManipulation::classify_layer_height(0., min_limit, max_limit) == Check::Zero);
    }
    SECTION("limits that are not set do not apply; zero is then left to the generic reset") {
        CHECK(ConfigManipulation::classify_layer_height(0., 0., 0.) == Check::InRange);
        CHECK(ConfigManipulation::classify_layer_height(5., 0., 0.) == Check::InRange);
        CHECK(ConfigManipulation::classify_layer_height(0.01, 0., max_limit) == Check::InRange);
        CHECK(ConfigManipulation::classify_layer_height(0.4, min_limit, 0.) == Check::InRange);
    }
}
