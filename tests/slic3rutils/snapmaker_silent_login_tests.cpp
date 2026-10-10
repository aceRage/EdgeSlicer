#include <catch2/catch.hpp>

#include "slic3r/Utils/SnapmakerSilentLogin.hpp"
#include "libslic3r/AppConfig.hpp"

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

// Privacy (EdgeSlicerSite PR 4): somebody who never signed in to Snapmaker on this computer gets
// no Snapmaker traffic at startup - the hidden web view is never created for them.
TEST_CASE("silent login: never signed in on this computer means no attempt", "[SMSilentLogin]")
{
    StartupInputs in;
    in.signed_in_before = false;
    CHECK(skip_reason(in) == "not signed in to Snapmaker on this computer");
    // Even with everything else saying go, and before the network is ever asked about.
    in.network_down = true;
    CHECK(skip_reason(in) == "not signed in to Snapmaker on this computer");
    // The preference still names itself first when both apply.
    in.pref_enabled = false;
    CHECK(skip_reason(in) == "turned off in Preferences");
}

TEST_CASE("silent login: the outcome decides whether the next start may try again", "[SMSilentLogin]")
{
    CHECK(session_marker_after(Outcome::SignedIn) == SessionMarker::Set);
    // The saved session is gone: stop contacting Snapmaker until the person signs in by hand.
    CHECK(session_marker_after(Outcome::NoSession) == SessionMarker::Clear);
    // A network hiccup or a cancelled attempt says nothing about the session.
    CHECK(session_marker_after(Outcome::TimedOut) == SessionMarker::Keep);
    CHECK(session_marker_after(Outcome::Failed) == SessionMarker::Keep);
    CHECK(session_marker_after(Outcome::Cancelled) == SessionMarker::Keep);
    CHECK(session_marker_after(Outcome::Skipped) == SessionMarker::Keep);
}

TEST_CASE("silent login: a fresh config has no Snapmaker sign-in to come back to", "[SMSilentLogin]")
{
    Slic3r::AppConfig cfg;
    REQUIRE(cfg.has(k_session_key));
    CHECK_FALSE(cfg.get_bool(k_session_key));
    // The Preferences toggle keeps its default; it only matters once somebody has signed in.
    CHECK(cfg.get_bool(k_pref_key));
}

// 2.4.5.2 and earlier: the token ran on to the next '?', so id.snapmaker.com's
// "?token=<T>&_act=login_success" became "<T>&_act=login_success", which the account service
// refuses (HTTP 200, code 110002) - and that refusal was taken for a sign-in.
TEST_CASE("silent login: the token stops at the next parameter", "[SMSilentLogin]")
{
    const std::string redirect = "https://account.snapmaker.com/?token=abc.DEF-123_x&_act=login_success";
    CHECK(has_token_param(redirect));
    CHECK(token_from_url(redirect) == "abc.DEF-123_x");
    CHECK(token_from_url("https://account.snapmaker.com/?redirect=y&token=t1&_act=login_success") == "t1");
    CHECK(token_from_url("https://example.invalid/cb#token=t2&x=1") == "t2");
}

TEST_CASE("silent login: only the parameter named token counts", "[SMSilentLogin]")
{
    CHECK_FALSE(has_token_param("https://example.invalid/cb?access_token=zzz"));
    CHECK(token_from_url("https://example.invalid/cb?access_token=zzz").empty());
    CHECK_FALSE(has_token_param("https://example.invalid/cb?refresh_token=r"));
    CHECK(token_from_url("https://example.invalid/cb?access_token=zzz&token=abc") == "abc");
    CHECK_FALSE(has_token_param("https://id.snapmaker.com?from=orca"));
    // Present but empty: the dialog reports an empty token rather than ignoring the redirect.
    CHECK(has_token_param("https://example.invalid/cb?token=&_act=login_success"));
    CHECK(token_from_url("https://example.invalid/cb?token=&_act=login_success").empty());
}

TEST_CASE("silent login: a percent-encoded token is decoded", "[SMSilentLogin]")
{
    CHECK(token_from_url("https://example.invalid/?token=a%2Bb%2Fc%3D%3D&_act=login_success") == "a+b/c==");
    // A literal '+' stays (a token has no spaces); a stray '%' is kept as it is.
    CHECK(token_from_url("https://example.invalid/?token=a+b") == "a+b");
    CHECK(token_from_url("https://example.invalid/?token=50%") == "50%");
    CHECK(token_from_url("https://example.invalid/?token=5%zz") == "5%zz");
}

