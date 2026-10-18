// The pure half of the hub's Elegoo SDCP backend (ElegooSdcp.hpp): reading frames, the state words,
// the controls and the printer row. No socket here - ElegooSdcpMonitor.cpp owns the connection.

#include "ElegooSdcp.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r {
namespace Sdcp {

using json = nlohmann::json;

namespace {

// A number, a bool or a numeric string; `def` for anything else (other SDCP firmware has sent both
// 1 and "1" for the same field).
bool num_at(const json& j, const char* key, double& out)
{
    if (!j.is_object()) return false;
    auto it = j.find(key);
    if (it == j.end()) return false;
    if (it->is_number()) {
        const double v = it->get<double>();
        if (!std::isfinite(v)) return false;
        out = v;
        return true;
    }
    if (it->is_boolean()) {
        out = it->get<bool>() ? 1 : 0;
        return true;
    }
    if (it->is_string()) {
        try {
            size_t       used = 0;
            const double v    = std::stod(it->get<std::string>(), &used);
            if (used == 0 || !std::isfinite(v)) return false;
            out = v;
            return true;
        } catch (...) {
            return false;
        }
    }
    return false;
}

bool int_at(const json& j, const char* key, int& out)
{
    double v = 0;
    if (!num_at(j, key, v)) return false;
    out = (int) std::lround(v);
    return true;
}

bool str_at(const json& j, const char* key, std::string& out)
{
    if (!j.is_object()) return false;
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return false;
    out = it->get<std::string>();
    return true;
}

std::vector<int> int_list(const json& v)
{
    std::vector<int> out;
    if (v.is_array()) {
        for (const json& e : v)
            if (e.is_number()) out.push_back((int) std::lround(e.get<double>()));
    } else if (v.is_number()) {
        out.push_back((int) std::lround(v.get<double>()));
    }
    return out;
}

bool has_canvas_list(const json& j, int depth)
{
    if (depth > 3 || !j.is_object()) return false;
    if (j.contains("canvas_list")) return true;
    auto it = j.find("Data");
    return it != j.end() && has_canvas_list(*it, depth + 1);
}

void merge_status(const json& st, Status& s)
{
    if (!st.is_object()) return;
    s.valid = true;
    if (st.contains("CurrentStatus")) s.current_status = int_list(st["CurrentStatus"]);
    double v = 0;
    if (num_at(st, "TempOfNozzle", v)) { s.nozzle = v; s.has_nozzle = true; }
    if (num_at(st, "TempTargetNozzle", v)) s.nozzle_target = v;
    if (num_at(st, "TempOfHotbed", v)) { s.bed = v; s.has_bed = true; }
    if (num_at(st, "TempTargetHotbed", v)) s.bed_target = v;
    if (num_at(st, "TempOfBox", v)) { s.box = v; s.has_box = true; }
    if (num_at(st, "TempTargetBox", v)) s.box_target = v;
    if (num_at(st, "ZOffset", v)) { s.z_offset = v; s.has_z_offset = true; }
    int_at(st, "AmsConnectStatus", s.ams_connect);
    int_at(st, "TimeLapseStatus", s.timelapse);
    if (st.contains("CurrentFanSpeed") && st["CurrentFanSpeed"].is_object()) {
        const json& f = st["CurrentFanSpeed"];
        // "ModeFan" is the same fan under a misspelt name some firmware uses (OpenCentauri api.md).
        if (!int_at(f, "ModelFan", s.model_fan)) int_at(f, "ModeFan", s.model_fan);
        int_at(f, "AuxiliaryFan", s.aux_fan);
        int_at(f, "BoxFan", s.box_fan);
    }
    if (st.contains("LightStatus") && st["LightStatus"].is_object()) {
        const json& l = st["LightStatus"];
        int_at(l, "SecondLight", s.second_light);
        if (l.contains("RgbLight")) s.rgb_light = int_list(l["RgbLight"]);
    }
    if (st.contains("RgbLight") && st["RgbLight"].is_array()) s.rgb_light = int_list(st["RgbLight"]);
    if (st.contains("PrintInfo") && st["PrintInfo"].is_object()) {
        const json& p = st["PrintInfo"];
        int_at(p, "Status", s.print_status);
        int_at(p, "CurrentLayer", s.current_layer);
        int_at(p, "TotalLayer", s.total_layer);
        if (num_at(p, "CurrentTicks", v)) s.current_ticks = (long long) std::llround(v);
        if (num_at(p, "TotalTicks", v)) s.total_ticks = (long long) std::llround(v);
        if (num_at(p, "Progress", v)) s.progress = v;
        str_at(p, "Filename", s.filename);
        if (!int_at(p, "PrintSpeedPct", s.speed_pct)) int_at(p, "PrintSpeed", s.speed_pct);
        int_at(p, "ErrorNumber", s.error_number);
    }
}

void merge_attributes(const json& a, Attributes& out)
{
    if (!a.is_object()) return;
    out.valid = true;
    str_at(a, "FirmwareVersion", out.firmware);
    str_at(a, "MachineName", out.machine_name);
    str_at(a, "ProtocolVersion", out.protocol);
    str_at(a, "MainboardID", out.mainboard_id);
    if (a.contains("Capabilities") && a["Capabilities"].is_array()) {
        out.capabilities.clear();
        for (const json& c : a["Capabilities"])
            if (c.is_string()) out.capabilities.push_back(c.get<std::string>());
    }
    int_at(a, "CameraStatus", out.camera_status);
    int_at(a, "MaximumVideoStreamAllowed", out.max_video_streams);
    int_at(a, "NumberOfVideoStreamConnected", out.video_streams);
}

// Steps on the way into a print (CurrentStatus 1 with one of these): shown as "printing".
bool is_print_preamble(int ps)
{
    switch (ps) {
    case PRINT_HOMING:
    case PRINT_FILE_CHECKING:
    case PRINT_PRINTERS_CHECKING:
    case PRINT_AUTO_LEVELING:
    case PRINT_PREHEATING:
    case PRINT_RESONANCE_TESTING:
    case PRINT_START:
    case PRINT_AUTO_LEVELING_DONE:
    case PRINT_PREHEATING_DONE:
    case PRINT_HOMING_DONE:
    case PRINT_RESONANCE_TESTING_DONE:
    case PRINT_AUTO_FEEDING:
        return true;
    default: return false;
    }
}

int clamp_pct(int v) { return v < 0 ? 0 : (v > 100 ? 100 : v); }

} // namespace

bool Status::machine_has(int code) const { return std::find(current_status.begin(), current_status.end(), code) != current_status.end(); }

Frame read_frame(const std::string& text, Status& status, Attributes& attrs)
{
    Frame f;
    // The heartbeat answer is bare text, not JSON.
    {
        std::string t = text;
        t.erase(std::remove_if(t.begin(), t.end(), [](char c) { return c == ' ' || c == '\r' || c == '\n' || c == '"'; }), t.end());
        if (t == "pong") {
            f.kind = FrameKind::Pong;
            return f;
        }
    }
    const json j = json::parse(text, nullptr, false, true);
    if (j.is_discarded() || !j.is_object()) return f;
    try {
        if (j.contains("Status") && j["Status"].is_object()) {
            const int before = status.ams_connect;
            merge_status(j["Status"], status);
            f.kind             = FrameKind::Status;
            f.ams_connect_seen = j["Status"].contains("AmsConnectStatus") || before != status.ams_connect;
            return f;
        }
        if (j.contains("Attributes") && j["Attributes"].is_object()) {
            merge_attributes(j["Attributes"], attrs);
            // The mainboard id sits beside the object on some firmware rather than inside it.
            if (attrs.mainboard_id.empty()) str_at(j, "MainboardID", attrs.mainboard_id);
            f.kind = FrameKind::Attributes;
            return f;
        }
        if (j.contains("Data") && j["Data"].is_object()) {
            const json& d = j["Data"];
            // Some answers wrap the status or the attributes once more.
            if (d.contains("Status") && d["Status"].is_object()) {
                merge_status(d["Status"], status);
                f.kind             = FrameKind::Status;
                f.ams_connect_seen = d["Status"].contains("AmsConnectStatus");
                return f;
            }
            if (d.contains("Attributes") && d["Attributes"].is_object()) {
                merge_attributes(d["Attributes"], attrs);
                f.kind = FrameKind::Attributes;
                return f;
            }
            f.kind = FrameKind::Response;
            int_at(d, "Cmd", f.cmd);
            str_at(d, "RequestID", f.request_id);
            if (d.contains("Data") && d["Data"].is_object()) int_at(d["Data"], "Ack", f.ack);
            f.has_canvas = has_canvas_list(j, 0);
            return f;
        }
        if (j.contains("canvas_list")) {
            f.kind       = FrameKind::Response;
            f.cmd        = CMD_CANVAS;
            f.has_canvas = true;
        }
    } catch (...) {
        f = Frame();
    }
    return f;
}

std::string request_text(int cmd, const json& data, const std::string& request_id, long long timestamp_ms,
                         const std::string& mainboard_id)
{
    nlohmann::ordered_json inner;
    inner["Cmd"]         = cmd;
    inner["Data"]        = data.is_object() ? nlohmann::ordered_json::parse(data.dump()) : nlohmann::ordered_json::object();
    inner["RequestID"]   = request_id;
    inner["MainboardID"] = mainboard_id;
    inner["TimeStamp"]   = timestamp_ms;
    inner["From"]        = 1;
    nlohmann::ordered_json root;
    root["Id"]   = "";
    root["Data"] = std::move(inner);
    if (!mainboard_id.empty()) root["Topic"] = "sdcp/request/" + mainboard_id;
    return root.dump();
}

std::string fan_key(const std::string& fan_id)
{
    if (fan_id == "part") return "ModelFan";
    if (fan_id == "aux") return "AuxiliaryFan";
    if (fan_id == "chamber") return "BoxFan";
    return std::string();
}

json fan_data(const std::string& fan_id, int percent)
{
    json j;
    j["TargetFanSpeed"][fan_key(fan_id)] = clamp_pct(percent);
    return j;
}

json light_data(bool on)
{
    json j;
    j["LightStatus"]["SecondLight"] = on ? 1 : 0;
    j["LightStatus"]["RgbLight"]    = on ? json::array({ 255, 255, 255 }) : json::array({ 0, 0, 0 });
    return j;
}

std::string ack_text(int cmd, int ack)
{
    if (ack == 0) return std::string();
    if (cmd == CMD_PAUSE || cmd == CMD_STOP || cmd == CMD_RESUME) {
        // SDCP_PRINT_CTRL_ACK_* (specification, Start Printing); the printer answers 1 when it is not
        // in a state that takes the command.
        if (ack == 1) return "the printer is busy and did not take the command";
        if (ack == 2) return "the printer could not find the print job";
    }
    return "the printer refused the command (Ack " + std::to_string(ack) + ")";
}

std::string hub_state(const Status& s)
{
    if (!s.valid) return std::string();
    const int  ps       = s.print_status;
    const bool printing = s.machine_has(MACHINE_PRINTING);
    if (printing) {
        if (ps == PRINT_PAUSED || ps == PRINT_FEEDING_OUT_PAUSED) return "paused";
        // Stopped / complete / error still under CurrentStatus 1 for a moment while the machine
        // winds down: say what the job ended as.
        if (ps == PRINT_STOPPED) return "cancelled";
        if (ps == PRINT_COMPLETE) return "complete";
        if (ps == PRINT_ERROR || ps == PRINT_FEEDING_OUT_ABNORMAL) return "error";
        return "printing"; // printing, pausing, stopping, resuming and every preamble step
    }
    // Not on a print: PrintInfo.Status keeps the last job's outcome (spec: "after the print is
    // completed, the sub-status value will continue to be maintained as Print Completed").
    if (ps == PRINT_COMPLETE) return "complete";
    if (ps == PRINT_STOPPED) return "cancelled";
    if (ps == PRINT_ERROR) return "error";
    return "standby";
}

std::string phase_text(const Status& s)
{
    if (!s.valid) return std::string();
    if (s.machine_has(MACHINE_PRINTING)) {
        switch (s.print_status) {
        case PRINT_HOMING: return "Homing";
        case PRINT_PAUSING: return "Pausing";
        case PRINT_STOPPING: return "Stopping";
        case PRINT_FILE_CHECKING: return "Checking the file";
        case PRINT_PRINTERS_CHECKING: return "Self-check";
        case PRINT_RESUMING: return "Resuming";
        case PRINT_AUTO_LEVELING:
        case PRINT_AUTO_LEVELING_DONE: return "Auto-leveling";
        case PRINT_PREHEATING:
        case PRINT_PREHEATING_DONE: return "Preheating";
        case PRINT_RESONANCE_TESTING:
        case PRINT_RESONANCE_TESTING_DONE: return "Resonance test";
        case PRINT_START:
        case PRINT_HOMING_DONE: return "Starting";
        case PRINT_AUTO_FEEDING: return "Loading filament";
        case PRINT_FEEDING_OUT: return "Unloading filament";
        case PRINT_FEEDING_OUT_ABNORMAL: return "Filament unload problem";
        case PRINT_FEEDING_OUT_PAUSED: return "Filament unload paused";
        default: return std::string();
        }
    }
    switch (s.machine()) {
    case MACHINE_FILE_TRANSFERRING: return "Receiving a file";
    case MACHINE_DEVICES_TESTING: return "Self-check";
    case MACHINE_AUTO_LEVELING: return "Auto-leveling";
    case MACHINE_RESONANCE_TESTING: return "Resonance test";
    case MACHINE_BUSY: return "Busy";
    case MACHINE_FILE_CHECKING: return "Checking the file";
    case MACHINE_HOMING: return "Homing";
    case MACHINE_FEEDING_OUT: return "Unloading filament";
    case MACHINE_PID_DETECT: return "PID calibration";
    default: return std::string();
    }
}

bool is_printing(const Status& s)
{
    const std::string st = hub_state(s);
    return st == "printing" || st == "paused";
}

bool can_pause(const Status& s)
{
    // While it actually prints, or is on its way into the print; not while it is already pausing,
    // stopping or resuming.
    if (!s.machine_has(MACHINE_PRINTING)) return false;
    return s.print_status == PRINT_PRINTING || is_print_preamble(s.print_status);
}

bool can_resume(const Status& s) { return s.machine_has(MACHINE_PRINTING) && s.print_status == PRINT_PAUSED; }

bool can_stop(const Status& s)
{
    if (!s.machine_has(MACHINE_PRINTING)) return false;
    return s.print_status != PRINT_STOPPING && s.print_status != PRINT_STOPPED && s.print_status != PRINT_COMPLETE;
}

long long seconds_left(const Status& s)
{
    if (s.total_ticks < 0 || s.current_ticks < 0) return -1;
    return std::max<long long>(0, s.total_ticks - s.current_ticks);
}

GUI::DeviceControls::Caps caps(const Snapshot& snap)
{
    using namespace GUI::DeviceControls;
    Caps          c;
    const Status& s = snap.status;
    // Readings only: Cmd 403 can carry TempTarget* (elegoo-link SET_TEMPERATURE) but its own command
    // table leaves that commented out for the CC1, so the phone does not offer it yet.
    if (s.has_bed) c.heaters.push_back(Heater { "bed", "Bed", s.bed, s.bed_target, DEFAULT_BED_MAX, false, false });
    if (s.has_nozzle) c.heaters.push_back(Heater { nozzle_id(0), "Nozzle", s.nozzle, s.nozzle_target, DEFAULT_NOZZLE_MAX, false, true });
    if (s.has_box) c.heaters.push_back(Heater { "chamber", "Chamber", s.box, s.box_target, DEFAULT_CHAMBER_MAX, false, false });
    if (s.speed_pct > 0) {
        c.has_speed = true;
        c.speed     = Speed { "factor", s.speed_pct, 50, 160, false };
    }
    if (s.second_light >= 0) {
        c.has_light = true;
        c.light_on  = s.second_light != 0;
    }
    if (s.model_fan >= 0) c.fans.push_back(Fan { "part", "Part cooling", clamp_pct(s.model_fan) });
    if (s.aux_fan >= 0) c.fans.push_back(Fan { "aux", "Auxiliary", clamp_pct(s.aux_fan) });
    if (s.box_fan >= 0) c.fans.push_back(Fan { "chamber", "Chamber", clamp_pct(s.box_fan) });
    return c;
}

json canvas_ams_json(const CanvasInfo& canvas)
{
    json out = json::array();
    for (size_t ui = 0; ui < canvas.units.size(); ++ui) {
        const CanvasUnit& u = canvas.units[ui];
        if (!u.connected) continue;
        json unit;
        unit["id"]     = std::to_string(u.canvas_id);
        unit["type"]   = "canvas";
        unit["name"]   = "CANVAS " + std::to_string(ui + 1);
        unit["nozzle"] = 0;
        unit["side"]   = "";
        json trays     = json::array();
        for (const CanvasTray& t : canvas.trays) {
            if (t.canvas_id != u.canvas_id) continue;
            json j;
            j["id"]     = std::to_string(t.tray_id);
            j["exists"] = t.has_filament;
            // The material as the printer names it ("PLA", "PETG"), and what the sync calls the spool
            // ("Generic PLA PRO", "ELEGOO PLA") where a Bambu tray has its sub-brand.
            j["type"]     = t.has_filament ? (t.filament_type.empty() ? t.tray_type : t.filament_type) : std::string();
            j["sub_type"] = t.has_filament ? canvas_tray_name(t) : std::string();
            j["color"]    = t.colour_hex();
            // In the toolhead right now: the printer's own active pair, or the tray's status 2.
            const bool active = (canvas.active_canvas_id == t.canvas_id && canvas.active_tray_id == t.tray_id) ||
                                t.state() == CanvasTrayState::Loaded;
            j["active"] = t.has_filament && active;
            trays.push_back(j);
        }
        unit["trays"] = trays;
        out.push_back(unit);
    }
    return out;
}

void fill_row(const Snapshot& snap, const std::string& host, json& row)
{
    // The send-capability fields list_hosts set stay; everything about the live state is ours.
    row["can_pause"]   = false;
    row["can_resume"]  = false;
    row["can_stop"]    = false;
    row["print_error"] = nullptr;
    row["sdcp_connected"] = snap.connected;
    if (!snap.connected) {
        if (snap.other_window) {
            // Another window on this PC holds the printer's one connection; its row is the live one
            // (the hub keeps the connected row) and this one says why it has nothing.
            row["status_error"] = "another EdgeSlicer window is connected to this printer";
            return;
        }
        if (!snap.ever_connected && snap.failures == 0) return; // the first attempt is still running: "unknown"
        row["online"]       = false;
        row["status"]       = "offline";
        row["printing"]     = false;
        row["status_error"] = snap.error.empty() ? std::string("the printer does not answer") : snap.error;
        return;
    }
    const Status&     s     = snap.status;
    const std::string state = hub_state(s);
    row["online"] = true;
    if (state.empty()) return; // connected, no status frame yet: still "unknown"
    row["status"]       = state;
    row["print_status"] = state;
    const bool active   = is_printing(s);
    row["printing"]     = active;
    row["can_pause"]    = can_pause(s);
    row["can_resume"]   = can_resume(s);
    row["can_stop"]     = can_stop(s);
    // The job: while it runs, and after it, as a Klipper printer keeps reporting its last file.
    if (!s.filename.empty() && (active || state == "complete" || state == "cancelled" || state == "error"))
        row["task"] = s.filename;
    const std::string phase = phase_text(s);
    if (!phase.empty())
        row["stage"] = phase;
    else if (!s.filename.empty() && active)
        row["stage"] = s.filename;
    if (active || state == "complete") {
        if (s.progress >= 0) row["percent"] = clamp_pct((int) std::lround(state == "complete" ? 100.0 : s.progress));
        const long long left = seconds_left(s);
        if (left >= 0) row["left_time_s"] = state == "complete" ? 0 : left;
        if (s.current_layer >= 0) row["layer"] = s.current_layer;
        if (s.total_layer >= 0) row["total_layers"] = s.total_layer;
    }
    if (state == "error") {
        const std::string code = s.error_number > 0 ? std::to_string(s.error_number) : std::string("error");
        row["print_error"] = { { "code", code },
                               { "message", s.error_number > 0 ? "The printer stopped the print (error " + code + ")."
                                                               : std::string("The printer stopped the print with an error.") } };
    }
    // The temperatures in the plain fields every card reads, and the whole set in `controls`.
    if (s.has_bed) {
        row["bed_temp"]   = s.bed;
        row["bed_target"] = s.bed_target;
    }
    if (s.has_nozzle) row["nozzles"] = json::array({ json { { "temp", s.nozzle }, { "target", s.nozzle_target } } });
    if (s.has_box) {
        row["chamber_temp"]   = s.box;
        row["chamber_target"] = s.box_target;
    }
    row["controls"] = GUI::DeviceControls::to_json(caps(snap));
    // The CANVAS, while one is attached and read.
    if (snap.has_canvas && s.ams_connect != 0) {
        const json ams = canvas_ams_json(snap.canvas);
        if (!ams.empty()) row["ams"] = ams;
    }
    // Where the app finds the camera: the Stream tab's relay joins a printer by `ip` (already on the
    // row); this says whether there is a camera at all and how many viewers it takes.
    json cam;
    cam["vendor"]    = "elegoo";
    cam["available"] = snap.attrs.camera_status == 1 ||
                       std::find(snap.attrs.capabilities.begin(), snap.attrs.capabilities.end(), "VIDEO_STREAM") != snap.attrs.capabilities.end();
    if (snap.attrs.max_video_streams > 0) cam["max_streams"] = snap.attrs.max_video_streams;
    if (!host.empty()) {
        cam["relay_path"]  = "/elegoo?ip=" + host;
        cam["status_path"] = "/elegoo/status?ip=" + host;
    }
    row["camera_hint"] = cam;
    json d;
    d["protocol"] = "sdcp";
    if (!snap.attrs.firmware.empty()) d["firmware"] = snap.attrs.firmware;
    if (s.machine() >= 0) d["machine_status"] = s.machine();
    if (s.print_status >= 0) d["print_status"] = s.print_status;
    if (!phase.empty()) d["phase"] = phase;
    if (s.ams_connect >= 0) d["canvas_attached"] = s.ams_connect != 0;
    if (s.timelapse >= 0) d["timelapse"] = s.timelapse != 0;
    if (s.has_z_offset) d["z_offset"] = s.z_offset;
    if (s.speed_pct > 0) d["speed_pct"] = s.speed_pct;
    row["sdcp"] = d;
}

} // namespace Sdcp
} // namespace Slic3r
