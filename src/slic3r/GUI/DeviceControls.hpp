#ifndef slic3r_GUI_DeviceControls_hpp_
#define slic3r_GUI_DeviceControls_hpp_

// What the phone's native printer screen may control on one printer, and the commands behind each
// control - kept free of wx, DeviceManager and the network so it can be unit tested on its own
// (tests/slic3rutils/device_controls_tests.cpp), exactly like NozzleTempRows.hpp.
//
// Two halves:
//
//  * Capabilities. `Caps` is what GET /api/printers adds to a row as `controls`: the heaters with
//    their current reading, their limit and whether a target can be set; the speed control
//    (Bambu's four levels, or Klipper's speed factor); the chamber light; and the fans. A client
//    draws exactly what is here and nothing else, so a printer that has no chamber light never gets
//    a light switch. Built by RemoteControl from a MachineObject (Bambu) or from a Moonraker
//    /printer/objects/query answer (a Snapmaker U1 or another Klipper printer).
//
//  * Commands. The checks POST /api/printers/{id}/control runs on action=set_temp | set_speed |
//    set_light | set_fan before anything is sent, and the Moonraker G-code each one becomes. The
//    Bambu side sends through the same MachineObject calls the desktop Device tab makes
//    (command_set_bed / command_set_nozzle[_new] / command_set_chamber, command_set_printing_speed,
//    command_set_chamber_light, command_control_fan_val); the helpers here only say which call
//    and with what value, so the numbers can be tested without a printer.
//
// Heater ids are one namespace for every printer: "bed", "chamber", and "nozzle<N>" where N is the
// printer's own extruder index - Bambu's extruder id (0 = right/main, 1 = left/deputy on the
// two-nozzle machines) or Klipper's extruder number (extruder, extruder1, ... on the U1), and
// "rack<P>" for a tool parked in a nozzle rack (P = rack position, 0-based; the H3C).
//
// Backward compatibility (tests/design_device_multi_tool_temps.md, D2): phone apps already
// installed read controls.heaters[] by hand and ignore keys they do not know, but draw every entry
// as a tappable tile. So heaters[] keeps exactly the entries it always had (bed, chamber, the tools
// on the extruders) and only gains keys (kind, group, present, mounted, state); rack positions go
// in a separate controls.rack[] those apps never read.

#include "NozzleTempRows.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace DeviceControls {

// Heating anything above this while the printer is not printing is a confirmed action: the phone
// asks the person first and says so with confirm=1, and the route refuses it without. A print in
// progress is already hot and already attended to, so a change there is not asked about.
constexpr int CONFIRM_ABOVE_C = 50;

// The desktop Device tab's own fallbacks (StatusPanel.cpp: bed_temp_range, nozzle_temp_range,
// nozzle_chamber_range), used when the printer reports no limit of its own.
constexpr int DEFAULT_BED_MAX     = 120;
constexpr int DEFAULT_NOZZLE_MAX  = 300;
constexpr int DEFAULT_CHAMBER_MAX = 60;

// Klipper's speed factor, as a percentage, where the printer accepts M220. The phone offers a few
// steps inside this range; anything outside it is refused before it reaches the printer.
constexpr int SPEED_FACTOR_MIN = 10;
constexpr int SPEED_FACTOR_MAX = 300;

// Klipper printers: the most extruders (extruder, extruder1, ... extruder15) asked for and listed.
// Only what the printer answers is ever listed; this is a sanity bound, not a model limit (D7).
constexpr int MAX_KLIPPER_TOOLS = 16;

