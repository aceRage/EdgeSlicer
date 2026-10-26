// The pure half of ElegooCc2.hpp: messages, parsing and the MQTT conversation over an abstract
// transport. The sockets live in ElegooCc2Net.cpp.

#include "ElegooCc2.hpp"
#include "ElegooStartPrint.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <mutex>
#include <random>
#include <thread>

#include <boost/algorithm/string.hpp>
#include <nlohmann/json.hpp>

namespace Slic3r {
namespace ElegooCc2 {

namespace {

using ojson = nlohmann::ordered_json;
using json  = nlohmann::json;

std::string trimmed(const std::string& s)
{
    return boost::algorithm::trim_copy(s);
}

json parse_object(const std::string& text)
{
    json j = json::parse(text, nullptr, false, true);
    if (j.is_discarded() || !j.is_object())
        return json();
    return j;
}

int int_at(const json& j, const char* key, int def)
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
            size_t used = 0;
            const int v = std::stoi(it->get<std::string>(), &used);
            if (used == it->get<std::string>().size())
                return v;
        } catch (...) {}
    }
    return def;
}

std::string str_at(const json& j, const char* key)
{
    if (!j.is_object())
        return std::string();
    auto it = j.find(key);
    if (it == j.end() || !it->is_string())
        return std::string();
    return it->get<std::string>();
}

long long steady_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int random_in(int lo, int hi)
{
    static std::mutex   mtx;
    std::lock_guard<std::mutex> lock(mtx);
    static std::mt19937 gen { std::random_device {}() };
    std::uniform_int_distribution<int> dis(lo, hi);
    return dis(gen);
}

} // namespace

bool is_cc2_model(const std::string& printer_model)
{
    const std::string m = trimmed(printer_model);
    return boost::iequals(m, "Elegoo Centauri Carbon 2") || boost::iequals(m, "Elegoo Centauri 2");
}

std::string token_or_default(const std::string& apikey)
{
    const std::string t = trimmed(apikey);
    return t.empty() ? std::string(DEFAULT_TOKEN) : t;
}

std::string host_of(const std::string& address)
{
    return sdcp_host_of(address);
}

bool parse_system_info(const std::string& body, std::string& serial, std::string& error)
{
    serial.clear();
    const json j = parse_object(body);
    if (j.is_null()) {
        error = "the printer's answer is not JSON";
        return false;
    }
    const int code = int_at(j, "error_code", -1);
    if (code != 0) {
        error = error_text(code);
        return false;
    }
    const json system_info = j.value("system_info", json());
    if (!system_info.is_object()) {
        error = "the printer's answer carries no system_info";
        return false;
    }
    serial = trimmed(str_at(system_info, "sn"));
    if (serial.empty()) {
        error = "the printer's answer carries no serial number";
        return false;
    }
    return true;
}

bool parse_upload_reply(const std::string& body, std::string& error)
{
    const json j = parse_object(body);
    if (j.is_null()) {
        error = "the printer's answer is not JSON";
        return false;
    }
    if (!j.contains("error_code")) {
        error = "the printer's answer carries no error_code";
        return false;
    }
    const int code = int_at(j, "error_code", -1);
    if (code != 0) {
        error = error_text(code);
        return false;
    }
    return true;
}

std::string content_range(size_t offset, size_t length, size_t total)
{
    const size_t last = length == 0 ? offset : offset + length - 1;
    return "bytes " + std::to_string(offset) + "-" + std::to_string(last) + "/" + std::to_string(total);
}

std::string make_client_id(int n) { return "1_PC_" + std::to_string(n); }
std::string request_id_for(const std::string& client_id) { return client_id + "_req"; }
std::string register_topic(const std::string& sn) { return "elegoo/" + sn + "/api_register"; }
std::string register_response_topic(const std::string& sn, const std::string& request_id)
{
    return "elegoo/" + sn + "/" + request_id + "/register_response";
}
std::string request_topic(const std::string& sn, const std::string& client_id) { return "elegoo/" + sn + "/" + client_id + "/api_request"; }
std::string response_topic(const std::string& sn, const std::string& client_id) { return "elegoo/" + sn + "/" + client_id + "/api_response"; }

