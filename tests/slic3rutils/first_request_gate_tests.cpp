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
    CHECK_FALSE(w.dialling(300));
    CHECK(w.on_status(400) == -1);  // not a report over this selection's live session
}

TEST_CASE("[FirstRequest] a report that lands before the session is up is not the first status", "[FirstRequest]")
{
    // The printer was watched before it was selected: its watch session's last reports are still
    // queued for the GUI thread when the selection dials the printer's own session.
    ConnectWatch w;
    w.on_selected(1000, false);
    CHECK(w.on_status(1054) == -1);
    CHECK(w.on_session_up(2338) == 1338);
    CHECK(w.on_status(3100) == 2100);
}

TEST_CASE("[FirstRequest] commands made while dialling wait, polls keep their latest copy", "[FirstRequest]")
{
    using Slic3r::FirstRequestGate::HeldCommand;
    using Slic3r::FirstRequestGate::HeldQueue;
    HeldQueue q;
    auto cmd = [](const char* key, const char* payload, bool info) {
        HeldCommand c;
        c.key     = key;
        c.payload = payload;
        c.info    = info;
        return c;
    };
    // StatusPanel asks for the PA calibration table on every refresh until an answer lands.
    CHECK_FALSE(q.hold(cmd("print/extrusion_cali_get", "seq 20005", true)));
    CHECK_FALSE(q.hold(cmd("system/ledctrl", "light on", false)));
    CHECK(q.hold(cmd("print/extrusion_cali_get", "seq 20006", true)));
    CHECK_FALSE(q.hold(cmd("system/ledctrl", "light off", false)));  // user commands are all kept, in order
    CHECK(q.hold(cmd("print/extrusion_cali_get", "seq 20007", true)));
    REQUIRE(q.size() == 3);

    const std::vector<HeldCommand> out = q.take();
    CHECK(q.empty());
    REQUIRE(out.size() == 3);
    CHECK(out[0].payload == "light on");
    CHECK(out[1].payload == "light off");
    CHECK(out[2].payload == "seq 20007");  // one poll, the latest
}

TEST_CASE("[FirstRequest] dialling ends when the session is up, fails, or the hold runs out", "[FirstRequest]")
{
    ConnectWatch w;
    CHECK_FALSE(w.dialling(10));  // nothing selected
    w.on_selected(1000, false);
    CHECK(w.dialling(1001));
    CHECK_FALSE(w.dialling(1000 + HOLD_MAX_MS));
    w.on_session_up(1500);
    CHECK_FALSE(w.dialling(1501));
    w.on_selected(5000, true);  // cloud session already up
    CHECK_FALSE(w.dialling(5001));
}