struct Heater
{
    std::string id;     // bed | chamber | nozzle<N> | rack<P>
    std::string label;  // "Bed", "Chamber", "Nozzle", "L", "R", "Nozzle 1".."Nozzle 16", "Rack 1".."Rack 9"
    double      temp { 0 };
    double      target { 0 };
    int         max { 0 };        // the highest target the printer accepts
    bool        settable { true }; // false: shown, but only read (an X1's chamber sensor)
    bool        active { false };  // the extruder the printer reports as current
    // ---- added for multi-tool temperatures; old clients ignore them (see the note at the top) ----
    // Empty kind / group are worked out from the id (heater_kind, heater_group), so the places that
    // build a bed, chamber or nozzle row need not say them.
    std::string kind;                 // nozzle | bed | chamber | cutter | pen | laser | fan | unknown
    std::string group;                // tool (on an extruder / carriage) | rack | env (bed, chamber)
    bool        present { true };     // false: an empty slot (no tool there)
    bool        mounted { false };    // a rack position whose nozzle is on the extruder right now
    std::string state;                // idle | heating | holding | cooling, when the printer says so; else empty
    bool        has_temp { true };    // false: no reading (a tool that is not a heater, or not reported)
};

struct Fan
{
    std::string id;    // part | aux | chamber | cavity
    std::string label;
    int         percent { 0 }; // 0..100, as last reported
};

struct Speed
{
    std::string kind;        // "level" (Bambu: 1 silent .. 4 ludicrous) or "factor" (Klipper M220, %)
    int         value { 0 }; // the level, or the percentage; 0 = not reported
    int         min { 0 }, max { 0 };
    bool        settable { true }; // Bambu takes a speed level only while it prints
};

struct Caps
{
    std::vector<Heater> heaters;
    // Rack positions (group "rack"), one per position the printer reports, in position order.
    // Emitted as controls.rack[] only when not empty; never part of heaters[] (D2). Read-only until
    // a rack target has been proven on hardware (D4): check_set_temp only looks in heaters.
    std::vector<Heater> rack;
    bool                has_speed { false };
    Speed               speed;
    bool                has_light { false };
    bool                light_on { false };
    std::vector<Fan>    fans;
    // Loading / unloading filament (FilamentCommands.hpp): whether the printer takes the
    // load_filament / unload_filament verbs at all. Which slot can do what right now is on each AMS
    // tray / toolhead as can_load / can_unload. `filament_assumed` marks the U1, whose macros come
    // from U1 owners rather than from Snapmaker (the owner verifies them on hardware).
    bool                filament_actions { false };
    bool                filament_assumed { false };
};

// ------------------------------------------------------------- capabilities ----

inline std::string nozzle_id(int extruder_index) { return "nozzle" + std::to_string(extruder_index); }

// "nozzle1" -> 1; -1 for anything that is not a nozzle id.
inline int nozzle_index_of(const std::string& heater_id)
{
    if (heater_id.size() < 7 || heater_id.compare(0, 6, "nozzle") != 0) return -1;
    int n = 0;
    for (size_t i = 6; i < heater_id.size(); ++i) {
        const char c = heater_id[i];
        if (c < '0' || c > '9') return -1;
        n = n * 10 + (c - '0');
        if (n > 63) return -1;
    }
    return n;
}

// "rack3" -> 3; -1 for anything that is not a rack id.
inline int rack_index_of(const std::string& heater_id)
{
    if (heater_id.size() < 5 || heater_id.compare(0, 4, "rack") != 0) return -1;
    int n = 0;
    for (size_t i = 4; i < heater_id.size(); ++i) {
        const char c = heater_id[i];
        if (c < '0' || c > '9') return -1;
        n = n * 10 + (c - '0');
        if (n > 63) return -1;
    }
    return n;
}

inline std::string rack_id(int position) { return "rack" + std::to_string(position); }

// What a heater is, when the row did not say: worked out from its id.
inline std::string heater_kind(const Heater& h)
{
    if (!h.kind.empty()) return h.kind;
    if (h.id == "bed") return "bed";
    if (h.id == "chamber") return "chamber";
    if (nozzle_index_of(h.id) >= 0 || rack_index_of(h.id) >= 0) return "nozzle";
    return "unknown";
}

inline std::string heater_group(const Heater& h)
{
    if (!h.group.empty()) return h.group;
    if (h.id == "bed" || h.id == "chamber") return "env";
    if (rack_index_of(h.id) >= 0) return "rack";
    return "tool";
}

