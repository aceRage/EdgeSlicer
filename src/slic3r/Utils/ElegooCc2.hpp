#ifndef slic3r_ElegooCc2_hpp_
#define slic3r_ElegooCc2_hpp_

// The Elegoo Centauri Carbon 2 (and the Centauri 2) on the LAN. They share the "Elegoo Link" host type
// with the Centauri Carbon 1 but none of its protocol: no SDCP websocket on :3030, no
// POST /uploadFile/upload. Instead:
//
//   HTTP :80   GET  /system/info?X-Token=<code>   {"error_code":0,"system_info":{"sn":...}}
//              PUT  /upload                        chunks with Content-Range, X-File-Name,
//                                                  X-File-MD5 (lower-case hex) and X-Token;
//                                                  each answers {"error_code":0}
//   MQTT :1883 user "elegoo", password = the access code (default "123456")
//              elegoo/<sn>/api_register                 <- {"client_id","request_id"}
//              elegoo/<sn>/<request_id>/register_response -> {"client_id","error":"ok"}
//              elegoo/<sn>/<client_id>/api_request       <- {"id":n,"method":m,"params":{...}}
//              elegoo/<sn>/<client_id>/api_response      -> {"id":n,"method":m,"result":{"error_code":0,...}}
//   methods    1002 status (result.machine_status.status), 2005 CANVAS (result.canvas_info),
//              1020 start print {"storage_media":"local","filename":..,"config":{"delay_video",
//              "printer_check","print_layout":"A"|"B","bedlevel_force","slot_map":[{t,canvas_id,tray_id}]}}
//
// Facts from upstream OrcaSlicer's ElegooLink.cpp (system/info, the chunked PUT, the default token)
// and Elegoo's elegoo-link library (Apache-2.0: the MQTT topics, registration, method numbers and
// field names), as used by ElegooSlicer. No code of elegoo-link is copied.
//
// The CANVAS is optional on this printer (the "Combo" is the CC2 sold with one). Whether one is there
// is decided at run time from the 2005 answer, as ElegooSlicer does: no connected unit means no
// mapping step and an empty slot_map.
//
// Everything in this header except the functions marked "networking" is pure and is exercised by
// tests/slic3rutils/elegoo_cc2_tests.cpp with a scripted MQTT transport.

#include <map>
#include <string>
#include <vector>

#include "ElegooCanvas.hpp"

