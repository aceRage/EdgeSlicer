#include "AnycubicLan.hpp"

#include "Http.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio.hpp>
#include <boost/beast/core/detail/base64.hpp>

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <random>
#include <sstream>
#include <thread>

namespace Slic3r {
namespace AnycubicLan {

using nlohmann::json;

namespace {

std::string json_str(const json &j, const char *key)
{
    if (!j.is_object()) return {};
    auto it = j.find(key);
    if (it == j.end()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<long long>());
    if (it->is_number()) return std::to_string(it->get<double>());
    return {};
}

bool json_int(const json &j, const char *key, int &out)
{
    if (!j.is_object()) return false;
    auto it = j.find(key);
    if (it == j.end()) return false;
    if (it->is_number_integer()) {
        out = (int) it->get<long long>();
        return true;
    }
    if (it->is_number()) {
        out = (int) it->get<double>();
        return true;
    }
    if (it->is_string()) {
        try {
            out = std::stoi(it->get<std::string>());
            return true;
        } catch (...) { return false; }
    }
    return false;
}

bool json_double(const json &j, const char *key, double &out)
{
    if (!j.is_object()) return false;
    auto it = j.find(key);
    if (it == j.end()) return false;
    if (it->is_number()) {
        out = it->get<double>();
        return true;
    }
    if (it->is_string()) {
        try {
            out = std::stod(it->get<std::string>());
            return true;
        } catch (...) { return false; }
    }
    return false;
}

std::string header_value(const std::string &text, const std::string &name)
{
    std::string lower_name = boost::algorithm::to_lower_copy(name);
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        boost::algorithm::to_lower(key);
        boost::algorithm::trim(key);
        if (key != lower_name) continue;
        std::string value = line.substr(colon + 1);
        boost::algorithm::trim(value);
        return value;
    }
    return {};
}

std::string ip_from_location(const std::string &location)
{
    std::string s = location;
    boost::algorithm::trim(s);
    const auto scheme = s.find("://");
    if (scheme != std::string::npos)
        s = s.substr(scheme + 3);
    const auto slash = s.find('/');
    if (slash != std::string::npos)
        s = s.substr(0, slash);
    if (!s.empty() && s.front() == '[') {
        const auto end = s.find(']');
        if (end != std::string::npos)
            return s.substr(1, end - 1);
    }
    const auto colon = s.rfind(':');
    if (colon != std::string::npos && s.find(':') == colon)
        s = s.substr(0, colon);
    return s;
}

std::string random_hex(size_t nbytes)
{
    static thread_local std::mt19937_64 rng { std::random_device {}() };
    std::uniform_int_distribution<int> dist(0, 255);
    static const char *kHex = "0123456789abcdef";
    std::string out;
    out.resize(nbytes * 2);
    for (size_t i = 0; i < nbytes; ++i) {
        const int b = dist(rng);
        out[i * 2]     = kHex[(b >> 4) & 0xf];
        out[i * 2 + 1] = kHex[b & 0xf];
    }
    return out;
}

json inner_data(const json &root)
{
    if (!root.is_object()) return json::object();
    if (root.contains("data") && root["data"].is_object()) {
        const json &d = root["data"];
        if (d.contains("data") && d["data"].is_object())
            return d["data"];
        return d;
    }
    return root;
}

std::string envelope_type(const json &root)
{
    if (root.contains("type") && root["type"].is_string())
        return root["type"].get<std::string>();
    if (root.contains("data") && root["data"].is_object() && root["data"].contains("type") && root["data"]["type"].is_string())
        return root["data"]["type"].get<std::string>();
    return {};
}

void merge_temp(const json &temp, Status &s)
{
    if (!temp.is_object()) return;
    if (json_double(temp, "curr_nozzle_temp", s.nozzle) || json_double(temp, "curr_nozzle", s.nozzle))
        s.has_nozzle = true;
    json_double(temp, "target_nozzle_temp", s.nozzle_target);
    json_double(temp, "target_nozzle", s.nozzle_target);
    if (json_double(temp, "curr_hotbed_temp", s.bed) || json_double(temp, "curr_bed", s.bed))
        s.has_bed = true;
    json_double(temp, "target_hotbed_temp", s.bed_target);
    json_double(temp, "target_bed", s.bed_target);
    double box = 0;
    if (json_double(temp, "curr_box_temp", box) || json_double(temp, "chamber_temp", box)) {
        s.has_box = true;
        s.box     = box;
    }
    json_double(temp, "target_box_temp", s.box_target);
}

void merge_project(const json &project, Status &s)
{
    if (!project.is_object()) return;
    const std::string st = json_str(project, "state");
    if (!st.empty()) s.project_state = st;
    const std::string fn = json_str(project, "filename");
    if (!fn.empty()) s.filename = fn;
    json_int(project, "progress", s.progress);
    json_int(project, "curr_layer", s.curr_layer);
    json_int(project, "total_layers", s.total_layers);
    json_int(project, "remain_time", s.remain_time_min);
    json_int(project, "print_time", s.print_time_min);
    json_int(project, "pause", s.pause);
}

} // namespace

bool parse_info(const std::string &body, Info &out)
{
    out = Info {};
    const json j = json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    out.device_type     = json_str(j, "deviceType");
    out.model_id        = json_str(j, "modelId");
    out.model_name      = json_str(j, "modelName");
    out.cn              = json_str(j, "cn");
    out.ctrl_type       = json_str(j, "ctrlType");
    out.ctrl_info_url   = json_str(j, "ctrlInfoUrl");
    out.ip              = json_str(j, "ip");
    out.device_name     = json_str(j, "deviceName");
    out.usn             = json_str(j, "usn");
    out.file_upload_url = json_str(j, "fileUploadurl");
    if (out.file_upload_url.empty())
        out.file_upload_url = json_str(j, "fileUploadUrl");
    out.rtsp_url = json_str(j, "rtspUrl");
    out.token    = json_str(j, "token");
    out.valid    = !out.token.empty() || !out.model_id.empty() || !out.cn.empty();
    return out.valid;
}

bool parse_ctrl_response(const std::string &body, CtrlBlob &out)
{
    out = CtrlBlob {};
    const json j = json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    json_int(j, "code", out.code);
    out.message = json_str(j, "message");
    const json data = j.contains("data") && j["data"].is_object() ? j["data"] : json::object();
    out.info_b64 = json_str(data, "info");
    out.iv       = json_str(data, "token");
    out.valid    = !out.info_b64.empty() && !out.iv.empty();
    return out.valid;
}

bool parse_credentials_json(const std::string &plain, Credentials &out)
{
    out = Credentials {};
    const json j = json::parse(plain, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    out.broker      = json_str(j, "broker");
    out.username    = json_str(j, "username");
    out.password    = json_str(j, "password");
    out.device_id   = json_str(j, "deviceId");
    out.model_id    = json_str(j, "modelId");
    if (out.model_id.empty())
        out.model_id = json_str(j, "modeId");
    out.model_name  = json_str(j, "modelName");
    out.device_type = json_str(j, "deviceType");
    out.ip          = json_str(j, "ip");
    out.cert        = json_str(j, "devicecrt");
    out.key         = json_str(j, "devicepk");
    out.broker_host = out.ip;
    out.broker_port = MQTT_PORT;
    if (!out.broker.empty()) {
        std::string b = out.broker;
        const auto scheme = b.find("://");
        if (scheme != std::string::npos)
            b = b.substr(scheme + 3);
        const auto colon = b.rfind(':');
        if (colon != std::string::npos) {
            out.broker_host = b.substr(0, colon);
            try {
                out.broker_port = std::stoi(b.substr(colon + 1));
            } catch (...) {}
        } else if (!b.empty()) {
            out.broker_host = b;
        }
    }
    if (out.broker_host.empty())
        out.broker_host = out.ip;
    out.valid = !out.username.empty() && !out.broker_host.empty();
    return out.valid;
}

bool parse_ssdp_message(const std::string &text, SsdpDevice &out)
{
    out = SsdpDevice {};
    std::string st = header_value(text, "ST");
    if (st.empty())
        st = header_value(text, "NT");
    if (st.size() < std::strlen(SSDP_ST_PREFIX) ||
        !boost::algorithm::istarts_with(st, SSDP_ST_PREFIX))
        return false;
    out.st       = st;
    out.model    = st.substr(std::strlen(SSDP_ST_PREFIX));
    out.usn      = header_value(text, "USN");
    out.location = header_value(text, "LOCATION");
    out.ip       = ip_from_location(out.location);
    out.valid    = !out.ip.empty() || !out.usn.empty();
    return out.valid;
}

bool parse_report(const std::string & /*topic*/, const std::string &payload, Status &status)
{
    const json root = json::parse(payload, nullptr, false);
    if (root.is_discarded() || !root.is_object()) return false;
    status.valid = true;
    const json data = inner_data(root);
    const std::string type = envelope_type(root);

    if (data.contains("temp") && data["temp"].is_object())
        merge_temp(data["temp"], status);
    merge_temp(data, status);

    if (data.contains("project") && data["project"].is_object())
        merge_project(data["project"], status);
    merge_project(data, status);

    const std::string name = json_str(data, "printerName");
    if (!name.empty()) status.printer_name = name;
    const std::string model = json_str(data, "model");
    if (!model.empty()) status.model = model;
    const std::string fw = json_str(data, "version");
    if (!fw.empty()) status.firmware = fw;
    const std::string ip = json_str(data, "ip");
    if (!ip.empty()) status.ip = ip;
    const std::string st = json_str(data, "state");
    if (!st.empty() && type != "print")
        status.state = st;

    json_int(data, "fan_speed_pct", status.fan_pct);
    json_int(data, "aux_fan_speed_pct", status.aux_fan_pct);
    json_int(data, "box_fan_level", status.box_fan);
    json_int(data, "print_speed_mode", status.print_speed_mode);

    if (data.contains("urls") && data["urls"].is_object()) {
        const std::string up = json_str(data["urls"], "fileUploadurl");
        if (!up.empty()) status.file_upload_url = up;
        const std::string rtsp = json_str(data["urls"], "rtspUrl");
        if (!rtsp.empty()) status.rtsp_url = rtsp;
    }
    return true;
}

bool signed_handshake_supported(const std::string &info_token) { return info_token.size() >= 32; }

std::string md5_hex(const std::string &data)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  len = 0;
    if (EVP_Digest(data.data(), data.size(), digest, &len, EVP_md5(), nullptr) != 1)
        return {};
    static const char *kHex = "0123456789abcdef";
    std::string out;
    out.resize(len * 2);
    for (unsigned int i = 0; i < len; ++i) {
        out[i * 2]     = kHex[(digest[i] >> 4) & 0xf];
        out[i * 2 + 1] = kHex[digest[i] & 0xf];
    }
    return out;
}

std::string sign_ctrl(const std::string &info_token, const std::string &ts_ms, const std::string &nonce)
{
    if (info_token.size() < 16) return {};
    const std::string inner = md5_hex(info_token.substr(0, 16));
    return md5_hex(inner + ts_ms + nonce);
}

bool aes128_cbc_decrypt(const std::string &key16, const std::string &iv16, const std::vector<unsigned char> &cipher, std::string &plain)
{
    plain.clear();
    if (key16.size() != 16 || iv16.size() != 16 || cipher.empty())
        return false;

    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!ctx) return false;

