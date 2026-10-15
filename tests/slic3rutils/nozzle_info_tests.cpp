#include <catch2/catch.hpp>

#include "slic3r/GUI/NozzleInfo.hpp"

#include <limits>

using namespace Slic3r::GUI::NozzleInfo;

// Orca #12814: a diameter the printer did not report is 0, which means unknown, not 0.4 mm.
TEST_CASE("A zero or invalid nozzle diameter is unknown", "[NozzleInfo]")
{
    CHECK_FALSE(diameter_known(0.f));
    CHECK_FALSE(diameter_known(-0.4f));
    CHECK_FALSE(diameter_known(std::numeric_limits<float>::quiet_NaN()));
    CHECK_FALSE(diameter_known(std::numeric_limits<float>::infinity()));
    CHECK(diameter_known(0.2f));
    CHECK(diameter_known(0.4f));
    CHECK(diameter_known(0.8f));
}
