#pragma once

// Hub-driven Live Activity updates (tests/research_live_activity_lag.md, plan B): the rules and the
// payloads, pure and wx-free. AppPush.cpp owns the devices, the tokens and the sending; this file
// decides *when* a printer's activity should be told something and *what* it may be told.
//
// Opt-in. Nothing here runs for a phone that did not switch "Lock Screen updates through this PC"
// on in the app (`live_activity.enabled` on /push/device), and the hub page can switch the whole
// thing off for every phone (AppPush option `live_activity`).
//
// What a Live Activity push may carry (owner decision, 2026-10-06). An ActivityKit push is decoded
// by the system with no Notification Service Extension in between, so it is plaintext to Apple
// and - in hosted mode - to the EdgeSlicer push service. It therefore carries only:
//   content-state: state, percent, layer, totalLayers, eta, barStart, stale, asOf
//   attributes (push-to-start only): printerId = the opaque per-hub id (PushIds::thread_id, the
//     same keyed HMAC the alerts' thread-id already is), printerName "" and jobKey ""
//   alert (push-to-start only): fixed text naming nothing
// Never a serial, a printer name, an address, a file name or account data. The app looks the
// printer's name up on the phone from the opaque id (/summary's `push_id`).
// tests/slic3rutils/live_activity_push_tests.cpp asserts it.
//
// The rules (per phone and printer, evaluated after every hub poll of the printers, ~10 s):
//   priority 10  a state change (printing / paused / attention / finished / failed / cancelled),
//                an error code that changed, and the end; never two within 10 s for one activity
//                (the later state simply goes out on the next poll)
//   priority 5   a +1 % step, or the ETA moving 60 s or more, at most every 30 s; a stale flag
//                change; and a heartbeat every 3 min when nothing changed
//   stale-date   now + 5 min on every push: if the PC sleeps or the hub stops, the activity says
//                "out of date" within 5 minutes instead of 15 past the ETA
//   restart      at 7 h 20 m the activity is ended and, with a push-to-start token (iOS 17.2+),
//                started again for the same print - Apple ends any activity at 8 h
//   start        a print the phone has no activity for (it was locked, the app closed) is started
//                by push-to-start once it has been seen printing for 30 s, unless the person
//                dismissed that print's activity
// Apple counts priority-10 Live Activity pushes against an hourly budget and not priority 5, so
// routine progress is 5. NSSupportsLiveActivitiesFrequentUpdates is not needed at these rates.

#include <nlohmann/json_fwd.hpp>

#include <string>

namespace Slic3r {
namespace GUI {
namespace LiveActivity {

// Swift's default JSONDecoder reads a `Date` as seconds since 2001-01-01 (Apple's reference date),
// which is what ActivityKit uses to decode a pushed content-state.
static const long long APPLE_REFERENCE_UNIX = 978307200;

enum class Phase { Preparing, Printing, Paused, Attention, Finished, Failed, Cancelled, Idle };

const char* phase_name(Phase p); // the app's PrintPhase raw values: "printing", "paused", ...
bool        is_active(Phase p);
bool        is_terminal(Phase p);
// The printer's own status word, as the app reads it (ProgressSession.swift PrintPhase.base).
Phase base_phase(const std::string& state, bool printing);
// base_phase, with an error on a running or paused print raised to Attention.
Phase classify(const std::string& state, bool printing, bool has_error);

// One printer as the hub's cache has it (HubServer::printers_json rows).
struct Row
{
    std::string id;
    std::string state;
    bool        printing { false };
    bool        online { true };
    bool        stale { false };
    int         percent { -1 };      // -1: not reported
    int         remaining_s { -1 };  // -1: not reported
    int         layer { -1 };
    int         total_layers { -1 };
    std::string error_code;          // "" when there is none
    std::string job;                 // for the job key only; never sent anywhere
    long long   age_ms { 0 };        // how long ago the hub read it
};

// A printers_json row. Unknown or missing fields keep the defaults above.
Row row_from_json(const nlohmann::json& row);

// What the activity shows: the app's ProgressContent, field for field.
struct Content
{
    Phase     state { Phase::Idle };
    int       percent { -1 };
    int       layer { -1 };
    int       total_layers { -1 };
    long long eta_s { 0 };       // unix seconds; 0 = none
    long long bar_start_s { 0 }; // unix seconds; 0 = none
    bool      stale { false };
    long long as_of_s { 0 };     // when the numbers were read (unix seconds)

