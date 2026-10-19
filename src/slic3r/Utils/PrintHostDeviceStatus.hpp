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
//  * A PrusaLink / PrusaConnect printer (a Buddy board on an MK4/MK4S/MINI/CORE One, or the
//    PrusaLink daemon in front of an MK3S) answers its own REST API: `/api/v1/status` gives the
//    state and both heaters, `/api/v1/job` the file it is printing. See PrusaLinkStatus.hpp. It
//    names no filaments at all - the API has no filament-identity story - so it comes back online
//    with a state and an empty slot list.
//  * A CrealityPrint host (K1/K2 + CFS) answers /info and a port-9999 WebSocket `boxsInfo`.
//    Both calls use a real deadline (CrealityWsClient / Http timeout); the send dialog runs
//    this probe off the UI thread and posts the result back.
//  * Everything else (Duet, Repetier, SimplyPrint, ...) is not probed: this fork has no client for
//    their status APIs, and guessing would cost a timeout per address.
//
// Blocking. The send dialog must not call this on the UI thread; it runs the probe on a
// worker and posts the result. timeout_s is the per-call deadline (Creality uses ~1 s).

namespace Slic3r {
namespace PrintHostDevices {

// One place a filament can be, as the printer describes it.
struct Slot
{
    int         index { 0 };    // 0-based, the tool/extruder number
    std::string type;           // "PLA", "" = the printer did not say
    std::string sub_type;
    std::string vendor;
    std::string color;          // "#RRGGBB", "" = the printer did not say
    bool        loaded { false };
    double      nozzle { 0.0 };
    // A CANVAS tray (Elegoo Centauri Carbon 1): the raw pair the start-print slot_map names, -1 for
    // every other kind of slot. `index` stays the flat 0-based number shown and stored elsewhere.
    int         unit { -1 };
    int         tray { -1 };
    std::string name;           // what the remembered mapping matches it by ("ELEGOO PLA Silk")
    int         min_temp { 0 }; // the tray's nozzle range, 0 = unknown
    int         max_temp { 0 };
    // A Creality CFS slot (port-9999 boxsInfo). boxId / materialId are what colorMatch sends;
    // -1 for every other kind of slot. External spool holder: cfs_external.
    int         cfs_box { -1 };
    int         cfs_material { -1 };
    bool        cfs_external { false };

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
    // The slots are CANVAS trays read over SDCP (Cmd 324), so the start command can carry a slot_map.
    bool              canvas { false };
    // CrealityPrint host: the probe spoke the port-9999 boxsInfo WebSocket and found CFS boxes.
    bool              cfs { false };
    int               cfs_box_count { 0 };
    std::string       cfs_variant; // "CFS", "CFS-C", ... when the printer named one
};

// Blocking. Callers that own a GUI run this on a worker. `timeout_s` is the per-call deadline.
Status probe(const Device& d, int timeout_s = 3);

// The half of probe() that has no network in it: a Moonraker `result.status` object turned
// into slots and a state. Exposed so the parsing is exercised by the Catch2 suite instead of
// only by a printer on somebody's desk. `status_json` is the JSON text of that object.
Status parse_moonraker_status(const std::string& status_json, const std::string& host_type_key);

// The half of the Elegoo probe that has no network in it: a CANVAS query result as slots.
Status status_from_canvas(const CanvasQueryResult& result);

// The half of the Creality probe that has no network in it: a boxsInfo body as slots.
Status status_from_cfs(const std::string& boxs_info_json);

// Material-first auto-match (research X3 / #610), with Creality's CFS-over-external ranking
// when `cfs_order` is true: CFS type+colour > CFS type > ext type+colour > ext type.
// Returns an index into `slots`, or -1.
int auto_match_slot(const std::string& type, const std::string& colour, const std::vector<Slot>& slots, bool cfs_order);

// What can be said before asking: false for every host type this fork has no status client for.
bool   can_probe(const std::string& host_type_key);
// The sentence the dialog shows when `probe()` came back with nothing to map.
std::string no_filament_note(const std::string& host_type_key);

} // namespace PrintHostDevices
} // namespace Slic3r

#endif // slic3r_PrintHostDeviceStatus_hpp_
