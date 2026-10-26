#pragma once

// Cloud-bound Bambu printers on the hub's rows: which ones the slicer listens to, what a row says
// when a printer has not reported, and when that row counts as stale.
//
// What went wrong (2026-10-25, the owner's Devices screen): a Bambu printer bound to the account
// only sends its status (push_status) to a client that has subscribed to its report topic, and the
// network plug-in subscribes to two things only - the printer selected on the Device tab, and the
// list the Multi-device pages hand it. Every other cloud printer was listed off the cloud's printer
// list (DeviceManager::parse_user_print_info) and never heard from, so:
//   * MachineObject::print_status stayed "" (it is only set from a push's gcode_state), but
//     is_in_printing() falls back to the cloud list's task_status ("RUNNING"), so the hub's row said
//     printing: true with an empty state, 0 % and no layers - the H2D and H2C rows;
//   * full_report_tick's pushall went out and its answer came back on a topic nobody listened to;
//   * the hub's age_s is the age of its poll of the slicer window, not of the printer's report, so
//     such a row was served as fresh every ten seconds.
//
// The fix has three parts, and the decisions behind each live here, wx-free, so a unit test can
// check them without the GUI:
//   * subscribe_due: the heartbeat subscribes every online cloud printer (DeviceManager::
//     cloud_watch_tick), and re-subscribes one that has gone quiet, at a pace a P1 can take;
//   * row_status: a row's status word, `printing` and where they came from, always consistent;
//   * report_stale: whether the printer's last report is too old to show as current.

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace Slic3r {
namespace BambuCloudWatch {

// MachineObject::is_in_printing_status, repeated here so the row and the predicate cannot disagree.
inline bool is_printing_status(const std::string& s)
{
    return s == "PAUSE" || s == "RUNNING" || s == "SLICING" || s == "PREPARE";
}

// Where a row's status word came from.
//   "report":     the printer's own push (gcode_state) - the only source with progress behind it;
//   "cloud_list": the account's printer list (task_status), which says what the printer was doing
//                 when the list was fetched and nothing else: no percent, no layers, no time left;
//   "none":       neither has said anything.
struct RowStatus
{
    std::string status;
    bool        printing { false };
    std::string source;
};

inline RowStatus row_status(const std::string& print_status, const std::string& cloud_list_status)
{
    RowStatus r;
    if (!print_status.empty()) {
        r.status = print_status;
        r.source = "report";
    } else if (!cloud_list_status.empty()) {
        r.status = cloud_list_status;
        r.source = "cloud_list";
    } else {
        r.source = "none";
    }
    r.printing = is_printing_status(r.status);
    return r;
}

// How old a printer's last report may be before the row stops passing for current.
// While it prints, a Bambu printer reports at least every few seconds (layer, percent, time left
// and the temperatures all move), so two minutes of silence means the reports stopped. An idle P1 /
// A1 only reports what changes and can be quiet much longer without anything being wrong.
constexpr long long PRINTING_STALE_MS = 2 * 60 * 1000;
constexpr long long IDLE_STALE_MS     = 15 * 60 * 1000;

// report_age_ms < 0: no report has ever been parsed for this printer.
inline bool report_stale(long long report_age_ms, bool printing)
{
    if (report_age_ms < 0) return true;
    return report_age_ms > (printing ? PRINTING_STALE_MS : IDLE_STALE_MS);
}

// The hub's age_s / stale for a Bambu row (HubServer::printers_json). The hub's own age is how long
// ago it polled the slicer window; the window's `report_age_s` is how old the printer's last report
// was at that moment (-1: none yet). The row is as old as the two together, and stale when either
// part says so. A row from a window that predates report_age_s keeps the poll's answer.
struct Freshness
{
    long long age_ms { 0 };
    bool      stale { false };
};

inline Freshness row_freshness(long long poll_age_ms, long long poll_stale_ms, bool has_report_age, long long report_age_s,
                               bool printing)
{
    Freshness f;
    f.age_ms = poll_age_ms;
    f.stale  = poll_age_ms > poll_stale_ms;
    if (!has_report_age) return f;
    if (report_age_s < 0) {
        f.stale = true;
        return f;
    }
    const long long total = report_age_s * 1000 + poll_age_ms;
    f.age_ms              = std::max(f.age_ms, total);
    if (report_stale(total, printing)) f.stale = true;
    return f;
}

// ---- the subscription pace ----------------------------------------------------------------------

// A subscribed printer that has said nothing for this long is subscribed again (which also asks it
// for a full report), at most once per RETRY_MS. Bambu asks clients not to request a P1's full
// report more than about once every five minutes, and a P1 left idle is quiet for minutes at a time
// legitimately, hence the slow retry.
constexpr long long SILENT_AFTER_MS = 2 * 60 * 1000;
constexpr long long RETRY_MS        = 5 * 60 * 1000;
// One account's printers, not a farm: past this many the plug-in is handed the first ones by id
// (the Multi-device pages exist for a farm, and pick their own set).
constexpr size_t MAX_PRINTERS = 16;

struct Candidate
{
    std::string id;
    long long   silence_ms { -1 }; // since the last report; < 0 = never reported
};

// id -> when it was last handed to the plug-in (ms, the caller's clock).
using Asked = std::map<std::string, long long>;

// Which candidates to hand to add_subscribe now. A printer new to the set goes at once; one already
// handed over goes again only while it is silent (or has never reported) and RETRY_MS has passed.
// Printers that are no longer candidates (offline, LAN mode, unbound) are forgotten, so one that
// comes back is subscribed at once.
inline std::vector<std::string> subscribe_due(Asked& asked, std::vector<Candidate> cands, long long now_ms)
{
    std::sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) { return a.id < b.id; });
    cands.erase(std::unique(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) { return a.id == b.id; }),
                cands.end());
    cands.erase(std::remove_if(cands.begin(), cands.end(), [](const Candidate& c) { return c.id.empty(); }), cands.end());
    if (cands.size() > MAX_PRINTERS) cands.resize(MAX_PRINTERS);

    for (auto it = asked.begin(); it != asked.end();) {
        const bool still = std::any_of(cands.begin(), cands.end(), [&](const Candidate& c) { return c.id == it->first; });
        it = still ? std::next(it) : asked.erase(it);
    }

    std::vector<std::string> due;
    for (const Candidate& c : cands) {
        auto it = asked.find(c.id);
        if (it == asked.end()) {
            asked[c.id] = now_ms;
            due.push_back(c.id);
            continue;
        }
        const bool silent = c.silence_ms < 0 || c.silence_ms > SILENT_AFTER_MS;
        if (silent && now_ms - it->second >= RETRY_MS) {
            it->second = now_ms;
            due.push_back(c.id);
        }
    }
    return due;
}

} // namespace BambuCloudWatch
} // namespace Slic3r