    // Everything but as_of_s.
    bool   same_reading(const Content& o) const;
    double relevance() const;    // ProgressContent.relevance
};

// The content for a row read `now_ms` (ProgressContent.make, with the row's age).
Content content_for(const Row& r, long long now_ms);

// The app's job key for a job name: the first 8 bytes of its SHA-256, hex. "" for no job.
std::string job_key(const std::string& job);

struct Rules
{
    long long p10_gap_ms       = 10 * 1000;
    long long p5_gap_ms        = 30 * 1000;
    long long eta_move_s       = 60;
    long long heartbeat_ms     = 3 * 60 * 1000;
    long long stale_after_s    = 5 * 60;
    long long restart_after_ms = (7 * 60 + 20) * 60 * 1000LL;
    long long start_grace_ms   = 30 * 1000;
    long long final_dismiss_s  = 10 * 60;
    // apns-expiration: a late progress push is worse than none; a state change is still news.
    int       ttl_p5_s         = 60;
    int       ttl_p10_s        = 10 * 60;
};

// What the hub remembers about one phone's activity for one printer.
struct Track
{
    // The activity's push token (hex) from the app, or "" when there is none. For an Android
    // phone, which has no tokens, the device row stands in: `virtual_activity` is true.
    std::string activity_token;
    bool        virtual_activity { false };
    std::string job_key;              // the job the activity is about
    long long   started_ms { 0 };     // when the activity began
    bool        has_sent { false };   // the hub has told it something since it was registered
    Content     last;                 // what it was last told
    std::string last_error_code;
    long long   last_sent_ms { 0 };
    long long   last_p10_ms { 0 };
    long long   active_since_ms { 0 };  // first poll that saw this print active (push-to-start grace)
    std::string start_sent_job;       // a push-to-start went out for this job...
    long long   start_sent_ms { 0 };  // ...at this time
    std::string dismissed_job;        // the person removed this job's activity: no push-to-start for it
};

enum class Action { None, Update, End, Start, EndAndStart };

struct Decision
{
    Action      action { Action::None };
    int         priority { 5 };
    Content     content;
    long long   dismissal_s { 0 };    // End: when the ended activity leaves the Lock Screen (unix s)
    const char* why { "" };
};

// The decision for one printer's activity on one phone after a poll.
//   row             the printer's row, or nullptr when the hub no longer lists it
//   can_start       the phone gave a push-to-start token and the hub may use it
Decision decide(const Track& t, const Row* row, bool can_start, long long now_ms, const Rules& rules = Rules());

// The track after a decision was carried out successfully at `now_ms`.
Track apply(const Track& t, const Decision& d, const Row* row, long long now_ms);

// ------------------------------------------------------------------ the payloads ----

// The content-state ActivityKit decodes into the app's ProgressContent: its Swift property names,
// absent optionals left out, dates in seconds since 2001-01-01.
nlohmann::json content_state(const Content& c);

// The `aps` objects (without the outer {"aps": ...}): update, end and push-to-start.
nlohmann::json aps_update(const Content& c, long long now_s, const Rules& rules = Rules());
nlohmann::json aps_end(const Content& c, long long now_s, long long dismissal_s);
nlohmann::json aps_start(const Content& c, const std::string& opaque_printer_id, long long now_s, const Rules& rules = Rules());

// The keys a Live Activity payload may ever contain, for the tests and the forwarder's own check.
bool content_state_key_allowed(const std::string& key);
bool attribute_key_allowed(const std::string& key);

// Android: the encrypted progress update's plaintext ("kind":"progress"). It is end-to-end
// encrypted like an alert, so it carries the printer's id and a /summary-shaped row the app's
// mapper already reads; nothing in it is visible to Google or the push service.
nlohmann::json progress_plaintext(const nlohmann::json& summary_row, long long now_ms);

const char* action_name(Action a);

} // namespace LiveActivity
} // namespace GUI
} // namespace Slic3r