// Only these take a target. A cutter, pen, laser, fan or a tool not known is never heated (safety
// rule 1 of the design note), whatever its settable flag says.
inline bool kind_takes_temperature(const std::string& kind) { return kind == "nozzle" || kind == "bed" || kind == "chamber"; }

inline const Heater* find_heater(const Caps& caps, const std::string& id)
{
    for (const Heater& h : caps.heaters)
        if (h.id == id) return &h;
    return nullptr;
}

inline const Fan* find_fan(const Caps& caps, const std::string& id)
{
    for (const Fan& f : caps.fans)
        if (f.id == id) return &f;
    return nullptr;
}

// A Bambu printer's nozzles as heaters: one "Nozzle" row on a single-nozzle printer, and L above R
// on the two-nozzle ones - the rows the Device tab draws (NozzleTempRows), addressed by the
// extruder id its set-temperature command takes.
inline std::vector<Heater> bambu_nozzle_heaters(const std::vector<NozzleTempSample>& extders, int total_count, int current_id,
                                                int nozzle_max)
{
    std::vector<Heater> out;
    const int max = nozzle_max > 0 ? nozzle_max : DEFAULT_NOZZLE_MAX;
    for (const NozzleTempRow& r : nozzle_temp_rows(extders, total_count, current_id)) {
        Heater h;
        h.id     = nozzle_id(r.extruder_id);
        h.label  = r.badge.empty() ? std::string("Nozzle") : r.badge;
        h.temp   = r.temp;
        h.target = r.target;
        h.max    = max;
        h.active = r.active;
        out.push_back(h);
    }
    return out;
}

// Klipper reports a fan's speed as 0..1.
inline int percent_of_unit(double v)
{
    if (!(v >= 0)) return 0;
    if (v > 1) v = 1;
    return (int) std::lround(v * 100.0);
}

// Bambu reports fan speeds as 0..255 (DeviceManager's fan_gear / *_fan_speed parsing).
inline int percent_of_byte(int v)
{
    if (v <= 0) return 0;
    if (v >= 255) return 100;
    return (int) std::lround(v * 100.0 / 255.0);
}

inline bool read_num(const nlohmann::json& obj, const char* key, double& out)
{
    if (!obj.is_object() || !obj.contains(key) || !obj[key].is_number()) return false;
    out = obj[key].get<double>();
    return true;
}

// Klipper's object name for extruder n: extruder, extruder1, extruder2, ...
inline std::string klipper_extruder_name(int n) { return n == 0 ? std::string("extruder") : "extruder" + std::to_string(n); }

// "extruder&extruder1&...&extruder<count-1>" for a /printer/objects/query string. Klipper answers an
// object it does not have with nothing usable (an empty object or no key), so asking for more than
// the printer has lists nothing extra: klipper_extruders only keeps what answered.
inline std::string klipper_extruders_query(int count = MAX_KLIPPER_TOOLS)
{
    std::string q;
    for (int i = 0; i < count; ++i) q += (i ? "&" : "") + klipper_extruder_name(i);
    return q;
}

// One extruder object a Klipper status answer carried.
struct KlipperTool
{
    int    index { 0 };          // N of extruder<N> (0 for "extruder")
    double temp { 0 }, target { 0 };
    bool   has_temp { false };   // "temperature" was a number
    bool   has_target { false }; // "target" was a number
};

// The one walk over a Klipper status answer's extruders that the U1 probe, the Moonraker host
// probe and moonraker_caps share: every extruder<N> (N < max_tools, capped at MAX_KLIPPER_TOOLS)
// that is in the answer as an object, in index order. Gaps are skipped, never filled in, and the
// index is kept, so extruder2 after a missing extruder1 is still tool 2. A name is never guessed:
// only objects the printer answered for are returned. Callers pick what they need (a reading, a
// target, an unbroken run from 0).
inline std::vector<KlipperTool> klipper_extruders(const nlohmann::json& status, int max_tools = MAX_KLIPPER_TOOLS)
{
    std::vector<KlipperTool> out;
    if (!status.is_object()) return out;
    if (max_tools > MAX_KLIPPER_TOOLS) max_tools = MAX_KLIPPER_TOOLS;
    for (int i = 0; i < max_tools; ++i) {
        const auto it = status.find(klipper_extruder_name(i));
        if (it == status.end() || !it->is_object()) continue;
        KlipperTool t;
        t.index      = i;
        t.has_temp   = read_num(*it, "temperature", t.temp);
        t.has_target = read_num(*it, "target", t.target);
        out.push_back(t);
    }
    return out;
}

