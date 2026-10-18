// The pure half of the CANVAS support declared in ElegooCanvas.hpp: parsing a command-324 payload,
// normalising what it carries, and the Cmd 0 -> Cmd 324 conversation over an abstract transport. No
// socket lives here, so the unit tests drive all of it with a scripted transport - the WebSocket
// itself sits in ElegooCanvasQuery.cpp.

#include "ElegooCanvas.hpp"

#include <nlohmann/json.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <regex>

namespace Slic3r {

namespace {

using cvjson = nlohmann::json;

std::string trim_and_upper(const std::string& input)
{
    std::string result = input;
    boost::trim(result);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return result;
}

std::string cv_trimmed(const std::string& s)
{
    std::string r = s;
    boost::trim(r);
    return r;
}

// The payload may arrive bare, wrapped once in "Data", or wrapped twice -
// which of the three depends on the firmware. Unwrap to whichever object
// actually carries canvas_list.
const cvjson* find_canvas_object(const cvjson& j)
{
    if (!j.is_object())
        return nullptr;
    if (j.contains("canvas_list"))
        return &j;
    if (j.contains("Data") && j["Data"].is_object()) {
        const cvjson& d1 = j["Data"];
        if (d1.contains("canvas_list"))
            return &d1;
        if (d1.contains("Data") && d1["Data"].is_object() && d1["Data"].contains("canvas_list"))
            return &d1["Data"];
    }
    return nullptr;
}

// A number, a bool or a numeric string; `def` for anything else. Firmware has been seen to send
// both 1 and "1" for the same field on other SDCP printers.
int cv_int_at(const cvjson& j, const char* key, int def)
{
    if (!j.is_object())
        return def;
    auto it = j.find(key);
    if (it == j.end())
        return def;
    if (it->is_number_integer() || it->is_number_unsigned())
        return it->get<int>();
    if (it->is_number_float())
        return static_cast<int>(it->get<double>());
    if (it->is_boolean())
        return it->get<bool>() ? 1 : 0;
    if (it->is_string()) {
        try {
            size_t      used = 0;
            const int   v    = std::stoi(it->get<std::string>(), &used);
            return used > 0 ? v : def;
        } catch (...) {
            return def;
        }
    }
    return def;
}

std::string cv_str_at(const cvjson& j, const char* key)
{
    if (!j.is_object())
        return std::string();
    auto it = j.find(key);
    if (it == j.end())
        return std::string();
    if (it->is_string())
        return cv_trimmed(it->get<std::string>());
    if (it->is_number())
        return it->dump();
    return std::string();
}

std::string cv_new_request_id()
{
    std::string id = boost::uuids::to_string(boost::uuids::random_generator()());
    id.erase(std::remove(id.begin(), id.end(), '-'), id.end());
    return id;
}

long long cv_now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

using CvClock = std::chrono::steady_clock;

int cv_remaining_ms(CvClock::time_point deadline)
{
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - CvClock::now()).count();
    return left > 0 ? static_cast<int>(left) : 0;
}

// The Ack of a response frame to `cmd`, -1 when the frame is not one ({"Data":{"Cmd":cmd,"Data":{"Ack":n}}}).
int cv_ack_for(const cvjson& frame, int cmd)
{
    if (!frame.is_object() || !frame.contains("Data") || !frame["Data"].is_object())
        return -1;
    const cvjson& d = frame["Data"];
    if (cv_int_at(d, "Cmd", -1) != cmd)
        return -1;
    if (!d.contains("Data") || !d["Data"].is_object())
        return -1;
    return cv_int_at(d["Data"], "Ack", -1);
}

int cv_find_ams(const cvjson& j, int depth)
{
    if (depth > 4 || !j.is_object())
        return -1;
    if (j.contains("AmsConnectStatus")) {
        const int v = cv_int_at(j, "AmsConnectStatus", -1);
        if (v >= 0)
            return v == 0 ? 0 : 1;
    }
    for (auto it = j.begin(); it != j.end(); ++it)
        if (it->is_object()) {
            const int v = cv_find_ams(*it, depth + 1);
            if (v >= 0)
                return v;
        }
    return -1;
}

const char* cv_state_name(CanvasTrayState s)
{
    switch (s) {
    case CanvasTrayState::Empty: return "empty";
    case CanvasTrayState::Preloaded: return "preloaded";
    case CanvasTrayState::Loaded: return "loaded";
    default: return "unknown";
    }
}

} // namespace

// ----------------------------------------------------------------------------- the model ----