std::string register_payload(const std::string& client_id, const std::string& request_id)
{
    ojson j;
    j["client_id"]  = client_id;
    j["request_id"] = request_id;
    return j.dump();
}

RegisterResult parse_register_response(const std::string& payload, const std::string& client_id, std::string& error)
{
    const json j = parse_object(payload);
    if (j.is_null()) {
        error = "the registration answer is not JSON";
        return RegisterResult::Invalid;
    }
    if (str_at(j, "client_id") != client_id)
        return RegisterResult::NotForUs;
    const std::string e = j.contains("error") && j["error"].is_string() ? j["error"].get<std::string>() : std::string("fail");
    if (e == "ok")
        return RegisterResult::Ok;
    if (boost::icontains(e, "too many clients")) {
        error = "the printer has too many connections open";
        return RegisterResult::TooManyClients;
    }
    error = e.empty() ? std::string("the printer refused the connection") : e;
    return RegisterResult::Refused;
}

std::string request_payload(int id, int method, const std::string& params_json)
{
    ojson params = ojson::parse(params_json.empty() ? std::string("{}") : params_json, nullptr, false);
    if (params.is_discarded() || !params.is_object())
        params = ojson::object();
    ojson j;
    j["id"]     = id;
    j["method"] = method;
    j["params"] = std::move(params);
    return j.dump(-1, ' ', false, ojson::error_handler_t::replace);
}

std::string start_print_params(const std::map<std::string, std::string>& extended_info,
                               int                                       pc_bed_type,
                               const std::string&                        filename,
                               bool                                      bedlevel_force)
{
    ojson config;
    config["delay_video"]    = ElegooStartPrint::flag(extended_info, "timeLapse") == 1;
    config["printer_check"]  = ElegooStartPrint::flag(extended_info, "heatedBedLeveling") == 1;
    // The printer's plate has two sides: "B" is the smooth PC side, "A" everything else.
    config["print_layout"]   = ElegooStartPrint::platform_type(extended_info, pc_bed_type) == 1 ? "B" : "A";
    config["bedlevel_force"] = bedlevel_force;
    bool                                              has_slot_map = false;
    const std::vector<ElegooStartPrint::SlotMapEntry> entries      = ElegooStartPrint::slot_map(extended_info, has_slot_map);
    ojson                                             arr          = ojson::array();
    for (const ElegooStartPrint::SlotMapEntry& e : entries) {
        ojson item;
        item["t"]         = e.t;
        item["canvas_id"] = e.canvas_id;
        item["tray_id"]   = e.tray_id;
        arr.push_back(std::move(item));
    }
    config["slot_map"] = std::move(arr);

    ojson params;
    params["storage_media"] = "local";
    params["filename"]      = filename;
    params["config"]        = std::move(config);
    return params.dump(-1, ' ', false, ojson::error_handler_t::replace);
}

Reply parse_reply(const std::string& payload)
{
    Reply      r;
    const json j = parse_object(payload);
    if (j.is_null() || !j.contains("id"))
        return r;
    r.id     = int_at(j, "id", -1);
    r.method = int_at(j, "method", -1);
    r.valid  = r.id >= 0;
    const json result = j.value("result", json());
    if (result.is_object()) {
        r.error_code = int_at(result, "error_code", -1);
        r.result     = result.dump();
    }
    return r;
}

namespace {
json machine_status_object(const std::string& result_json)
{
    const json j = parse_object(result_json);
    return j.is_null() ? json() : j.value("machine_status", json());
}
} // namespace

int machine_status(const std::string& result_json) { return int_at(machine_status_object(result_json), "status", -1); }
int machine_sub_status(const std::string& result_json) { return int_at(machine_status_object(result_json), "sub_status", -1); }

