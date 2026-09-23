#include <catch2/catch.hpp>

#include "slic3r/Utils/HubHomeLogic.hpp"
#include "slic3r/Utils/LoginUserAgent.hpp"

using namespace Slic3r;
using namespace Slic3r::HubHome;

// The Home tab's hub view (src/slic3r/GUI/HubHomeView.cpp) follows these rules; the view itself
// needs a running hub and a WebView2 and is click-tested by hand.

static const std::string TOKEN = "abc123def456ghi7";

TEST_CASE("hub home: the page address is built from the hub's port, token and theme", "[HubHome]")
{
    CHECK(hub_page_url(13640, TOKEN, true) == "http://127.0.0.1:13640/r/abc123def456ghi7/?embed=1&theme=dark");
    CHECK(hub_page_url(13641, TOKEN, false) == "http://127.0.0.1:13641/r/abc123def456ghi7/?embed=1&theme=light");
    // A fallback port is used as given: nothing assumes 13640.
    CHECK(hub_page_url(13659, TOKEN, true) == "http://127.0.0.1:13659/r/abc123def456ghi7/?embed=1&theme=dark");
}

TEST_CASE("hub home: no address without a usable port and token", "[HubHome]")
{
    CHECK(hub_page_url(0, TOKEN, true).empty());
    CHECK(hub_page_url(-1, TOKEN, true).empty());
    CHECK(hub_page_url(70000, TOKEN, true).empty());
    CHECK(hub_page_url(13640, "", true).empty());
    CHECK(hub_page_url(13640, "short", true).empty());                       // under 10
    CHECK(hub_page_url(13640, std::string(33, 'a'), true).empty());          // over 32
    CHECK(hub_page_url(13640, "ABCDEFGHIJKL", true).empty());                // upper case
    CHECK(hub_page_url(13640, "abc/../def456", true).empty());               // path characters
    CHECK(hub_page_url(13640, "abcdefghij?x=1", true).empty());              // query injection
}

TEST_CASE("hub home: navigation stays on the loaded hub page", "[HubHome]")
{
    const int port = 13641;
    CHECK(is_hub_page_url("http://127.0.0.1:13641/r/abc123def456ghi7/", port, TOKEN));
    CHECK(is_hub_page_url("http://127.0.0.1:13641/r/abc123def456ghi7/?embed=1&theme=light", port, TOKEN));
    CHECK(is_hub_page_url("http://127.0.0.1:13641/r/abc123def456ghi7/#devices", port, TOKEN));
    CHECK(is_hub_page_url("http://127.0.0.1:13641/r/abc123def456ghi7", port, TOKEN));
    CHECK(is_hub_page_url("http://127.0.0.1:13641/r/abc123def456ghi7?embed=1", port, TOKEN));

    // Another port (the stale one after a move), another token (after "New link"), another
    // path or origin: all refused.
    CHECK_FALSE(is_hub_page_url("http://127.0.0.1:13640/r/abc123def456ghi7/", port, TOKEN));
    CHECK_FALSE(is_hub_page_url("http://127.0.0.1:13641/r/zzz123def456ghi7/", port, TOKEN));
    CHECK_FALSE(is_hub_page_url("http://127.0.0.1:13641/r/abc123def456ghi7x/", port, TOKEN));
    CHECK_FALSE(is_hub_page_url("http://127.0.0.1:13641/hub/", port, TOKEN));
    CHECK_FALSE(is_hub_page_url("http://localhost:13641/r/abc123def456ghi7/", port, TOKEN));
    CHECK_FALSE(is_hub_page_url("http://192.168.1.5:13641/r/abc123def456ghi7/", port, TOKEN));
    CHECK_FALSE(is_hub_page_url("https://127.0.0.1:13641/r/abc123def456ghi7/", port, TOKEN));
    CHECK_FALSE(is_hub_page_url("http://127.0.0.1:136410/r/abc123def456ghi7/", port, TOKEN));
    CHECK_FALSE(is_hub_page_url("http://127.0.0.1:13641.evil.com/r/abc123def456ghi7/", port, TOKEN));
    // Nothing is allowed before a page was loaded.
    CHECK_FALSE(is_hub_page_url("http://127.0.0.1:13641/r/abc123def456ghi7/", 0, ""));

    CHECK(is_allowed_navigation("about:blank", port, TOKEN));
    CHECK(is_allowed_navigation("data:text/html,<p>x</p>", port, TOKEN));
    CHECK(is_allowed_navigation("about:blank", 0, ""));
    CHECK_FALSE(is_allowed_navigation("https://github.com/aceRage/EdgeSlicer", port, TOKEN));
    CHECK_FALSE(is_allowed_navigation("file:///C:/Windows/win.ini", port, TOKEN));
    CHECK_FALSE(is_allowed_navigation("http://127.0.0.1:13619/web/flutter_web/index.html", port, TOKEN));
}

