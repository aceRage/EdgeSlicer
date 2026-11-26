#include <catch2/catch.hpp>

#include "slic3r/Utils/WebViewPendingLoad.hpp"

using namespace Slic3r::WebViewPendingLoad;

// WebViewPendingLoad.hpp: the macOS WKWebView gate that keeps Create()'s empty LoadURL and
// about:blank out of history, so Back does not land on a white page.

TEST_CASE("webview pending load: empty and about:blank are blank", "[WebViewPendingLoad]")
{
    CHECK(is_blank_url(""));
    CHECK(is_blank_url(static_cast<const char *>(nullptr)));
    CHECK(is_blank_url(std::string()));
    CHECK(is_blank_url("about:blank"));
    CHECK(is_blank_url(std::string("about:blank")));
    CHECK_FALSE(is_blank_url("http://127.0.0.1:13619/web/home/index.html"));
    CHECK_FALSE(is_blank_url("about:blank?x=1"));
    CHECK_FALSE(is_blank_url(" ABOUT:BLANK"));
    CHECK_FALSE(is_blank_url("file:///tmp/index.html"));
}

TEST_CASE("webview pending load: Create's empty LoadURL must not wipe the pending URL", "[WebViewPendingLoad]")
{
    Gate g;
    g.pending = "http://127.0.0.1:13619/web/home/index.html";

    CHECK(g.load("") == LoadAction::Ignore);
    CHECK(g.pending == "http://127.0.0.1:13619/web/home/index.html");
    CHECK(g.load("about:blank") == LoadAction::Ignore);
    CHECK(g.pending == "http://127.0.0.1:13619/web/home/index.html");
    CHECK(g.flush() == "http://127.0.0.1:13619/web/home/index.html");
    CHECK(g.handler_installed);
    CHECK(g.pending.empty());
}

TEST_CASE("webview pending load: a real URL before the handler is stashed", "[WebViewPendingLoad]")
{
    Gate g;
    CHECK(g.load("http://127.0.0.1:13619/web/flutter_web/index.html?path=2") == LoadAction::Stash);
    CHECK(g.pending == "http://127.0.0.1:13619/web/flutter_web/index.html?path=2");
    CHECK(g.flush() == "http://127.0.0.1:13619/web/flutter_web/index.html?path=2");
}

TEST_CASE("webview pending load: after the handler, about:blank dialogs still load", "[WebViewPendingLoad]")
{
    Gate g;
    CHECK(g.flush().empty());
    CHECK(g.load("about:blank") == LoadAction::Load);
    CHECK(g.load("") == LoadAction::Load);
    CHECK(g.load("http://127.0.0.1:13619/web/orca/stream_center.html") == LoadAction::Load);
}

TEST_CASE("webview pending load: a blank pending URL is not flushed as a navigation", "[WebViewPendingLoad]")
{
    Gate g;
    CHECK(g.load("about:blank") == LoadAction::Ignore);
    CHECK(g.pending.empty());
    CHECK(g.flush().empty());
}
