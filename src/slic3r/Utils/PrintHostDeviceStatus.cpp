#include "PrintHostDeviceStatus.hpp"

#include "Http.hpp"
#include "PrusaLinkStatus.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/log/trivial.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <limits>

namespace Slic3r {
namespace PrintHostDevices {

using nlohmann::json;

// ------------------------------------------------------------------ helpers ----

std::string Slot::label() const
{
    if (unit >= 0) {
        // A CANVAS tray: numbered as the printer's screen counts them, 1..4 per unit.
        std::string out = "Tray " + std::to_string(index + 1);
        if (loaded && !type.empty())
            out += " " + (sub_type.empty() ? type : sub_type);
        else if (!loaded)
            out += " (empty)";
        return out;
    }
    std::string out = name.empty() ? "T" + std::to_string(index + 1) : name;
    if (!type.empty()) {
        out += " " + type;
        if (!sub_type.empty() && sub_type != type)
            out += " " + sub_type;
    } else if (!loaded) {
        out += " (empty)";
    }
    return out;
}

std::string slot_tile_label(const std::vector<Slot>& slots, int pos)
{
    if (pos < 0 || pos >= (int) slots.size())
        return std::to_string(pos + 1);
    const Slot& s = slots[pos];
    if (s.unit < 0)
        return s.name.empty() ? "T" + std::to_string(s.index + 1) : s.name;
    // The send dialog's tiles (third_party_slots_from + bambu_style_slot_label): units numbered by
    // order of appearance, the slot by its place in its unit, as letters.
    std::vector<int> units;
    int              place = 0;
    for (int i = 0; i <= pos; ++i) {
        if (slots[i].unit < 0)
            continue;
        if (std::find(units.begin(), units.end(), slots[i].unit) == units.end())
            units.push_back(slots[i].unit);
        if (i < pos && slots[i].unit == s.unit)
            ++place;
    }
    const int unit = (int) (std::find(units.begin(), units.end(), s.unit) - units.begin());
    if (place >= 26)
        return std::to_string(unit + 1) + "-" + std::to_string(place + 1);
    return std::to_string(unit + 1) + std::string(1, (char) ('A' + place));
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
    Http::get(url).tls_policy(Http::TlsPolicy::PrintHost) // printer: keep accepting self-signed certificates
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
    // The host types this fork has a status client for. Adding one here means writing that client;
    // there is no generic "ask a print host how it is" in this codebase.
    return speaks_moonraker(host_type_key) || PrusaLinkStatus::speaks_prusalink(host_type_key) ||
           host_type_key == "elegoolink";
}

std::string no_filament_note(const std::string& host_type_key)
{
    if (host_type_key == "elegoolink")
        // A Centauri Carbon 1 with a CANVAS reports its trays (ElegooCanvas.hpp); anything else on
        // Elegoo Link - or a CC1 without a CANVAS, or one that did not answer - has nothing to map.
        return "Elegoo Link reports no CANVAS slot data, so there is nothing to map. The plate is sent exactly as it was sliced.";
    if (PrusaLinkStatus::speaks_prusalink(host_type_key))
        // Measured against the API, not guessed: PrusaLink's /api/v1/status reports the printer's
        // state and its two heaters, and /api/v1/job the file it is on. Neither says what filament
        // is loaded - a Buddy board has no filament-identity story at all (the MMU's slots are not
        // in this API), so there is nothing to map.
        return "PrusaLink reports the printer's state and temperatures but not what filament is "
               "loaded, so there is nothing to map. The plate is sent exactly as it was sliced.";
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

// A number or a boolean, as the QIDI firmware is not consistent about which it sends.
static bool truthy_at(const json& j, const std::string& key)
{
    if (!j.is_object() || !j.contains(key))
        return false;
    const json& v = j[key];
    return v.is_boolean() ? v.get<bool>() : (v.is_number() ? v.get<double>() != 0.0 : false);
}

static int int_at(const json& j, const char* key, int fallback)
{
    return j.is_object() && j.contains(key) && j[key].is_number() ? j[key].get<int>() : fallback;
}

// A QIDI Box (X-Max 4, X-Plus 4, Q2): `multi_color_controller`, read the way QIDIStudio's
// QDSDevice::updateBoxDataByJson reads it - `box_count` and `boxes` either at the top level or
// under `box`, each box a `filament_info` array of four slots and a `box_inserted_<n>` flag per
// slot. Slot n of box b is Box slot b * 4 + n, the number the firmware's `value_t<tool>` names.
// The extruder's own spool (`extra`) is left out: QIDIStudio's numbering for it is not settled.
static void slots_from_qidi_box(const json& status, std::vector<Slot>& out, bool& have_filaments, bool& is_box)
{
    if (!status.is_object() || !status.contains("multi_color_controller") || !status["multi_color_controller"].is_object())
        return;
    const json& mcc   = status["multi_color_controller"];
    const json& boxes = mcc.contains("box") && mcc["box"].is_object() ? mcc["box"] : mcc;
    const int   count = int_at(mcc, "box_count", int_at(boxes, "box_count", 0));
    if (count <= 0)
        return; // the controller is there, the Box is not plugged in
    const json& list = array_at(boxes, "boxes");
    for (size_t b = 0; b < list.size() && b < 4; ++b) {
        const json& box   = list[b];
        const json& infos = array_at(box, "filament_info");
        for (size_t n = 0; n < infos.size() && n < 4; ++n) {
            const json& fi = infos[n];
            Slot s;
            s.index  = (int) (b * 4 + n);
            s.name   = "Box " + std::to_string(b + 1) + " slot " + std::to_string(n + 1);
            s.loaded = truthy_at(box, "box_inserted_" + std::to_string(n));
            if (s.loaded) {
                s.type     = str_at(fi, "filament_type");
                s.sub_type = str_at(fi, "filament_str"); // "PLA Rapido"
                s.vendor   = str_at(fi, "vendor_str");
                const json& colours = array_at(fi, "color_rgb_str");
                s.color    = colour_of(arr_str(colours, 0));
                if (!s.type.empty() || !s.color.empty())
                    have_filaments = true;
            }
            out.push_back(std::move(s));
        }
    }
    is_box = !out.empty();
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

// The network-free half: a Moonraker `result.status` object -> what this dialog can say about the
// printer. Shared by probe() and by parse_moonraker_status(), which is what the tests call.
// save_variables' `variables`, when the answer carried them; null otherwise.
static const json* qidi_variables_of(const json& status)
{
    if (!status.is_object() || !status.contains("save_variables") || !status["save_variables"].is_object())
        return nullptr;
    const json& sv = status["save_variables"];
    return sv.contains("variables") && sv["variables"].is_object() ? &sv["variables"] : nullptr;
}

// The variables only a QIDI Box firmware keeps (QIDIStudio writes enable_box and value_t<n>).
static bool qidi_variables_present(const json* vars)
{
    return vars != nullptr && (vars->contains("enable_box") || vars->contains("box_count") || vars->contains("filament_slot0"));
}

static Status fill_from_status(const json& status, const std::string& host_type_key, const QidiFilaList* filas = nullptr)
{
    Status st;
    if (!status.is_object() || status.empty()) {
        st.note = "this printer answered, but not as a Moonraker printer.";
        return st;
    }
    st.online = true;
    if (status.contains("print_stats") && status["print_stats"].is_object())
        st.state = str_at(status["print_stats"], "state");
    const json* vars     = qidi_variables_of(status);
    st.qidi              = (status.contains("multi_color_controller") && status["multi_color_controller"].is_object()) ||
                           qidi_variables_present(vars);
    st.qidi_polar_cooler = st.qidi && status.contains("output_pin polar_cooler") && status["output_pin polar_cooler"].is_object();
    slots_from_print_task_config(status, st.slots, st.slots_have_filaments);
    if (st.slots.empty())
        slots_from_qidi_box(status, st.slots, st.slots_have_filaments, st.qidi_box);
    // The Box named no materials there (or is not described there at all): read it the way
    // QIDIStudio does, from save_variables and the printer's filament list.
    if (st.qidi && !st.slots_have_filaments && qidi_variables_present(vars) && filas != nullptr) {
        bool              have = false;
        std::vector<Slot> from_vars = slots_from_qidi_variables(vars->dump(), *filas, have);
        if (!from_vars.empty()) {
            st.slots                = std::move(from_vars);
            st.slots_have_filaments = have;
            st.qidi_box             = true;
        }
    }
    // A Box the firmware counts but whose slots nobody could read is still a Box: the send must not
    // treat it as absent and start on value_t<n> (qidi_send_policy needs a slot for every filament).
    if (st.qidi && !st.qidi_box && vars != nullptr && int_at(*vars, "box_count", 0) > 0)
        st.qidi_box = true;
    if (st.slots.empty())
        slots_from_extruders(status, st.slots);
    if (st.slots.empty() || !st.slots_have_filaments)
        st.note = no_filament_note(host_type_key);
    return st;
}

// A PrusaLink / PrusaConnect printer, through the client that speaks its REST API. It answers the
// state (and the temperatures and the job, which this dialog has no field for) but never names a
// filament, so the slot list stays empty and `note` says why.
static Status probe_prusalink(const Device& d, int timeout_s)
{
    Status                 st;
    PrusaLinkStatus::Auth  auth;
    auth.auth_type = d.auth_type;
    auth.apikey    = d.apikey;
    auth.user      = d.user;
    auth.password  = d.password;
    const PrusaLinkStatus::Status pl = PrusaLinkStatus::probe(d.address, auth, timeout_s);
    st.probed = true;
    st.error  = pl.error;
    if (!pl.answered) {
        st.note = pl.authorized ? ("this printer did not answer" + (pl.error.empty() ? std::string() : (": " + pl.error))) :
                                  "this printer refused the API key or password in its preset.";
        return st;
    }
    st.online = true;
    st.state  = pl.state;
    st.note   = no_filament_note(d.host_type);
    return st;
}

Status status_from_canvas(const CanvasQueryResult& r)
{
    Status st;
    st.probed = true;
    st.online = r.reached;
    st.error  = r.error;
    st.state  = r.print_state; // what Cmd 0 said, CANVAS or not
    if (!r.ok) {
        if (r.no_canvas)
            st.note = "The printer reports no CANVAS attached, so there is nothing to map. The plate is sent exactly as it was sliced.";
        else if (!r.reached)
            st.note = "this printer did not answer" + (r.error.empty() ? std::string() : (": " + r.error));
        else
            st.note = no_filament_note("elegoolink");
        return st;
    }
    st.canvas = true;
    for (const CanvasTray& t : r.info.trays) {
        Slot s;
        s.index    = t.slot_index;
        s.unit     = t.canvas_id;
        s.tray     = t.tray_id;
        s.loaded   = t.has_filament;
        s.min_temp = t.min_nozzle_temp;
        s.max_temp = t.nozzle_temp;
        if (t.has_filament) {
            s.type     = t.tray_type;
            s.sub_type = t.filament_name.empty() ? t.filament_type : t.filament_name;
            s.vendor   = t.brand;
            s.color    = t.colour_hex();
            s.name     = canvas_tray_name(t);
            st.slots_have_filaments = true;
        }
        if (t.canvas_id == r.info.active_canvas_id && t.tray_id == r.info.active_tray_id)
            st.active_slot = (int) st.slots.size();
        st.slots.push_back(std::move(s));
    }
    if (!st.slots_have_filaments)
        st.note = "The CANVAS reports no loaded trays, so there is nothing to map. The plate is sent exactly as it was sliced.";
    return st;
}

// A Centauri Carbon 1: its CANVAS over SDCP. Any other Elegoo model has nothing this fork can read.
static Status probe_elegoo(const Device& d, int timeout_s)
{
    if (!canvas_capable_model(d.printer_model)) {
        Status st;
        st.note = no_filament_note(d.host_type);
        return st;
    }
    CanvasQueryOptions opts;
    opts.frame_timeout_ms = std::max(1, timeout_s) * 1000;
    opts.step_timeout_ms  = std::max(2, 2 * timeout_s) * 1000;
    return status_from_canvas(canvas_query(d.address, opts));
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
    if (d.host_type == "elegoolink")
        return probe_elegoo(d, timeout_s);
    st.probed = true;
    if (PrusaLinkStatus::speaks_prusalink(d.host_type))
        return probe_prusalink(d, timeout_s);
    const std::string base = moonraker_base(d.address);
    if (base.empty()) {
        st.note = no_filament_note(d.host_type);
        return st;
    }
    // One call, read-only, never a command - the same objects RemoteControl::describe_hosts asks
    // for plus print_task_config (Snapmaker) and multi_color_controller (QIDI Box), which is where
    // the loaded filaments live when there are any, and the QIDI polar cooler's pin, which says
    // whether to offer its switch. Moonraker leaves out the ones a printer lacks.
    // A model that may carry a QIDI Box also gets save_variables in the same call: that is where
    // QIDIStudio reads the Box from, and where value_t<n> / enable_box live.
    const bool  qidi_model = qidi_box_capable_model(d.printer_model);
    std::string body;
    if (!moonraker_get(base + "/printer/objects/query?print_stats&print_task_config&multi_color_controller"
                              "&output_pin%20polar_cooler&extruder&extruder1&extruder2&extruder3" +
                           std::string(qidi_model ? "&save_variables=variables" : ""),
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
    json   status = j.is_object() ? j.value("result", json::object()).value("status", json::object()) : json::object();
    Status parsed = fill_from_status(status, d.host_type);
    if (parsed.qidi && !parsed.slots_have_filaments) {
        // The Box named no materials in multi_color_controller: read it as QIDIStudio does.
        if (qidi_variables_of(status) == nullptr) {
            std::string vars, verr;
            if (query_qidi_variables(base, vars, verr, timeout_s)) {
                try {
                    status["save_variables"] = json { { "variables", json::parse(vars) } };
                } catch (...) {}
            }
        }
        if (qidi_variables_present(qidi_variables_of(status))) {
            std::string cfg, cerr;
            QidiFilaList filas;
            if (moonraker_get(base + "/server/files/config/officiall_filas_list.cfg", cfg, cerr, timeout_s))
                filas = parse_qidi_filas_cfg(cfg);
            else
                BOOST_LOG_TRIVIAL(warning) << "PrintHostDevices::probe: could not read the QIDI filament list (" << cerr << ")";
            parsed = fill_from_status(status, d.host_type, &filas);
        }
    }
    parsed.probed  = true;
    parsed.error   = st.error;
    return parsed;
}

QidiFilaList parse_qidi_filas_cfg(const std::string& text)
{
    QidiFilaList out;
    std::string  section;
    size_t       pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos)
            end = text.size();
        std::string line = boost::algorithm::trim_copy(text.substr(pos, end - pos));
        pos              = end + 1;
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        if (line.front() == '[') {
            const size_t close = line.find(']');
            section            = boost::algorithm::trim_copy(line.substr(1, close == std::string::npos ? std::string::npos : close - 1));
            continue;
        }
        size_t eq = line.find_first_of("=:");
        if (eq == std::string::npos)
            continue;
        const std::string key   = boost::algorithm::trim_copy(line.substr(0, eq));
        const std::string value = boost::algorithm::trim_copy(line.substr(eq + 1));
        try {
            if (boost::istarts_with(section, "fila") && section.size() > 4) {
                const int id = std::stoi(section.substr(4));
                if (boost::iequals(key, "type"))
                    out.type[id] = value;
                else if (boost::iequals(key, "filament"))
                    out.name[id] = value;
            } else if (boost::iequals(section, "colordict")) {
                const std::string c = colour_of(value);
                if (!c.empty())
                    out.colour[std::stoi(key)] = c;
            }
        } catch (...) {}
    }
    return out;
}

// A save_variables value as a number: Klipper keeps what SAVE_VARIABLE was given, an int or a
// quoted string, so both are read.
static int var_int(const json& vars, const std::string& key, int fallback)
{
    if (!vars.is_object() || !vars.contains(key))
        return fallback;
    const json& v = vars[key];
    if (v.is_number())
        return v.get<int>();
    if (v.is_string()) {
        try {
            return std::stoi(v.get<std::string>());
        } catch (...) {}
    }
    return fallback;
}

static std::string var_str(const json& vars, const std::string& key)
{
    if (!vars.is_object() || !vars.contains(key))
        return {};
    const json& v = vars[key];
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_number_integer())
        return std::to_string(v.get<long long>());
    return {};
}

std::vector<Slot> slots_from_qidi_variables(const std::string& variables_json, const QidiFilaList& list, bool& have_filaments)
{
    std::vector<Slot> out;
    have_filaments = false;
    json vars;
    try {
        vars = json::parse(variables_json);
    } catch (...) {
        return out;
    }
    const int boxes = std::min(4, var_int(vars, "box_count", 0));
    for (int n = 0; n < boxes * 4; ++n) {
        Slot s;
        s.index         = n;
        s.name          = "Box " + std::to_string(n / 4 + 1) + " slot " + std::to_string(n % 4 + 1);
        const int fila  = var_int(vars, "filament_slot" + std::to_string(n), 0);
        const int color = var_int(vars, "color_slot" + std::to_string(n), -1);
        auto      t     = list.type.find(fila);
        if (fila > 0 && t != list.type.end() && !t->second.empty()) {
            s.loaded = true;
            s.type   = t->second;
            auto nm  = list.name.find(fila);
            if (nm != list.name.end())
                s.sub_type = nm->second;
            auto c = list.colour.find(color);
            if (c != list.colour.end())
                s.color = c->second;
            have_filaments = true;
        }
        out.push_back(std::move(s));
    }
    return out;
}

bool qidi_mapping_confirmed(const std::string& variables_json, const std::string& mapping, int use_box, std::string& why)
{
    json vars;
    try {
        vars = json::parse(variables_json);
    } catch (...) {
        why = "the printer's saved variables could not be read";
        return false;
    }
    if (!vars.is_object()) {
        why = "the printer's saved variables could not be read";
        return false;
    }
    if (use_box >= 0 || !mapping.empty()) {
        const int want = use_box == 0 ? 0 : 1;
        const int have = var_int(vars, "enable_box", -1);
        if (have != want) {
            why = "enable_box is " + (have < 0 ? std::string("not set") : std::to_string(have)) + ", not " + std::to_string(want);
            return false;
        }
    }
    if (use_box == 0)
        return true;
    std::string pair;
    for (size_t i = 0; i <= mapping.size(); ++i) {
        if (i < mapping.size() && mapping[i] != ',') {
            pair += mapping[i];
            continue;
        }
        const size_t colon = pair.find(':');
        if (colon != std::string::npos) {
            try {
                const int         tool = std::stoi(pair.substr(0, colon));
                const int         slot = std::stoi(pair.substr(colon + 1));
                const std::string have = var_str(vars, "value_t" + std::to_string(tool));
                const std::string want = "slot" + std::to_string(slot);
                if (have != want) {
                    why = "value_t" + std::to_string(tool) + " is '" + have + "', not '" + want + "'";
                    return false;
                }
            } catch (...) {}
        }
        pair.clear();
    }
    return true;
}

bool query_qidi_variables(const std::string& base_url, std::string& variables_json, std::string& error, int timeout_s)
{
    variables_json.clear();
    error.clear();
    std::string body;
    if (!moonraker_get(base_url + "/printer/objects/query?save_variables=variables", body, error, timeout_s))
        return false;
    try {
        const json  j      = json::parse(body);
        const json  status = j.is_object() ? j.value("result", json::object()).value("status", json::object()) : json::object();
        const json* vars   = qidi_variables_of(status);
        if (vars == nullptr) {
            error = "the printer has no save_variables";
            return false;
        }
        variables_json = vars->dump();
        return true;
    } catch (...) {
        error = "the answer was not JSON";
        return false;
    }
}

int probe_timeout_for(const std::string& printer_model)
{
    return qidi_box_capable_model(printer_model) ? 10 : 3;
}

Status parse_moonraker_status(const std::string& status_json, const std::string& host_type_key)
{
    json status;
    try {
        status = json::parse(status_json);
    } catch (...) {
        status = json::object();
    }
    Status st = fill_from_status(status, host_type_key);
    st.probed = can_probe(host_type_key);
    return st;
}

std::string qidi_box_mapping_script(const std::string& mapping)
{
    // value_t0..value_t15 and slot0..slot15 are what the firmware's save_variables declares.
    constexpr int max_tools = 16, max_slots = 16;
    std::string   lines;
    std::string   pair;
    for (size_t i = 0; i <= mapping.size(); ++i) {
        if (i < mapping.size() && mapping[i] != ',') {
            pair += mapping[i];
            continue;
        }
        const size_t colon = pair.find(':');
        if (colon != std::string::npos) {
            try {
                const int tool = std::stoi(pair.substr(0, colon));
                const int slot = std::stoi(pair.substr(colon + 1));
                // Quoted exactly as QIDIStudio's own SelectMachine quotes it, VALUE="'slotN'": the
                // double quotes are Klipper's shlex parse, the single ones SAVE_VARIABLE's
                // literal_eval, so the variable lands as the string slotN.
                if (tool >= 0 && tool < max_tools && slot >= 0 && slot < max_slots)
                    lines += "SAVE_VARIABLE VARIABLE=value_t" + std::to_string(tool) + " VALUE=\"'slot" + std::to_string(slot) + "'\"\n";
            } catch (...) {}
        }
        pair.clear();
    }
    if (lines.empty())
        return lines;
    return "SAVE_VARIABLE VARIABLE=enable_box VALUE=1\n" + lines;
}

bool busy_printing(const std::string& state)
{
    return boost::iequals(state, "printing") || boost::iequals(state, "paused");
}

std::string print_state_from_query(const std::string& reply_json)
{
    try {
        const json j = json::parse(reply_json);
        if (!j.is_object() || !j.contains("result") || !j["result"].is_object())
            return {};
        const json& status = j["result"].contains("status") ? j["result"]["status"] : json();
        if (!status.is_object() || !status.contains("print_stats"))
            return {};
        return str_at(status["print_stats"], "state");
    } catch (...) {
        return {};
    }
}

bool query_print_state(const std::string& base_url, std::string& state, std::string& error, int timeout_s)
{
    state.clear();
    error.clear();
    if (base_url.empty()) {
        error = "no address";
        return false;
    }
    std::string body;
    if (!moonraker_get(base_url + "/printer/objects/query?print_stats", body, error, timeout_s))
        return false;
    state = print_state_from_query(body);
    if (state.empty()) {
        error = "the printer did not say what it is doing";
        return false;
    }
    return true;
}

// The send dialog's swatch_colour: "#RRGGBB" (anything after the sixth digit ignored), grey for
// anything that is not a colour.
static void rgb_of(const std::string& raw, int& r, int& g, int& b)
{
    r = g = b = 160;
    std::string s = raw;
    if (!s.empty() && s[0] == '#')
        s = s.substr(1);
    if (s.size() < 6)
        return;
    int v[6];
    for (int i = 0; i < 6; ++i) {
        const char c = s[i];
        if (c >= '0' && c <= '9')
            v[i] = c - '0';
        else if (c >= 'a' && c <= 'f')
            v[i] = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            v[i] = c - 'A' + 10;
        else
            return;
    }
    r = v[0] * 16 + v[1];
    g = v[2] * 16 + v[3];
    b = v[4] * 16 + v[5];
}

// Redmean, the send dialog's colour_distance and SnapmakerLan::auto_match's, so the auto-matches
// agree about which slot is "the closest colour".
static double colour_distance(const std::string& a, const std::string& b)
{
    int ar, ag, ab, br, bg, bb;
    rgb_of(a, ar, ag, ab);
    rgb_of(b, br, bg, bb);
    const double rm = (ar + br) / 2.0;
    const double dr = ar - br, dg = ag - bg, db = ab - bb;
    return (2 + rm / 256) * dr * dr + 4 * dg * dg + (2 + (255 - rm) / 256) * db * db;
}

int auto_match_slot(const std::vector<Slot>& slots, const std::string& colour, const std::string& type, bool qidi_box)
{
    int    best   = 0;
    double best_d = 0;
    if (!qidi_box) {
        // Every other host: the closest colour among the loaded slots the printer gave a colour
        // for, the rule this dialog has always used.
        for (size_t i = 0; i < slots.size(); ++i) {
            const Slot& s = slots[i];
            if (!s.loaded || s.color.empty())
                continue;
            const double dist = colour_distance(colour, s.color);
            if (best == 0 || dist < best_d) {
                best   = (int) i + 1;
                best_d = dist;
            }
        }
        return best;
    }
    // A QIDI Box never matches on colour alone: its firmware heats for the plate's material
    // whatever is fed, so an ASA plate auto-sent to the PLA that happens to be the right shade is
    // a failed print. Only a slot holding the filament's own material is a candidate - the closest
    // colour among those, then one the printer gave no colour for - and "As sliced" otherwise, so
    // the person picks the slot.
    if (type.empty())
        return 0;
    for (size_t i = 0; i < slots.size(); ++i) {
        const Slot& s = slots[i];
        if (!s.loaded || !boost::iequals(type, s.type))
            continue;
        const double dist = s.color.empty() ? std::numeric_limits<double>::max() : colour_distance(colour, s.color);
        if (best == 0 || dist < best_d) {
            best   = (int) i + 1;
            best_d = dist;
        }
    }
    return best;
}

bool qidi_box_capable_model(const std::string& printer_model)
{
    // Spaces, dashes and underscores dropped, so "Qidi X-Max 4", "QIDI XMax4" and "qidi x-plus 4"
    // all read the same. The QIDI Box works with the Q2 (and Q2C), X-Max 4, X-Plus 4 and X-Plus 5.
    std::string m;
    for (char c : printer_model)
        if (c != ' ' && c != '-' && c != '_')
            m += (char) std::tolower((unsigned char) c);
    if (m.find("qidi") == std::string::npos)
        return false;
    for (const char* family : { "q2", "max4", "plus4", "plus5" })
        if (m.find(family) != std::string::npos)
            return true;
    return false;
}

bool qidi_polar_cooler_model(const std::string& printer_model)
{
    // QIDIStudio 2.07's machine profiles set is_support_polar_cooler for exactly the Box-capable
    // models (Q2, Q2C, fdm_machine_x_common -> X-Max 4 / X-Plus 4, X-Plus 5).
    return qidi_box_capable_model(printer_model);
}

bool filament_types_match(const std::string& plate_type, const std::string& slot_type)
{
    const std::string a = boost::algorithm::trim_copy(plate_type);
    const std::string b = boost::algorithm::trim_copy(slot_type);
    if (a.empty() || b.empty())
        return false;
    return boost::iequals(a, b);
}

SlotSendPolicy slot_send_policy(const SlotSendFacts& f)
{
    if (!f.probe_ok || !f.slot_unit)
        // Nobody knows what is in the slots (it did not answer, or answered without them). Where a
        // print must never start blind, upload only unless the person explicitly chooses to print;
        // anything else is not ours.
        return f.slots_required ? SlotSendPolicy::UploadOnlyNoProbe : SlotSendPolicy::NotApplicable;
    if (f.busy)
        return SlotSendPolicy::UploadOnlyBusy;
    // No slot unit feeding this print: nothing to map, nothing to go stale.
    if (!f.unit_in_use)
        return SlotSendPolicy::Start;
    // No used filament known at all is not "nothing to map": T0 still loads from somewhere.
    if (f.used_filaments <= 0 || f.mapped_filaments < f.used_filaments)
        return (f.mapped_filaments == 0 && f.as_sliced_ok) ? SlotSendPolicy::StartAsSliced : SlotSendPolicy::NeedsMapping;
    if (f.type_mismatches > 0)
        return SlotSendPolicy::ConfirmTypeMismatch;
    return SlotSendPolicy::Start;
}

QidiSendPolicy qidi_send_policy(const QidiSendFacts& f)
{
    SlotSendFacts s;
    // A model that may carry a Box is never started on a Box nobody read.
    s.slots_required = f.model_may_have_box;
    s.probe_ok       = f.probe_ok;
    // It answered, but without the multi_color_controller that names the Box: for a model that may
    // carry one that is the same as not reading it at all.
    s.slot_unit = f.qidi;
    s.busy      = f.busy;
    // No Box plugged in, or switched off for this print (enable_box 0 goes out): the extruder's own
    // spool feeds every tool, there is no saved value_t<n> to go stale.
    s.unit_in_use      = f.box_present && f.use_box;
    s.used_filaments   = f.used_filaments;
    s.mapped_filaments = f.mapped_filaments;
    s.type_mismatches  = f.type_mismatches;
    s.as_sliced_ok     = false; // a Box always feeds T<n> from value_t<n>: there is no safe "as sliced"
    return slot_send_policy(s);
}

QidiAutoMapping qidi_auto_mapping(const Status& st, const std::vector<SendFilament>& filaments)
{
    QidiAutoMapping out;
    QidiSendFacts&  f = out.facts;
    f.probe_ok    = st.online;
    f.qidi        = st.qidi;
    f.box_present = st.qidi_box;
    f.busy        = busy_printing(st.state);
    // The send dialog's own starting value (qidi_option_defaults): the Box on whenever there is one.
    f.use_box = st.qidi_box;
    for (const SendFilament& fil : filaments) {
        if (!fil.used)
            continue;
        ++f.used_filaments;
        if (!st.qidi_box)
            continue;
        const int pos = auto_match_slot(st.slots, fil.colour, fil.type, true);
        if (pos <= 0 || pos > (int) st.slots.size())
            continue;
        const Slot& s = st.slots[pos - 1];
        ++f.mapped_filaments;
        if (!filament_types_match(fil.type, s.type))
            ++f.type_mismatches;
        if (!out.mapping.empty())
            out.mapping += ",";
        out.mapping += std::to_string(fil.index) + ":" + std::to_string(s.index);
    }
    return out;
}

std::string qidi_send_refusal(QidiSendPolicy policy)
{
    switch (policy) {
    case QidiSendPolicy::UploadOnlyBusy:
        return "The QIDI printer is busy with another print, so this plate was not sent or started.";
    case QidiSendPolicy::UploadOnlyNoProbe:
        return "Couldn't read the QIDI Box, so the print was not started: the printer would feed whatever slots it used "
               "last time. Send it as an upload only, or retry once the printer answers.";
    case QidiSendPolicy::NeedsMapping:
        return "A filament on this plate has no QIDI Box slot of the same material, so the print was not started: the "
               "printer would feed whatever slot it used last time. Load that material in the Box, or send from the PC's "
               "send dialog and pick the slots there.";
    case QidiSendPolicy::ConfirmTypeMismatch:
        return "A filament on this plate would be fed from a QIDI Box slot holding another material, so the print was "
               "not started. Send it from the PC's send dialog to confirm the slots.";
    default:
        return {};
    }
}

std::string qidi_start_script(const QidiPrintOptions& options, const std::string& mapping)
{
    std::string out;
    if (options.use_box == 0) {
        // The Box off: the print runs from the extruder's own spool, and a mapping means nothing.
        out += "SAVE_VARIABLE VARIABLE=enable_box VALUE=0\n";
    } else {
        out += qidi_box_mapping_script(mapping);
        if (out.empty() && options.use_box == 1)
            out += "SAVE_VARIABLE VARIABLE=enable_box VALUE=1\n";
    }
    if (options.polar_cooler >= 0)
        out += std::string("SAVE_VARIABLE VARIABLE=enable_polar_cooler VALUE=") + (options.polar_cooler ? "1" : "0") + "\n";
    return out;
}

std::string qidi_leveling_script(const QidiPrintOptions& options)
{
    // The firmware's G31/G32 set _km_globals.bedmesh_before_print, which its G29 reads: probe a
    // fresh mesh before this print, or load the saved one.
    if (options.bed_leveling < 0)
        return {};
    return options.bed_leveling ? "G31\n" : "G32\n";
}

QidiOptionDefaults qidi_option_defaults(const Status& st, const std::string& remembered_leveling,
                                        const std::string& remembered_timelapse, const std::string& remembered_polar_cooler)
{
    auto remembered = [](const std::string& v, bool fallback) { return v.empty() ? fallback : v == "1"; };
    QidiOptionDefaults d;
    // A Box starts on whenever there is one, loaded or not, readable or not: printing from the
    // extruder's own spool (enable_box 0) feeds whatever is in the toolhead - possibly the last
    // print's material - so it is a choice the person makes by unticking, never a default.
    d.use_box = st.qidi_box;
    // Off unless the person turned it on last time: a mesh probe adds minutes to every print.
    d.bed_leveling = remembered(remembered_leveling, false);
    d.timelapse    = remembered(remembered_timelapse, false);
    d.polar_cooler = remembered(remembered_polar_cooler, false);
    return d;
}

} // namespace PrintHostDevices
} // namespace Slic3r
