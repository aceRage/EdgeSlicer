// Orca #11407: "Auto slice after changes" in Preferences, with a delay in seconds.
#include <catch2/catch.hpp>

#include "libslic3r/AppConfig.hpp"

#include <limits>

using namespace Slic3r;

TEST_CASE("Auto slice is off by default with a one second delay", "[AppConfig][AutoSlice]")
{
    AppConfig config;
    CHECK_FALSE(config.get_bool("auto_slice_after_change"));
    CHECK(config.get("auto_slice_change_delay_seconds") == "1");
}

TEST_CASE("The auto slice delay reads as whole seconds", "[AppConfig][AutoSlice]")
{
    CHECK(AppConfig::auto_slice_delay_seconds("") == 0);
    CHECK(AppConfig::auto_slice_delay_seconds("0") == 0);
    CHECK(AppConfig::auto_slice_delay_seconds("3") == 3);
    CHECK(AppConfig::auto_slice_delay_seconds("abc") == 0);
    CHECK(AppConfig::auto_slice_delay_seconds("-4") == 0);
    // The delay goes to a wxTimer in milliseconds, which must fit an int.
    CHECK(AppConfig::auto_slice_delay_seconds("99999999999") == std::numeric_limits<int>::max() / 1000);
}
