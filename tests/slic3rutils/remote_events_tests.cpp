// The printer event watcher's transition rule (RemoteEvents::step).
//
// The rule is pure: previous memory plus one snapshot gives the events, and `now.at` is the only
// clock it sees. So a whole print - including the flaps that used to make it announce itself three
// times - can be driven through it here with no printer, no network and no hub.
//
// What these cases are about: a repeat "started" was arriving several times, long after the real
// start. Two causes were named. The transition rule fired on every prev != printing -> printing
// flip with no memory of the job, so a printer that blipped through idle / unknown / offline
// re-announced the print it was already on; and the job-change rule fired whenever the job name
// changed spelling. Both are covered below, alongside the cases that must still announce.

#include <catch2/catch.hpp>

#include "slic3r/GUI/LanReconnectLadder.hpp"
#include "slic3r/GUI/PrintErrorCommands.hpp"
#include "slic3r/GUI/RemoteEvents.hpp"

#include <string>
#include <vector>

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::RemoteEvents;

namespace {

// One printer, watched and online, in one state. The defaults are the normal case; a test that
// wants a flap turns one of them off.
PrinterState pr(const std::string& state, const std::string& job = "bench.gcode", const char* raw = nullptr)
{
    PrinterState p;
    p.id        = "P1";
    p.name      = "X1C";
    p.kind      = "bambu";
    p.watched   = true;
    p.online    = true;
    p.state     = state;
    p.raw_state = raw ? raw : state;
    p.job       = job;
    return p;
}

PrinterState offline(const PrinterState& in)
{
    PrinterState p = in;
    p.online       = false;
    p.state.clear();
    p.raw_state.clear();
    return p;
}

PrinterState unwatched(const PrinterState& in)
{
    PrinterState p = in;
    p.watched      = false;
    p.state.clear();
    p.raw_state.clear();
    return p;
}

// One poll. `at` advances well past the three-minute cooldown by default so that what the cases
// measure is the job memory and not the cooldown - the cooldown alone was never the fix.
struct Driver
{
    Memory    mem;
    long long at { 1000000 };

    std::vector<Event> poll(const PrinterState& p, long long advance_ms = 300000)
    {
        at += advance_ms;
        Snapshot s;
        s.at             = at;
        s.printers[p.id] = p;
        return step(mem, s, 180000);
    }
    std::vector<Event> poll(const PrinterState& p, std::vector<RawChange>& raw, long long advance_ms = 300000)
    {
        at += advance_ms;
        Snapshot s;
        s.at             = at;
        s.printers[p.id] = p;
        raw.clear();
        return step(mem, s, 180000, &raw);
    }
};

int count_of(const std::vector<Event>& v, const char* kind)
{
    int n = 0;
    for (const Event& e : v)
        if (e.kind == kind) ++n;
    return n;
}

} // namespace

TEST_CASE("[RemoteEvents] a print announces its start exactly once", "[RemoteEvents]")
{
    Driver d;
    REQUIRE(d.poll(pr("idle", "")).empty()); // seeding poll
    std::vector<Event> ev = d.poll(pr("printing"));
    REQUIRE(count_of(ev, "started") == 1);
    // Still printing, poll after poll: nothing more.
    for (int i = 0; i < 5; ++i) REQUIRE(d.poll(pr("printing")).empty());
}

TEST_CASE("[RemoteEvents] an idle blip mid-print does not re-announce the start", "[RemoteEvents]")
{
    Driver d;
    d.poll(pr("idle", ""));
    REQUIRE(count_of(d.poll(pr("printing")), "started") == 1);
    // The flap: a raw state the mapper reads as idle, then back to printing. This is the Klipper /
    // Snapmaker / Elegoo raw-state mapping case and it used to announce a second start.
    d.poll(pr("idle", "bench.gcode", "standby"));
    REQUIRE(count_of(d.poll(pr("printing")), "started") == 0);
    // And again, several times over - the cooldown would have expired between each of these.
    for (int i = 0; i < 3; ++i) {
        d.poll(pr("idle", "bench.gcode", "standby"));
        REQUIRE(count_of(d.poll(pr("printing")), "started") == 0);
    }
}

TEST_CASE("[RemoteEvents] going offline and coming back mid-print does not re-announce", "[RemoteEvents]")
{
    Driver d;
    d.poll(pr("idle", ""));
    REQUIRE(count_of(d.poll(pr("printing")), "started") == 1);
    // A brief offline reset: the watcher loses sight of the printer, then sees it printing again.
    // The first watched poll only seeds, the next one used to see prev = "" -> "printing".
    d.poll(offline(pr("printing")));
    d.poll(pr("printing")); // re-seed
    REQUIRE(count_of(d.poll(pr("printing")), "started") == 0);
}

