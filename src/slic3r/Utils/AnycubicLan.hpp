#ifndef slic3r_AnycubicLan_hpp_
#define slic3r_AnycubicLan_hpp_

// Native Anycubic LAN-mode protocol (stock firmware), clean-room from public notes.
//
// No Anycubic binary or decompilation was used. The behaviour here is taken from:
//  * Rinkhals issue #356 (https://github.com/jbatonnet/Rinkhals/issues/356) — captured
//    GET :18910/info and signed POST /ctrl on Kobra S1 firmware 2.5.9.9, including the
//    published token / ts / nonce / sign vector used by the tests.
//  * chrisfore/anycubic_ha_local (MIT) README — LAN mode is GET /info → signed POST /ctrl →
//    AES decrypt → TLS MQTT :9883; model IDs; Kobra 2's older unsigned handshake.
//  * rvanderp3/kobra-connect (Apache-2.0) README — MQTT topics and the slicer command JSON
//    envelope (`msgid` + nested `data.{did,bid,type,action,data}`).
//  * Public GitLab notes (grunna/anycubic-kobra-x-lan NOTES.md) — the /ctrl signature
//    `md5(md5(info.token[0:16]) + ts + nonce)` and AES-128-CBC/PKCS7 with key
//    info.token[16:32] and IV ctrl.data.token.
//  * Rinkhals gkapi MQTT docs — print/start payload fields and info/report JSON shape.
//
// Pure parsers, signing, crypto and message builders live here (no MQTT session). HTTP
// handshake is a thin wrapper around those, so the tests can drive every step with the
// published vectors and a constructed ciphertext. The PrintHost and the hub monitor sit
// in AnycubicLanHost.cpp / AnycubicLanMonitor.cpp.