namespace Slic3r {
namespace ElegooCc2 {

constexpr int         MQTT_PORT     = 1883;
constexpr const char* MQTT_USER     = "elegoo";
constexpr const char* DEFAULT_TOKEN = "123456";

constexpr int METHOD_GET_ATTRIBUTES = 1001;
constexpr int METHOD_GET_STATUS     = 1002;
constexpr int METHOD_START_PRINT    = 1020;
constexpr int METHOD_GET_CANVAS     = 2005;

// Error codes the printer answers in result.error_code (elegoo-link's table).
constexpr int ERR_FILAMENT_RUNOUT       = 109;
constexpr int ERR_ACCESS_DENIED         = 1000;
constexpr int ERR_INVALID_PARAMETER     = 1001;
constexpr int ERR_INVALID_PARAMETER2    = 1003;
constexpr int ERR_BUSY                  = 1009;
constexpr int ERR_FILE_NOT_FOUND        = 1021;
constexpr int ERR_MISSING_BED_LEVELING  = 1026;

// "Elegoo Centauri Carbon 2" and "Elegoo Centauri 2" (any case, surrounding blanks ignored), the two
// models that speak this protocol. Not the Centauri Carbon 1, not anything else.
bool is_cc2_model(const std::string& printer_model);

// The access code the printer wants as X-Token and MQTT password: the preset's API key, else the
// printer's factory default.
std::string token_or_default(const std::string& apikey);

// The host part of a print_host value ("http://h:80/x" -> "h").
std::string host_of(const std::string& address);

// ---- HTTP answers ----

// /system/info: true and the serial number when error_code is 0 and system_info.sn is there.
bool parse_system_info(const std::string& body, std::string& serial, std::string& error);
// One /upload chunk: true when error_code is 0.
bool parse_upload_reply(const std::string& body, std::string& error);
// "bytes <first>-<last>/<total>" for one chunk.
std::string content_range(size_t offset, size_t length, size_t total);

// ---- MQTT topics and messages ----

std::string make_client_id(int n);                       // "1_PC_<n>"
std::string request_id_for(const std::string& client_id); // "<client_id>_req"
std::string register_topic(const std::string& sn);
std::string register_response_topic(const std::string& sn, const std::string& request_id);
std::string request_topic(const std::string& sn, const std::string& client_id);
std::string response_topic(const std::string& sn, const std::string& client_id);

std::string register_payload(const std::string& client_id, const std::string& request_id);

enum class RegisterResult { Ok, NotForUs, TooManyClients, Refused, Invalid };
// error: the printer's own words for Refused, a short English reason otherwise.
RegisterResult parse_register_response(const std::string& payload, const std::string& client_id, std::string& error);

// {"id":id,"method":method,"params":params_json} - params_json must be a JSON object text.
std::string request_payload(int id, int method, const std::string& params_json = "{}");

// What the start-print dialog chose, as ElegooLink's extended_info carries it (ElegooStartPrint.hpp:
// "timeLapse", "heatedBedLeveling", "bedType", "canvasAttached", "slotMap"). The slot_map key is
// always sent, as elegoo-link does; it holds entries only when a CANVAS was read and mapped.
// bedlevel_force: the retry after the printer answered ERR_MISSING_BED_LEVELING.
std::string start_print_params(const std::map<std::string, std::string>& extended_info,
                               int                                       pc_bed_type,
                               const std::string&                        filename,
                               bool                                      bedlevel_force);

struct Reply
{
    bool        valid      = false; // parsed, with an id
    int         id         = -1;
    int         method     = -1;
    int         error_code = -1;    // -1 when the result carries none
    std::string result;             // the result object as JSON text ("" when none)
};
Reply parse_reply(const std::string& payload);

constexpr int STATUS_IDLE         = 1;
constexpr int SUB_PRINT_COMPLETED = 2077;
// machine_status.status / .sub_status of a 1002 answer, -1 when absent.
int machine_status(const std::string& result_json);
int machine_sub_status(const std::string& result_json);
// The words the send dialog and the hub use ("standby", "printing", ...); "" for -1.
std::string state_name(int status);
// True when a print must not be started: anything but idle or a finished print; unknown (-1) is
// not busy.
bool status_is_busy(int status, int sub_status = -1);

// An error_code in the words the person sees (English; the GUI translates its own wrapper).
std::string error_text(int error_code);

// ---- the conversation, without a socket ----

class MqttTransport
{
public:
    virtual ~MqttTransport() = default;
    virtual bool connect(const std::string& client_id, std::string& error)                          = 0;
    virtual bool subscribe(const std::string& topic, std::string& error)                            = 0;
    virtual bool publish(const std::string& topic, const std::string& payload, std::string& error)  = 0;
    // One message, waiting at most timeout_ms. False on timeout or a dropped connection.
    virtual bool receive(std::string& topic, std::string& payload, int timeout_ms)                   = 0;
    virtual void close()                                                                             = 0;
};

struct SessionOptions
{
    int         register_timeout_ms = 3000; // elegoo-link waits 3 s
    int         reply_timeout_ms    = 5000;
    std::string client_id;                  // empty = generated
    int         first_id = 0;               // 0 = generated
};

// One registered session: connect, subscribe the reply topics, register, then call().
class Session
{
public:
    Session(MqttTransport& transport, std::string sn, const SessionOptions& opts = SessionOptions());
    ~Session();
    // False with error set when the printer could not be reached or refused us.
    bool open(std::string& error);
    // Publish one request and wait for its answer (same id); other messages are skipped.
    bool call(int method, const std::string& params_json, Reply& reply, std::string& error);
    void close();
    const std::string& client_id() const { return m_client_id; }

private:
    MqttTransport& m_transport;
    std::string    m_sn;
    SessionOptions m_opts;
    std::string    m_client_id;
    std::string    m_request_id;
    int            m_next_id = 1;
    bool           m_open    = false;
};

// 1002 then 2005 over an already-opened session. Fills the same CanvasQueryResult the CC1 path
// does: ok with info when a connected CANVAS answered, no_canvas when the printer has none.
CanvasQueryResult canvas_query_over(Session& session);

struct StartResult
{
    bool        ok         = false;
    bool        busy       = false; // refused before sending: the printer is not idle
    int         error_code = -1;
    std::string error;              // English, for the log and the dialog
    std::string payload;            // the last start command sent, for the log and the tests
};
// Status check (up to busy_checks answers, busy_interval_ms apart, while the printer is busy), then
// 1020; one retry with bedlevel_force when the printer asks for leveling data and the person asked
// for leveling (ElegooSlicer does the same).
StartResult start_print_over(Session&                                  session,
                             const std::map<std::string, std::string>& extended_info,
                             int                                       pc_bed_type,
                             const std::string&                        filename,
                             int                                       busy_checks      = 10,
                             int                                       busy_interval_ms = 1000);

// ---- networking (ElegooCc2Net.cpp) ----

// GET /system/info. Blocking; bounded by timeout_s.
bool fetch_serial(const std::string& address, const std::string& token, std::string& serial, std::string& error, int timeout_s = 5);

// The whole CANVAS read for a printer at `address`: serial, MQTT session, 1002, 2005.
CanvasQueryResult canvas_query(const std::string& address, const std::string& token,
                               const CanvasQueryOptions& opts = CanvasQueryOptions());

// The start command after an upload.
StartResult start_print(const std::string&                        address,
                        const std::string&                        token,
                        const std::map<std::string, std::string>& extended_info,
                        int                                       pc_bed_type,
                        const std::string&                        filename);

} // namespace ElegooCc2
} // namespace Slic3r

#endif
