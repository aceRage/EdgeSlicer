#include <catch2/catch.hpp>

#include "slic3r/Utils/DevicePageLoad.hpp"

using namespace Slic3r::DevicePageLoad;

// When the Device tab's web view navigates (DevicePageLoad.hpp): nothing loads before startup is
// over, and one connect or preset switch loads the Device page once.

static const std::string DEVICE = "http://127.0.0.1:13619/web/flutter_web/index.html?path=2&locale=en-US&dark_mode=1";
static const std::string DEVICE_LIGHT = "http://127.0.0.1:13619/web/flutter_web/index.html?path=2&locale=en-US&dark_mode=0";
static const std::string MISSING = "http://127.0.0.1:13619/web/orca/missing_connection.html";
static const std::string HOST = "http://printer.lan/";

TEST_CASE("device page load: our own pages", "[DevicePageLoad]")
{
    CHECK(own_page(DEVICE));
    CHECK(own_page("http://127.0.0.1:13619/web/flutter_web/index.html?path=5"));
    CHECK(!own_page(MISSING));
    CHECK(!own_page(HOST));
    CHECK(!own_page(""));
}

TEST_CASE("device page load: everything asked for during startup is held, the last one wins", "[DevicePageLoad]")
{
    Gate g(false);
    CHECK(!g.released());
    CHECK(g.request(DEVICE, "", Kind::Show, 0) == Action::Hold);
    CHECK(g.request(MISSING, "", Kind::Show, 10) == Action::Hold);
    CHECK(g.request(DEVICE, "key", Kind::Reload, 20) == Action::Hold);
    CHECK(g.holding());
    CHECK(g.held_url() == DEVICE);

    Request r;
    REQUIRE(g.release(r));
    CHECK(r.url == DEVICE);
    CHECK(r.apikey == "key");
    CHECK(g.released());
    CHECK(!g.holding());

    // Released once: a second release has nothing to load, and requests now go through.
    Request again;
    CHECK(!g.release(again));
    CHECK(g.request(MISSING, "", Kind::Show, 30) == Action::Load);
}

TEST_CASE("device page load: a release with nothing held loads nothing", "[DevicePageLoad]")
{
    Gate g(false);
    Request r;
    CHECK(!g.release(r));
    CHECK(g.released());
    CHECK(g.request(DEVICE, "", Kind::Show, 0) == Action::Load);
}

TEST_CASE("device page load: a view made after startup is not held", "[DevicePageLoad]")
{
    Gate g(true);
    CHECK(g.request(DEVICE, "", Kind::Show, 0) == Action::Load);
    Request r;
    CHECK(!g.release(r));
}

TEST_CASE("device page load: one connect asking twice loads once", "[DevicePageLoad]")
{
    Gate g(true);
    // sw_mqtt_set_engine from the send dialog (need_reload): the preset row shows the page and the
    // connect asks for a reload, back to back.
    REQUIRE(g.request(DEVICE, "", Kind::Show, 1000) == Action::Load);
    g.started(DEVICE, 1000);
    CHECK(g.request(DEVICE, "", Kind::Reload, 1001) == Action::SkipLoading);
    CHECK(g.request(DEVICE, "", Kind::Show, 1002) == Action::SkipLoading);
}

TEST_CASE("device page load: showing a page the view already shows does nothing", "[DevicePageLoad]")
{
    Gate g(true);
    REQUIRE(g.request(DEVICE, "", Kind::Show, 0) == Action::Load);
    g.started(DEVICE, 0);
    g.finished(DEVICE, true);
    // A preset switch to the U1 (the page was loaded at startup) or the connect's own Show.
    CHECK(g.request(DEVICE, "", Kind::Show, 60000) == Action::SkipShowing);
    // A connect from another page asks for a reload: that one loads.
    CHECK(g.request(DEVICE, "", Kind::Reload, 60001) == Action::Load);
    g.started(DEVICE, 60001);
    CHECK(g.request(DEVICE, "", Kind::Reload, 60002) == Action::SkipLoading);
    g.finished(DEVICE, true);
    // The next connect, a minute later, reloads again: once.
    CHECK(g.request(DEVICE, "", Kind::Reload, 120000) == Action::Load);
}

TEST_CASE("device page load: a different URL always loads", "[DevicePageLoad]")
{
    Gate g(true);
    g.started(DEVICE, 0);
    g.finished(DEVICE, true);
    // Theme or language changed: the URL differs, so the page loads again.
    CHECK(g.request(DEVICE_LIGHT, "", Kind::Show, 10) == Action::Load);
    g.started(DEVICE_LIGHT, 10);
    // Another page entirely, while the first is still loading.
    CHECK(g.request(MISSING, "", Kind::Show, 20) == Action::Load);
}

TEST_CASE("device page load: somebody else's page reloads when asked to show it", "[DevicePageLoad]")
{
    Gate g(true);
    g.started(HOST, 0);
    g.finished(HOST, true);
    CHECK(g.request(HOST, "", Kind::Show, 100) == Action::Load);
    // ... but not twice at once.
    g.started(HOST, 100);
    CHECK(g.request(HOST, "", Kind::Show, 101) == Action::SkipLoading);
}

TEST_CASE("device page load: a failed or stuck load does not block the next one", "[DevicePageLoad]")
{
    Gate g(true);
    g.started(DEVICE, 0);
    g.finished(DEVICE, false);
    CHECK(g.request(DEVICE, "", Kind::Show, 10) == Action::Load);

    // No report at all: after the window the same URL may load again.
    Gate s(true);
    s.started(DEVICE, 0);
    CHECK(s.request(DEVICE, "", Kind::Reload, Gate::LOADING_WINDOW_MS - 1) == Action::SkipLoading);
    CHECK(s.request(DEVICE, "", Kind::Reload, Gate::LOADING_WINDOW_MS) == Action::Load);
}

TEST_CASE("device page load: reports for a replaced page change nothing", "[DevicePageLoad]")
{
    Gate g(true);
    g.started(MISSING, 0);
    g.started(DEVICE, 5);
    g.finished(MISSING, true); // the page it replaced
    CHECK(g.request(DEVICE, "", Kind::Show, 6) == Action::SkipLoading);
    // The loaded report may carry a different query (the page server adds its token).
    g.finished("http://127.0.0.1:13619/web/flutter_web/index.html?path=2&edge_page_token=1", true);
    CHECK(g.request(DEVICE, "", Kind::Show, 7) == Action::SkipShowing);
    // A late failure report after the page loaded does not undo it.
    g.finished(DEVICE, false);
    CHECK(g.request(DEVICE, "", Kind::Show, 8) == Action::SkipShowing);
}
