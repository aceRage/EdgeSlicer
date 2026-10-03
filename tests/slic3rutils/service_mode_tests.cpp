#include <catch2/catch.hpp>

#include "slic3r/Utils/ServiceMode.hpp"

using namespace Slic3r::ServiceMode;

TEST_CASE("service mode: only an explicit yes turns it on", "[ServiceMode]")
{
    CHECK(is_truthy("1"));
    CHECK(is_truthy("true"));
    CHECK(is_truthy("TRUE"));
    CHECK(is_truthy(" Yes "));
    CHECK(is_truthy("on"));
    CHECK_FALSE(is_truthy(""));
    CHECK_FALSE(is_truthy("0"));
    CHECK_FALSE(is_truthy("false"));
    CHECK_FALSE(is_truthy("no"));
    CHECK_FALSE(is_truthy("2"));
    CHECK_FALSE(is_truthy("service"));
}

TEST_CASE("service mode: enable() is what children inherit", "[ServiceMode]")
{
    set_env(ENV_SERVICE, "0");
    CHECK_FALSE(enabled());
    enable();
    CHECK(enabled());
    set_env(ENV_SERVICE, "0"); // leave the test process as we found it
    CHECK_FALSE(enabled());
}

TEST_CASE("service mode: the number of instances is 1 unless a sane number says otherwise", "[ServiceMode]")
{
    CHECK(wanted_instances(nullptr) == 1);
    CHECK(wanted_instances("") == 1);
    CHECK(wanted_instances("0") == 0);
    CHECK(wanted_instances("2") == 2);
    CHECK(wanted_instances("4") == 4);
    CHECK(wanted_instances("5") == 1);
    CHECK(wanted_instances("-1") == 1);
    CHECK(wanted_instances("two") == 1);
    CHECK(wanted_instances("2x") == 1);
}
