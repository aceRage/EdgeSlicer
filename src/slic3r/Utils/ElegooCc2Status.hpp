#ifndef slic3r_ElegooCc2Status_hpp_
#define slic3r_ElegooCc2Status_hpp_

// What an Elegoo Centauri Carbon 2 (or Centauri 2) reports over MQTT, turned into the printer row the
// hub's /summary, the Printers tab, the phone app and the CC2 device page read for every other
// printer (the same row Sdcp::fill_row writes for a Centauri Carbon 1).
//
// Pure: no socket, no wx. The connection that feeds it lives in ElegooCc2Monitor.cpp; the tests
// (tests/slic3rutils/elegoo_cc2_monitor_tests.cpp) drive everything here with made-up messages.
//
// The shapes come from Elegoo's elegoo-link library (Apache-2.0), the transport ElegooSlicer links:
// src/lan/adapters/elegoo_fdm_cc2/elegoo_fdm_cc2_message_adapter.cpp - the 1002 answer is the whole
// status, a method 6000 push on elegoo/<sn>/api_status is a delta merged into it key by key (objects
// recursively, everything else replaced), the push ids count up by one and five gaps in a row mean
// "ask for the whole status again"; machine_status {status, sub_status, progress, exception_status},
// print_status {filename, current_layer, total_layer, print_duration, total_duration,
// remaining_time_sec}, extruder / heater_bed {temperature, target}, ztemperature_sensor
// {temperature} (elegoo-link calls it the chamber), fans {fan, aux_fan, box_fan, heater_fan,
// controller_fan: {speed (percent), rpm}}, led {status}, canvas_info (the 2005 shape). No code of
// elegoo-link is copied. None of it is checked against a real CC2 yet (the owner has none).

#include "ElegooCanvas.hpp"
#include "slic3r/GUI/DeviceControls.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace Slic3r {
namespace ElegooCc2 {

// machine_status.status, elegoo-link's table.
enum MachineState
{
    MS_INITIALIZING    = 0,
    MS_IDLE            = 1,
    MS_PRINTING        = 2,
    MS_FILAMENT_OP     = 3, // 3 and 4
    MS_FILAMENT_OP2    = 4,
    MS_AUTO_LEVELING   = 5,
    MS_PID_CALIBRATING = 6,
    MS_RESONANCE_TEST  = 7,
    MS_SELF_CHECK      = 8,
    MS_UPDATING        = 9,
    MS_HOMING          = 10,
    MS_FILE_TRANSFER   = 11,
    MS_VIDEO_COMPOSING = 12,
    MS_EXTRUDER_OP     = 13,
    MS_EMERGENCY_STOP  = 14,
    MS_POWER_LOSS      = 15,
};

// machine_status.sub_status while printing (status 2), elegoo-link's table.
enum PrintSubState
{
    SUB_PAUSING          = 2501,
    SUB_PAUSED           = 2502,
    SUB_PAUSED_2         = 2505,
    SUB_STOPPING         = 2503,
    SUB_STOPPED          = 2504,
    SUB_RESUMING         = 2401,
    SUB_RESUMED          = 2402,
    SUB_PRINTING         = 2075,
    SUB_COMPLETED        = 2077,
};

// The printer's status: the last whole answer (1002) with every push since merged in.
struct Status
{
    bool           valid { false };   // a whole status arrived (1002); pushes before that are dropped
    nlohmann::json raw = nlohmann::json::object(); // the merged result object
    int            last_push_id { -1 };
    int            gaps { 0 };        // pushes in a row whose id was not the next one

    int machine() const;    // machine_status.status, -1 unknown
    int sub() const;        // machine_status.sub_status, -1 unknown
};

// A 1002 answer's result: replaces everything.
void merge_full(Status& s, const nlohmann::json& result);

enum class PushResult
{
    Merged,     // merged
    NeedFull,   // not merged (no whole status yet), or five gaps in a row: ask 1002 again
    Ignored,    // not a status push
};
// A message on the status topic: {"id":n,"method":6000,"result":{...}}.
PushResult merge_push(Status& s, const std::string& payload);
// The same, already parsed.
PushResult merge_push(Status& s, const nlohmann::json& message);

// Readings (all -1 / false / "" when the printer did not say).
double      progress(const Status& s);           // machine_status.progress, percent
std::string filename(const Status& s);
int         current_layer(const Status& s);
int         total_layer(const Status& s);
long long   seconds_left(const Status& s);       // print_status.remaining_time_sec
std::vector<int> exception_codes(const Status& s); // machine_status.exception_status
bool        has_canvas_info(const Status& s);
// The CANVAS as pushed in the status (canvas_info), parsed by the shared CANVAS parser.
bool        canvas_from_status(const Status& s, CanvasInfo& out);

// The state in the words the hub uses for every other printer: printing | paused | complete |
// cancelled | error | standby; "" before the first whole status. The steps on the way (preheating,
// homing, leveling, pausing, stopping, resuming) are "printing" while the machine is on a print, as
// the CC1's are.
std::string hub_state(const Status& s);
// The step the printer is on, for the card's second line ("Preheating the nozzle", "Auto-leveling",
// "Pausing", "Receiving a file"); "" when hub_state says it all.
std::string phase_text(const Status& s);
bool        is_printing(const Status& s); // a print is under way (paused counts)
bool        can_pause(const Status& s);
bool        can_resume(const Status& s);
bool        can_stop(const Status& s);

// Everything the connection knows, copied out under its lock.
struct Snapshot
{
    bool        exists { false };
    bool        connected { false };
    bool        ever_connected { false };
    bool        connecting { false };
    bool        other_window { false }; // another EdgeSlicer window on this PC holds the connection
    int         failures { 0 };
    std::string error;                  // why it is not connected (English, no address, no serial)
    long long   status_at_ms { 0 };     // steady clock: when the last status (whole or push) arrived
    long long   now_ms { 0 };           // steady clock when the snapshot was taken (for age_s)
    Status      status;
    std::string firmware;               // 1001 software_version.ota_version
    bool        has_canvas { false };   // a connected CANVAS was read (2005 or the status)
    bool        canvas_known { false }; // the printer has said whether it has one
    CanvasInfo  canvas;
};

// The controls a CC2 row offers: bed, nozzle and chamber as readings, the light and the fans as
// readings. elegoo-link's LAN table leaves the CC2's set-temperature (1028), light (1029) and fan
// (1030) methods commented out, so nothing is settable from here yet.
GUI::DeviceControls::Caps caps(const Snapshot& s);

// The printer row for /api/printers, /summary, the Printers tab and the device page, written into
// `row` (which already has id, name, model, kind...). The same fields Sdcp::fill_row writes, with
// "cc2" (protocol details) where the CC1 row has "sdcp", and "cc2_connected" where it has
// "sdcp_connected". `stale_after_s`: a status older than this is reported but its buttons are off.
void fill_row(const Snapshot& s, nlohmann::json& row, int stale_after_s = 120);

} // namespace ElegooCc2
} // namespace Slic3r

#endif // slic3r_ElegooCc2Status_hpp_
