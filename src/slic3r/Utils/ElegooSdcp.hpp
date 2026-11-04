#ifndef slic3r_ElegooSdcp_hpp_
#define slic3r_ElegooSdcp_hpp_

// What an Elegoo Centauri Carbon 1 (CC1) reports over SDCP, turned into the printer row the hub, the
// Printers monitor and the phone app already read for a Bambu printer or a Snapmaker U1.
//
// Pure: no socket, no wx. The persistent connection that feeds it lives in ElegooSdcpMonitor.cpp;
// the tests (tests/slic3rutils/elegoo_sdcp_tests.cpp) drive everything here with frames shaped like
// the owner's capture (firmware V1.4.49, SDCP V3.0.0), with made-up ids.
//
// Sources for the numbers (no code copied from any of them):
//  * Elegoo's SDCP V3.0.0 specification (github.com/cbd-tech/SDCP-Smart-Device-Control-Protocol-V3.0.0):
//    the envelope, Cmd 0 / 1 / 129 / 130 / 131, the status and attribute objects, "ping" / "pong".
//  * elegoo-link (github.com/ELEGOO-3D/elegoo-link, Apache-2.0), the transport ElegooSlicer links:
//    src/lan/adapters/elegoo_fdm_cc/elegoo_fdm_cc_message_adapter.cpp - the CC1 machine status table
//    (sdcp_machine_status_t, 0..11), the print status table (sdcp_print_status_t, 0..26), the active
//    command table (0, 1, 128, 129 pause, 130 stop, 131 resume, 324 CANVAS), and the fan / light /
//    temperature fields of Cmd 403; src/lan/adapters/elegoo_cc_adapters.h - the "ping" heartbeat.
//  * OpenCentauri docs/software/api.md (MIT): Cmd 403 TargetFanSpeed {ModelFan, AuxiliaryFan, BoxFan},
//    and the status fields (LightStatus.SecondLight, RgbLight, CurrentFanSpeed).
//  * elegoo-homeassistant (github.com/danielcherubini/elegoo-homeassistant, MIT):
//    custom_components/elegoo_printer/websocket/client.py set_light_status / set_fan_speed (Cmd 403
//    {"LightStatus":{"SecondLight","RgbLight"}} and {"TargetFanSpeed":{...}}), light.py (on = SecondLight
//    1 with RgbLight [255,255,255], off = 0 with [0,0,0]).