    std::vector<unsigned char> out(cipher.size() + EVP_MAX_BLOCK_LENGTH);
    int out_len = 0, final_len = 0;
    if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_128_cbc(), nullptr,
                           reinterpret_cast<const unsigned char *>(key16.data()),
                           reinterpret_cast<const unsigned char *>(iv16.data())) != 1)
        return false;
    if (EVP_DecryptUpdate(ctx.get(), out.data(), &out_len, cipher.data(), (int) cipher.size()) != 1)
        return false;
    if (EVP_DecryptFinal_ex(ctx.get(), out.data() + out_len, &final_len) != 1)
        return false;
    plain.assign(reinterpret_cast<char *>(out.data()), out_len + final_len);
    return true;
}

bool decrypt_ctrl(const Info &info, const CtrlBlob &blob, Credentials &out)
{
    out = Credentials {};
    if (!signed_handshake_supported(info.token) || blob.iv.size() != 16 || blob.info_b64.empty())
        return false;
    std::vector<unsigned char> cipher(boost::beast::detail::base64::decoded_size(blob.info_b64.size()));
    const auto decoded = boost::beast::detail::base64::decode(cipher.data(), blob.info_b64.data(), blob.info_b64.size());
    cipher.resize(decoded.first);
    std::string plain;
    if (!aes128_cbc_decrypt(info.token.substr(16, 16), blob.iv, cipher, plain))
        return false;
    return parse_credentials_json(plain, out);
}