TEST_CASE("account answer: HTTP 200 with a refusal code is not a sign-in", "[SMSilentLogin]")
{
    // What id.snapmaker.com answers for a token it does not accept (checked 2026-10-10).
    const AccountAnswer a = read_account_answer(200, R"({"code":110002,"msg":"Missing authorization"})");
    CHECK_FALSE(a.ok);
    CHECK(a.rejected);
    CHECK(a.user_id.empty());
    CHECK(a.why == "rejected: code 110002 (Missing authorization)");
}

TEST_CASE("account answer: an account with an id is a sign-in", "[SMSilentLogin]")
{
    const AccountAnswer a = read_account_answer(200,
        R"({"code":200,"msg":"ok","data":{"id":4242,"nickname":"Nick","icon":"https://example.invalid/i.png","account":"a@example.invalid"}})");
    REQUIRE(a.ok);
    CHECK(a.user_id == "4242");
    CHECK(a.nickname == "Nick");
    CHECK(a.icon == "https://example.invalid/i.png");
    CHECK(a.account == "a@example.invalid");
    CHECK(a.why.empty());

    // No code at all, an id beyond 32 bits, or an id sent as text: still an account.
    const AccountAnswer big = read_account_answer(200, R"({"data":{"id":12345678901234}})");
    REQUIRE(big.ok);
    CHECK(big.user_id == "12345678901234");
    const AccountAnswer text = read_account_answer(200, R"({"code":0,"data":{"id":"u-77"}})");
    REQUIRE(text.ok);
    CHECK(text.user_id == "u-77");
    CHECK(text.nickname.empty());
    // Odd types for the details are dropped, not fatal.
    const AccountAnswer odd = read_account_answer(200, R"({"data":{"id":1,"nickname":null,"icon":5}})");
    REQUIRE(odd.ok);
    CHECK(odd.nickname.empty());
    CHECK(odd.icon.empty());
}

TEST_CASE("account answer: no account id means no sign-in", "[SMSilentLogin]")
{
    for (const char* body : {R"({"code":200})", R"({"code":200,"data":null})", R"({"code":200,"data":{}})",
                             R"({"code":200,"data":{"id":""}})", R"({"code":200,"data":{"id":null,"nickname":"N"}})",
                             R"({"code":200,"data":[1,2]})"}) {
        INFO(body);
        const AccountAnswer a = read_account_answer(200, body);
        CHECK_FALSE(a.ok);
        CHECK(a.rejected);
        CHECK(a.user_id.empty());
    }
}

TEST_CASE("account answer: transport and server trouble is not a refusal", "[SMSilentLogin]")
{
    const AccountAnswer bad_gateway = read_account_answer(502, "<html>Bad gateway</html>");
    CHECK_FALSE(bad_gateway.ok);
    CHECK_FALSE(bad_gateway.rejected);
    CHECK(bad_gateway.why == "HTTP 502");
    const AccountAnswer none = read_account_answer(0, "");
    CHECK_FALSE(none.ok);
    CHECK_FALSE(none.rejected);
    const AccountAnswer html = read_account_answer(200, "<html>maintenance</html>");
    CHECK_FALSE(html.ok);
    CHECK_FALSE(html.rejected);
    CHECK(html.why == "unreadable answer");
    // 401 / 403: the service looked at the token and refused it.
    CHECK(read_account_answer(401, "").rejected);
    CHECK(read_account_answer(403, "").rejected);
}

TEST_CASE("account answer: the reason never carries more than a short server message", "[SMSilentLogin]")
{
    const std::string long_msg(500, 'x');
    const AccountAnswer a = read_account_answer(200, R"({"code":1,"msg":")" + long_msg + R"("})");
    CHECK_FALSE(a.ok);
    CHECK(a.why.size() < 120);
}

TEST_CASE("silent login: a refused session signs out and stops startup attempts", "[SMSilentLogin]")
{
    CHECK(session_marker_after(Outcome::Rejected) == SessionMarker::Clear);
    CHECK(log_line(Outcome::Rejected, "account lookup: rejected: code 110002 (Missing authorization)") ==
          "Snapmaker silent login: session rejected (account lookup: rejected: code 110002 (Missing authorization))");
}
