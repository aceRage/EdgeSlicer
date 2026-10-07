// Hub-driven Live Activity updates: the rules and the payloads (see LiveActivityPush.hpp).
// Pure and wx-free; AppPush.cpp does the sending.
#include "LiveActivityPush.hpp"

#include <nlohmann/json.hpp>

#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>

namespace Slic3r {
namespace GUI {
namespace LiveActivity {

using json = nlohmann::json;

// Named, not anonymous: the unity build merges this file with its neighbours.
namespace la_impl {

static std::string lower_trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char) s[a])) ++a;
    while (b > a && std::isspace((unsigned char) s[b - 1])) --b;
    std::string out = s.substr(a, b - a);
    for (char& c : out) c = (char) std::tolower((unsigned char) c);
    return out;
}

static std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char) s[a])) ++a;
    while (b > a && std::isspace((unsigned char) s[b - 1])) --b;
    return s.substr(a, b - a);
}

static int int_of(const json& row, const char* key, int fallback)
{
    if (!row.is_object() || !row.contains(key)) return fallback;
    const json& v = row[key];
    if (v.is_number_integer()) return v.get<int>();
    if (v.is_number()) return (int) std::lround(v.get<double>());
    return fallback;
}

static bool bool_of(const json& row, const char* key, bool fallback)
{
    if (!row.is_object() || !row.contains(key) || !row[key].is_boolean()) return fallback;
    return row[key].get<bool>();
}

static std::string str_of(const json& row, const char* key)
{
    if (!row.is_object() || !row.contains(key) || !row[key].is_string()) return std::string();
    return row[key].get<std::string>();
}

// The printer's own status word: print_status first, as /summary and the app read it.
static std::string state_of(const json& row)
{
    if (row.is_object() && row.contains("print_status") && row["print_status"].is_string())
        return row["print_status"].get<std::string>();
    return str_of(row, "status");
}

// HubServer's row_job: a Bambu row has `task`, a print host has the file in `stage`.
static std::string job_of(const json& row)
{
    std::string job = str_of(row, "task");
    if (job.empty()) job = str_of(row, "subtask_name");
    if (job.empty()) job = str_of(row, "stage");
    return job;
}

static std::string error_code_of(const json& row)
{
    if (row.is_object() && row.contains("print_error") && row["print_error"].is_object()) {
        const json& e = row["print_error"];
        if (e.contains("code")) {
            if (e["code"].is_string()) return e["code"].get<std::string>();
            if (e["code"].is_number()) return e["code"].dump();
        }
        return "error";
    }
    const std::string e = trim(str_of(row, "error"));
    return e;
}

static long long floor_s(double s) { return (long long) std::floor(s); }

static long long bar_start(long long eta_s, long long now_s, int percent, int remaining_s)
{
    if (remaining_s < 0) return 0;
    const int p = percent < 0 ? 0 : percent;
    if (p >= 100) return 0;
    if (p <= 0) return now_s;
    const double total = (double) remaining_s / (1.0 - (double) p / 100.0);
    return floor_s((double) eta_s - total);
}

static json ref_date(long long unix_s) { return unix_s - APPLE_REFERENCE_UNIX; }

} // namespace la_impl

using namespace la_impl;

const char* phase_name(Phase p)
{
    switch (p) {
    case Phase::Preparing: return "preparing";
    case Phase::Printing: return "printing";
    case Phase::Paused: return "paused";
    case Phase::Attention: return "attention";
    case Phase::Finished: return "finished";
    case Phase::Failed: return "failed";
    case Phase::Cancelled: return "cancelled";
    case Phase::Idle: return "idle";
    }
    return "idle";
}

bool is_active(Phase p) { return p == Phase::Preparing || p == Phase::Printing || p == Phase::Paused || p == Phase::Attention; }
bool is_terminal(Phase p) { return p == Phase::Finished || p == Phase::Failed || p == Phase::Cancelled; }

Phase base_phase(const std::string& state, bool printing)
{
    const std::string s = lower_trim(state);
    if (s == "running" || s == "printing" || s == "busy") return Phase::Printing;
    if (s == "prepare" || s == "preparing" || s == "slicing") return Phase::Preparing;
    if (s == "pause" || s == "paused" || s == "pausing") return Phase::Paused;
    if (s == "finish" || s == "finished" || s == "complete" || s == "completed") return Phase::Finished;
    if (s == "failed" || s == "error") return Phase::Failed;
    if (s == "cancelled" || s == "canceled" || s == "cancelling" || s == "stopped") return Phase::Cancelled;
    return printing ? Phase::Printing : Phase::Idle;
}