std::string ssdp_msearch_request(const std::string &model)
{
    std::string st = std::string(SSDP_ST_PREFIX) + model;
    std::ostringstream oss;
    oss << "M-SEARCH * HTTP/1.1\r\n"
        << "HOST: 239.255.255.250:1900\r\n"
        << "MAN: \"ssdp:discover\"\r\n"
        << "MX: 3\r\n"
        << "ST: " << st << "\r\n"
        << "\r\n";
    return oss.str();
}

std::string bare_host(const std::string &host_or_url)
{
    std::string s = host_or_url;
    boost::algorithm::trim(s);
    const auto scheme = s.find("://");
    if (scheme != std::string::npos)
        s = s.substr(scheme + 3);
    const auto slash = s.find('/');
    if (slash != std::string::npos)
        s = s.substr(0, slash);
    if (!s.empty() && s.front() == '[') {
        const auto end = s.find(']');
        if (end != std::string::npos)
            return s.substr(1, end - 1);
    }
    // Drop a trailing :port so LAN HTTP always uses 18910.
    const auto colon = s.rfind(':');
    if (colon != std::string::npos && s.find(':') == colon)
        s = s.substr(0, colon);
    return s;
}

std::string lan_http_base(const std::string &host_or_url)
{
    return "http://" + bare_host(host_or_url) + ":" + std::to_string(HTTP_PORT);
}