CanvasTrayState canvas_tray_state(int status)
{
    if (status < 0)
        return CanvasTrayState::Unknown;
    if (status == 0)
        return CanvasTrayState::Empty;
    if (status == 1)
        return CanvasTrayState::Preloaded;
    return CanvasTrayState::Loaded;
}

CanvasTrayState CanvasTray::state() const { return canvas_tray_state(status); }

std::string CanvasTray::colour_hex() const
{
    if (!has_filament || tray_color.size() < 6)
        return std::string();
    return "#" + tray_color.substr(0, 6);
}

int CanvasInfo::loaded_count() const
{
    return static_cast<int>(std::count_if(trays.begin(), trays.end(), [](const CanvasTray& t) { return t.has_filament; }));
}

const CanvasTray* CanvasInfo::find(int canvas_id, int tray_id) const
{
    for (const CanvasTray& t : trays)
        if (t.canvas_id == canvas_id && t.tray_id == tray_id)
            return &t;
    return nullptr;
}

std::string canvas_normalize_color(const std::string& color)
{
    std::string value = color;
    boost::trim(value);

    if (value.size() >= 2 && (value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0))
        value = value.substr(2);
    if (!value.empty() && value[0] == '#')
        value = value.substr(1);

    std::string normalized;
    for (char c : value) {
        if (std::isxdigit(static_cast<unsigned char>(c)))
            normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }

    // RRGGBB is the common case; this tree stores colours with an alpha byte.
    if (normalized.size() == 6)
        normalized += "FF";

    if (normalized.size() != 8)
        return "00000000";

    return normalized;
}

std::string canvas_normalize_filament_type(const std::string& filament_type)
{
    const std::string upper = trim_and_upper(filament_type);

    // Order matters where one name contains another: "PETG" must be tested
    // before "PET", and "PC" before "PA" so a "PC-FR" is not read as a nylon.
    // Everything else (PLA Silk, PLA-CF, ASA-CF, TPU 95A, PAHT-CF, PPA-CF...)
    // collapses onto its family by substring.
    if (upper.find("PLA") != std::string::npos)   return "PLA";
    if (upper.find("PETG") != std::string::npos)  return "PETG";
    if (upper.find("PET") != std::string::npos)   return "PETG";
    if (upper.find("ABS") != std::string::npos)   return "ABS";
    if (upper.find("ASA") != std::string::npos)   return "ASA";
    if (upper.find("TPU") != std::string::npos)   return "TPU";
    if (upper.find("PVA") != std::string::npos)   return "PVA";
    if (upper.find("HIPS") != std::string::npos)  return "HIPS";
    if (upper.find("PC") != std::string::npos)    return "PC";
    if (upper.find("PA") != std::string::npos || upper.find("NYLON") != std::string::npos)
        return "PA";

    return upper;
}

std::string canvas_generic_filament_id(const std::string& filament_type)
{
    const std::string upper = trim_and_upper(filament_type);

    // OrcaFilamentLibrary generic ids. Only the families the CANVAS normaliser
    // can actually produce are listed; anything else stays unmapped.
    if (upper == "PLA")   return "OGFL99";
    if (upper == "PETG")  return "OGFG99";
    if (upper == "ABS")   return "OGFB99";
    if (upper == "ASA")   return "OGFB98";
    if (upper == "TPU")   return "OGFU99";
    if (upper == "PA")    return "OGFN99";
    if (upper == "PC")    return "OGFC99";
    if (upper == "PVA")   return "OGFS99";
    if (upper == "HIPS")  return "OGFS98";

    return std::string();
}

std::string canvas_clean_brand(const std::string& brand)
{
    // The owner's CC1 (firmware V1.4.49) reports an untagged or hand-entered spool's brand as
    // four em dashes (U+2014) with spaces between them. A brand made only of dashes and blanks is
    // no brand.
    const std::string b = cv_trimmed(brand);
    for (size_t i = 0; i < b.size();) {
        const unsigned char c = static_cast<unsigned char>(b[i]);
        if (c == ' ' || c == '\t' || c == '-' || c == '_') {
            ++i;
            continue;
        }
        // UTF-8 U+2010..U+2015 (hyphen, figure dash, en dash, em dash, horizontal bar) and U+2212 (minus).
        if (c == 0xE2 && i + 2 < b.size()) {
            const unsigned char c1 = static_cast<unsigned char>(b[i + 1]);
            const unsigned char c2 = static_cast<unsigned char>(b[i + 2]);
            if ((c1 == 0x80 && c2 >= 0x90 && c2 <= 0x95) || (c1 == 0x88 && c2 == 0x92)) {
                i += 3;
                continue;
            }
        }
        // U+00A0, a non-breaking space.
        if (c == 0xC2 && i + 1 < b.size() && static_cast<unsigned char>(b[i + 1]) == 0xA0) {
            i += 2;
            continue;
        }
        return b;
    }
    return std::string();
}

