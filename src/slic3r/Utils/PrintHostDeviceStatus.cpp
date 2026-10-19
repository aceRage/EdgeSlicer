#include "PrintHostDeviceStatus.hpp"

#include "CrealityPrint.hpp"
#include "CrealityPrintModel.hpp"
#include "Http.hpp"
#include "PrusaLinkStatus.hpp"

#include <boost/log/trivial.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

namespace Slic3r {
namespace PrintHostDevices {

using nlohmann::json;

// ------------------------------------------------------------------ helpers ----

std::string Slot::label() const
{
    if (cfs_box < 0 && unit < 0 && !name.empty()) {
        std::string out = name;
        if (loaded && !type.empty())
            out += " " + type;
        else if (!loaded)
            out += " (empty)";
        return out;
    }
    if (cfs_box >= 0) {
        std::string out = name.empty() ? (cfs_external ? std::string("External") :
                                                         std::string("CFS ") + char('A' + std::max(0, std::min(3, cfs_material)))) :
                                         name;
        if (loaded && !type.empty())
            out += " " + type;
        else if (!loaded)
            out += " (empty)";
        return out;
    }
    if (unit >= 0) {
        // A CANVAS tray: numbered as the printer's screen counts them, 1..4 per unit.
        std::string out = "Tray " + std::to_string(index + 1);
        if (loaded && !type.empty())
            out += " " + (sub_type.empty() ? type : sub_type);
        else if (!loaded)
            out += " (empty)";
        return out;
    }
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
           host_type_key == "elegoolink" || host_type_key == "crealityprint";
}

std::string no_filament_note(const std::string& host_type_key)
{
    if (host_type_key == "crealityprint")
        return "This Creality printer did not report CFS slots, so there is nothing to map. The plate is sent exactly as it was sliced.";
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
static Status fill_from_status(const json& status, const std::string& host_type_key)
{
    Status st;
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
        st.slots.push_back(std::move(s));
    }
    if (!st.slots_have_filaments)
        st.note = "The CANVAS reports no loaded trays, so there is nothing to map. The plate is sent exactly as it was sliced.";
    return st;
}

Status status_from_cfs(const std::string& boxs_info_json)
{
    Status st;
    st.probed = true;
    const CrealityPrintModel::CfsParseResult parsed = CrealityPrintModel::parse_cfs_response(boxs_info_json);
    if (!parsed.ok) {
        st.note  = no_filament_note("crealityprint");
        st.error = parsed.error;
        return st;
    }
    st.online        = true;
    st.cfs           = parsed.box_count > 0;
    st.cfs_box_count = parsed.box_count;
    st.cfs_variant   = parsed.variant;
    int idx          = 0;
    for (const CrealityPrintModel::CfsSlot& c : parsed.slots) {
        Slot s;
        s.index        = idx++;
        s.type         = c.filament_type;
        s.sub_type     = c.brand_name;
        s.vendor       = c.vendor;
        s.color        = c.color_hex;
        s.loaded       = c.loaded;
        s.cfs_box      = c.box_id;
        s.cfs_material = c.slot_id;
        s.cfs_external = c.external;
        if (c.external)
            s.name = "External";
        else {
            const std::string letter(1, static_cast<char>('A' + std::max(0, std::min(3, c.slot_id))));
            s.name = (c.variant.empty() ? std::string("CFS") : c.variant) + " " + std::to_string(c.box_id) + letter;
        }
        if (s.loaded && (!s.type.empty() || !s.color.empty()))
            st.slots_have_filaments = true;
        st.slots.push_back(std::move(s));
    }
    if (!st.cfs)
        st.note = no_filament_note("crealityprint");
    else if (!st.slots_have_filaments)
        st.note = "The CFS reports no loaded slots, so there is nothing to map. The plate is sent exactly as it was sliced.";
    return st;
}

Status status_from_direct_rack(const std::string& boxs_info_json)
{
    Status st;
    st.probed      = true;
    st.direct_rack = true;
    const CrealityPrintModel::DirectRackParseResult parsed = CrealityPrintModel::parse_direct_rack_response(boxs_info_json);
    if (!parsed.ok) {
        st.note  = CrealityPrintModel::k3_unreported_holders_note();
        st.error = parsed.error;
        return st;
    }
    st.online = true;
    int idx   = 0;
    for (const CrealityPrintModel::DirectRackSlot& h : parsed.slots) {
        Slot s;
        s.index  = idx;
        s.name   = CrealityPrintModel::k3_tool_label(idx);
        s.type   = h.filament_type;
        s.sub_type = h.brand_name;
        s.vendor = h.vendor;
        s.color  = h.color_hex;
        s.loaded = h.loaded;
        if (s.loaded && (!s.type.empty() || !s.color.empty()))
            st.slots_have_filaments = true;
        st.slots.push_back(std::move(s));
        ++idx;
    }
    if (!st.slots_have_filaments)
        st.note = CrealityPrintModel::k3_empty_holders_note();
    else
        st.note = CrealityPrintModel::k3_holders_note();
    return st;
}

int auto_match_slot(const std::string& type, const std::string& colour, const std::vector<Slot>& slots, bool cfs_order)
{
    std::vector<CrealityPrintModel::CfsSlot> cfs;
    cfs.reserve(slots.size());
    for (const Slot& s : slots) {
        CrealityPrintModel::CfsSlot c;
        c.filament_type = s.type;
        c.color_hex     = s.color;
        c.loaded        = s.loaded;
        c.external      = cfs_order ? s.cfs_external : false;
        cfs.push_back(std::move(c));
    }
    return CrealityPrintModel::auto_match_index(type, colour, cfs);
}

// CrealityPrint: /info (reachability + model) then boxsInfo. Both use the caller's deadline
// (about 1 s from the send dialog). An unreachable or silent printer returns when that expires.
static Status probe_creality(const Device& d, int timeout_s)
{
    Status st;
    st.probed = true;
    std::string body;
    std::string info_err;
    const std::string host = d.address;
    if (host.empty()) {
        st.note = "this device has no address";
        return st;
    }
    const int wait = std::max(1, timeout_s);
    if (!CrealityPrint::fetch_info(host, body, info_err, wait)) {
        st.error = info_err;
        st.note  = "this printer did not answer" + (info_err.empty() ? std::string() : (": " + info_err));
        return st;
    }
    st.online               = true;
    const std::string model = CrealityPrintModel::resolve_model(CrealityPrintModel::parse_model_from_info(body),
                                                                d.printer_model);
    if (CrealityPrintModel::is_direct_rack_model(model)) {
        const std::string boxs = CrealityPrint::query_boxes_info(host, wait);
        if (boxs.empty()) {
            st.online = true;
            st.note   = CrealityPrintModel::k3_unreported_holders_note();
            return st;
        }
        Status parsed = status_from_direct_rack(boxs);
        parsed.probed = true;
        parsed.online = true;
        return parsed;
    }
    if (!CrealityPrintModel::model_supports_multi_color(model) && !CrealityPrintModel::looks_like_k1_family(model)) {
        st.note = no_filament_note("crealityprint");
        return st;
    }
    const std::string boxs = CrealityPrint::query_boxes_info(host, wait);
    if (boxs.empty()) {
        st.note = no_filament_note("crealityprint");
        return st;
    }
    Status parsed = status_from_cfs(boxs);
    parsed.probed = true;
    parsed.online = true;
    return parsed;
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
    if (d.host_type == "crealityprint")
        return probe_creality(d, timeout_s);
    st.probed = true;
    if (PrusaLinkStatus::speaks_prusalink(d.host_type))
        return probe_prusalink(d, timeout_s);
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
    Status parsed = fill_from_status(status, d.host_type);
    parsed.probed  = true;
    parsed.error   = st.error;
    return parsed;
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

} // namespace PrintHostDevices
} // namespace Slic3r