Phase classify(const std::string& state, bool printing, bool has_error)
{
    const Phase p = base_phase(state, printing);
    return has_error && is_active(p) ? Phase::Attention : p;
}

Row row_from_json(const json& row)
{
    Row r;
    if (!row.is_object()) return r;
    r.id           = str_of(row, "id");
    r.state        = state_of(row);
    r.printing     = bool_of(row, "printing", false);
    r.online       = bool_of(row, "online", true);
    r.stale        = bool_of(row, "stale", false);
    r.percent      = int_of(row, "percent", -1);
    r.remaining_s  = int_of(row, "left_time_s", -1);
    r.layer        = int_of(row, "layer", -1);
    r.total_layers = int_of(row, "total_layers", -1);
    r.error_code   = error_code_of(row);
    r.job          = job_of(row);
    const int age  = int_of(row, "age_s", 0);
    r.age_ms       = age > 0 ? (long long) age * 1000 : 0;
    return r;
}

bool Content::same_reading(const Content& o) const
{
    return state == o.state && percent == o.percent && layer == o.layer && total_layers == o.total_layers &&
           eta_s == o.eta_s && bar_start_s == o.bar_start_s && stale == o.stale;
}

double Content::relevance() const
{
    switch (state) {
    case Phase::Attention: return 100;
    case Phase::Paused: return 60;
    case Phase::Printing:
    case Phase::Preparing: return 30 + (double) (percent < 0 ? 0 : percent) / 10;
    default: return 10;
    }
}

Content content_for(const Row& r, long long now_ms)
{
    Content c;
    const Phase base  = base_phase(r.state, r.printing);
    c.state           = classify(r.state, r.printing, !r.error_code.empty());
    const int pct     = r.percent < 0 ? -1 : std::min(100, std::max(0, r.percent));
    const bool counting = base == Phase::Printing || base == Phase::Preparing;
    const long long read_at = (now_ms - std::max(0LL, r.age_ms)) / 1000;
    if (counting && r.remaining_s > 0) c.eta_s = read_at + r.remaining_s;
    c.percent      = c.state == Phase::Finished ? 100 : pct;
    c.layer        = r.layer >= 0 ? r.layer : -1;
    c.total_layers = r.total_layers > 0 ? r.total_layers : -1;
    c.bar_start_s  = c.eta_s ? bar_start(c.eta_s, read_at, pct, r.remaining_s) : 0;
    // Offline and stale are one thing here: what the activity would show is a memory.
    c.stale        = r.stale || !r.online;
    c.as_of_s      = read_at;
    return c;
}

std::string job_key(const std::string& job)
{
    const std::string name = trim(job);
    if (name.empty()) return std::string();
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char*) name.data(), name.size(), digest);
    static const char* hx = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 8; ++i) { out += hx[digest[i] >> 4]; out += hx[digest[i] & 15]; }
    return out;
}

const char* action_name(Action a)
{
    switch (a) {
    case Action::None: return "none";
    case Action::Update: return "update";
    case Action::End: return "end";
    case Action::Start: return "start";
    case Action::EndAndStart: return "end+start";
    }
    return "none";
}