std::string canvas_clean_code(const std::string& code)
{
    // "0x00000" (any number of zeros) = the CANVAS read no RFID tag.
    std::string c = cv_trimmed(code);
    std::string digits = (c.size() >= 2 && (c.rfind("0x", 0) == 0 || c.rfind("0X", 0) == 0)) ? c.substr(2) : c;
    if (digits.empty() || digits.find_first_not_of('0') == std::string::npos)
        return std::string();
    return c;
}

std::string canvas_tray_vendor(const CanvasTray& tray)
{
    const std::string brand = canvas_clean_brand(tray.brand);
    return brand.empty() ? std::string("Generic") : brand;
}

std::string canvas_tray_name(const CanvasTray& tray)
{
    if (!tray.has_filament)
        return std::string();
    const std::string brand = canvas_clean_brand(tray.brand);
    std::string       name  = cv_trimmed(tray.filament_name);
    if (name.empty())
        name = cv_trimmed(tray.filament_type);
    if (name.empty())
        name = tray.tray_type;
    // No brand (an untagged or hand-entered spool): just what the printer calls the filament. The
    // preset match then goes by the "Generic" vendor and the type.
    if (brand.empty())
        return name;
    if (boost::iequals(brand, "Generic"))
        return "Generic " + tray.tray_type;
    // "ELEGOO" + "ELEGOO PLA" stays "ELEGOO PLA".
    if (boost::istarts_with(name, brand))
        return name;
    return brand + " " + name;
}

bool canvas_parse_payload(const std::string& payload, CanvasInfo& info, std::string& error)
{
    info = CanvasInfo();

    cvjson j = cvjson::parse(payload, nullptr, false, true);
    if (j.is_discarded()) {
        error = "CANVAS reply is not valid JSON";
        return false;
    }

    const cvjson* obj = find_canvas_object(j);
    if (obj == nullptr) {
        error = "CANVAS reply carries no canvas_list";
        return false;
    }

    const cvjson& canvas_list = (*obj)["canvas_list"];
    if (!canvas_list.is_array() || canvas_list.empty()) {
        error = "canvas_list is empty or not an array";
        return false;
    }

    info.unit_count       = static_cast<int>(canvas_list.size());
    info.active_canvas_id = cv_int_at(*obj, "active_canvas_id", -1);
    info.active_tray_id   = cv_int_at(*obj, "active_tray_id", -1);
    info.auto_refill      = cv_int_at(*obj, "auto_refill", -1);

    // Slots are numbered across units by position, so a disconnected unit still consumes its share
    // of the numbering - otherwise unplugging unit 1 would silently renumber every slot behind it.
    // The raw canvas_id / tray_id pair is kept beside it for the start-print slot_map, whatever base
    // the firmware counts tray_id from.
    int slot_offset = 0;
    int unit_pos    = 0;
    for (const auto& canvas : canvas_list) {
        CanvasUnit unit;
        unit.canvas_id = cv_int_at(canvas, "canvas_id", unit_pos);
        unit.connected = cv_int_at(canvas, "connected", 0) != 0;
        ++unit_pos;

        if (!canvas.is_object() || !canvas.contains("tray_list") || !canvas["tray_list"].is_array()) {
            info.units.push_back(unit);
            continue;
        }

        const auto& tray_list = canvas["tray_list"];
        unit.tray_count       = static_cast<int>(tray_list.size());
        info.units.push_back(unit);
        if (!unit.connected) {
            slot_offset += unit.tray_count;
            continue;
        }

        int pos = 0;
        for (const auto& tray : tray_list) {
            CanvasTray t;
            t.slot_index      = slot_offset + pos;
            t.canvas_id       = unit.canvas_id;
            t.tray_id         = cv_int_at(tray, "tray_id", pos);
            t.position        = pos;
            t.status          = cv_int_at(tray, "status", -1);
            t.filament_type   = cv_str_at(tray, "filament_type");
            // A placeholder brand (em dashes) or the no-tag code ("0x00000") is kept as nothing, so
            // the sync name, the Device tab card and the preset match never show or match it.
            t.brand           = canvas_clean_brand(cv_str_at(tray, "brand"));
            t.filament_name   = cv_str_at(tray, "filament_name");
            t.filament_code   = canvas_clean_code(cv_str_at(tray, "filament_code"));
            t.min_nozzle_temp = cv_int_at(tray, "min_nozzle_temp", 0);
            t.nozzle_temp     = cv_int_at(tray, "max_nozzle_temp", 0);
            ++pos;

            // Presence is "the printer named a material and a colour". The raw status stays as data
            // (CanvasTray::state) because its meaning differs between firmware and sources: a public
            // capture shows loaded trays at status 0, elegoo-link reads 0 as empty.
            const std::string raw_colour = cv_str_at(tray, "filament_color");
            const std::string colour     = canvas_normalize_color(raw_colour);
            t.has_filament               = !t.filament_type.empty() && !raw_colour.empty() && colour != "00000000";

            if (t.has_filament) {
                t.tray_type     = canvas_normalize_filament_type(t.filament_type);
                t.tray_color    = colour;
                t.tray_info_idx = canvas_generic_filament_id(t.tray_type);
            }

            info.trays.emplace_back(std::move(t));
        }

        slot_offset += unit.tray_count;
    }

    return true;
}