// The controls of a Klipper printer, from one /printer/objects/query status object: the bed, every
// extruder it answered for (extruder, extruder1, ... up to max_extruders, at most
// MAX_KLIPPER_TOOLS), the speed factor (gcode_move), the part fan (fan), and - where the printer
// has them, as a Snapmaker U1 does - the cavity light ("led cavity_led") and the cavity fan
// ("fan_generic cavity_fan"). Only what answered is listed; an extruder missing from the middle is
// left out and the ones after it keep their numbers.
inline Caps moonraker_caps(const nlohmann::json& status, int bed_max = DEFAULT_BED_MAX, int nozzle_max = DEFAULT_NOZZLE_MAX,
                           int max_extruders = MAX_KLIPPER_TOOLS)
{
    Caps caps;
    if (!status.is_object()) return caps;
    double temp = 0, target = 0;
    if (status.contains("heater_bed") && read_num(status["heater_bed"], "temperature", temp)) {
        read_num(status["heater_bed"], "target", target);
        caps.heaters.push_back(Heater { "bed", "Bed", temp, target, bed_max, true, false });
    }
    std::vector<Heater> nozzles;
    for (const KlipperTool& t : klipper_extruders(status, max_extruders)) {
        if (!t.has_temp) continue;
        nozzles.push_back(Heater { nozzle_id(t.index), "", t.temp, t.has_target ? t.target : 0.0, nozzle_max, true, false });
    }
    for (Heater& h : nozzles) {
        h.label = nozzles.size() == 1 ? std::string("Nozzle") : "Nozzle " + std::to_string(nozzle_index_of(h.id) + 1);
        caps.heaters.push_back(h);
    }
    double v = 0;
    if (status.contains("gcode_move") && read_num(status["gcode_move"], "speed_factor", v)) {
        caps.has_speed = true;
        caps.speed     = Speed { "factor", (int) std::lround(v * 100.0), SPEED_FACTOR_MIN, SPEED_FACTOR_MAX, true };
    }
    if (status.contains("led cavity_led") && status["led cavity_led"].is_object()) {
        const nlohmann::json& led = status["led cavity_led"];
        // color_data is [[r, g, b, w], ...] per LED in the chain; the light is on when any channel is.
        if (led.contains("color_data") && led["color_data"].is_array()) {
            caps.has_light = true;
            for (const auto& px : led["color_data"])
                if (px.is_array())
                    for (const auto& ch : px)
                        if (ch.is_number() && ch.get<double>() > 0.001) caps.light_on = true;
        }
    }
    if (status.contains("fan") && read_num(status["fan"], "speed", v))
        caps.fans.push_back(Fan { "part", "Part cooling", percent_of_unit(v) });
    if (status.contains("fan_generic cavity_fan") && read_num(status["fan_generic cavity_fan"], "speed", v))
        caps.fans.push_back(Fan { "cavity", "Cavity", percent_of_unit(v) });
    return caps;
}

// The query string that asks a Klipper printer for everything moonraker_caps reads, on top of the
// print state. Object names with a space are percent-encoded, as Moonraker expects them.
inline std::string moonraker_controls_query() { return "gcode_move&fan&led%20cavity_led&fan_generic%20cavity_fan"; }