TEST_CASE("[RemoteEvents] an unwatched gap mid-print does not re-announce", "[RemoteEvents]")
{
    Driver d;
    d.poll(pr("idle", ""));
    REQUIRE(count_of(d.poll(pr("printing")), "started") == 1);
    // The LAN case: the session moved to another printer, so this one is listed but not watched.
    for (int i = 0; i < 3; ++i) d.poll(unwatched(pr("printing")));
    d.poll(pr("printing")); // re-seed on its turn coming round
    REQUIRE(count_of(d.poll(pr("printing")), "started") == 0);
}

TEST_CASE("[RemoteEvents] two spellings of one file are one print", "[RemoteEvents]")
{
    Driver d;
    d.poll(pr("idle", ""));
    REQUIRE(count_of(d.poll(pr("printing", "bench.gcode.3mf")), "started") == 1);
    // Every spelling the same job has been seen wearing: a leading path, the extension dropped,
    // a different case, trailing space. None of them is a new print.
    REQUIRE(count_of(d.poll(pr("printing", "/cache/bench.gcode.3mf")), "started") == 0);
    REQUIRE(count_of(d.poll(pr("printing", "bench")), "started") == 0);
    REQUIRE(count_of(d.poll(pr("printing", "BENCH.GCODE")), "started") == 0);
    REQUIRE(count_of(d.poll(pr("printing", "bench.3mf ")), "started") == 0);
}

TEST_CASE("[RemoteEvents] a genuinely different job does announce", "[RemoteEvents]")
{
    Driver d;
    d.poll(pr("idle", ""));
    REQUIRE(count_of(d.poll(pr("printing", "bench.gcode")), "started") == 1);
    REQUIRE(count_of(d.poll(pr("printing", "calibration_cube.gcode")), "started") == 1);
}

TEST_CASE("[RemoteEvents] the same file printed again after it finished announces again", "[RemoteEvents]")
{
    Driver d;
    d.poll(pr("idle", ""));
    REQUIRE(count_of(d.poll(pr("printing")), "started") == 1);
    REQUIRE(count_of(d.poll(pr("finished")), "finished") == 1);
    d.poll(pr("idle", ""));
    // A second run of the same file is a second print and must be announced.
    REQUIRE(count_of(d.poll(pr("printing")), "started") == 1);
}

TEST_CASE("[RemoteEvents] a cancelled print reopens the job, a paused one does not", "[RemoteEvents]")
{
    SECTION("cancelled")
    {
        Driver d;
        d.poll(pr("idle", ""));
        REQUIRE(count_of(d.poll(pr("printing")), "started") == 1);
        REQUIRE(count_of(d.poll(pr("cancelled")), "cancelled") == 1);
        d.poll(pr("idle", ""));
        REQUIRE(count_of(d.poll(pr("printing")), "started") == 1);
    }
    SECTION("paused and resumed")
    {
        Driver d;
        d.poll(pr("idle", ""));
        REQUIRE(count_of(d.poll(pr("printing")), "started") == 1);
        REQUIRE(count_of(d.poll(pr("paused")), "paused") == 1);
        std::vector<Event> back = d.poll(pr("printing"));
        // Resuming is a "resumed", never a second "started".
        REQUIRE(count_of(back, "resumed") == 1);
        REQUIRE(count_of(back, "started") == 0);
    }
}

TEST_CASE("[RemoteEvents] a failure reopens the job", "[RemoteEvents]")
{
    Driver d;
    d.poll(pr("idle", ""));
    REQUIRE(count_of(d.poll(pr("printing")), "started") == 1);
    REQUIRE(count_of(d.poll(pr("failed")), "failed") == 1);
    d.poll(pr("idle", ""));
    REQUIRE(count_of(d.poll(pr("printing")), "started") == 1);
}

TEST_CASE("[RemoteEvents] the watcher joining a print already running stays silent", "[RemoteEvents]")
{
    Driver d;
    // First sight is already printing: the seeding poll, and nothing may be announced for it.
    REQUIRE(d.poll(pr("printing")).empty());
    REQUIRE(count_of(d.poll(pr("printing")), "started") == 0);
    // Even after it flaps.
    d.poll(offline(pr("printing")));
    d.poll(pr("printing"));
    REQUIRE(count_of(d.poll(pr("printing")), "started") == 0);
}