bool canvas_capable_model(const std::string& printer_model)
{
    return boost::iequals(cv_trimmed(printer_model), "Elegoo Centauri Carbon");
}

std::string sdcp_host_of(const std::string& address)
{
    std::string h = cv_trimmed(address);
    const size_t scheme = h.find("://");
    if (scheme != std::string::npos)
        h = h.substr(scheme + 3);
    const size_t at = h.find('@');
    if (at != std::string::npos && at < h.find('/'))
        h = h.substr(at + 1);
    const size_t slash = h.find_first_of("/?#");
    if (slash != std::string::npos)
        h = h.substr(0, slash);
    if (!h.empty() && h[0] == '[') {
        const size_t close = h.find(']');
        return close == std::string::npos ? h.substr(1) : h.substr(1, close - 1);
    }
    // host:port, but not a bare IPv6 address (more than one colon).
    const size_t colon = h.find(':');
    if (colon != std::string::npos && h.find(':', colon + 1) == std::string::npos)
        h = h.substr(0, colon);
    return h;
}

std::string sdcp_request(int cmd, const std::string& request_id, long long timestamp_ms)
{
    using ojson = nlohmann::ordered_json;
    ojson inner;
    inner["Cmd"]         = cmd;
    inner["Data"]        = ojson::object();
    inner["RequestID"]   = request_id;
    inner["MainboardID"] = "";
    inner["TimeStamp"]   = timestamp_ms;
    inner["From"]        = 1;
    ojson root;
    root["Id"]   = "";
    root["Data"] = std::move(inner);
    return root.dump();
}

int sdcp_ams_connect_status(const std::string& frame)
{
    const cvjson j = cvjson::parse(frame, nullptr, false, true);
    if (j.is_discarded())
        return -1;
    return cv_find_ams(j, 0);
}

// ---------------------------------------------------------------------------- redaction ----

namespace {

bool cv_identifying_key(const std::string& key)
{
    const std::string k = boost::to_lower_copy(key);
    static const char* exact[] = { "id", "sn", "requestid", "topic", "name", "filename", "taskid", "serial", "uuid", "ssid", "host" };
    for (const char* e : exact)
        if (k == e)
            return true;
    static const char* parts[] = { "mainboard", "serial", "password", "token", "ssid", "wifi" };
    for (const char* p : parts)
        if (k.find(p) != std::string::npos)
            return true;
    return boost::ends_with(k, "ip") || boost::ends_with(k, "mac") || boost::ends_with(k, "address");
}

std::string cv_redact_string(const std::string& s)
{
    static const std::regex ipv4(R"((\d{1,3}\.){3}\d{1,3})");
    static const std::regex mac(R"(([0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2})");
    static const std::regex long_hex(R"([0-9A-Fa-f]{16,})");
    std::string out = std::regex_replace(s, mac, "<mac>");
    out             = std::regex_replace(out, ipv4, "<ip>");
    out             = std::regex_replace(out, long_hex, "<id>");
    return out;
}

void cv_redact_json(cvjson& j)
{
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            // A version string is not an identity, and "1.1.40.2" must not turn into "<ip>".
            if (it->is_string() && boost::icontains(it.key(), "version"))
                continue;
            if (cv_identifying_key(it.key())) {
                // Keep the shape (an empty field stays recognisably empty) but never the value.
                if (it->is_string())
                    *it = it->get<std::string>().empty() ? std::string() : std::string("<redacted>");
                else if (!it->is_null())
                    *it = "<redacted>";
                continue;
            }
            cv_redact_json(*it);
        }
    } else if (j.is_array()) {
        for (auto& v : j)
            cv_redact_json(v);
    } else if (j.is_string()) {
        j = cv_redact_string(j.get<std::string>());
    }
}

