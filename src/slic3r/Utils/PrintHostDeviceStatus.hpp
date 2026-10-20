#ifndef slic3r_PrintHostDeviceStatus_hpp_
#define slic3r_PrintHostDeviceStatus_hpp_

#include <string>
#include <vector>

#include "PrintHostDevices.hpp"
#include "ElegooCanvas.hpp"

// Asking one print-host device what it is doing and what is loaded in it.
//
// This is the honest half of the "map the plate's filaments to the printer's slots" story, and the
// honesty is the point: **most print hosts cannot answer the second question at all.**
//
//  * Elegoo Link (the Centauri Carbon) speaks SDCP over its own websocket on port 3030. The print
//    status (`Cmd 0`) names no filament; a Centauri Carbon 1 with a CANVAS attached answers
//    `Cmd 324` with its trays (ElegooCanvas.hpp, read by the sidebar sync). Without one, or for any
//    other Elegoo model, this returns `probed = false` with a note that says so, and the send goes
//    out exactly as the plate was sliced.
//  * A Moonraker/Klipper box does answer. `/printer/objects/query` gives `print_stats.state`, and a
//    Snapmaker-flavoured Moonraker also carries `print_task_config`, whose parallel arrays are the
//    loaded filaments per toolhead (the same object SnapmakerLan::toolheads_of reads,
//    SnapmakerLan.cpp:456, and the same one SSWCP's machine filament info comes from). Where there
//    is no `print_task_config`, the `extruder`/`extruder1`... objects still say how many tools the
//    machine has, which is a slot list without materials in it.
//  * A QIDI printer with a QIDI Box (X-Max 4, X-Plus 4, Q2) is a Moonraker box too, and its
//    `multi_color_controller` object lists every Box slot with the material the printer read off
//    its RFID tag or was told on the touchscreen - the same object QIDIStudio's device page reads.
//    The Box firmware decides which slot feeds `T<n>` from the saved variable `value_t<n>`, which
//    only the sending slicer sets, so for these printers the mapping is not a nicety:
//    qidi_box_mapping_script() is what the send path runs before the print starts.
//  * A PrusaLink / PrusaConnect printer (a Buddy board on an MK4/MK4S/MINI/CORE One, or the
//    PrusaLink daemon in front of an MK3S) answers its own REST API: `/api/v1/status` gives the
//    state and both heaters, `/api/v1/job` the file it is printing. See PrusaLinkStatus.hpp. It
//    names no filaments at all - the API has no filament-identity story - so it comes back online
//    with a state and an empty slot list.
//  * Everything else (Duet, Repetier, SimplyPrint, ...) is not probed: this fork has no client for
//    their status APIs, and guessing would cost a timeout per address.
//
// Blocking. Never call it from the GUI thread.

namespace Slic3r {
namespace PrintHostDevices {

// One place a filament can be, as the printer describes it.
struct Slot
{
    int         index { 0 };    // 0-based, the tool/extruder number (QIDI Box: box * 4 + slot)
    // Its own name, "" = none. A QIDI Box slot: "Box 1 slot 2", shown instead of "T<n>". A CANVAS
    // tray: what the remembered mapping matches it by ("ELEGOO PLA Silk"); label() shows "Tray <n>".
    std::string name;
    std::string type;          // "PLA", "" = the printer did not say
    std::string sub_type;
    std::string vendor;
    std::string color;          // "#RRGGBB", "" = the printer did not say
    bool        loaded { false };
    double      nozzle { 0.0 };
    // A CANVAS tray (Elegoo Centauri Carbon 1): the raw pair the start-print slot_map names, -1 for
    // every other kind of slot. `index` stays the flat 0-based number shown and stored elsewhere.
    int         unit { -1 };
    int         tray { -1 };
    int         min_temp { 0 }; // the tray's nozzle range, 0 = unknown
    int         max_temp { 0 };