// One heater as the `controls` JSON carries it. The first eight keys are the original shape and are
// never renamed or dropped; the rest were added later and old clients ignore them. A tool that is
// not a heater, or a reading not reported, has null temp / target and is never settable.
inline nlohmann::json heater_json(const Heater& h)
{
    const std::string kind     = heater_kind(h);
    const bool        readable = h.has_temp && kind_takes_temperature(kind);
    nlohmann::json    j        = { { "id", h.id },
                                   { "label", h.label },
                                   { "temp", readable ? nlohmann::json(h.temp) : nlohmann::json(nullptr) },
                                   { "target", readable ? nlohmann::json(h.target) : nlohmann::json(nullptr) },
                                   { "min", 0 },
                                   { "max", h.max },
                                   { "settable", h.settable && kind_takes_temperature(kind) && h.present },
                                   { "active", h.active },
                                   { "kind", kind },
                                   { "group", heater_group(h) },
                                   { "present", h.present },
                                   { "mounted", h.mounted } };
    if (!h.state.empty()) j["state"] = h.state;
    return j;
}

inline nlohmann::json to_json(const Caps& caps)
{
    nlohmann::json j = nlohmann::json::object();
    nlohmann::json hs = nlohmann::json::array();
    for (const Heater& h : caps.heaters) hs.push_back(heater_json(h));
    j["heaters"] = hs;
    if (!caps.rack.empty()) {
        nlohmann::json rs = nlohmann::json::array();
        for (const Heater& h : caps.rack) rs.push_back(heater_json(h));
        j["rack"] = rs;
    }
    if (caps.has_speed) {
        nlohmann::json s = { { "kind", caps.speed.kind }, { "value", caps.speed.value }, { "min", caps.speed.min },
                             { "max", caps.speed.max },   { "settable", caps.speed.settable } };
        if (caps.speed.kind == "level")
            s["levels"] = nlohmann::json::array({ { { "value", 1 }, { "label", "Silent" } },
                                                  { { "value", 2 }, { "label", "Standard" } },
                                                  { { "value", 3 }, { "label", "Sport" } },
                                                  { { "value", 4 }, { "label", "Ludicrous" } } });
        j["speed"] = s;
    }
    if (caps.has_light) j["light"] = { { "on", caps.light_on } };
    nlohmann::json fs = nlohmann::json::array();
    for (const Fan& f : caps.fans) fs.push_back({ { "id", f.id }, { "label", f.label }, { "percent", f.percent } });
    j["fans"] = fs;
    if (caps.filament_actions) {
        j["filament"] = { { "load", true }, { "unload", true } };
        if (caps.filament_assumed) j["filament"]["assumed"] = true;
    }
    return j;
}

// ---------------------------------------------------------------- commands ----

// A whole number of degrees, 0..999, and nothing else: "60", not "60.5", "-1" or "60C".
inline bool parse_int(const std::string& text, int lo, int hi, int& out)
{
    if (text.empty() || text.size() > 4) return false;
    int n = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        n = n * 10 + (c - '0');
    }
    if (n < lo || n > hi) return false;
    out = n;
    return true;
}

// The checks set_temp runs before anything is sent. 0 when it may go out, otherwise the HTTP status
// and a sentence for the phone. `printing` is the printer's own "a print is in progress".
inline int check_set_temp(const Caps& caps, const std::string& heater, const std::string& target_text, bool confirm,
                          bool printing, int& target, std::string& why)
{
    const Heater* h = find_heater(caps, heater);
    if (!h) {
        why = heater.empty() ? "heater is required (bed, chamber or nozzle<N>, as controls.heaters lists them)"
                             : "this printer has no heater called " + heater;
        return heater.empty() ? 400 : 404;
    }
    if (!kind_takes_temperature(heater_kind(*h))) {
        why = "the " + h->label + " is not a heater";
        return 409;
    }
    if (!h->present) {
        why = "there is no tool in " + h->label;
        return 409;
    }
    if (!h->settable) {
        why = "the " + h->label + " temperature can only be read on this printer";
        return 409;
    }
    if (!parse_int(target_text, 0, 999, target)) {
        why = "target must be a whole number of degrees";
        return 400;
    }
    if (target > h->max) {
        why = "the " + h->label + " limit is " + std::to_string(h->max) + " C";
        return 400;
    }
    if (target > CONFIRM_ABOVE_C && !printing && !confirm) {
        why = "heating above " + std::to_string(CONFIRM_ABOVE_C) + " C while the printer is idle needs confirm=1";
        return 400;
    }
    return 0;
}

