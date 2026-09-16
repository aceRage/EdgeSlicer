#include <catch2/catch.hpp>

#include "slic3r/Utils/LoginUserAgent.hpp"

using namespace Slic3r;

// Bambu Studio 02.08.02.61 (reference clone 66e405477) builds the login webview UA as:
//   "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
//   "Chrome/107.0.0.0 Safari/537.36 Edg/107.0.1418.52 BBL-Slicer/v<ver> (<theme>) BBL-Language/<lang>"
// These tests pin that layout: the browser prefix FIRST, the BBL tokens LAST, in that order.

TEST_CASE("bbl_login_user_agent: windows layout matches Bambu Studio", "[LoginUA]")
{
    const std::string ua = bbl_login_user_agent(LoginUAPlatform::Windows, false, "en_US");
    REQUIRE(ua ==
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
            "Chrome/107.0.0.0 Safari/537.36 Edg/107.0.1418.52 BBL-Slicer/v02.08.02.61 (light) BBL-Language/en");
}

TEST_CASE("bbl_login_user_agent: mac layout matches Bambu Studio", "[LoginUA]")
{
    const std::string ua = bbl_login_user_agent(LoginUAPlatform::MacOS, true, "de_DE");
    REQUIRE(ua ==
            "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) "
            "BBL-Slicer/v02.08.02.61 (dark) BBL-Language/de");
}

TEST_CASE("bbl_login_user_agent: linux uses the same WebKit prefix as mac", "[LoginUA]")
{
    REQUIRE(bbl_login_user_agent(LoginUAPlatform::Linux, false, "fr") ==
            bbl_login_user_agent(LoginUAPlatform::MacOS, false, "fr"));
}

TEST_CASE("bbl_login_user_agent: browser prefix comes first, BBL tokens last", "[LoginUA]")
{
    const std::string ua = bbl_login_user_agent(LoginUAPlatform::Windows, false, "en");
    // The fork used to emit "BBL-Slicer/v.. Mozilla/5.0 .."; that order is the regression.
    REQUIRE(ua.rfind("Mozilla/5.0", 0) == 0);
    const auto slicer_pos = ua.find("BBL-Slicer/v");
    const auto lang_pos   = ua.find("BBL-Language/");
    REQUIRE(slicer_pos != std::string::npos);
    REQUIRE(lang_pos != std::string::npos);
    REQUIRE(slicer_pos > ua.find("Edg/107.0.1418.52"));
    REQUIRE(lang_pos > slicer_pos);
}

TEST_CASE("bbl_login_user_agent: reports the gated Bambu version, not our own", "[LoginUA]")
{
    // The version token must be BBL_LOGIN_UA_VERSION regardless of the slicer version we
    // pass in; reporting EdgeSlicer's own version is what fails bambulab's gate.
    const std::string ua = bbl_login_user_agent(LoginUAPlatform::Windows, false, "en", "BBL-Slicer", "01.10.01.50");
    REQUIRE(ua.find(std::string("BBL-Slicer/v") + BBL_LOGIN_UA_VERSION) != std::string::npos);
    REQUIRE(ua.find("01.10.01.50") == std::string::npos);
}

TEST_CASE("bbl_login_user_agent: non-BBL brand keeps our own version", "[LoginUA]")
{
    const std::string ua = bbl_login_user_agent(LoginUAPlatform::MacOS, false, "en", "SM-Slicer", "01.10.01.50");
    REQUIRE(ua.find("SM-Slicer/v01.10.01.50") != std::string::npos);
    REQUIRE(ua.find(BBL_LOGIN_UA_VERSION) == std::string::npos);
}

TEST_CASE("bbl_login_user_agent: language is the bare primary subtag", "[LoginUA]")
{
    // bambulab expects "zh", not "zh_CN" / "zh-CN".
    REQUIRE(bbl_login_user_agent(LoginUAPlatform::Windows, false, "zh_CN").find("BBL-Language/zh") != std::string::npos);
    REQUIRE(bbl_login_user_agent(LoginUAPlatform::Windows, false, "zh_CN").find("BBL-Language/zh_CN") == std::string::npos);
    REQUIRE(bbl_login_user_agent(LoginUAPlatform::Windows, false, "pt-BR").find("BBL-Language/pt") != std::string::npos);
    // Empty locale falls back to "en" rather than emitting a dangling token.
    REQUIRE(bbl_login_user_agent(LoginUAPlatform::Windows, false, "").find("BBL-Language/en") != std::string::npos);
}