TEST_CASE("[RemoteEvents] raw-state changes are reported for the log", "[RemoteEvents]")
{
    Driver                 d;
    std::vector<RawChange> raw;
    d.poll(pr("idle", "", "IDLE"), raw); // first sight: seeds, reports no change
    REQUIRE(raw.empty());
    d.poll(pr("printing", "bench.gcode", "RUNNING"), raw);
    REQUIRE(raw.size() == 1);
    CHECK(raw[0].from == "IDLE");
    CHECK(raw[0].to == "RUNNING");
    CHECK(raw[0].was_visible);
    CHECK(raw[0].visible);
    // Losing sight of the printer is itself a raw change, and marked as not visible, so the log
    // says whether the printer changed its mind or the watcher simply stopped being able to see it.
    d.poll(offline(pr("printing")), raw);
    REQUIRE(raw.size() == 1);
    CHECK(raw[0].to == "<none>");
    CHECK_FALSE(raw[0].visible);
}

TEST_CASE("[RemoteEvents] job memory survives a printer going offline but not its removal", "[RemoteEvents]")
{
    Driver d;
    d.poll(pr("idle", ""));
    REQUIRE(count_of(d.poll(pr("printing")), "started") == 1);
    d.poll(offline(pr("printing")));
    REQUIRE(d.mem.jobs.count("P1") == 1);
    // The printer leaves the snapshot entirely (removed from the Device tab): now the memory goes.
    Snapshot empty;
    empty.at = d.at += 300000;
    step(d.mem, empty, 180000);
    REQUIRE(d.mem.jobs.count("P1") == 0);
}

TEST_CASE("[RemoteEvents] two printers keep their own job memory", "[RemoteEvents]")
{
    Memory    mem;
    long long at   = 1000000;
    auto      poll = [&mem, &at](const PrinterState& a, const PrinterState& b) {
        at += 300000;
        Snapshot s;
        s.at             = at;
        s.printers[a.id] = a;
        s.printers[b.id] = b;
        return step(mem, s, 180000);
    };
    PrinterState p1 = pr("idle", "");
    PrinterState p2 = pr("idle", "");
    p2.id           = "P2";
    p2.name         = "P1S";
    poll(p1, p2);

    PrinterState p1p = pr("printing", "a.gcode");
    REQUIRE(count_of(poll(p1p, p2), "started") == 1);

    // P2 starting its own print is its own event; P1 flapping at the same time is still not.
    PrinterState p2p = pr("printing", "b.gcode");
    p2p.id           = "P2";
    p2p.name         = "P1S";
    std::vector<Event> ev = poll(pr("idle", "a.gcode", "standby"), p2p);
    REQUIRE(count_of(ev, "started") == 1);
    REQUIRE(ev[0].printer_id == "P2");
    REQUIRE(count_of(poll(p1p, p2p), "started") == 0);
}

// The ladder the LAN reconnect tick walks. It is the real function off DeviceManager, not a copy:
// the cadence a log is meant to show (15 s, 30 s, 60 s, 60 s...) is this and nothing else.
TEST_CASE("[RemoteEvents] the LAN reconnect backoff is 15s, 30s, 60s and then capped", "[RemoteEvents]")
{
    namespace L = Slic3r::LanReconnectLadder;
    CHECK(L::backoff_ms(1) == 15000);
    CHECK(L::backoff_ms(2) == 30000);
    CHECK(L::backoff_ms(3) == 60000);
    CHECK(L::backoff_ms(4) == 60000);
    CHECK(L::backoff_ms(50) == 60000);
    // The grace window before the first retry: a LAN session is "gone" once no push has landed for
    // DISCONNECT_TIMEOUT, and the tick waits that long again before touching it.
    CHECK(L::GRACE_MS == 15000);
    CHECK(L::MAX_MS == 60000);
}

// The notification body, shared by ntfy, Web Push and the native-app push plane.
//
// The error text now carries its own code (HMSQuery::format_error), so the old unconditional
// `body += " (" + code + ")"` printed it twice: "... (0C00 0100 0002 0015). (0C00010000020015)"
// on the lock screen. The code is still appended when the text does not name it, because an event
// from a relayed hub or a Klipper printer may carry a code and a sentence that never mentions it.
TEST_CASE("[RemoteEvents] a push body names the error code exactly once", "[RemoteEvents]")
{
    // Already named in the grouped spelling the text was built with: not repeated.
    CHECK(notification_body("X1C: Nozzle Camera is malfunctioning. (0C00 0100 0002 0015)", "0C00010000020015") ==
          "X1C: Nozzle Camera is malfunctioning. (0C00 0100 0002 0015)");

    // Already named in the raw spelling, as a relayed body may carry it: also not repeated.
    CHECK(notification_body("X1C reported error 0C00010000020015.", "0C00010000020015") ==
          "X1C reported error 0C00010000020015.");

    // Not named at all - the code is added, grouped, so the owner has something to look up.
    CHECK(notification_body("X1C: the bed is too cold", "05004046") == "X1C: the bed is too cold (0500 4046)");

    // No code: nothing is appended, and an empty body stays empty rather than becoming " ()".
    CHECK(notification_body("X1C finished the print", "") == "X1C finished the print");
    CHECK(notification_body("", "") == "");
}