std::string info_url(const std::string &host_or_url) { return lan_http_base(host_or_url) + "/info"; }

std::string ctrl_url(const std::string &host_or_url, const std::string &ts_ms, const std::string &nonce, const std::string &sign,
                     const std::string &did)
{
    return lan_http_base(host_or_url) + "/ctrl?ts=" + ts_ms + "&nonce=" + nonce + "&sign=" + sign + "&did=" + did;
}

std::string random_nonce(int length)
{
    static const char kChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    static thread_local std::mt19937 rng { std::random_device {}() };
    std::uniform_int_distribution<int> dist(0, (int) sizeof(kChars) - 2);
    std::string out;
    out.resize((size_t) std::max(1, length));
    for (char &c : out)
        c = kChars[dist(rng)];
    return out;
}

std::string make_pc_device_id() { return boost::algorithm::to_upper_copy(random_hex(16)); }

std::string make_msgid()
{
    const std::string h = random_hex(16);
    return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" + h.substr(20);
}

std::string now_ts_ms()
{
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return std::to_string(ms);
}

std::string mqtt_slicer_topic(const std::string &model_id, const std::string &cn, const std::string &leaf)
{
    return std::string(MQTT_SLICER_PREFIX) + model_id + "/" + cn + "/" + leaf;
}

std::string mqtt_public_filter(const std::string &model_id, const std::string &cn)
{
    return std::string(MQTT_PUBLIC_PREFIX) + model_id + "/" + cn + "/#";
}

