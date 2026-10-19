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
    // The slots are CANVAS trays read over SDCP (Cmd 324), so the start command can carry a slot_map.
    bool              canvas { false };
};

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