TEST_CASE("[FirstRequest] a session already up at selection holds nothing", "[FirstRequest]")
{
    // A cloud printer while the cloud session is connected.
    ConnectWatch w;
    w.on_selected(5000, true);
    CHECK_FALSE(w.hold(5001));
    CHECK(w.held == 0);
    CHECK(w.on_status(5300) == 300);  // the session was up at selection: this one counts
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

// ---- the selection gate: held commands across the LAN and cloud session events ----
//
// Hand test 3 (first selection of a session, a LAN printer, signed in): get_version and
// extrusion_cali_get were held, the cloud session came up, then the LAN session - and the held
// extrusion_cali_get was dropped as "did not come up in time" in the very second the session came
// up, and the "early request(s) held" count said 1 instead of 2. The LAN handler flushed the held
// commands only after its own pushall, and that pushall's publish took the still-full queue for an
// expired hold. The host below is wired the way MachineObject, DeviceManager::on_cloud_session and
// GUI_App's on_local_connect are now.
namespace {

using Slic3r::FirstRequestGate::HeldCommand;
using Slic3r::FirstRequestGate::SelectionGate;
using Slic3r::FirstRequestGate::event_applies;

struct GateHost
{
    bool          lan_printer = true;
    long long     lan_up_at   = -1; // when the fake plug-in's LAN session is up (-1: never)
    long long     cloud_up_at = -1; // ... and the cloud session
    SelectionGate gate;
    std::vector<std::string> sent, not_sent;
    int                      refused = 0;
    long long                up_ms = -1;
    int                      up_held = 0;

    bool session_up(long long now) const
    {
        const long long at = lan_printer ? lan_up_at : cloud_up_at;
        return at >= 0 && now >= at;
    }
    void plugin_publish(long long now, const std::string& key)
    {
        if (session_up(now)) sent.push_back(key);
        else ++refused;
    }
    void report(const SelectionGate::Dropped& d, long long now)
    {
        for (const HeldCommand& c : d.commands) {
            if (d.send) plugin_publish(now, c.key);
            else not_sent.push_back(c.key + ": " + d.why);
        }
    }
    // MachineObject::publish_json
    void publish(long long now, const std::string& key, bool info)
    {
        if (gate.dialling(now)) {
            HeldCommand c;
            c.key  = key;
            c.info = info;
            gate.hold_command(now, c);
            return;
        }
        if (gate.queued() > 0) report(gate.settle(now), now);
        plugin_publish(now, key);
    }
    // command_request_push_all / command_get_version: held, not queued
    void status_request(long long now, const std::string& key)
    {
        if (gate.hold_status(now)) return;
        publish(now, key, true);
    }
    // MachineObject::note_session_up
    void session_up_event(long long now)
    {
        SelectionGate::Up u = gate.session_up(now);
        if (u.ms >= 0) { up_ms = u.ms; up_held = u.held; }
        for (const HeldCommand& c : u.send) publish(now, c.key, c.info);
    }
    // GUI_App's on_local_connect(Ok)
    void lan_event(long long now)
    {
        if (!event_applies(lan_printer, true)) return;
        session_up_event(now);
        status_request(now, "pushing/pushall");
        status_request(now, "info/get_version");
    }
    // DeviceManager::on_cloud_session(0)
    void cloud_event(long long now)
    {
        if (!event_applies(lan_printer, false)) return;
        session_up_event(now);
    }
    // the first 1.1 s after the selection, as in the hand test, plus a user command
    void early_requests()
    {
        gate.select(1, false); // 1, not 0: a selection time of 0 means "nothing selected"
        status_request(142, "info/get_version");
        publish(486, "print/extrusion_cali_get", true);
        publish(700, "system/ledctrl", false);
        publish(1036, "print/extrusion_cali_get", true);
    }
};

} // namespace

TEST_CASE("[FirstRequest] LAN printer: cloud session up first, then the LAN session", "[FirstRequest]")
{
    GateHost h;
    h.lan_up_at   = 1241;
    h.cloud_up_at = 1100;
    h.early_requests();
    h.cloud_event(1100); // says nothing about a LAN printer
    CHECK(h.sent.empty());
    CHECK(h.gate.queued() == 2);
    h.lan_event(1241);

    CHECK(h.refused == 0);
    CHECK(h.not_sent.empty());
    REQUIRE(h.sent.size() == 4);
    CHECK(h.sent[0] == "system/ledctrl");            // held, in the order asked
    CHECK(h.sent[1] == "print/extrusion_cali_get");  // one copy, the latest
    CHECK(h.sent[2] == "pushing/pushall");           // the connected callback's own
    CHECK(h.sent[3] == "info/get_version");
    CHECK(h.up_ms == 1240); // up at 1241, selected at 1
    CHECK(h.up_held == 3); // get_version, extrusion_cali_get, ledctrl
}

TEST_CASE("[FirstRequest] LAN printer: LAN session up first, then the cloud session", "[FirstRequest]")
{
    GateHost h;
    h.lan_up_at   = 1241;
    h.cloud_up_at = 1300;
    h.early_requests();
    h.lan_event(1241);
    h.cloud_event(1300);

    CHECK(h.refused == 0);
    CHECK(h.not_sent.empty());
    REQUIRE(h.sent.size() == 4); // nothing sent twice
    CHECK(h.sent[0] == "system/ledctrl");
    CHECK(h.sent[1] == "print/extrusion_cali_get");
    CHECK(h.up_held == 3);
}

TEST_CASE("[FirstRequest] cloud printer: held commands go out when the cloud session is up, in either order", "[FirstRequest]")
{
    for (int order = 0; order < 2; ++order) {
        DYNAMIC_SECTION("order " << order) {
            GateHost h;
            h.lan_printer = false;
            h.cloud_up_at = 1200;
            h.early_requests();
            if (order == 0) { h.lan_event(1150); h.cloud_event(1200); }
            else            { h.cloud_event(1200); h.lan_event(1250); }
            CHECK(h.refused == 0);
            CHECK(h.not_sent.empty());
            REQUIRE(h.sent.size() == 2);
            CHECK(h.sent[0] == "system/ledctrl");
            CHECK(h.sent[1] == "print/extrusion_cali_get");
            CHECK(h.up_held == 3);
        }
    }
}

TEST_CASE("[FirstRequest] held commands are reported not sent with the real reason", "[FirstRequest]")
{
    SECTION("the dial failed") {
        GateHost h;
        h.early_requests();
        h.report(h.gate.session_down(), 1300);
        REQUIRE(h.not_sent.size() == 2);
        CHECK(h.not_sent[0] == "system/ledctrl: the session did not come up");
    }
    SECTION("the hold ran out") {
        GateHost h; // the plug-in never reports
        h.early_requests();
        h.publish(HOLD_MAX_MS + 5, "info/get_version", true);
        REQUIRE(h.not_sent.size() == 2);
        CHECK(h.not_sent[1] == "print/extrusion_cali_get: the session did not come up within 15 s");
    }
    SECTION("a new selection") {
        GateHost h;
        h.early_requests();
        h.report(h.gate.select(1300, false), 1300);
        REQUIRE(h.not_sent.size() == 2);
        CHECK(h.not_sent[0] == "system/ledctrl: a new selection replaced the session they were waiting for");
    }
}

// ---- dials that no selection announced, and a refused first dial ----
//
// Hand test on the build with #436 (log 2026-10-11-19-41-39): the printer restored from the last
// session was dialled when the Device tab opened; 117 ms later the plug-in reported the dial
// failed (the printer's broker refused it while the watch session the dial had just replaced was
// still closing). That ended the hold, so get_version and four extrusion_cali_get polls went to a
// plug-in with no session (code -1) until the printer was dialled again 2.7 s later.

TEST_CASE("[FirstRequest] a dial started without a selection holds too", "[FirstRequest]")
{
    // MachineObject::connect arms the gate when nothing did (dial_started).
    GateHost h;
    h.lan_up_at = 1400;
    h.report(h.gate.dial_started(1), 1);
    h.status_request(100, "info/get_version");
    h.publish(300, "print/extrusion_cali_get", true);
    CHECK(h.refused == 0);
    // A second dial_started while dialling (the selection's own connect) changes nothing.
    CHECK(h.gate.dial_started(350).commands.empty());
    CHECK(h.gate.queued() == 1);
    h.lan_event(1400);
    CHECK(h.refused == 0);
    CHECK(h.not_sent.empty());
    REQUIRE(h.sent.size() == 3);
    CHECK(h.sent[0] == "print/extrusion_cali_get");
}

TEST_CASE("[FirstRequest] a refused first dial is dialled once more, still holding", "[FirstRequest]")
{
    GateHost h;
    h.lan_up_at = 3000;
    h.gate.select(1, false);
    REQUIRE(h.gate.dial_failed(117));          // first refusal: dial again, keep holding
    h.status_request(125, "info/get_version");
    h.publish(304, "print/extrusion_cali_get", true);
    h.publish(587, "print/extrusion_cali_get", true);
    h.publish(1090, "print/extrusion_cali_get", true);
    h.publish(1200, "system/ledctrl", false);
    CHECK(h.gate.dial_started(1617).commands.empty()); // the re-dial keeps what is held
    h.publish(2100, "print/extrusion_cali_get", true);
    CHECK(h.refused == 0);
    h.lan_event(3000);
    CHECK(h.refused == 0);
    CHECK(h.not_sent.empty());
    REQUIRE(h.sent.size() == 4);
    CHECK(h.sent[0] == "system/ledctrl");
    CHECK(h.sent[1] == "print/extrusion_cali_get");
    CHECK(h.sent[2] == "pushing/pushall");
    CHECK(h.sent[3] == "info/get_version");
}

TEST_CASE("[FirstRequest] a second refusal reports the held commands not sent", "[FirstRequest]")
{
    GateHost h;
    h.early_requests();
    REQUIRE(h.gate.dial_failed(200));
    CHECK_FALSE(h.gate.dial_failed(2000)); // the one re-dial was refused too
    h.report(h.gate.session_down(), 2000);
    REQUIRE(h.not_sent.size() == 2);
    CHECK(h.not_sent[0] == "system/ledctrl: the session did not come up");
    // A new selection gets its own re-dial.
    h.gate.select(5000, false);
    CHECK(h.gate.dial_failed(5100));
}

TEST_CASE("[FirstRequest] an established session that drops is not re-dialled by the gate", "[FirstRequest]")
{
    GateHost h;
    h.lan_up_at = 500;
    h.gate.select(1, false);
    h.lan_event(500);
    CHECK_FALSE(h.gate.dial_failed(60000)); // not dialling: the reconnect ladder owns this
}

// Re-selecting the LAN printer whose session is already up (DeviceManager::set_selected_machine's
// keep_lan_session path, PR #444): connect_printer is idempotent and the plug-in reports nothing
// back, so no session-up event follows. A hold armed for that connect would never be ended by one.
TEST_CASE("[FirstRequest] re-selecting over a live LAN session holds nothing", "[FirstRequest]")
{
    GateHost h;
    h.lan_up_at = 500;
    h.gate.select(1, false);
    h.lan_event(500); // the first selection's session is up
    h.sent.clear();

    SECTION("the bug: a plain dial_started arms a hold no event ends") {
        h.gate.dial_started(30000); // what connect() did before it knew the session was up
        CHECK(h.gate.dialling(30000));
        h.publish(30100, "system/ledctrl", false);
        h.publish(46000, "print/extrusion_cali_get", true); // the hold ran out: the first is dropped
        CHECK(h.sent.size() == 1);
        REQUIRE(h.not_sent.size() == 1);
        CHECK(h.not_sent[0] == "system/ledctrl: the session did not come up within 15 s");
    }
    SECTION("dial_on_live_session: commands go straight out") {
        SelectionGate::Up u = h.gate.dial_on_live_session(30000);
        CHECK(u.send.empty());
        CHECK_FALSE(h.gate.dialling(30000));
        h.publish(30100, "system/ledctrl", false);
        h.status_request(30200, "pushing/pushall");
        h.publish(46000, "print/extrusion_cali_get", true);
        CHECK(h.not_sent.empty());
        CHECK(h.refused == 0);
        REQUIRE(h.sent.size() == 3);
        CHECK(h.sent[0] == "system/ledctrl");
        CHECK(h.sent[1] == "pushing/pushall");
        CHECK(h.sent[2] == "print/extrusion_cali_get");
        CHECK(h.gate.queued() == 0);
        CHECK(h.gate.held() == 0);
    }
    SECTION("a later real dial still holds, and the re-dial after a refusal is its own") {
        h.gate.dial_on_live_session(30000);
        h.gate.dial_started(40000); // e.g. the reconnect ladder after a drop
        CHECK(h.gate.dialling(40000));
        CHECK(h.gate.dial_failed(40100));
    }
}

TEST_CASE("[FirstRequest] re-selecting over a live session sends what an earlier dial held", "[FirstRequest]")
{
    // The session's first report arrived (lan_session_up) before its Ok callback, and the user
    // re-selected in between: what that dial held goes out, nothing is reported lost.
    GateHost h;
    h.lan_up_at = 300;
    h.gate.select(1, false);
    h.publish(100, "system/ledctrl", false);
    REQUIRE(h.gate.queued() == 1);
    SelectionGate::Up u = h.gate.dial_on_live_session(400);
    REQUIRE(u.send.size() == 1);
    for (const HeldCommand& c : u.send) h.publish(400, c.key, c.info);
    CHECK(h.sent == std::vector<std::string>{"system/ledctrl"});
    CHECK_FALSE(h.gate.dialling(400));
    // A late Ok for the first dial changes nothing.
    h.lan_event(600);
    CHECK(h.not_sent.empty());
    CHECK(h.refused == 0);
}
