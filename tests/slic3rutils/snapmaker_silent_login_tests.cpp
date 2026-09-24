#include <catch2/catch.hpp>

#include "slic3r/Utils/SnapmakerSilentLogin.hpp"

using namespace Slic3r::SMSilentLogin;

// GUI_App::sm_start_silent_login and SMUserLogin's silent mode follow these rules; the web view
// part needs WebView2 and a real session cookie and is checked by hand.

TEST_CASE("silent login: runs on a normal visible start", "[SMSilentLogin]")
{
    StartupInputs in;
    CHECK(skip_reason(in).empty());
}

TEST_CASE("silent login: each skip condition is reported", "[SMSilentLogin]")
{
    {
        StartupInputs in; in.pref_enabled = false;
        CHECK(skip_reason(in) == "turned off in Preferences");
    }
    {
        StartupInputs in; in.is_editor = false;
        CHECK(skip_reason(in) == "G-code viewer");
    }
    {
        StartupInputs in; in.hidden_instance = true;
        CHECK(skip_reason(in) == "hidden instance");
    }
    {
        StartupInputs in; in.main_window_ready = false;
        CHECK(skip_reason(in) == "main window not ready");
    }
    {
        StartupInputs in; in.already_signed_in = true;
        CHECK(skip_reason(in) == "already signed in");
    }
    {
        StartupInputs in; in.login_dialog_open = true;
        CHECK(skip_reason(in) == "sign-in dialog already open");
    }
    {
        StartupInputs in; in.network_down = true;
        CHECK(skip_reason(in) == "no network");
    }
}

TEST_CASE("silent login: the preference wins over everything else", "[SMSilentLogin]")
{
    StartupInputs in;
    in.pref_enabled    = false;
    in.hidden_instance = true;
    in.network_down    = true;
    CHECK(skip_reason(in) == "turned off in Preferences");
    // A hidden instance is skipped even with the preference on and the network up.
    in.pref_enabled = true;
    in.network_down = false;
    CHECK(skip_reason(in) == "hidden instance");
}

TEST_CASE("silent login: watchdog waits while nothing is decided", "[SMSilentLogin]")
{
    CHECK(on_tick(0, false, 0) == Tick::Wait);
    CHECK(on_tick(k_overall_timeout_ms - 1, false, 0) == Tick::Wait);
    // The page loaded a moment ago: a valid session still has time to redirect.
    CHECK(on_tick(3000, true, 1000) == Tick::Wait);
    CHECK(on_tick(10000, true, k_settle_ms - 1) == Tick::Wait);
}

TEST_CASE("silent login: a page that loads and sits still means no session", "[SMSilentLogin]")
{
    CHECK(on_tick(9000, true, k_settle_ms) == Tick::NoSession);
    CHECK(on_tick(k_settle_ms + 500, true, k_settle_ms + 500) == Tick::NoSession);
    // Re-navigating right up to the deadline still ends signed out, not "timed out".
    CHECK(on_tick(k_overall_timeout_ms, true, 100) == Tick::NoSession);
}

TEST_CASE("silent login: nothing loaded by the deadline is a timeout", "[SMSilentLogin]")
{
    CHECK(on_tick(k_overall_timeout_ms, false, 0) == Tick::TimedOut);
    CHECK(on_tick(k_overall_timeout_ms + 5000, false, 0) == Tick::TimedOut);
}

TEST_CASE("silent login: timing constants stay sane", "[SMSilentLogin]")
{
    STATIC_REQUIRE(k_settle_ms < k_overall_timeout_ms);
    STATIC_REQUIRE(k_tick_ms > 0);
    STATIC_REQUIRE(k_tick_ms < k_settle_ms);
    STATIC_REQUIRE(k_overall_timeout_ms <= 30000); // teardown within half a minute
}

TEST_CASE("silent login: token is read from the redirect address", "[SMSilentLogin]")
{
    CHECK(token_from_url("https://id.snapmaker.com?from=orca").empty());
    CHECK(token_from_url("").empty());
    CHECK(token_from_url("https://example.invalid/cb?token=abc.DEF-123") == "abc.DEF-123");
    CHECK(token_from_url("https://example.invalid/cb#token=xyz?next=1") == "xyz");
    CHECK(token_from_url("https://example.invalid/cb?token=") == "");
}

TEST_CASE("silent login: log line names the outcome and never needs the token", "[SMSilentLogin]")
{
    CHECK(log_line(Outcome::SignedIn) == "Snapmaker silent login: signed in");
    CHECK(log_line(Outcome::NoSession) == "Snapmaker silent login: no session");
    CHECK(log_line(Outcome::TimedOut) == "Snapmaker silent login: timed out");
    CHECK(log_line(Outcome::Skipped, "hidden instance") == "Snapmaker silent login: skipped (hidden instance)");
    CHECK(log_line(Outcome::Cancelled, "sign-in dialog opened") == "Snapmaker silent login: cancelled (sign-in dialog opened)");
    CHECK(log_line(Outcome::Failed, "account lookup HTTP 401") == "Snapmaker silent login: failed (account lookup HTTP 401)");
}
