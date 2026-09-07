#pragma once

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r {
namespace GUI {

// The printer event watcher (P4 of docs/superpowers/specs/2026-09-03-phone-mobile-capabilities-research.md).
//
// The slicer instance is the only place where printer state already lives: a Bambu printer's
// MachineObject is fed by the networking plugin's MQTT push, a Snapmaker on the LAN answers
// Moonraker over plain HTTP, and a print host is probed by the same code /api/printers uses. So the
// watcher runs here, on the instance's own one-second GUI heartbeat, and is edge-triggered: it
// remembers what each printer looked like last time and emits an event only where something
// actually changed.
//
// Each event is posted to the hub (POST /hub/event on its loopback control plane), which owns
// delivery: the tray balloon, the ring the phone reads, and - once P5 lands - the relays. Posting
// is fire-and-forget off the GUI thread; with no hub running the event is simply dropped.
namespace RemoteEvents {

// What one printer looked like at one moment, in one vocabulary. Bambu's print_status and
// Klipper's print_stats.state are both normalised into `state` so the transition rule below has
// only one set of words to know about.
struct PrinterState
{
    std::string id, name, kind; // kind: bambu | snapmaker | printhost | connect
    // False when this instance cannot actually see this printer's print state. In LAN mode only
    // the connected Bambu printer reports one; the rest are known by discovery alone and their
    // "idle" is an absence of information, not a fact - no event may ever be invented for them.
    bool        watched { false };
    bool        online { false };
    std::string state; // idle | preparing | printing | paused | finished | failed | cancelled | ""
    std::string raw_state; // what the printer itself said (FINISH, complete, ...), for the text
    std::string job;       // the file / task it is on
    std::string stage;     // the printer's own words for what it is doing (Bambu get_curr_stage())
    int         stage_curr { -1 }; // Bambu stage index; 6 is "Paused due to filament runout"
    std::string error_code, error_text;
};

struct Snapshot
{
    long long                           at { 0 }; // unix ms; the only clock the rule sees
    std::map<std::string, PrinterState> printers;
};

// One event, without the id and time the hub assigns.
struct Event
{
    std::string printer_id, printer_name, printer_kind;
    std::string kind;     // started | finished | failed | cancelled | paused | resumed | runout | error
    std::string severity; // info | warning | error
    std::string title, text, code, job;
    nlohmann::json to_json(long instance_pid) const;
};

// ---- the canonical kind list ----
// Every kind the rule above can emit, in the order a person reads them, and nothing else. The
// hub validates an incoming event against this set and each notification channel's per-kind
// filter is a subset of it, so the list has to be one list. It lives in this header rather than
// in RemoteEvents.cpp because the hub-side modules that filter on it (RemoteNotify, WebPush,
// AppPush) are wx-free and must not link the watcher to ask what the kinds are.
inline const std::vector<std::string>& all_kinds()
{
    static const std::vector<std::string> k { "started", "finished", "failed",  "cancelled",
                                              "paused",  "resumed",  "runout",  "error" };
    return k;
}

inline bool is_kind(const std::string& s)
{
    const std::vector<std::string>& k = all_kinds();
    return std::find(k.begin(), k.end(), s) != k.end();
}

// A channel's per-kind filter, as an allow-list. Empty means every kind: that is what a hub
// upgraded from a build without filters has and what a new channel starts as, so nothing anybody
// already set up begins dropping events the day this lands. Minimum severity is a separate,
// coarser filter and both must allow a kind for it to be delivered.
inline bool kind_allowed(const std::vector<std::string>& filter, const std::string& kind)
{
    if (filter.empty()) return true;
    return std::find(filter.begin(), filter.end(), kind) != filter.end();
}

// The filter spelled out: the kinds that are actually on, always a real list even when the
// filter is the empty "everything".
inline std::vector<std::string> enabled_kinds(const std::vector<std::string>& filter)
{
    if (filter.empty()) return all_kinds();
    std::vector<std::string> out;
    for (const std::string& k : all_kinds())
        if (kind_allowed(filter, k)) out.push_back(k);
    return out;
}

// What a GET hands back next to `kinds`: every kind with a boolean, which is the shape a row of
// checkboxes binds to and saves the page from having to know the list itself.
inline nlohmann::json events_map(const std::vector<std::string>& filter)
{
    nlohmann::json j = nlohmann::json::object();
    for (const std::string& k : all_kinds()) j[k] = kind_allowed(filter, k);
    return j;
}

// Whether a settings body is trying to set the filter at all - so a POST that only changes the
// minimum severity leaves the kinds alone.
inline bool has_kind_filter(const nlohmann::json& in)
{
    if (!in.is_object()) return false;
    return (in.contains("kinds") && in["kinds"].is_array()) || (in.contains("events") && in["events"].is_object());
}

// Read a filter out of a settings body, in either of the two shapes it is written in:
//   "kinds":  ["finished","failed"]        the stored form, an allow-list, replaces the filter
//   "events": {"started": false}           the page's form, a patch over what is already set
// The map is a patch on purpose: a checkbox sends the one box that moved, and sending one box
// must not silently turn the other seven off. A list holding every kind is stored as the empty
// "everything", so ticking the last box goes back to the default instead of leaving a filter
// that has to be widened by hand the day a ninth kind is added.
//
// The two shapes disagree about the empty case, and on purpose. `"kinds": []` has meant
// "everything" since the filter was first stored and hubs in the field are written that way, so
// it still does. A map that turns the last kind off means the opposite - nothing - and there is
// no way to store that; it is refused, because the channel already has an on/off of its own and
// silently reading "none" as "all" is the one outcome nobody would guess.
inline bool read_kinds(const nlohmann::json& in, std::vector<std::string>& inout, std::string& why)
{
    std::vector<std::string> next = enabled_kinds(inout);
    bool                     from_events = false;
    if (in.contains("kinds") && in["kinds"].is_array()) {
        next.clear();
        for (const auto& k : in["kinds"]) {
            if (!k.is_string() || !is_kind(k.get<std::string>())) {
                why = "kinds must name event kinds this hub sends";
                return false;
            }
            const std::string s = k.get<std::string>();
            if (std::find(next.begin(), next.end(), s) == next.end()) next.push_back(s);
        }
    }
    if (in.contains("events") && in["events"].is_object()) {
        from_events = true;
        for (auto it = in["events"].begin(); it != in["events"].end(); ++it) {
            if (!is_kind(it.key()) || !it.value().is_boolean()) {
                why = "events must map event kinds this hub sends to true or false";
                return false;
            }
            const bool on  = it.value().get<bool>();
            auto       pos = std::find(next.begin(), next.end(), it.key());
            if (on && pos == next.end()) next.push_back(it.key());
            if (!on && pos != next.end()) next.erase(pos);
        }
    }
    if (from_events && next.empty()) {
        why = "leave at least one event type on, or turn the whole channel off";
        return false;
    }
    // Stored in the canonical order, and "all of them" stored as the empty list.
    std::vector<std::string> tidy;
    for (const std::string& k : all_kinds())
        if (std::find(next.begin(), next.end(), k) != next.end()) tidy.push_back(k);
    inout = tidy.size() == all_kinds().size() ? std::vector<std::string>() : tidy;
    return true;
}

// The kind a "Send test" should wear: the one the person is most likely to care about among the
// kinds they left on, so the test arrives shaped like the notification it is standing in for. A
// filter with nothing on still tests as "finished" - the button's job is to prove the relay
// works - and the answer says which kinds are on, so an empty filter reads as itself.
inline std::string test_kind(const std::vector<std::string>& filter)
{
    static const char* const prefer[] = { "finished", "failed",  "started", "runout",
                                          "error",    "paused",  "resumed", "cancelled" };
    for (const char* p : prefer)
        if (kind_allowed(filter, p)) return p;
    return "finished";
}

// Everything the watcher carries from one poll to the next.
struct Memory
{
    Snapshot                         last;
    std::map<std::string, long long> last_emit; // "<printer>|<kind>|<code or job>" -> snapshot time
    // The poll at which each printer was seeded: the first snapshot in which the watcher could
    // actually see its print state (watched and online). Until a printer has one, and until a
    // later poll has run, nothing it does can produce an event - the first sight only seeds.
    // A printer that goes offline or unwatched loses its entry and is seeded again on its return.
    std::map<std::string, long long> seen_at;
};

// The transition rule, and the only place an event is decided: the previous memory plus the
// snapshot just taken give the events to send, with `mem` advanced to the new state. It reads no
// clock, touches no network and holds no globals - `now.at` is the only time it knows - so a test
// can drive a whole print through it without a printer. `cooldown_ms` suppresses a repeat of the
// same printer + kind + code (a flapping error, a reconnect that re-announces a start).
std::vector<Event> step(Memory& mem, const Snapshot& now, long long cooldown_ms = 180000);

// ---- the live watcher ----
// Called from RemoteAccess's one-second GUI heartbeat; polls at its own slower rate.
void heartbeat();
void stop();
// This instance's own recent events (the last 50), for GET /api/events?since=.
// The answer also carries `watcher`: when the last poll finished, how long it took, and one entry
// per printer with the poll that seeded it. That is the honest answer to "is this printer being
// watched yet?" - a caller that needs a state change to be reported rather than silently seeded
// waits for the printer to carry a seen_at and for last_poll to have moved past it.
nlohmann::json recent(int since);

// Test hook (the SNORCA_DEBUG_ROUTES back door): run `step` over snapshots handed in as JSON and
// report what each one produced. This is how the transition rule is covered without hardware.
nlohmann::json replay(const nlohmann::json& in);

} // namespace RemoteEvents
} // namespace GUI
} // namespace Slic3r