Decision decide(const Track& t, const Row* row, bool can_start, long long now_ms, const Rules& rules)
{
    Decision d;
    const bool has_activity = t.virtual_activity || !t.activity_token.empty();
    const long long now_s   = now_ms / 1000;

    if (!row) {
        // The hub no longer lists the printer (removed, or a Snapmaker card that went away).
        if (has_activity && t.has_sent) {
            d.action = Action::End; d.priority = 10; d.content = t.last; d.dismissal_s = now_s; d.why = "printer gone";
        } else if (!t.virtual_activity && !t.activity_token.empty()) {
            d.action = Action::End; d.priority = 10; d.content = t.last; d.dismissal_s = now_s; d.why = "printer gone";
        }
        return d;
    }

    const Content     c     = content_for(*row, now_ms);
    const std::string job   = job_key(row->job);
    d.content               = c;

    if (!has_activity) {
        // Nothing to update. Start one by push (iOS 17.2+) for a print the phone has none for.
        if (!is_active(c.state) || !can_start) return d;
        if (!job.empty() && t.dismissed_job == job) return d;
        // Already asked for this job; the app registers the new activity's token when iOS hands
        // it over. Asked again only for the 8 h restart (EndAndStart), never in a loop.
        if (t.start_sent_ms > 0 && t.start_sent_job == job) return d;
        if (t.active_since_ms <= 0 || now_ms - t.active_since_ms < rules.start_grace_ms) return d;
        d.action = Action::Start; d.priority = 10; d.why = "print under way with no activity";
        return d;
    }

    // An Android phone hears nothing before its first print state worth telling.
    if (t.virtual_activity && !t.has_sent && !is_active(c.state)) return d;

    const bool new_job = !t.job_key.empty() && !job.empty() && job != t.job_key;
    const bool reprint = t.has_sent && t.last.percent >= 50 && c.percent >= 0 && c.percent <= 5 && is_active(c.state);
    if ((new_job || reprint) && t.has_sent) {
        if (t.virtual_activity) {
            d.action = Action::Update; d.priority = 10; d.why = "new print";
        } else if (can_start && is_active(c.state)) {
            d.action = Action::EndAndStart; d.priority = 10; d.dismissal_s = now_s; d.why = "new print";
        } else {
            d.action = Action::End; d.priority = 10; d.content = t.last; d.dismissal_s = now_s; d.why = "new print";
        }
        return d;
    }

    if (is_terminal(c.state)) {
        d.action      = Action::End;
        d.priority    = 10;
        d.dismissal_s = c.state == Phase::Cancelled ? now_s : now_s + rules.final_dismiss_s;
        d.why         = "print ended";
        return d;
    }
    if (!is_active(c.state)) {
        // Idle with no outcome seen: the print went away between polls.
        d.action = Action::End; d.priority = 10; d.content = t.has_sent ? t.last : c; d.dismissal_s = now_s;
        d.why = "printer idle";
        return d;
    }

    if (!t.virtual_activity && t.started_ms > 0 && now_ms - t.started_ms >= rules.restart_after_ms && can_start) {
        d.action = Action::EndAndStart; d.priority = 10; d.dismissal_s = now_s; d.why = "8 h limit";
        return d;
    }

    if (!t.has_sent) {
        d.action = Action::Update; d.priority = 5; d.why = "first";
        return d;
    }

    const Content& last     = t.last;
    const long long since   = now_ms - t.last_sent_ms;
    const bool state_change = last.state != c.state || t.last_error_code != row->error_code ||
                              (last.eta_s == 0) != (c.eta_s == 0);
    if (state_change) {
        // Coalesced, not dropped: the next poll sends the then-current state.
        if (now_ms - t.last_p10_ms < rules.p10_gap_ms) return d;
        d.action = Action::Update; d.priority = 10; d.why = "state";
        return d;
    }
    if (last.stale != c.stale) {
        d.action = Action::Update; d.priority = 5; d.why = "stale";
        return d;
    }
    if (last.same_reading(c)) {
        if (since >= rules.heartbeat_ms) { d.action = Action::Update; d.priority = 5; d.why = "heartbeat"; }
        return d;
    }
    if (since < rules.p5_gap_ms) return d;
    if (c.percent > last.percent) { d.action = Action::Update; d.priority = 5; d.why = "percent"; return d; }
    if (c.eta_s && last.eta_s && std::llabs(c.eta_s - last.eta_s) >= rules.eta_move_s) {
        d.action = Action::Update; d.priority = 5; d.why = "eta";
        return d;
    }
    if (since >= rules.heartbeat_ms) { d.action = Action::Update; d.priority = 5; d.why = "heartbeat"; }
    return d;
}

Track apply(const Track& t, const Decision& d, const Row* row, long long now_ms)
{
    Track n = t;
    const std::string job = row ? job_key(row->job) : t.job_key;
    switch (d.action) {
    case Action::None: break;
    case Action::Update:
        n.has_sent        = true;
        n.last            = d.content;
        n.last_sent_ms    = now_ms;
        if (d.priority >= 10) n.last_p10_ms = now_ms;
        n.last_error_code = row ? row->error_code : t.last_error_code;
        if (!job.empty()) n.job_key = job;
        break;
    case Action::End:
        // The activity is over. Its token is dead; an Android "activity" starts afresh.
        n.activity_token  = std::string();
        n.has_sent        = false;
        n.last            = Content();
        n.last_error_code = std::string();
        n.last_sent_ms    = 0;
        n.last_p10_ms     = now_ms;
        n.started_ms      = 0;
        n.job_key         = std::string();
        break;
    case Action::Start:
    case Action::EndAndStart:
        // The new activity's token arrives from the app; until then there is nothing to update.
        n.activity_token  = std::string();
        n.has_sent        = false;
        n.last            = d.content;
        n.last_error_code = row ? row->error_code : std::string();
        n.last_sent_ms    = now_ms;
        n.last_p10_ms     = now_ms;
        n.started_ms      = 0;
        n.job_key         = job;
        n.start_sent_job  = job;
        n.start_sent_ms   = now_ms;
        break;
    }
    return n;
}

// ------------------------------------------------------------------ the payloads ----

