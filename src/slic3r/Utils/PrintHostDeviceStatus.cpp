#include "PrintHostDeviceStatus.hpp"

#include "Http.hpp"

#include <boost/log/trivial.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>

namespace Slic3r {
namespace PrintHostDevices {

using nlohmann::json;

// ------------------------------------------------------------------ helpers ----

std::string Slot::label() const
{
    std::string out = "T" + std::to_string(index + 1);
    if (!type.empty()) {
        out += " " + type;
        if (!sub_type.empty() && sub_type != type)
            out += " " + sub_type;
    } else if (!loaded) {
        out += " (empty)";
    }
    return out;
}

// Moonraker::make_url's own rule, as RemoteControl.cpp:79 copies it: an address without a scheme
// becomes http://, and the MQTT ports the Device tab stores are dropped so the printer's HTTP API
// on port 80 is addressed instead.
static std::string moonraker_base(const std::string& host)
{
    std::string h = host;
    while (!h.empty() && (h.back() == '/' || h.back() == ' '))
        h.pop_back();
    if (h.empty())
        return h;
    if (h.compare(0, 7, "http://") == 0 || h.compare(0, 8, "https://") == 0)
        return h;
    const size_t mqtt_plain = h.find(":1884"), mqtt_tls = h.find(":8883");
    if (mqtt_plain != std::string::npos)
        h = h.substr(0, mqtt_plain);
    else if (mqtt_tls != std::string::npos)
        h = h.substr(0, mqtt_tls);
    return "http://" + h;
}

static bool moonraker_get(const std::string& url, std::string& body, std::string& error, int timeout_s)
{
    bool ok = false;
    Http::get(url)
        .timeout_connect(timeout_s)
        .timeout_max(timeout_s)
        .size_limit(512 * 1024)
        .on_error([&](std::string reply, std::string err, unsigned status) {
            error = err.empty() ? ("HTTP " + std::to_string(status)) : err;
            if (!reply.empty())
                body = reply;
        })
        .on_complete([&](std::string reply, unsigned) {
            body = reply;
            ok   = true;
        })
        .perform_sync();
    if (!ok && error.empty())
        error = "no answer";
    return ok;
}

static std::string str_at(const json& j, const char* key)
{
    return j.is_object() && j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
}

// "RRGGBBAA" or "#RRGGBB" or "RRGGBB" -> "#RRGGBB". Anything else -> "".
static std::string colour_of(const std::string& raw)
{
    std::string s = raw;
    if (!s.empty() && s[0] == '#')
        s = s.substr(1);
    if (s.size() < 6)
        return std::string();
    for (size_t i = 0; i < 6; ++i)
        if (!std::isxdigit((unsigned char) s[i]))
            return std::string();
    return "#" + s.substr(0, 6);
}

static const json& array_at(const json& j, const char* key)
{
    static const json empty = json::array();
    if (!j.is_object())
        return empty;
    auto it = j.find(key);
    return (it != j.end() && it->is_array()) ? *it : empty;
}

static std::string arr_str(const json& a, size_t i)
{
    return i < a.size() && a[i].is_string() ? a[i].get<std::string>() : std::string();
}

// ------------------------------------------------------------ what we can ask ----

bool can_probe(const std::string& host_type_key)
{
    // Exactly the host types RemoteControl already probes. Adding one here means writing a client
    // for its status API; there is no generic "ask a print host how it is" in this codebase.
    return speaks_moonraker(host_type_key);
}

std::string no_filament_note(const std::string& host_type_key)
{
    if (host_type_key == "elegoolink")
        // Measured against the fork's own client, not guessed: see the header.
        return "Elegoo Link reports no filament or slot data (its SDCP client asks only for the "
               "print status), so there is nothing to map. The plate is sent exactly as it was sliced.";
    if (speaks_moonraker(host_type_key))
        return "This printer answered, but did not say what is loaded in it. The plate is sent "
               "exactly as it was sliced.";
    return "This host type does not report its loaded filaments, so there is nothing to map. The "
           "plate is sent exactly as it was sliced.";
}

// ------------------------------------------------------------------- the probe ----

// The Snapmaker-flavoured Moonraker object that carries the loaded filaments, one entry per
// toolhead. SnapmakerLan::toolheads_of (SnapmakerLan.cpp:456) reads the same arrays; so does
// SSWCP's machine filament info. A stock Klipper does not have this object and simply answers
// without it.
static void slots_from_print_task_config(const json& status, std::vector<Slot>& out, bool& have_filaments)
{
    const json& cfg = status.is_object() && status.contains("print_task_config") && status["print_task_config"].is_object() ?
                          status["print_task_config"] :
                          json::object();
    if (!cfg.is_object() || cfg.empty())
        return;
    const json& types    = array_at(cfg, "filament_type");
    const json& subs     = array_at(cfg, "filament_sub_type");
    const json& vendors  = array_at(cfg, "filament_vendor");
    const json& colours  = array_at(cfg, "filament_color_rgba");
    const json& exists   = array_at(cfg, "filament_exist");
    const size_t n = std::max(std::max(types.size(), colours.size()), exists.size());
    for (size_t i = 0; i < n; ++i) {
        Slot s;
        s.index    = (int) i;
        s.type     = arr_str(types, i);
        s.sub_type = arr_str(subs, i);
        s.vendor   = arr_str(vendors, i);
        s.color    = colour_of(arr_str(colours, i));
        s.loaded   = i < exists.size() ? (exists[i].is_boolean() ? exists[i].get<bool>() :
                                          (exists[i].is_number() ? exists[i].get<double>() != 0.0 : false)) :
                                         !s.type.empty();
        if (!s.type.empty() || !s.color.empty())
            have_filaments = true;
        out.push_back(std::move(s));
    }
}

// No print_task_config: the extruder objects still say how many tools there are. A slot list with
// no materials in it - the caller shows it as tools, not as filaments.
static void slots_from_extruders(const json& status, std::vector<Slot>& out)
{
    static const char* names[] = { "extruder", "extruder1", "extruder2", "extruder3" };
    for (int i = 0; i < 4; ++i) {
        if (!status.is_object() || !status.contains(names[i]) || !status[names[i]].is_object())
            continue;
        const json& e = status[names[i]];
        if (e.empty())
            continue;
        Slot s;
        s.index  = i;
        s.nozzle = e.contains("nozzle_diameter") && e["nozzle_diameter"].is_number() ? e["nozzle_diameter"].get<double>() : 0.0;
        out.push_back(std::move(s));
    }
}

Status probe(const Device& d, int timeout_s)
{
    Status st;
    if (d.address.empty()) {
        st.note = "this device has no address";
        return st;
    }
    if (!can_probe(d.host_type)) {
        st.note = no_filament_note(d.host_type);
        return st;
    }
    st.probed = true;
    const std::string base = moonraker_base(d.address);
    if (base.empty()) {
        st.note = no_filament_note(d.host_type);
        return st;
    }
    // One call, read-only, never a command - the same objects RemoteControl::describe_hosts asks
    // for plus print_task_config, which is where the loaded filaments live when there are any.
    std::string body;
    if (!moonraker_get(base + "/printer/objects/query?print_stats&print_task_config&extruder&extruder1&extruder2&extruder3",
                       body, st.error, timeout_s)) {
        st.note = "this printer did not answer" + (st.error.empty() ? std::string() : (": " + st.error));
        return st;
    }
    json j;
    try {
        j = json::parse(body);
    } catch (...) {
        st.error = "the answer was not JSON";
        st.note  = "this printer answered something this slicer could not read.";
        return st;
    }
    const json status = j.is_object() ? j.value("result", json::object()).value("status", json::object()) : json::object();
    if (!status.is_object() || status.empty()) {
        st.note = "this printer answered, but not as a Moonraker printer.";
        return st;
    }
    st.online = true;
    if (status.contains("print_stats") && status["print_stats"].is_object())
        st.state = str_at(status["print_stats"], "state");
    slots_from_print_task_config(status, st.slots, st.slots_have_filaments);
    if (st.slots.empty())
        slots_from_extruders(status, st.slots);
    if (st.slots.empty() || !st.slots_have_filaments)
        st.note = no_filament_note(d.host_type);
    return st;
}

} // namespace PrintHostDevices
} // namespace Slic3r