std::string mqtt_broker_uri(const std::string &host, int port) { return "mqtts://" + host + ":" + std::to_string(port); }

json command_envelope(const std::string &msgid, const std::string &did, const std::string &bid, const std::string &type,
                      const std::string &action, const json &inner)
{
    json env;
    env["msgid"]           = msgid;
    env["data"]["did"]     = did;
    env["data"]["bid"]     = bid;
    env["data"]["type"]    = type;
    env["data"]["action"]  = action;
    env["data"]["data"]    = inner.is_null() ? json::object() : inner;
    return env;
}

json print_start_data(const PrintStart &job)
{
    json data;
    data["taskid"]   = "-1";
    data["filename"] = job.filename;
    data["filetype"] = job.filetype;
    if (job.filesize >= 0) data["filesize"] = job.filesize;
    if (!job.md5.empty()) data["md5"] = job.md5;
    if (job.use_ams || !job.ams.empty()) {
        json ams;
        ams["use_ams"] = job.use_ams || !job.ams.empty();
        json mapping   = json::array();
        for (const AmsBoxMap &m : job.ams) {
            json e;
            e["paint_index"]   = m.paint_index;
            e["ams_index"]     = m.ams_index;
            e["paint_color"]   = m.paint_color;
            e["ams_color"]     = m.ams_color;
            e["material_type"] = m.material_type;
            mapping.push_back(e);
        }
        ams["ams_box_mapping"] = mapping;
        data["ams_settings"]   = ams;
    }
    json task;
    task["auto_leveling"]           = job.auto_leveling;
    task["vibration_compensation"]  = job.vibration_compensation;
    data["task_settings"]           = task;
    return data;
}

std::vector<AmsBoxMap> ams_from_filament_mapping(const std::string &mapping)
{
    std::vector<AmsBoxMap> out;
    if (mapping.empty()) return out;
    std::vector<std::string> parts;
    boost::algorithm::split(parts, mapping, boost::is_any_of(","));
    for (std::string p : parts) {
        boost::algorithm::trim(p);
        if (p.empty()) continue;
        const auto colon = p.find(':');
        if (colon == std::string::npos) continue;
        AmsBoxMap m;
        try {
            m.paint_index = std::stoi(p.substr(0, colon));
            m.ams_index   = std::stoi(p.substr(colon + 1));
        } catch (...) { continue; }
        out.push_back(m);
    }
    return out;
}

std::string hub_state(const Status &s)
{
    if (s.pause == 1 || s.pause == 2) return "paused";
    if (s.pause == 4) return "cancelled";
    if (s.pause == 3) return "printing";
    std::string st = s.project_state;
    boost::algorithm::to_lower(st);
    if (st == "printing" || st == "print") return "printing";
    if (st == "pause" || st == "paused") return "paused";
    if (st == "finish" || st == "finished" || st == "complete") return "complete";
    if (st == "stop" || st == "stopped" || st == "cancel" || st == "cancelled") return "cancelled";
    if (st == "fail" || st == "failed" || st == "error") return "error";
    std::string machine = s.state;
    boost::algorithm::to_lower(machine);
    if (machine == "busy" || machine == "printing") return "printing";
    if (machine == "free" || machine == "idle" || machine.empty()) return s.valid ? "standby" : std::string();
    return "standby";
}

bool is_printing(const Status &s)
{
    const std::string st = hub_state(s);
    return st == "printing" || st == "paused";
}
bool can_pause(const Status &s)  { return hub_state(s) == "printing"; }
bool can_resume(const Status &s) { return hub_state(s) == "paused"; }
bool can_stop(const Status &s)   { return is_printing(s); }