inline int check_set_speed(const Caps& caps, const std::string& value_text, int& value, std::string& why)
{
    if (!caps.has_speed) { why = "this printer's speed cannot be changed from here"; return 409; }
    if (!caps.speed.settable) { why = "the speed can only be changed while a print is running"; return 409; }
    if (!parse_int(value_text, caps.speed.min, caps.speed.max, value)) {
        why = "value must be " + std::to_string(caps.speed.min) + ".." + std::to_string(caps.speed.max) +
              (caps.speed.kind == "level" ? " (1 silent, 2 standard, 3 sport, 4 ludicrous)" : " (percent)");
        return 400;
    }
    return 0;
}

inline int check_set_light(const Caps& caps, const std::string& on_text, bool& on, std::string& why)
{
    if (!caps.has_light) { why = "this printer has no light the phone can switch"; return 409; }
    if (on_text != "0" && on_text != "1") { why = "on must be 1 or 0"; return 400; }
    on = on_text == "1";
    return 0;
}

inline int check_set_fan(const Caps& caps, const std::string& fan, const std::string& percent_text, int& percent, std::string& why)
{
    if (!find_fan(caps, fan)) {
        why = fan.empty() ? "fan is required (as controls.fans lists them)" : "this printer has no fan called " + fan;
        return fan.empty() ? 400 : 404;
    }
    if (!parse_int(percent_text, 0, 100, percent)) { why = "percent must be 0..100"; return 400; }
    return 0;
}

// Klipper's own name for a heater id: heater_bed, extruder, extruder1, ...; empty for one it has not.
inline std::string klipper_heater(const std::string& heater_id)
{
    if (heater_id == "bed") return "heater_bed";
    const int n = nozzle_index_of(heater_id);
    if (n < 0) return std::string();
    return n == 0 ? std::string("extruder") : "extruder" + std::to_string(n);
}

// The G-code a Klipper printer is sent for each control - what the Snapmaker app's own device page
// sends for a toolhead (SET_HEATER_TEMPERATURE HEATER=extruder<N> TARGET=<t>), and the standard
// Klipper commands for the rest. Empty when the control has no Klipper spelling.
inline std::string moonraker_temp_script(const std::string& heater_id, int target)
{
    const std::string heater = klipper_heater(heater_id);
    if (heater.empty()) return std::string();
    return "SET_HEATER_TEMPERATURE HEATER=" + heater + " TARGET=" + std::to_string(target);
}

inline std::string moonraker_speed_script(int percent) { return "M220 S" + std::to_string(percent); }

inline std::string moonraker_light_script(bool on) { return std::string("SET_LED LED=cavity_led WHITE=") + (on ? "1" : "0"); }

inline std::string moonraker_fan_script(const std::string& fan_id, int percent)
{
    if (fan_id == "part") return "M106 S" + std::to_string((int) std::lround(percent * 255.0 / 100.0));
    if (fan_id == "cavity") {
        char buf[64];
        std::snprintf(buf, sizeof buf, "SET_FAN_SPEED FAN=cavity_fan SPEED=%.2f", percent / 100.0);
        return buf;
    }
    return std::string();
}

// Bambu: the desktop's fan popup works in ten steps and sends floor(step * 25.5) through
// command_control_fan_val (Widgets/FanControl.cpp). A percentage is rounded to the nearest step
// first, so 50 % sends what the popup's fifth step sends.
inline int bambu_fan_value(int percent)
{
    int step = (int) std::lround(percent / 10.0);
    if (step < 0) step = 0;
    if (step > 10) step = 10;
    return (int) std::floor(step * 25.5f);
}

// MachineObject::FanType: COOLING_FAN = 1 (part), BIG_COOLING_FAN = 2 (aux), CHAMBER_FAN = 3.
inline int bambu_fan_type(const std::string& fan_id)
{
    if (fan_id == "part") return 1;
    if (fan_id == "aux") return 2;
    if (fan_id == "chamber") return 3;
    return 0;
}

}}} // namespace Slic3r::GUI::DeviceControls

#endif // slic3r_GUI_DeviceControls_hpp_