#include "ElegooCanvas.hpp"
#include "slic3r/GUI/DeviceControls.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace Slic3r {
namespace Sdcp {

constexpr int CMD_STATUS     = 0;
constexpr int CMD_ATTRIBUTES = 1;
constexpr int CMD_PAUSE      = 129;
constexpr int CMD_STOP       = 130;
constexpr int CMD_RESUME     = 131;
constexpr int CMD_CANVAS     = 324;
constexpr int CMD_CONTROL    = 403; // fans, light (and, unused here, temperatures and speed)

// CurrentStatus[0], elegoo-link sdcp_machine_status_t.
enum MachineStatus
{
    MACHINE_IDLE              = 0,
    MACHINE_PRINTING          = 1,
    MACHINE_FILE_TRANSFERRING = 2,
    MACHINE_EXPOSURE_TESTING  = 3,
    MACHINE_DEVICES_TESTING   = 4,
    MACHINE_AUTO_LEVELING     = 5,
    MACHINE_RESONANCE_TESTING = 6,
    MACHINE_BUSY              = 7,
    MACHINE_FILE_CHECKING     = 8,
    MACHINE_HOMING            = 9,
    MACHINE_FEEDING_OUT       = 10,
    MACHINE_PID_DETECT        = 11,
};

// PrintInfo.Status, elegoo-link sdcp_print_status_t (0..10 are the specification's own; 2..4 are
// resin-only and never seen on a CC1).
enum PrintStatus
{
    PRINT_IDLE                  = 0,
    PRINT_HOMING                = 1,
    PRINT_PAUSING               = 5,
    PRINT_PAUSED                = 6,
    PRINT_STOPPING              = 7,
    PRINT_STOPPED               = 8,
    PRINT_COMPLETE              = 9,
    PRINT_FILE_CHECKING         = 10,
    PRINT_PRINTERS_CHECKING     = 11,
    PRINT_RESUMING              = 12,
    PRINT_PRINTING              = 13,
    PRINT_ERROR                 = 14,
    PRINT_AUTO_LEVELING         = 15,
    PRINT_PREHEATING            = 16,
    PRINT_RESONANCE_TESTING     = 17,
    PRINT_START                 = 18,
    PRINT_AUTO_LEVELING_DONE    = 19,
    PRINT_PREHEATING_DONE       = 20,
    PRINT_HOMING_DONE           = 21,
    PRINT_RESONANCE_TESTING_DONE = 22,
    PRINT_AUTO_FEEDING          = 23,
    PRINT_FEEDING_OUT           = 24,
    PRINT_FEEDING_OUT_ABNORMAL  = 25,
    PRINT_FEEDING_OUT_PAUSED    = 26,
};

// The printer's status, merged from every Cmd 0 status push: a field the frame does not carry keeps
// the value an earlier frame gave it, so a firmware that pushes only what changed still adds up to
// the whole picture.
struct Status
{
    bool             valid { false };   // at least one status object was merged
    std::vector<int> current_status;    // CurrentStatus; [0] is what the machine is doing
    int              print_status { -1 }; // PrintInfo.Status
    int              current_layer { -1 }, total_layer { -1 };
    long long        current_ticks { -1 }, total_ticks { -1 }; // seconds on the CC1 (fw V1.4.49)
    double           progress { -1 };   // PrintInfo.Progress, percent
    std::string      filename;          // PrintInfo.Filename
    int              speed_pct { -1 };  // PrintInfo.PrintSpeedPct
    int              error_number { -1 }; // PrintInfo.ErrorNumber
    bool             has_nozzle { false }, has_bed { false }, has_box { false };
    double           nozzle { 0 }, nozzle_target { 0 }, bed { 0 }, bed_target { 0 }, box { 0 }, box_target { 0 };
    int              model_fan { -1 }, aux_fan { -1 }, box_fan { -1 }; // CurrentFanSpeed, percent
    int              second_light { -1 };   // LightStatus.SecondLight: 0 off, 1 on
    std::vector<int> rgb_light;             // LightStatus.RgbLight (or a top-level RgbLight)
    int              ams_connect { -1 };    // AmsConnectStatus: 1 a CANVAS is attached, 0 not
    int              timelapse { -1 };      // TimeLapseStatus
    bool             has_z_offset { false };
    double           z_offset { 0 };