json content_state(const Content& c)
{
    json s = json::object();
    s["state"] = phase_name(c.state);
    s["stale"] = c.stale;
    if (c.percent >= 0) s["percent"] = c.percent;
    if (c.layer >= 0) s["layer"] = c.layer;
    if (c.total_layers > 0) s["totalLayers"] = c.total_layers;
    if (c.eta_s > 0) s["eta"] = ref_date(c.eta_s);
    if (c.bar_start_s > 0) s["barStart"] = ref_date(c.bar_start_s);
    if (c.as_of_s > 0) s["asOf"] = ref_date(c.as_of_s);
    return s;
}

static double rounded_relevance(const Content& c) { return std::round(c.relevance() * 10) / 10; }

json aps_update(const Content& c, long long now_s, const Rules& rules)
{
    json a;
    a["event"]           = "update";
    a["timestamp"]       = now_s;
    a["content-state"]   = content_state(c);
    a["stale-date"]      = now_s + rules.stale_after_s;
    a["relevance-score"] = rounded_relevance(c);
    return a;
}

json aps_end(const Content& c, long long now_s, long long dismissal_s)
{
    json a;
    a["event"]          = "end";
    a["timestamp"]      = now_s;
    a["content-state"]  = content_state(c);
    a["dismissal-date"] = dismissal_s;
    return a;
}

json aps_start(const Content& c, const std::string& opaque_printer_id, long long now_s, const Rules& rules)
{
    json a;
    a["event"]           = "start";
    a["timestamp"]       = now_s;
    a["content-state"]   = content_state(c);
    a["stale-date"]      = now_s + rules.stale_after_s;
    a["relevance-score"] = rounded_relevance(c);
    a["attributes-type"] = "PrintActivityAttributes";
    // The opaque id only; the app looks the name up on the phone (/summary push_id). jobKey is
    // empty rather than the job's hash: a hash of a file name can be confirmed by a guess.
    a["attributes"]      = json{ { "printerId", opaque_printer_id }, { "printerName", "" }, { "jobKey", "" } };
    // Apple shows push-to-start with an alert; it names nothing.
    a["alert"]           = json{ { "title", "Print in progress" }, { "body", "Live progress is on your Lock Screen" } };
    return a;
}

bool content_state_key_allowed(const std::string& key)
{
    static const char* const keys[] = { "state", "percent", "layer", "totalLayers", "eta", "barStart", "stale", "asOf" };
    for (const char* k : keys)
        if (key == k) return true;
    return false;
}

bool attribute_key_allowed(const std::string& key)
{
    return key == "printerId" || key == "printerName" || key == "jobKey";
}

json progress_plaintext(const json& row, long long now_ms)
{
    // A /summary-shaped row, as small as it can be: the Android mapper (DeviceSummary.fromJson)
    // reads these names, and the plaintext must stay well under the 2800-byte cap.
    json r = json::object();
    const std::string id = str_of(row, "id");
    r["id"]           = id;
    r["name"]         = str_of(row, "name");
    r["model"]        = str_of(row, "model");
    r["kind"]         = str_of(row, "kind");
    r["vendor"]       = r["kind"];
    r["state"]        = state_of(row);
    r["status"]       = r["state"];
    r["print_status"] = r["state"];
    r["online"]       = bool_of(row, "online", true);
    r["printing"]     = bool_of(row, "printing", false);
    r["percent"]      = int_of(row, "percent", 0);
    r["left_time_s"]  = int_of(row, "left_time_s", 0);
    r["layer"]        = int_of(row, "layer", 0);
    r["total_layers"] = int_of(row, "total_layers", 0);
    r["job"]          = job_of(row);
    r["task"]         = r["job"];
    r["stale"]        = bool_of(row, "stale", false);
    r["age_s"]        = int_of(row, "age_s", 0);
    if (row.is_object() && row.contains("print_error") && row["print_error"].is_object()) {
        const json& e = row["print_error"];
        // ("small" is a macro in the Windows headers.)
        json brief    = json::object();
        if (e.contains("code")) brief["code"] = e["code"];
        if (e.contains("message") && e["message"].is_string()) brief["message"] = e["message"].get<std::string>().substr(0, 200);
        r["print_error"] = brief;
    } else {
        r["print_error"] = nullptr;
    }
    const std::string err = str_of(row, "error");
    if (!err.empty()) r["error"] = err.substr(0, 200);

    json p;
    p["kind"]       = "progress";
    p["v"]          = 1;
    p["title"]      = "";
    p["body"]       = "";
    p["severity"]   = "info";
    p["printer_id"] = id;
    p["time"]       = now_ms;
    p["row"]        = r;
    return p;
}

} // namespace LiveActivity
} // namespace GUI
} // namespace Slic3r