cvjson cv_redacted_json(const std::string& frame)
{
    cvjson j = cvjson::parse(frame, nullptr, false, true);
    if (j.is_discarded())
        return cvjson("<non-JSON frame, " + std::to_string(frame.size()) + " bytes>");
    cv_redact_json(j);
    return j;
}

cvjson cv_parsed_to_json(const CanvasInfo& info)
{
    cvjson out;
    out["unit_count"]       = info.unit_count;
    out["active_canvas_id"] = info.active_canvas_id;
    out["active_tray_id"]   = info.active_tray_id;
    out["auto_refill"]      = info.auto_refill;
    cvjson units              = cvjson::array();
    for (const CanvasUnit& u : info.units)
        units.push_back({ { "canvas_id", u.canvas_id }, { "connected", u.connected }, { "tray_count", u.tray_count } });
    out["units"] = std::move(units);
    cvjson trays   = cvjson::array();
    for (const CanvasTray& t : info.trays)
        trays.push_back({ { "slot_index", t.slot_index },
                          { "canvas_id", t.canvas_id },
                          { "tray_id", t.tray_id },
                          { "position", t.position },
                          { "status", t.status },
                          { "state", cv_state_name(t.state()) },
                          { "has_filament", t.has_filament },
                          { "filament_type", t.filament_type },
                          { "family", t.tray_type },
                          { "brand", t.brand },
                          { "filament_name", t.filament_name },
                          { "filament_code", t.filament_code },
                          { "colour", t.tray_color },
                          { "min_nozzle_temp", t.min_nozzle_temp },
                          { "max_nozzle_temp", t.nozzle_temp },
                          { "sync_name", canvas_tray_name(t) } });
    out["trays"] = std::move(trays);
    return out;
}

} // namespace

std::string canvas_redact_frame(const std::string& frame)
{
    const cvjson j = cv_redacted_json(frame);
    return j.is_string() ? j.get<std::string>() : j.dump();
}

// ----------------------------------------------------------------------- the conversation ----

CanvasQueryResult canvas_query_over(SdcpTransport& transport, const CanvasQueryOptions& opts)
{
    CanvasQueryResult result;
    const std::string request_id = opts.request_id.empty() ? cv_new_request_id() : opts.request_id;
    const long long   ts         = opts.timestamp_ms != 0 ? opts.timestamp_ms : cv_now_ms();
    std::string       err;

    // ---- Cmd 0: is a CANVAS attached? ----
    if (!transport.open(err)) {
        result.error = "the printer did not accept a connection: " + err;
        return result;
    }
    result.reached = true;
    if (!transport.send(sdcp_request(0, request_id, ts), err)) {
        transport.close();
        result.error = "the status request could not be sent: " + err;
        return result;
    }
    {
        const auto deadline = CvClock::now() + std::chrono::milliseconds(opts.step_timeout_ms);
        for (int i = 0; i < opts.max_frames; ++i) {
            const int left = std::min(opts.frame_timeout_ms, cv_remaining_ms(deadline));
            std::string frame;
            if (left <= 0 || !transport.receive(frame, left, err))
                break;
            const int st = sdcp_ams_connect_status(frame);
            if (st >= 0) {
                result.ams_connect_status = st;
                break;
            }
        }
    }
    transport.close();
    if (result.ams_connect_status == 0) {
        result.no_canvas = true;
        result.error     = "no CANVAS is attached (AmsConnectStatus 0)";
        return result;
    }

    // ---- Cmd 324: what is in it? A fresh connection: the CC1 often closes after answering. ----
    if (!transport.open(err)) {
        result.error = "the printer did not accept a second connection: " + err;
        return result;
    }
    if (!transport.send(sdcp_request(324, request_id, ts), err)) {
        transport.close();
        result.error = "the CANVAS request could not be sent: " + err;
        return result;
    }
    std::string last_error = "no CANVAS payload in the reply";
    const auto  deadline   = CvClock::now() + std::chrono::milliseconds(opts.step_timeout_ms);
    for (int i = 0; i < opts.max_frames; ++i) {
        const int left = std::min(opts.frame_timeout_ms, cv_remaining_ms(deadline));
        std::string frame;
        if (left <= 0) {
            last_error = "no CANVAS payload before the timeout";
            break;
        }
        if (!transport.receive(frame, left, err)) {
            last_error = "no CANVAS payload: " + err;
            break;
        }
        std::string parse_error;
        if (canvas_parse_payload(frame, result.info, parse_error)) {
            result.ok = true;
            break;
        }
        const cvjson j   = cvjson::parse(frame, nullptr, false, true);
        const int  ack = j.is_discarded() ? -1 : cv_ack_for(j, 324);
        if (ack > 0) {
            last_error = "the printer refused the CANVAS request (Ack " + std::to_string(ack) + ")";
            break;
        }
    }
    transport.close();
    if (!result.ok)
        result.error = last_error;
    return result;
}

