// The first request after a Bambu printer is selected (FirstRequestGate::ConnectWatch).
//
// Selecting a printer starts an asynchronous dial; the Device tab's refresh asked the printer for
// its full status a few milliseconds later, before the session existed, and the plug-in refused it
// (send_message_to_printer ... ret=-1 / publish_json ... code: -1) - on every first selection, with
// nothing actually wrong. These cases drive the rule with a fake plug-in session: no printer, no
// network, no wx.

#include <catch2/catch.hpp>

#include "slic3r/GUI/FirstRequestGate.hpp"

#include <string>
#include <vector>

using Slic3r::FirstRequestGate::ConnectWatch;
using Slic3r::FirstRequestGate::HOLD_MAX_MS;

namespace {

// A fake network plug-in session: connect() returns at once, the session comes up `dial_ms`
// later (or never, when dial_ms < 0), and a publish before that is refused with -1 - which is
// exactly what UltraNet's sendToPrinter / sendCloud do while the TLS dial is in flight.
struct FakeSession
{
    long long dial_ms   = 700;
    long long dialed_at = -1;
    std::vector<std::string> sent;    // what reached the printer
    int                      refused = 0; // publishes the plug-in refused (-1)

    void connect(long long now) { dialed_at = now; }
    bool up(long long now) const { return dialed_at >= 0 && dial_ms >= 0 && now >= dialed_at + dial_ms; }
    int publish(long long now, const std::string& what)
    {
        if (!up(now)) { ++refused; return -1; }
        sent.push_back(what);
        return 0;
    }
};

// The host side, wired the way MachineObject / DeviceManager / GUI_App use the watch:
//  * select(): DeviceManager::set_selected_machine -> MachineObject::connect + on_selected;
//  * tick(): MonitorPanel::update_all -> check_pushing -> keep_alive, which asks for a pushall
//    while no report has landed (m_push_count == 0), at most every 3 s (REQUEST_PUSH_MIN_TIME);
//  * connected(): GUI_App's on_local_connect(Ok) handler - pushall(request_now) + get_version;
//  * report(): a push_status arriving (MachineObject::parse_json).
struct Host
{
    FakeSession& s;
    ConnectWatch watch;
    long long    last_request_push = -100000;
    int          push_count        = 0;
    std::vector<long long> up_logged, status_logged; // what the one-line-per-connect log said

    explicit Host(FakeSession& session) : s(session) {}

    int request(long long now, const std::string& what)
    {
        if (watch.hold(now)) return 0; // held: the connected callback asks for it
        return s.publish(now, what);
    }
    int pushall(long long now, bool request_now = false)
    {
        if (now - last_request_push < 3000 && !request_now) return -1; // throttled, never sent
        last_request_push = now;
        return request(now, "pushall");
    }
    void select(long long now)
    {
        push_count = 0;
        s.connect(now);
        watch.on_selected(now, false);
    }
    void tick(long long now)
    {
        if (push_count == 0) pushall(now);
    }
    void connected(long long now)
    {
        const long long ms = watch.on_session_up(now);
        if (ms >= 0) up_logged.push_back(ms);
        pushall(now, true);
        request(now, "get_version");
    }
    void report(long long now)
    {
        ++push_count;
        const long long ms = watch.on_status(now);
        if (ms >= 0) status_logged.push_back(ms);
    }
};

} // namespace

TEST_CASE("[FirstRequest] an early pushall is held, the connected callback sends exactly one", "[FirstRequest]")
{
    FakeSession s;
    s.dial_ms = 700;
    Host h(s);

    h.select(1000);
    h.tick(1003);  // the synchronous update_all right after the selection
    h.tick(2000);  // the refresh timer while the dial is still in flight (throttled)
    CHECK(s.refused == 0);  // nothing went to a session that was not there
    CHECK(s.sent.empty());
    CHECK(h.watch.held == 1);

    h.connected(1700);
    REQUIRE(s.sent.size() == 2);
    CHECK(s.sent[0] == "pushall");
    CHECK(s.sent[1] == "get_version");

    h.report(1900);
    h.tick(4000);  // a report has landed: keep_alive asks for nothing more
    h.tick(9000);
    CHECK(s.sent.size() == 2);  // one pushall, not two
    CHECK(s.refused == 0);

    REQUIRE(h.up_logged.size() == 1);
    CHECK(h.up_logged[0] == 700);
    REQUIRE(h.status_logged.size() == 1);
    CHECK(h.status_logged[0] == 900);
}

TEST_CASE("[FirstRequest] without the hold, the first pushall is refused - the bug", "[FirstRequest]")
{
    // The same sequence with the request sent straight to the plug-in, as before the fix.
    FakeSession s;
    s.dial_ms = 700;
    s.connect(1000);
    CHECK(s.publish(1003, "pushall") == -1);
    CHECK(s.refused == 1);
}

TEST_CASE("[FirstRequest] a dial that never reports stops holding after HOLD_MAX_MS", "[FirstRequest]")
{
    FakeSession s;
    s.dial_ms = -1;  // the printer is off: the session never comes up, no callback either
    Host h(s);

    h.select(0);
    for (long long t = 3; t < HOLD_MAX_MS; t += 1000) h.tick(t);
    CHECK(s.refused == 0);  // held, every one of them

    h.tick(HOLD_MAX_MS + 10);
    CHECK(s.refused == 1);  // now it goes out, fails and is logged as before
}

TEST_CASE("[FirstRequest] a reported failure stops holding at once", "[FirstRequest]")
{
    ConnectWatch w;
    w.on_selected(100, false);
    CHECK(w.hold(200));
    w.on_session_down();
    CHECK_FALSE(w.hold(300));
    CHECK(w.on_status(400) == 300);  // a later report still gets its line
}

TEST_CASE("[FirstRequest] a session already up at selection holds nothing", "[FirstRequest]")
{
    // A cloud printer while the cloud session is connected.
    ConnectWatch w;
    w.on_selected(5000, true);
    CHECK_FALSE(w.hold(5001));
    CHECK(w.held == 0);
    CHECK(w.on_status(5300) == 300);
    CHECK(w.on_status(6300) == -1);  // only the first report is logged
}

TEST_CASE("[FirstRequest] each selection logs its own connect and first status once", "[FirstRequest]")
{
    ConnectWatch w;
    CHECK(w.on_status(10) == -1);  // never selected: nothing to log
    CHECK_FALSE(w.hold(10));

    w.on_selected(1000, false);
    CHECK(w.on_session_up(1500) == 500);
    CHECK(w.on_session_up(1600) == -1);  // a re-dial callback for the same selection: no second line
    CHECK(w.on_status(1800) == 800);
    CHECK(w.on_status(1900) == -1);

    w.on_selected(20000, false);  // selected again
    CHECK(w.hold(20001));
    CHECK(w.on_session_up(20400) == 400);
    CHECK_FALSE(w.hold(20401));
    CHECK(w.on_status(20600) == 600);
}
