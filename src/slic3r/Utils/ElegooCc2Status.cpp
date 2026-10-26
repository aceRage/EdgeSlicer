// The pure half of ElegooCc2Status.hpp: merging the CC2's status and turning it into a printer row.

#include "ElegooCc2Status.hpp"
#include "ElegooCc2.hpp"
#include "ElegooSdcp.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace Slic3r {
namespace ElegooCc2 {

namespace {

using json = nlohmann::json;

const json& obj_at(const json& j, const char* key)
{
    static const json empty = json::object();
    if (!j.is_object()) return empty;
    auto it = j.find(key);
    return (it != j.end() && it->is_object()) ? *it : empty;
}

bool num_at(const json& j, const char* key, double& out)
{
    if (!j.is_object()) return false;
    auto it = j.find(key);
    if (it == j.end()) return false;
    if (it->is_number()) {
        out = it->get<double>();
        return std::isfinite(out);
    }
    if (it->is_string()) {
        try {
            size_t            used = 0;
            const std::string t    = it->get<std::string>();
            out                    = std::stod(t, &used);
            return used == t.size() && std::isfinite(out);
        } catch (...) {}
    }
    return false;
}

int int_at(const json& j, const char* key, int def)
{
    double v = 0;
    return num_at(j, key, v) ? static_cast<int>(std::lround(v)) : def;
}

std::string str_at(const json& j, const char* key)
{
    if (!j.is_object()) return std::string();
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

int clamp_pct(int v) { return v < 0 ? 0 : (v > 100 ? 100 : v); }

// elegoo-link's mergeJsonRecursive: objects merge key by key, everything else is replaced;
// exception_code is always replaced whole.
void merge_into(json& target, const json& source)
{
    if (!source.is_object()) return;
    for (auto it = source.begin(); it != source.end(); ++it) {
        if (it.key() != "exception_code" && it->is_object() && target.contains(it.key()) && target[it.key()].is_object())
            merge_into(target[it.key()], *it);
        else
            target[it.key()] = *it;
    }
}

bool on_print(const Status& s) { return s.machine() == MS_PRINTING; }

bool is_paused_sub(int sub) { return sub == SUB_PAUSED || sub == SUB_PAUSED_2; }

} // namespace

int Status::machine() const
{
    const json& ms = obj_at(raw, "machine_status");
    return valid ? int_at(ms, "status", -1) : -1;
}

int Status::sub() const
{
    const json& ms = obj_at(raw, "machine_status");
    return valid ? int_at(ms, "sub_status", -1) : -1;
}

void merge_full(Status& s, const json& result)
{
    if (!result.is_object()) return;
    s.raw = result;
    s.raw.erase("error_code");
    s.valid = true;
    s.gaps  = 0;
    // The next push continues from here, whatever its id.
    s.last_push_id = -1;
}

PushResult merge_push(Status& s, const json& message)
{
    if (!message.is_object() || int_at(message, "method", -1) != METHOD_STATUS_PUSH) return PushResult::Ignored;
    const int id = int_at(message, "id", -1);
    if (!s.valid) {
        if (id >= 0) s.last_push_id = id;
        return PushResult::NeedFull;
    }
    if (id >= 0) {
        if (s.last_push_id >= 0 && id != s.last_push_id + 1)
            ++s.gaps;
        else
            s.gaps = 0;
        s.last_push_id = id;
    }
    const json& result = message.contains("result") ? message["result"] : json();
    if (result.is_object()) merge_into(s.raw, result);
    if (s.gaps >= 5) {
        s.gaps = 0;
        return PushResult::NeedFull;
    }
    return PushResult::Merged;
}

PushResult merge_push(Status& s, const std::string& payload)
{
    const json j = json::parse(payload, nullptr, false, true);
    if (j.is_discarded()) return PushResult::Ignored;
    return merge_push(s, j);
}

double progress(const Status& s)
{
    double v = -1;
    const json& ms = obj_at(s.raw, "machine_status");
    return s.valid && num_at(ms, "progress", v) ? v : -1;
}

std::string filename(const Status& s)
{
    const json& ps = obj_at(s.raw, "print_status");
    return s.valid ? str_at(ps, "filename") : std::string();
}

int current_layer(const Status& s)
{
    const json& ps = obj_at(s.raw, "print_status");
    return s.valid ? int_at(ps, "current_layer", -1) : -1;
}

int total_layer(const Status& s)
{
    const json& ps = obj_at(s.raw, "print_status");
    return s.valid ? int_at(ps, "total_layer", -1) : -1;
}

long long seconds_left(const Status& s)
{
    double v = -1;
    const json& ps = obj_at(s.raw, "print_status");
    if (!s.valid || !num_at(ps, "remaining_time_sec", v) || v < 0) return -1;
    return static_cast<long long>(v);
}

std::vector<int> exception_codes(const Status& s)
{
    std::vector<int> out;
    if (!s.valid) return out;
    const json& ms = obj_at(s.raw, "machine_status");
    if (!ms.contains("exception_status") || !ms["exception_status"].is_array()) return out;
    for (const json& v : ms["exception_status"])
        if (v.is_number_integer()) out.push_back(v.get<int>());
    return out;
}

bool has_canvas_info(const Status& s) { return s.valid && obj_at(s.raw, "canvas_info").contains("canvas_list"); }

bool canvas_from_status(const Status& s, CanvasInfo& out)
{
    if (!has_canvas_info(s)) return false;
    std::string why;
    CanvasInfo  info;
    if (!canvas_parse_payload(obj_at(s.raw, "canvas_info").dump(), info, why)) return false;
    out = std::move(info);
    return true;
}

std::string hub_state(const Status& s)
{
    if (!s.valid) return std::string();
    const int m   = s.machine();
    const int sub = s.sub();
    if (m == MS_PRINTING) {
        if (is_paused_sub(sub)) return "paused";
        if (sub == SUB_STOPPED) return "cancelled";
        if (sub == SUB_COMPLETED) return "complete";
        return "printing"; // printing, pausing, stopping, resuming and every preamble step
    }
    if (m == MS_EMERGENCY_STOP) return "error";
    return "standby";
}

std::string phase_text(const Status& s)
{
    if (!s.valid) return std::string();
    const int m = s.machine();
    if (m == MS_PRINTING) {
        switch (s.sub()) {
        case 1081:
        case 1082:
        case 1086: return "Downloading the file";
        case 1045:
        case 1096: return "Preheating the nozzle";
        case 1405:
        case 1906: return "Preheating the bed";
        case 2801:
        case 2802: return "Homing";
        case 2901:
        case 2902: return "Auto-leveling";
        case SUB_PAUSING: return "Pausing";
        case SUB_RESUMING: return "Resuming";
        case SUB_STOPPING: return "Stopping";
        default: return std::string();
        }
    }
    if (m == MS_IDLE || m < 0) return std::string();
    // Anything else the machine is doing, in the send dialog's words, capitalised.
    std::string t = state_name(m);
    if (!t.empty()) t[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(t[0])));
    return t;
}

bool is_printing(const Status& s)
{
    const std::string st = hub_state(s);
    return st == "printing" || st == "paused";
}

bool can_pause(const Status& s)
{
    if (!on_print(s)) return false;
    const int sub = s.sub();
    return !is_paused_sub(sub) && sub != SUB_PAUSING && sub != SUB_STOPPING && sub != SUB_STOPPED && sub != SUB_COMPLETED &&
           sub != SUB_RESUMING;
}

bool can_resume(const Status& s) { return on_print(s) && is_paused_sub(s.sub()); }

bool can_stop(const Status& s)
{
    if (!on_print(s)) return false;
    const int sub = s.sub();
    return sub != SUB_STOPPING && sub != SUB_STOPPED && sub != SUB_COMPLETED;
}

GUI::DeviceControls::Caps caps(const Snapshot& snap)
{
    using namespace GUI::DeviceControls;
    Caps          c;
    const Status& s = snap.status;
    if (!s.valid) return c;
    double t = 0, tt = 0;
    const json& bed = obj_at(s.raw, "heater_bed");
    if (num_at(bed, "temperature", t)) {
        tt = 0;
        num_at(bed, "target", tt);
        c.heaters.push_back(Heater { "bed", "Bed", t, tt, DEFAULT_BED_MAX, false, false });
    }
    const json& ext = obj_at(s.raw, "extruder");
    if (num_at(ext, "temperature", t)) {
        tt = 0;
        num_at(ext, "target", tt);
        c.heaters.push_back(Heater { nozzle_id(0), "Nozzle", t, tt, DEFAULT_NOZZLE_MAX, false, true });
    }
    const json& cham = obj_at(s.raw, "ztemperature_sensor");
    if (num_at(cham, "temperature", t)) c.heaters.push_back(Heater { "chamber", "Chamber", t, 0, DEFAULT_CHAMBER_MAX, false, false });
    const json& led = obj_at(s.raw, "led");
    double      lv  = 0;
    if (num_at(led, "status", lv)) {
        c.has_light = true;
        c.light_on  = lv > 0;
    }
    const json& fans = obj_at(s.raw, "fans");
    struct F { const char* key; const char* id; const char* label; };
    for (const F& f : { F { "fan", "part", "Part cooling" }, F { "aux_fan", "aux", "Auxiliary" }, F { "box_fan", "chamber", "Chamber" } }) {
        double sp = 0;
        const json& fan = obj_at(fans, f.key);
        if (num_at(fan, "speed", sp)) c.fans.push_back(Fan { f.id, f.label, clamp_pct(static_cast<int>(std::lround(sp))) });
    }
    return c;
}

void fill_row(const Snapshot& snap, json& row, int stale_after_s)
{
    // The send-capability fields list_hosts set stay; everything about the live state is ours.
    row["can_pause"]     = false;
    row["can_resume"]    = false;
    row["can_stop"]      = false;
    row["print_error"]   = nullptr;
    row["cc2_connected"] = snap.connected;
    const bool live = snap.connected;
    if (!live) {
        if (snap.other_window) {
            row["status_error"] = "another EdgeSlicer window is connected to this printer";
            return;
        }
        if (snap.failures > 0) {
            row["online"]       = false;
            row["status"]       = "offline";
            row["printing"]     = false;
            row["status_error"] = snap.error.empty() ? std::string("the printer does not answer") : snap.error;
            return;
        }
        if (!snap.status.valid) return; // never heard: "unknown"
    }
    const Status&     s     = snap.status;
    const std::string state = hub_state(s);
    row["online"] = true;
    if (state.empty()) return;
    const long long age_s = snap.status_at_ms > 0 && snap.now_ms > snap.status_at_ms ? (snap.now_ms - snap.status_at_ms) / 1000 : 0;
    // A live connection whose status has stopped coming: show it, but offer no buttons on it.
    const bool fresh = live && age_s <= stale_after_s;
    row["status"]       = state;
    row["print_status"] = state;
    const bool active   = is_printing(s);
    row["printing"]     = active;
    row["can_pause"]    = fresh && can_pause(s);
    row["can_resume"]   = fresh && can_resume(s);
    row["can_stop"]     = fresh && can_stop(s);
    const std::string file = filename(s);
    if (!file.empty() && (active || state == "complete" || state == "cancelled" || state == "error")) row["task"] = file;
    const std::string phase = phase_text(s);
    if (!phase.empty())
        row["stage"] = phase;
    else if (!file.empty() && active)
        row["stage"] = file;
    if (active || state == "complete") {
        const double pr = progress(s);
        if (pr >= 0) row["percent"] = clamp_pct(state == "complete" ? 100 : static_cast<int>(std::lround(pr)));
        const long long left = seconds_left(s);
        if (left >= 0) row["left_time_s"] = state == "complete" ? 0 : left;
        const int cl = current_layer(s), tl = total_layer(s);
        if (cl >= 0) row["layer"] = cl;
        if (tl > 0) row["total_layers"] = tl;
    }
    const std::vector<int> exc = exception_codes(s);
    if (state == "error" || (!exc.empty() && active)) {
        const std::string code = !exc.empty() ? std::to_string(exc.front()) : std::string("error");
        row["print_error"] = { { "code", code },
                               { "message", state == "error" ? std::string("The printer reports an emergency stop.") :
                                                               "The printer reports exception " + code + "." } };
    }
    const GUI::DeviceControls::Caps c = caps(snap);
    for (const auto& h : c.heaters) {
        if (h.id == "bed") {
            row["bed_temp"]   = h.temp;
            row["bed_target"] = h.target;
        } else if (h.id == "chamber") {
            row["chamber_temp"] = h.temp;
        } else {
            row["nozzles"] = json::array({ json { { "temp", h.temp }, { "target", h.target } } });
        }
    }
    row["controls"] = GUI::DeviceControls::to_json(c);
    // The CANVAS (the Combo), while one is connected and read; nothing at all on a plain CC2.
    if (snap.has_canvas) {
        const json ams = Sdcp::canvas_ams_json(snap.canvas);
        if (!ams.empty()) row["ams"] = ams;
    }
    json d;
    d["protocol"] = "mqtt";
    d["live"]     = live;
    d["age_s"]    = age_s;
    d["stale"]    = live && !fresh;
    if (!snap.firmware.empty()) d["firmware"] = snap.firmware;
    if (s.machine() >= 0) d["machine_status"] = s.machine();
    if (s.sub() >= 0) d["sub_status"] = s.sub();
    if (!phase.empty()) d["phase"] = phase;
    if (snap.canvas_known) d["canvas_attached"] = snap.has_canvas;
    if (snap.has_canvas && snap.canvas.auto_refill >= 0) d["auto_refill"] = snap.canvas.auto_refill != 0;
    if (!exc.empty()) d["exceptions"] = exc;
    row["cc2"] = d;
}

} // namespace ElegooCc2
} // namespace Slic3r