#include "slic3r/GUI/DeviceControls.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace Slic3r {
namespace AnycubicLan {

constexpr int         HTTP_PORT          = 18910;
constexpr int         MQTT_PORT          = 9883;
constexpr int         CAMERA_PORT        = 18088;
constexpr const char *SSDP_ST_PREFIX     = "ac:3dprinter:";
constexpr const char *HOST_TYPE_KEY      = "anycubiclan";
constexpr const char *MQTT_SLICER_PREFIX = "anycubic/anycubicCloud/v1/slicer/printer/";
constexpr const char *MQTT_PUBLIC_PREFIX = "anycubic/anycubicCloud/v1/printer/public/";

struct Info
{
    bool        valid { false };
    std::string device_type;
    std::string model_id;
    std::string model_name;
    std::string cn; // printer serial / device id used in MQTT topics
    std::string ctrl_type;
    std::string ctrl_info_url;
    std::string ip;
    std::string device_name;
    std::string usn;
    std::string file_upload_url;
    std::string rtsp_url;
    std::string token;
};

struct CtrlBlob
{
    bool        valid { false };
    int         code { 0 };
    std::string message;
    std::string info_b64; // ciphertext
    std::string iv;       // 16-byte ASCII IV (ctrl.data.token)
};

struct Credentials
{
    bool        valid { false };
    std::string broker; // mqtts://ip:9883
    std::string broker_host;
    int         broker_port { MQTT_PORT };
    std::string username;
    std::string password;
    std::string device_id;
    std::string model_id;
    std::string model_name;
    std::string device_type;
    std::string ip;
    std::string cert; // PEM, from `devicecrt`
    std::string key;  // PEM, from `devicepk`
};

struct SsdpDevice
{
    bool        valid { false };
    std::string st;
    std::string usn;
    std::string location;
    std::string ip;
    std::string model; // ST suffix after ac:3dprinter:
};

struct AmsBoxMap
{
    int              paint_index { 0 };
    int              ams_index { 0 };
    std::vector<int> paint_color { 255, 255, 255, 255 };
    std::vector<int> ams_color { 255, 255, 255, 255 };
    std::string      material_type;
};

struct PrintStart
{
    std::string           filename;
    std::string           md5;
    long long             filesize { -1 };
    int                   filetype { 1 };
    bool                  use_ams { false };
    std::vector<AmsBoxMap> ams;
    int                   auto_leveling { 0 };
    int                   vibration_compensation { 0 };
};

// Live state merged from MQTT reports (info/report, tempature/report, …).
struct Status
{
    bool        valid { false };
    std::string printer_name;
    std::string model;
    std::string firmware;
    std::string ip;
    std::string state;         // printer-level: free, busy, …
    std::string project_state; // printing, finish, …
    std::string filename;
    int         pause { -1 }; // kobra-connect PauseState: 0 printing, 1 paused, 2 pausing, 3 resuming, 4 stopping
    int         progress { -1 };
    int         curr_layer { -1 };
    int         total_layers { -1 };
    int         remain_time_min { -1 }; // printer reports minutes
    int         print_time_min { -1 };
    bool        has_nozzle { false };
    bool        has_bed { false };
    bool        has_box { false };
    double      nozzle { 0 }, nozzle_target { 0 };
    double      bed { 0 }, bed_target { 0 };
    double      box { 0 }, box_target { 0 };
    int         fan_pct { -1 };
    int         aux_fan_pct { -1 };
    int         box_fan { -1 };
    int         print_speed_mode { -1 };
    std::string file_upload_url;
    std::string rtsp_url;
};

struct Snapshot
{
    bool        exists { false };
    bool        connected { false };
    bool        connecting { false };
    int         failures { 0 };
    std::string error;
    Status      status;
    Info        info;
};

// ---- host type --------------------------------------------------------------

inline const char *host_type_key() { return HOST_TYPE_KEY; }
inline bool        is_developer_host_type(const std::string &key) { return key == HOST_TYPE_KEY; }

// ---- parsers (no network) ---------------------------------------------------

bool parse_info(const std::string &body, Info &out);
bool parse_ctrl_response(const std::string &body, CtrlBlob &out);
bool parse_credentials_json(const std::string &plain, Credentials &out);
bool parse_ssdp_message(const std::string &text, SsdpDevice &out);

// MQTT reports: both the flat gkapi envelope (`type`/`action`/`data`) and the nested
// slicer envelope (`data.type` / `data.action` / `data.data`) are accepted. Fields the
// payload does not carry keep the values `status` already has.
bool parse_report(const std::string &topic, const std::string &payload, Status &status);

// ---- signing and crypto -----------------------------------------------------

// True when /info.token is long enough for the 2.5.9.9 signed handshake (32 ASCII chars).
bool signed_handshake_supported(const std::string &info_token);

std::string md5_hex(const std::string &data);
// sign = md5( md5(token[0:16]) + ts + nonce )  — lowercase hex. `did` is not signed.
std::string sign_ctrl(const std::string &info_token, const std::string &ts_ms, const std::string &nonce);

bool aes128_cbc_decrypt(const std::string &key16, const std::string &iv16, const std::vector<unsigned char> &cipher, std::string &plain);
bool decrypt_ctrl(const Info &info, const CtrlBlob &blob, Credentials &out);

// ---- builders ---------------------------------------------------------------

// `model` empty → ST: ac:3dprinter:  (all models). Otherwise ST: ac:3dprinter:{model}
std::string ssdp_msearch_request(const std::string &model = std::string());

std::string bare_host(const std::string &host_or_url);
std::string lan_http_base(const std::string &host_or_url); // http://<ip>:18910
std::string info_url(const std::string &host_or_url);
std::string ctrl_url(const std::string &host_or_url, const std::string &ts_ms, const std::string &nonce, const std::string &sign,
                     const std::string &did);

std::string random_nonce(int length = 6);
std::string make_pc_device_id();
std::string make_msgid();
std::string now_ts_ms();

std::string mqtt_slicer_topic(const std::string &model_id, const std::string &cn, const std::string &leaf);
std::string mqtt_public_filter(const std::string &model_id, const std::string &cn);
std::string mqtt_broker_uri(const std::string &host, int port = MQTT_PORT);

// Nested slicer envelope (kobra-connect / ACSN strings).
nlohmann::json command_envelope(const std::string &msgid, const std::string &did, const std::string &bid, const std::string &type,
                                const std::string &action, const nlohmann::json &inner);
nlohmann::json print_start_data(const PrintStart &job);
std::vector<AmsBoxMap> ams_from_filament_mapping(const std::string &mapping);

// ---- hub row ----------------------------------------------------------------

std::string hub_state(const Status &s);
bool        is_printing(const Status &s);
bool        can_pause(const Status &s);
bool        can_resume(const Status &s);
bool        can_stop(const Status &s);
GUI::DeviceControls::Caps caps(const Snapshot &s);
void        fill_row(const Snapshot &s, const std::string &host, nlohmann::json &row);

// GET /info + signed POST /ctrl + AES decrypt. Uses Http. No MQTT.
bool handshake(const std::string &host_or_url, Info &info, Credentials &creds, std::string &error, int timeout_s = 8);

// UDP M-SEARCH to 239.255.255.250:1900. Untested against hardware.
bool discover(std::vector<SsdpDevice> &out, std::string &error, int timeout_ms = 3000, const std::string &model = std::string());

} // namespace AnycubicLan
} // namespace Slic3r

#endif