    // Something to put in a dropdown.
    std::string label() const;
};

struct Status
{
    bool              probed { false }; // this host type has a status API this fork can speak
    bool              online { false }; // and it answered
    std::string       state;            // print_stats.state ("standby", "printing", ...), "" = unknown
    std::string       error;            // why it did not answer
    std::string       note;             // in words, for the dialog: why there is no slot list
    std::vector<Slot> slots;            // empty = no mapping is possible, see `note`
    // The printer named the materials, not just the tools. Only then is a mapping table worth
    // showing: a bare tool list with nothing in it tells a person nothing they did not know.
    bool              slots_have_filaments { false };
    // The slots are a QIDI Box's: a filament left "As sliced" is fed from whichever slot the
    // printer last used for that tool, so the dialog matches by material, never by colour alone.
    bool              qidi_box { false };
    // QIDI firmware (X-Max 4, X-Plus 4, Q2, X-Plus 5 - the ones with a multi_color_controller,
    // Box or not): the send dialog offers QIDIStudio's print options. qidi_polar_cooler: the
    // printer has the extruder cooler that QIDIStudio's "Polar cooler" switch drives.
    bool              qidi { false };
    bool              qidi_polar_cooler { false };
    // The slots are CANVAS trays read over SDCP (Cmd 324), so the start command can carry a slot_map.
    bool              canvas { false };
};

// QIDIStudio's print options, as the send dialog's checkboxes answered them. -1 = not offered, so
// the printer keeps its own setting; 0 = off; 1 = on.
struct QidiPrintOptions
{
    int use_box { -1 };      // SAVE_VARIABLE enable_box
    int bed_leveling { -1 }; // G31 (mesh before the print) / G32 (use the saved mesh)
    int polar_cooler { -1 }; // SAVE_VARIABLE enable_polar_cooler
};

// The G-code QIDIStudio sends before a LAN start, less the bed leveling: the Box (on with the
// slot mapping, or off), then the polar cooler. Newline-separated for one /printer/gcode/script
// call; empty when there is nothing to send. The send stops if the printer refuses it - a Box
// left on the previous print's slots prints the wrong material. The bed leveling is
// qidi_leveling_script(), its own call, and the timelapse is Moonraker's own setting, not G-code.
std::string qidi_start_script(const QidiPrintOptions& options, const std::string& mapping);

// "G31\n" (probe a fresh mesh before the print) / "G32\n" (use the saved one) / "" (not offered).
// Its own /printer/gcode/script call after qidi_start_script(): a refusal is logged and the print
// goes ahead, it never blocks the Box mapping or the print.
std::string qidi_leveling_script(const QidiPrintOptions& options);

// The send dialog's starting values for a QIDI printer. The Box starts on only when the printer
// said at least one Box slot is loaded; bed leveling, timelapse and polar cooler start off unless
// the person's last choice was remembered ("1" / "0" from the app config, "" = never chosen).
struct QidiOptionDefaults
{
    bool use_box { false };
    bool bed_leveling { false };
    bool timelapse { false };
    bool polar_cooler { false };
};
QidiOptionDefaults qidi_option_defaults(const Status& st, const std::string& remembered_leveling,
                                        const std::string& remembered_timelapse, const std::string& remembered_polar_cooler);

// "0:3,1:5" (filament:slot, PrintHostSendDialog::filament_mapping) -> the commands QIDIStudio
// sends before a QIDI Box print: enable the Box, then point each tool's `value_t<n>` at its slot.
// Newline-separated, for one /printer/gcode/script call. Empty when nothing in the mapping is
// usable (the firmware keeps 16 tools and 16 Box slots, 0-based).
std::string qidi_box_mapping_script(const std::string& mapping);

// print_stats.state names a print in progress: "printing" or "paused". A QIDI printer in either
// state is not sent its start-of-print script - re-pointing value_t<n> or enable_box under a
// running print would change what it feeds - and the send dialog does not offer to start a print.
bool busy_printing(const std::string& state);
// print_stats.state out of a /printer/objects/query?print_stats reply; "" when it does not say.
std::string print_state_from_query(const std::string& reply_json);
// Blocking, off the GUI thread: one read-only /printer/objects/query?print_stats against
// `base_url` (http(s)://host[:port], PrinterStorage::base_url_of). False with `error` when the
// printer did not answer or did not say.
bool query_print_state(const std::string& base_url, std::string& state, std::string& error, int timeout_s = 5);

// The send dialog's auto-match for one plate filament (`colour` "#RRGGBB", `type` "PLA"): the
// 1-based index into `slots` it preselects, 0 = "As sliced". Every host but a QIDI Box: the
// closest colour (redmean) among the loaded slots that have a colour. A QIDI Box: only loaded
// slots of the filament's own material, closest colour first, then one with no colour.
int auto_match_slot(const std::vector<Slot>& slots, const std::string& colour, const std::string& type, bool qidi_box);

// ---- Starting a print on a QIDI Box printer -----------------------------------------------------
//
// The Box firmware feeds T<n> from the slot its saved variable value_t<n> names, and only the
// sending slicer sets it. A print started without a fresh mapping runs on whatever the printer was
// told for its previous print: a PLA plate fed from the ASA slot, at PLA settings. So every path
// that starts a print on such a printer - the desktop send dialog, the hub / phone / Stream tab
// send, an archive reprint - asks qidi_send_policy() first, and only Start lets it go.

// The profile's printer_model names a QIDI that can carry a QIDI Box (Q2 / Q2C, X-Max 4,
// X-Plus 4, X-Plus 5). Used only when the printer itself could not be asked: such a printer is
// then treated as having a Box, so nothing is started on a mapping nobody has seen.
bool qidi_box_capable_model(const std::string& printer_model);

// A plate filament's material against a Box slot's ("PLA" vs "ASA"). Case and surrounding blanks
// do not count; an unknown material on either side is not a match.
bool filament_types_match(const std::string& plate_type, const std::string& slot_type);

// One plate filament, as the policy and the automatic mapping need it.
struct SendFilament
{
    int         index { 0 };    // 0-based, the slicer's filament/extruder number (T<index>)
    std::string type;           // "PLA"
    std::string colour;         // "#RRGGBB"
    bool        used { false }; // this plate prints with it
};

// What is known when a print is about to start.
struct QidiSendFacts
{
    bool model_may_have_box { false }; // qidi_box_capable_model(printer_model)
    bool probe_ok { false };           // the printer answered the status query (Status::online)
    bool qidi { false };               // it answered as QIDI firmware (Status::qidi)
    bool box_present { false };        // with a QIDI Box plugged in (Status::qidi_box)
    bool busy { false };               // printing or paused
    bool use_box { true };             // the Box is used for this print (the dialog's switch)
    int  used_filaments { 0 };         // filaments this plate prints with
    int  mapped_filaments { 0 };       // ... that have a Box slot picked
    int  type_mismatches { 0 };        // ... whose slot holds another material (or an unknown one)
};

enum class QidiSendPolicy
{
    NotApplicable,       // not a QIDI Box printer: the send goes on exactly as before
    Start,               // mapping complete (or the Box is off / absent): start the print
    UploadOnlyBusy,      // the printer is printing or paused: never re-point its Box under a print
    UploadOnlyNoProbe,   // a Box printer that could not be read: upload only unless the person chooses to print
    NeedsMapping,        // a used filament has no Box slot: the printer would use its stale value_t<n>
    ConfirmTypeMismatch, // a filament goes to a slot holding another material: ask first
};

QidiSendPolicy qidi_send_policy(const QidiSendFacts& facts);

// The automatic material-first mapping (auto_match_slot with qidi_box) for every used filament,
// as the send dialog would preselect it. For sends with nobody at a dialog: the hub, the phone,
// an archive reprint. `mapping` is "filament:slot,..." for qidi_start_script(); `facts` is filled
// from `st` and the filaments (model_may_have_box is left to the caller).
struct QidiAutoMapping
{
    std::string   mapping;
    QidiSendFacts facts;
};
QidiAutoMapping qidi_auto_mapping(const Status& st, const std::vector<SendFilament>& filaments);

// Plain English, for the hub / phone job result and the log: why the print was not started.
// Empty for NotApplicable and Start.
std::string qidi_send_refusal(QidiSendPolicy policy);

// Blocking, off the GUI thread. `timeout_s` is per HTTP call.
Status probe(const Device& d, int timeout_s = 3);

// The half of probe() that has no network in it: a Moonraker `result.status` object turned
// into slots and a state. Exposed so the parsing is exercised by the Catch2 suite instead of
// only by a printer on somebody's desk. `status_json` is the JSON text of that object.
Status parse_moonraker_status(const std::string& status_json, const std::string& host_type_key);

// The half of the Elegoo probe that has no network in it: a CANVAS query result as slots.
Status status_from_canvas(const CanvasQueryResult& result);

// What can be said before asking: false for every host type this fork has no status client for.
bool   can_probe(const std::string& host_type_key);
// The sentence the dialog shows when `probe()` came back with nothing to map.
std::string no_filament_note(const std::string& host_type_key);

} // namespace PrintHostDevices
} // namespace Slic3r

#endif // slic3r_PrintHostDeviceStatus_hpp_