GUI::DeviceControls::Caps caps(const Snapshot &snap)
{
    using namespace GUI::DeviceControls;
    Caps          c;
    const Status &s = snap.status;
    if (s.has_bed) c.heaters.push_back(Heater { "bed", "Bed", s.bed, s.bed_target, DEFAULT_BED_MAX, false, false });
    if (s.has_nozzle)
        c.heaters.push_back(Heater { nozzle_id(0), "Nozzle", s.nozzle, s.nozzle_target, DEFAULT_NOZZLE_MAX, false, true });
    if (s.has_box)
        c.heaters.push_back(Heater { "chamber", "Chamber", s.box, s.box_target, DEFAULT_CHAMBER_MAX, false, false });
    if (s.fan_pct >= 0) c.fans.push_back(Fan { "part", "Part cooling", s.fan_pct });
    if (s.aux_fan_pct >= 0) c.fans.push_back(Fan { "aux", "Auxiliary", s.aux_fan_pct });
    if (s.box_fan >= 0) c.fans.push_back(Fan { "chamber", "Chamber", s.box_fan });
    return c;
}

void fill_row(const Snapshot &snap, const std::string &host, json &row)
{
    row["can_pause"]  = false;
    row["can_resume"] = false;
    row["can_stop"]   = false;
    row["print_error"] = nullptr;
    row["anycubic_connected"] = snap.connected;
    if (!snap.connected) {
        if (snap.failures > 0) {
            row["online"]       = false;
            row["status"]       = "offline";
            row["printing"]     = false;
            row["status_error"] = snap.error.empty() ? std::string("the printer does not answer") : snap.error;
            return;
        }
        if (!snap.status.valid) return;
    }
    const Status &s     = snap.status;
    const std::string state = hub_state(s);
    row["online"] = true;
    if (state.empty()) return;
    row["status"]       = state;
    row["print_status"] = state;
    const bool active   = is_printing(s);
    row["printing"]     = active;
    row["can_pause"]    = snap.connected && can_pause(s);
    row["can_resume"]   = snap.connected && can_resume(s);
    row["can_stop"]     = snap.connected && can_stop(s);
    if (!s.filename.empty() && (active || state == "complete" || state == "cancelled" || state == "error"))
        row["task"] = s.filename;
    if (!s.filename.empty() && active)
        row["stage"] = s.filename;
    if (active || state == "complete") {
        if (s.progress >= 0) row["percent"] = state == "complete" ? 100 : s.progress;
        if (s.remain_time_min >= 0) row["left_time_s"] = state == "complete" ? 0 : s.remain_time_min * 60;
        if (s.curr_layer >= 0) row["layer"] = s.curr_layer;
        if (s.total_layers >= 0) row["total_layers"] = s.total_layers;
    }
    if (s.has_bed) {
        row["bed_temp"]   = s.bed;
        row["bed_target"] = s.bed_target;
    }
    if (s.has_nozzle)
        row["nozzles"] = json::array({ json { { "temp", s.nozzle }, { "target", s.nozzle_target } } });
    if (s.has_box) {
        row["chamber_temp"]   = s.box;
        row["chamber_target"] = s.box_target;
    }
    row["controls"] = GUI::DeviceControls::to_json(caps(snap));
    json cam;
    cam["vendor"]    = "anycubic";
    cam["rkind"]     = "anycubic";
    cam["available"] = !s.rtsp_url.empty() || !snap.info.rtsp_url.empty();
    if (!host.empty()) cam["ip"] = host;
    const std::string rtsp = !s.rtsp_url.empty() ? s.rtsp_url : snap.info.rtsp_url;
    if (!rtsp.empty()) cam["url"] = rtsp;
    row["camera_hint"] = cam;
    json d;
    d["protocol"] = "anycubic-lan";
    d["live"]     = snap.connected;
    if (!s.firmware.empty()) d["firmware"] = s.firmware;
    if (!snap.info.model_id.empty()) d["model_id"] = snap.info.model_id;
    row["anycubic"] = d;
}