// ---- the printer-error event's action buttons (phase 2) ----
//
// The app's notification used to carry a sentence and nothing else: "X1C: the toolhead camera is
// not working properly". Tapping it opened the app, which opened the page, which is where the
// buttons were. These cases pin the payload that lets the app draw them on the card itself - and,
// more importantly, pin that adding them changed nothing about the text, the title or the
// severity, because every existing consumer reads those.
TEST_CASE("[RemoteEvents] an error event carries the actions the app can offer", "[RemoteEvents]")
{
    PrinterState p = pr("printing");
    p.error_code   = "05008051";
    p.error_text   = "the filament is tangled (0500 8051)";
    p.job_id       = "42";
    // What RemoteEvents fills in from the printer: the same resolved set the status JSON carries.
    bool fallback = false;
    for (const PrintErrorRemoteAction& a :
         describe_print_error_actions(resolve_print_error_actions({23, 3}, fallback), true)) {
        PrintErrorEventAction e;
        e.id           = a.id;
        e.verb         = a.verb;
        e.label        = a.label;
        e.needs_job_id = a.needs_job_id;
        e.remote_safe  = a.remote_safe;
        p.error_actions.push_back(e);
    }

    Driver d;
    REQUIRE(d.poll(pr("printing")).empty()); // seeding poll, no error yet
    std::vector<Event> ev = d.poll(p);

    REQUIRE(count_of(ev, "error") == 1);
    const Event* e = nullptr;
    for (const Event& x : ev)
        if (x.kind == "error") e = &x;
    REQUIRE(e);

    SECTION("the actions reach the event, and the text is untouched by them")
    {
        REQUIRE(e->actions.size() == 2);
        REQUIRE(e->actions[0].verb == "idle_ignore_error");
        REQUIRE(e->actions[1].verb == "resume_error");
        REQUIRE(e->job_id == "42");
        // Unchanged: this is what every channel already renders.
        // The rule's own wording, unchanged: "<printer>: <what the printer said>".
        REQUIRE(e->text == "X1C: the filament is tangled (0500 8051)");
        REQUIRE(e->severity == "error");
        REQUIRE(e->code == "05008051");
    }

    SECTION("the JSON carries them as ids, verbs and labels")
    {
        const nlohmann::json j = e->to_json(0);
        REQUIRE(j["actions"].is_array());
        REQUIRE(j["actions"].size() == 2);
        REQUIRE(j["actions"][1]["id"] == PrintErrorAction::RESUME_PRINTING_DEFECTS);
        REQUIRE(j["actions"][1]["verb"] == "resume_error");
        REQUIRE(j["actions"][1]["needs_job_id"] == true);
        REQUIRE(j["actions"][1]["remote_safe"] == true);
        REQUIRE_FALSE(j["actions"][1]["label"].get<std::string>().empty());
        REQUIRE(j["job_id"] == "42");
        // And the fields that were there before are exactly as they were.
        REQUIRE(j["kind"] == "error");
        REQUIRE(j["code"] == "05008051");
        REQUIRE(j["text"] == "X1C: the filament is tangled (0500 8051)");
    }
}

TEST_CASE("[RemoteEvents] an error with no actions carries no actions field at all", "[RemoteEvents]")
{
    // A Klipper printer's error, an HMS item, a relayed hub's event: none of them has a button
    // set. The payload must then be byte-for-byte what a consumer written before this field saw,
    // rather than an empty array it has to learn to ignore.
    PrinterState p = pr("printing");
    p.error_code   = "05008051";
    p.error_text   = "something went wrong";

    Driver d;
    REQUIRE(d.poll(pr("printing")).empty());
    std::vector<Event> ev = d.poll(p);
    REQUIRE(count_of(ev, "error") == 1);

    for (const Event& e : ev) {
        if (e.kind != "error") continue;
        const nlohmann::json j = e.to_json(0);
        REQUIRE_FALSE(j.contains("actions"));
        REQUIRE_FALSE(j.contains("job_id"));
        REQUIRE(j["code"] == "05008051");
    }
}