    int machine() const { return current_status.empty() ? -1 : current_status.front(); }
    bool machine_has(int code) const;
};

// Cmd 1. `mainboard_id` is only ever put back into our own requests: never logged, never in a row.
struct Attributes
{
    bool                     valid { false };
    std::string              firmware;     // FirmwareVersion, "V1.4.49"
    std::string              machine_name; // MachineName (the model, "Centauri Carbon")
    std::string              protocol;     // ProtocolVersion
    std::string              mainboard_id;
    std::vector<std::string> capabilities; // FILE_TRANSFER, PRINT_CONTROL, VIDEO_STREAM
    int                      camera_status { -1 };     // CameraStatus: 1 connected
    int                      max_video_streams { -1 }; // MaximumVideoStreamAllowed
    int                      video_streams { -1 };     // NumberOfVideoStreamConnected
};

enum class FrameKind
{
    Unknown,    // not JSON, or JSON we have no use for
    Pong,       // the heartbeat's answer
    Status,     // a status object (push, or the answer to Cmd 0)
    Attributes, // an attributes object
    Response,   // {"Data":{"Cmd":n,"Data":{"Ack":..},"RequestID":..}} - also the CANVAS payload
};

struct Frame
{
    FrameKind   kind { FrameKind::Unknown };
    int         cmd { -1 };         // Response: Data.Cmd
    int         ack { -1 };         // Response: Data.Data.Ack, -1 when absent
    std::string request_id;         // Response: Data.RequestID
    bool        has_canvas { false }; // carries a canvas_list somewhere (Cmd 324's payload)
    bool        ams_connect_seen { false };
};

// One frame off the socket: what it is, and - for a status or attributes frame - merged into
// `status` / `attrs`. Never throws.
Frame read_frame(const std::string& text, Status& status, Attributes& attrs);

// A request in the envelope every SDCP client sends (spec "Request Parameters"; ElegooLink's start
// print): {"Id":"","Data":{"Cmd","Data","RequestID","MainboardID","TimeStamp","From":1}[,"Topic"]}.
// MainboardID and Topic are filled once Cmd 1 has told us the id, and left empty before, which is
// what the CANVAS read and the start-print command already send to the owner's printer.
std::string request_text(int cmd, const nlohmann::json& data, const std::string& request_id, long long timestamp_ms,
                         const std::string& mainboard_id);

// ---- controls ----

// Cmd 403's TargetFanSpeed key for one of the row's fan ids ("part" ModelFan, "aux" AuxiliaryFan,
// "chamber" BoxFan); empty for anything else.
std::string fan_key(const std::string& fan_id);
// {"TargetFanSpeed":{"<key>":percent}}
nlohmann::json fan_data(const std::string& fan_id, int percent);
// {"LightStatus":{"SecondLight":1,"RgbLight":[255,255,255]}} / {..0, [0,0,0]} - Home Assistant's pair.
nlohmann::json light_data(bool on);

// Ack of a print control (spec SDCP_PRINT_CTRL_ACK_*): 0 ok, 1 busy, 2 file not found, ... as a
// sentence for the phone.
std::string ack_text(int cmd, int ack);

// ---- what the hub says about it ----

// The printer's state in the words the hub already uses for a Klipper printer (RemoteEvents
// klipper_state, the app's PrintPhase): printing | paused | complete | cancelled | error | standby.
// The steps on the way (homing, preheating, levelling, pausing, stopping) are "printing" while the
// machine is on a print, so the event watcher sees one clean start and one clean end. Empty before
// the first status frame.
std::string hub_state(const Status& s);
// The step the printer is on, for the card's second line: "Preheating", "Auto-leveling", "Pausing"...
// Empty when there is nothing more to say than hub_state.
std::string phase_text(const Status& s);
bool        is_printing(const Status& s); // a print is under way (paused counts)
bool        can_pause(const Status& s);
bool        can_resume(const Status& s);
bool        can_stop(const Status& s);
// Seconds left: TotalTicks - CurrentTicks, never negative; -1 when the printer did not say.
long long   seconds_left(const Status& s);

// Everything the connection knows, copied out under its lock.
struct Snapshot
{
    bool        exists { false };         // a connection was asked for
    bool        connected { false };      // the websocket is open and has answered
    bool        ever_connected { false };
    bool        connecting { false };     // an attempt is in flight right now
    bool        other_window { false };   // another EdgeSlicer window on this PC holds the connection
    int         failures { 0 };           // attempts in a row that failed
    std::string error;                    // why it is not connected (English, no address in it)
    long long   status_at_ms { 0 };       // when the last status frame arrived (steady clock)
    Status      status;
    Attributes  attrs;
    bool        has_canvas { false };
    CanvasInfo  canvas;
    long long   canvas_at_ms { 0 };
};

// The controls a CC1 row offers (DeviceControls::Caps): bed, nozzle and chamber as readings only,
// the speed as a reading, the light and the three fans as settable (Cmd 403).
GUI::DeviceControls::Caps caps(const Snapshot& s);

// The Canvas units in the hub's `ams` shape (the Bambu one the app draws):
// [{id, type "canvas", name "CANVAS 1", nozzle 0, side "", trays [{id, exists, type, sub_type, color,
// active}]}]. `remain` is left out (the printer does not know it), and nothing offers load / unload.
nlohmann::json canvas_ams_json(const CanvasInfo& canvas);

// The printer row for /api/printers, /summary and the Printers monitor, written into `row` (which
// already has id, name, model, kind...). `host` is the printer's bare address, for the camera hint.
void fill_row(const Snapshot& s, const std::string& host, nlohmann::json& row);

} // namespace Sdcp
} // namespace Slic3r

#endif // slic3r_ElegooSdcp_hpp_
