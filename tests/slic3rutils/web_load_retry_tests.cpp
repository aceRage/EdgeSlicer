#include <catch2/catch.hpp>

#include "slic3r/Utils/WebLoadRetry.hpp"

using namespace Slic3r::WebLoadRetry;

// The Device tab's reaction to a page that does not load (WebLoadRetry.hpp): a loopback page that
// drops its connection is retried and logged as a warning; nothing is fatal.

static const char* const PAGE = "http://127.0.0.1:13619/web/orca/missing_connection.html?edge_page_token=22a2";

TEST_CASE("web load retry: what counts as this machine", "[WebLoadRetry]")
{
    CHECK(is_loopback_url(PAGE));
    CHECK(is_loopback_url("http://localhost:13619/web/flutter_web/index.html?path=2"));
    CHECK(is_loopback_url("HTTP://LOCALHOST/"));
    CHECK(is_loopback_url("http://[::1]:13640/hub/"));
    CHECK(is_loopback_url("https://127.0.0.1"));
    CHECK(!is_loopback_url("http://192.168.1.20/"));
    CHECK(!is_loopback_url("http://127.0.0.1.example.com/"));
    CHECK(!is_loopback_url("http://localhost.evil/"));
    CHECK(!is_loopback_url("http://127.0.0.1@printer.lan/")); // userinfo, not the host
    CHECK(!is_loopback_url("file:///C:/web/index.html"));
    CHECK(!is_loopback_url("127.0.0.1:13619/web"));
    CHECK(!is_loopback_url(""));
}

TEST_CASE("web load retry: a page is its URL without query and fragment", "[WebLoadRetry]")
{
    CHECK(page_of(PAGE) == "http://127.0.0.1:13619/web/orca/missing_connection.html");
    CHECK(page_of("http://127.0.0.1:13619/a.html#x") == "http://127.0.0.1:13619/a.html");
    CHECK(page_of("http://127.0.0.1:13619/a.html") == "http://127.0.0.1:13619/a.html");
}

TEST_CASE("web load retry: the decision", "[WebLoadRetry]")
{
    Facts f;
    f.failure  = Failure::Connection;
    f.loopback = true;

    SECTION("the 2026-10-08 failure: retried, as a warning, never fatal")
    {
        const Decision d = decide(f);
        CHECK(d.retry);
        CHECK(d.delay_ms == 500);
        CHECK(d.log == Log::Warning);
    }
    SECTION("the delay grows and is capped")
    {
        CHECK(delay_ms(0) == 500);
        CHECK(delay_ms(1) == 1000);
        CHECK(delay_ms(2) == 2000);
        CHECK(delay_ms(3) == 4000);
        CHECK(delay_ms(4) == 8000);
        CHECK(delay_ms(50) == 8000);
        CHECK(delay_ms(-1) == 500);
        int total = 0;
        for (int i = 0; i < MAX_RETRIES; ++i) total += delay_ms(i);
        CHECK(total >= 15000); // long enough for a hub handover and a busy GUI thread
        CHECK(total <= 60000);
    }
    SECTION("out of retries: an error, still not fatal, no more retries")
    {
        f.retries_so_far = MAX_RETRIES;
        const Decision d = decide(f);
        CHECK(!d.retry);
        CHECK(d.log == Log::Error);
    }
    SECTION("a printer's own web UI on the LAN is not retried")
    {
        f.loopback = false;
        const Decision d = decide(f);
        CHECK(!d.retry);
        CHECK(d.log == Log::Error);
    }
    SECTION("errors a retry cannot fix are not retried")
    {
        f.failure = Failure::Other;
        CHECK(!decide(f).retry);
    }
    SECTION("a load replaced by another one is no failure at all")
    {
        f.superseded     = true;
        const Decision d = decide(f);
        CHECK(!d.retry);
        CHECK(d.log == Log::Info);
        f.superseded = false;
        f.failure    = Failure::Cancelled;
        CHECK(decide(f).log == Log::Info);
        CHECK(!decide(f).retry);
    }
}
