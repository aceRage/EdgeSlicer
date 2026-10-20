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

// Orca #13330: every nozzle diameter check passes when the printer has not reported one, and
// otherwise compares within a micron (preset values are doubles, reports are floats).
TEST_CASE("Nozzle diameter matches when equal or unknown", "[NozzleInfo]")
{
    CHECK(diameter_matches_or_unknown(0.f, 0.4f));
    CHECK(diameter_matches_or_unknown(0.f, 0.6f));
    CHECK(diameter_matches_or_unknown(0.4f, 0.4f));
    CHECK(diameter_matches_or_unknown(0.4f, float(0.4)));
    CHECK(diameter_matches_or_unknown(0.6f, 0.6004f));
    CHECK_FALSE(diameter_matches_or_unknown(0.4f, 0.6f));
    CHECK_FALSE(diameter_matches_or_unknown(0.2f, 0.4f));
    CHECK_FALSE(diameter_matches_or_unknown(0.8f, 0.f));
}

// Orca #13255: preset lookups key on the printer preset's diameter until the printer reports one,
// so the AMS filament list is not empty (a "0.0" lookup matches no printer preset).
TEST_CASE("Unknown nozzle diameter falls back to the preset's", "[NozzleInfo]")
{
    CHECK(diameter_or(0.f, 0.4f) == 0.4f);
    CHECK(diameter_or(0.f, 0.6f) == 0.6f);
    CHECK(diameter_or(0.2f, 0.4f) == 0.2f);
    CHECK(diameter_or(0.8f, 0.f) == 0.8f);
}