std::string state_name(int status)
{
    switch (status) {
    case 0: return "initializing";
    case 1: return "standby";
    case 2: return "printing";
    case 3:
    case 4: return "filament operation";
    case 5: return "auto-leveling";
    case 6: return "PID calibration";
    case 7: return "resonance test";
    case 8: return "self-check";
    case 9: return "updating";
    case 10: return "homing";
    case 11: return "receiving a file";
    case 12: return "making a time-lapse video";
    case 13: return "extruder operation";
    case 14: return "emergency stop";
    case 15: return "power-loss recovery";
    default: return status < 0 ? std::string() : "busy";
    }
}

bool status_is_busy(int status, int sub_status)
{
    // Idle, or a finished print still on the screen (sub-status 2077): ElegooSlicer starts on both.
    return status >= 0 && status != STATUS_IDLE && sub_status != SUB_PRINT_COMPLETED;
}

std::string error_text(int error_code)
{
    switch (error_code) {
    case 0: return std::string();
    case ERR_FILAMENT_RUNOUT: return "the printer reports a filament runout";
    case ERR_ACCESS_DENIED: return "the printer refused the access code";
    case ERR_INVALID_PARAMETER:
    case ERR_INVALID_PARAMETER2: return "the printer refused the request as invalid";
    case ERR_BUSY: return "the printer is busy";
    case ERR_FILE_NOT_FOUND: return "the printer cannot find the uploaded file";
    case ERR_MISSING_BED_LEVELING: return "the printer has no bed leveling data";
    case -1: return "the printer did not say whether it worked";
    default: return "the printer answered error " + std::to_string(error_code);
    }
}

// ---- Session ----

Session::Session(MqttTransport& transport, std::string sn, const SessionOptions& opts)
    : m_transport(transport), m_sn(std::move(sn)), m_opts(opts)
{
    m_client_id  = m_opts.client_id.empty() ? make_client_id(random_in(1000, 9999)) : m_opts.client_id;
    m_request_id = request_id_for(m_client_id);
    m_next_id    = m_opts.first_id > 0 ? m_opts.first_id : random_in(1000, 900000);
}

Session::~Session() { close(); }

bool Session::open(std::string& error)
{
    if (m_open)
        return true;
    if (m_sn.empty()) {
        error = "the printer's serial number is unknown";
        return false;
    }
    if (!m_transport.connect(m_client_id, error))
        return false;
    m_open = true;
    const std::string reg_reply = register_response_topic(m_sn, m_request_id);
    if (!m_transport.subscribe(response_topic(m_sn, m_client_id), error) || !m_transport.subscribe(reg_reply, error) ||
        !m_transport.publish(register_topic(m_sn), register_payload(m_client_id, m_request_id), error)) {
        close();
        return false;
    }
    const long long deadline = steady_ms() + m_opts.register_timeout_ms;
    for (;;) {
        const long long left = deadline - steady_ms();
        std::string     topic, payload;
        if (left <= 0 || !m_transport.receive(topic, payload, static_cast<int>(left))) {
            error = "the printer did not answer the registration";
            close();
            return false;
        }
        if (topic != reg_reply)
            continue;
        std::string          why;
        const RegisterResult r = parse_register_response(payload, m_client_id, why);
        if (r == RegisterResult::NotForUs)
            continue;
        if (r == RegisterResult::Ok)
            return true;
        error = why;
        close();
        return false;
    }
}

bool Session::call(int method, const std::string& params_json, Reply& reply, std::string& error)
{
    reply = Reply();
    if (!m_open) {
        error = "not connected";
        return false;
    }
    const int id = m_next_id++;
    if (!m_transport.publish(request_topic(m_sn, m_client_id), request_payload(id, method, params_json), error))
        return false;
    const std::string want     = response_topic(m_sn, m_client_id);
    const long long   deadline = steady_ms() + m_opts.reply_timeout_ms;
    for (;;) {
        const long long left = deadline - steady_ms();
        std::string     topic, payload;
        if (left <= 0 || !m_transport.receive(topic, payload, static_cast<int>(left))) {
            error = "the printer did not answer request " + std::to_string(method);
            return false;
        }
        if (topic != want)
            continue;
        Reply r = parse_reply(payload);
        if (!r.valid || r.id != id)
            continue;
        reply = std::move(r);
        return true;
    }
}