std::string canvas_capture_over(SdcpTransport& transport, const std::string& context_json, const CanvasQueryOptions& opts)
{
    const std::string request_id = opts.request_id.empty() ? cv_new_request_id() : opts.request_id;
    const long long   ts         = opts.timestamp_ms != 0 ? opts.timestamp_ms : cv_now_ms();

    cvjson doc;
    doc["format"] = "edgeslicer-canvas-capture-1";
    doc["note"]   = "SDCP replies of an Elegoo printer for the CANVAS feature. Mainboard ids, serial numbers, "
                    "IP/MAC addresses, the printer name, topics and file names are removed.";
    {
        cvjson ctx = cvjson::parse(context_json, nullptr, false, true);
        doc["context"] = (ctx.is_discarded() || !ctx.is_object()) ? cvjson::object() : ctx;
    }

    cvjson        steps = cvjson::array();
    int         ams   = -1;
    std::string firmware;
    CanvasInfo  parsed;
    bool        have_parsed = false;
    std::string parse_error = "no Cmd 324 payload";
    bool        reachable   = true;

    for (int cmd : { 0, 1, 324 }) {
        cvjson step;
        step["cmd"]     = cmd;
        step["request"] = cvjson::parse(sdcp_request(cmd, "<request-id>", 0));
        cvjson        frames = cvjson::array();
        std::string err;
        if (!reachable) {
            step["error"] = "skipped: the printer did not accept a connection";
        } else if (!transport.open(err)) {
            step["error"] = "connect: " + cv_redact_string(err);
            reachable     = false;
        } else if (!transport.send(sdcp_request(cmd, request_id, ts), err)) {
            step["error"] = "send: " + cv_redact_string(err);
            transport.close();
        } else {
            const auto deadline = CvClock::now() + std::chrono::milliseconds(opts.step_timeout_ms);
            bool       done     = false;
            for (int i = 0; i < opts.max_frames && !done; ++i) {
                const int left = std::min(opts.frame_timeout_ms, cv_remaining_ms(deadline));
                std::string frame;
                if (left <= 0) {
                    step["end"] = "timeout";
                    break;
                }
                if (!transport.receive(frame, left, err)) {
                    step["end"] = "receive: " + cv_redact_string(err);
                    break;
                }
                frames.push_back(cv_redacted_json(frame));
                const cvjson j = cvjson::parse(frame, nullptr, false, true);
                if (cmd == 0) {
                    const int st = j.is_discarded() ? -1 : cv_find_ams(j, 0);
                    if (st >= 0) {
                        ams  = st;
                        done = true;
                    }
                } else if (cmd == 1) {
                    if (!j.is_discarded() && j.is_object() && j.contains("Attributes") && j["Attributes"].is_object()) {
                        firmware = cv_str_at(j["Attributes"], "FirmwareVersion");
                        done     = true;
                    }
                } else {
                    CanvasInfo  info;
                    std::string perr;
                    if (canvas_parse_payload(frame, info, perr)) {
                        parsed      = std::move(info);
                        have_parsed = true;
                        done        = true;
                    } else {
                        parse_error = perr;
                    }
                }
            }
            if (done)
                step["end"] = "answered";
            transport.close();
        }
        step["frames"] = std::move(frames);
        steps.push_back(std::move(step));
    }

    doc["steps"]              = std::move(steps);
    doc["ams_connect_status"] = ams;
    doc["firmware_version"]   = firmware;
    if (have_parsed)
        doc["parsed"] = cv_parsed_to_json(parsed);
    else
        doc["parsed_error"] = parse_error;
    return doc.dump(2, ' ', false, cvjson::error_handler_t::replace);
}

} // namespace Slic3r