bool handshake(const std::string &host_or_url, Info &info, Credentials &creds, std::string &error, int timeout_s)
{
    info  = Info {};
    creds = Credentials {};
    error.clear();
    const std::string url = info_url(host_or_url);
    std::string       body;
    bool              ok = false;
    Http::get(url)
        .timeout_connect(timeout_s)
        .timeout_max(timeout_s)
        .tls_policy(Http::TlsPolicy::PrintHost)
        .on_error([&](std::string reply, std::string err, unsigned status) {
            error = err.empty() ? ("HTTP " + std::to_string(status)) : err;
            body  = reply;
        })
        .on_complete([&](std::string reply, unsigned) {
            body = reply;
            ok   = true;
        })
        .perform_sync();
    if (!ok) {
        if (error.empty()) error = "GET /info failed";
        return false;
    }
    if (!parse_info(body, info)) {
        error = "GET /info: not Anycubic LAN JSON";
        return false;
    }
    if (info.ctrl_type != "lan") {
        error = "printer is not in LAN mode (ctrlType=" + info.ctrl_type + ")";
        return false;
    }
    if (!signed_handshake_supported(info.token)) {
        error = "unsigned /ctrl handshake (Kobra 2 / pre-2.5.9.9) is not implemented";
        return false;
    }
    const std::string ts    = now_ts_ms();
    const std::string nonce = random_nonce();
    const std::string sign  = sign_ctrl(info.token, ts, nonce);
    const std::string did   = make_pc_device_id();
    const std::string curl  = ctrl_url(host_or_url, ts, nonce, sign, did);
    std::string       cbody;
    bool              cok = false;
    std::string       cerr;
    Http::post(curl)
        .timeout_connect(timeout_s)
        .timeout_max(timeout_s)
        .tls_policy(Http::TlsPolicy::PrintHost)
        .set_post_body(std::string())
        .on_error([&](std::string reply, std::string err, unsigned status) {
            cerr  = err.empty() ? ("HTTP " + std::to_string(status)) : err;
            cbody = reply;
        })
        .on_complete([&](std::string reply, unsigned) {
            cbody = reply;
            cok   = true;
        })
        .perform_sync();
    if (!cok) {
        error = cerr.empty() ? "POST /ctrl failed" : cerr;
        return false;
    }
    CtrlBlob blob;
    if (!parse_ctrl_response(cbody, blob)) {
        error = "POST /ctrl: unexpected JSON";
        return false;
    }
    if (!decrypt_ctrl(info, blob, creds)) {
        error = "POST /ctrl: AES decrypt failed";
        return false;
    }
    if (creds.broker_host.empty())
        creds.broker_host = bare_host(host_or_url);
    return true;
}

bool discover(std::vector<SsdpDevice> &out, std::string &error, int timeout_ms, const std::string &model)
{
    out.clear();
    error.clear();
    try {
        using boost::asio::ip::udp;
        boost::asio::io_context io;
        udp::socket             sock(io);
        sock.open(udp::v4());
        sock.set_option(boost::asio::socket_base::reuse_address(true));
        sock.set_option(boost::asio::socket_base::broadcast(true));
        sock.bind(udp::endpoint(udp::v4(), 0));
        const std::string req = ssdp_msearch_request(model);
        const udp::endpoint dest(boost::asio::ip::make_address("239.255.255.250"), 1900);
        sock.send_to(boost::asio::buffer(req), dest);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(200, timeout_ms));
        while (std::chrono::steady_clock::now() < deadline) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) break;
            sock.non_blocking(true);
            std::array<char, 4096> buf {};
            udp::endpoint          from;
            boost::system::error_code ec;
            const std::size_t n = sock.receive_from(boost::asio::buffer(buf), from, 0, ec);
            if (ec == boost::asio::error::would_block) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            if (ec || n == 0) continue;
            SsdpDevice d;
            if (!parse_ssdp_message(std::string(buf.data(), n), d)) continue;
            if (d.ip.empty())
                d.ip = from.address().to_string();
            out.push_back(d);
        }
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}

} // namespace AnycubicLan
} // namespace Slic3r