void Session::close()
{
    if (!m_open)
        return;
    m_open = false;
    m_transport.close();
}

CanvasQueryResult canvas_query_over(Session& session)
{
    CanvasQueryResult out;
    out.reached = true;
    Reply       status;
    std::string error;
    if (session.call(METHOD_GET_STATUS, "{}", status, error) && status.error_code == 0)
        out.print_state = state_name(machine_status(status.result));

    Reply canvas;
    if (!session.call(METHOD_GET_CANVAS, "{}", canvas, error)) {
        out.error = error;
        return out;
    }
    if (canvas.error_code != 0) {
        out.error = error_text(canvas.error_code);
        return out;
    }
    const json result      = parse_object(canvas.result);
    const json canvas_info = result.is_null() ? json() : result.value("canvas_info", json());
    if (!canvas_info.is_object()) {
        // A plain CC2 may answer without the object at all.
        out.no_canvas          = true;
        out.ams_connect_status = 0;
        out.error              = "no CANVAS attached";
        return out;
    }
    CanvasInfo  info;
    std::string why;
    const bool  parsed = canvas_parse_payload(canvas_info.dump(), info, why);
    bool        any_connected = false;
    for (const CanvasUnit& u : info.units)
        any_connected = any_connected || u.connected;
    if (!parsed || !any_connected) {
        // An empty canvas_list, or only disconnected units: the printer has no CANVAS to feed from.
        out.no_canvas          = true;
        out.ams_connect_status = 0;
        out.error              = "no CANVAS attached";
        return out;
    }
    out.ams_connect_status = 1;
    out.info               = std::move(info);
    out.ok                 = true;
    return out;
}

StartResult start_print_over(Session&                                  session,
                             const std::map<std::string, std::string>& extended_info,
                             int                                       pc_bed_type,
                             const std::string&                        filename,
                             int                                       busy_checks,
                             int                                       busy_interval_ms)
{
    StartResult out;
    std::string error;
    // The printer may still be taking in the file it was just sent: ask again for a while, as
    // ElegooSlicer does (10 times, a second apart), before calling it busy. A status it does not
    // answer is not a reason to refuse: the start command itself will say.
    for (int i = 0; i < std::max(1, busy_checks); ++i) {
        if (i > 0 && busy_interval_ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(busy_interval_ms));
        Reply status;
        if (!session.call(METHOD_GET_STATUS, "{}", status, error) || status.error_code != 0) {
            out.busy = false;
            break;
        }
        const int st  = machine_status(status.result);
        const int sub = machine_sub_status(status.result);
        out.busy      = status_is_busy(st, sub);
        if (!out.busy)
            break;
        out.error = "the printer is " + state_name(st) + ", so the print was not started";
    }
    if (out.busy)
        return out;
    out.error.clear();
    bool force = false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        out.payload = start_print_params(extended_info, pc_bed_type, filename, force);
        Reply r;
        if (!session.call(METHOD_START_PRINT, out.payload, r, error)) {
            out.error = error;
            return out;
        }
        out.error_code = r.error_code;
        if (r.error_code == 0) {
            out.ok = true;
            out.error.clear();
            return out;
        }
        out.error = error_text(r.error_code);
        if (r.error_code == ERR_MISSING_BED_LEVELING && !force && ElegooStartPrint::flag(extended_info, "heatedBedLeveling") == 1) {
            force = true;
            continue;
        }
        return out;
    }
    return out;
}

} // namespace ElegooCc2
} // namespace Slic3r