TEST_CASE("hub home: only web links go to the system browser", "[HubHome]")
{
    CHECK(is_external_browser_url("https://www.gnu.org/licenses/agpl-3.0.html"));
    CHECK(is_external_browser_url("http://example.com/"));
    CHECK(is_external_browser_url("HTTPS://GITHUB.COM/aceRage/EdgeSlicer"));
    CHECK_FALSE(is_external_browser_url("https://"));
    CHECK_FALSE(is_external_browser_url("file:///C:/Windows/System32/calc.exe"));
    CHECK_FALSE(is_external_browser_url("javascript:alert(1)"));
    CHECK_FALSE(is_external_browser_url("ms-settings:privacy"));
    CHECK_FALSE(is_external_browser_url("bambustudioopen://x"));
    CHECK_FALSE(is_external_browser_url("https://example.com/a b"));
    CHECK_FALSE(is_external_browser_url("https://example.com/\n--flag"));
    CHECK_FALSE(is_external_browser_url(""));
}

TEST_CASE("hub home: a re-check follows port moves, new links and a stopped hub", "[HubHome]")
{
    // Nothing changed: leave the page (and its camera streams) alone.
    CHECK(recheck(true, 13640, TOKEN, 13640, TOKEN) == Recheck::Keep);
    // Phone access toggled and the listener rebound on another port.
    CHECK(recheck(true, 13642, TOKEN, 13640, TOKEN) == Recheck::Reload);
    // "New link" or a restarted hub with another token.
    CHECK(recheck(true, 13640, "newtoken12345", 13640, TOKEN) == Recheck::Reload);
    // The first load (nothing loaded yet) is a reload too.
    CHECK(recheck(true, 13640, TOKEN, 0, "") == Recheck::Reload);
    // No hub, or an answer that cannot be used.
    CHECK(recheck(false, 13640, TOKEN, 13640, TOKEN) == Recheck::HubDown);
    CHECK(recheck(true, 0, TOKEN, 13640, TOKEN) == Recheck::HubDown);
    CHECK(recheck(true, 13640, "", 13640, TOKEN) == Recheck::HubDown);
}

TEST_CASE("hub home: theme parameter and live switch script", "[HubHome]")
{
    CHECK(std::string(theme_name(true)) == "dark");
    CHECK(std::string(theme_name(false)) == "light");
    CHECK(theme_script(true) == "if (window.__edgeTheme) window.__edgeTheme('dark');");
    CHECK(theme_script(false) == "if (window.__edgeTheme) window.__edgeTheme('light');");
}

TEST_CASE("login UA: views report the platform they were built for", "[LoginUA][HubHome]")
{
    // WebView::RecreateAll() refreshes every view's User-Agent after a theme switch; it used to
    // pass MacOS on every platform.
#if defined(_WIN32)
    CHECK(current_login_ua_platform() == LoginUAPlatform::Windows);
    CHECK(bbl_login_user_agent(current_login_ua_platform(), true, "en", "SM-Slicer", "2.4.0.0").find("Windows NT") != std::string::npos);
#elif defined(__APPLE__)
    CHECK(current_login_ua_platform() == LoginUAPlatform::MacOS);
#else
    CHECK(current_login_ua_platform() == LoginUAPlatform::Linux);
#endif
}
