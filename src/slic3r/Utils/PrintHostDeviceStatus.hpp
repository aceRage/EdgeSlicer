#ifndef slic3r_PrintHostDeviceStatus_hpp_
#define slic3r_PrintHostDeviceStatus_hpp_

#include <map>
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
//    is no `print_task_config`, a Happy Hare `mmu` object (gates/materials/colors) or an AFC /
//    BoxTurtle `lane_data` namespace can still name what is loaded; those are read-only for the
//    send-dialog mapping table. Upstream's MoonrakerPrinterAgent start_print is a stub and does
//    not send a gate/lane mapping at start, so neither does this probe. If none of those objects
//    are present, the `extruder`/`extruder1`... objects still say how many tools the machine has,
//    which is a slot list without materials in it.
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
//  * A CrealityPrint host (K1/K2 + CFS) answers /info and a port-9999 WebSocket `boxsInfo`.
//    Both calls use a real deadline (CrealityWsClient / Http timeout); the send dialog runs
//    this probe off the UI thread and posts the result back.
//  * Everything else (Duet, Repetier, SimplyPrint, ...) is not probed: this fork has no client for
//    their status APIs, and guessing would cost a timeout per address.
//
// Blocking. PrintHostSendDialog::refresh_device_status runs this on the UI thread with a busy
// cursor for every host except CrealityPrint (PrintHostDialogs.cpp), whose /info + boxsInfo pair
// runs on a worker and is posted back. Happy Hare rides in the first objects query, so it costs no
// request; only an AFC / BoxTurtle box (its `AFC` object answered) adds one GET, for its lane_data
// database namespace.

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
    std::string color;          // "#RRGGBB", "" = the printer did not say / unparsable name
    std::string color_name;     // original CSS name when `color` is empty
    bool        loaded { false };
    double      nozzle { 0.0 };
    // A CANVAS tray (Elegoo Centauri Carbon 1): the raw pair the start-print slot_map names, -1 for
    // every other kind of slot. `index` stays the flat 0-based number shown and stored elsewhere.
    int         unit { -1 };
    int         tray { -1 };
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
    // The slots are a QIDI Box's: a filament left "As sliced" is fed from whichever slot the
    // printer last used for that tool, so the dialog matches by material, never by colour alone.
    bool              qidi_box { false };
    // The printer's answer about its Box contradicts itself (box counts that differ, slot motors
    // for other slots than box_count names, filament reported past the counted Boxes, a slot the
    // controller calls loaded that its own sensor calls empty): nothing is mapped automatically
    // and qidi_send_policy keeps the send to an upload. `qidi_box_problem` says what, in words.
    bool              qidi_box_inconsistent { false };
    std::string       qidi_box_problem;
    // QIDI firmware (X-Max 4, X-Plus 4, Q2, X-Plus 5 - the ones with a multi_color_controller,
    // Box or not): the send dialog offers QIDIStudio's print options. qidi_polar_cooler: the
    // printer has the extruder cooler that QIDIStudio's "Polar cooler" switch drives.
    bool              qidi { false };
    bool              qidi_polar_cooler { false };
    // The slots are CANVAS trays read over SDCP (Cmd 324), so the start command can carry a slot_map.
    bool              canvas { false };
    // A CANVAS: the printer's own auto-refill setting (2005 auto_refill: 1 on, 0 off), -1 unknown.
    int               canvas_auto_refill { -1 };
    // A CANVAS: the position in `slots` of the tray in the extruder now (active_canvas_id /
    // active_tray_id), -1 when there is none or the printer did not say.
    int               active_slot { -1 };
    // Happy Hare `mmu` gates or AFC/BoxTurtle lanes filled the slot list (not Snapmaker tools).
    // Display only: no gate or lane mapping is sent at print start.
    bool              happy_hare { false };
    bool              afc { false };
    // The slots are an MMU's (gates / lanes / Box slots), shown four to a unit as 1A-1D, 2A-2D.
    bool              mmu_grouped() const { return qidi_box || happy_hare || afc; }
    // CrealityPrint host: the probe spoke the port-9999 boxsInfo WebSocket and found CFS boxes.
    bool              cfs { false };
    int               cfs_box_count { 0 };
    std::string       cfs_variant; // "CFS", "CFS-C", ... when the printer named one
};

// A slot's tile label as the send dialog's map shows it ("1A", "1B"... for a CANVAS or QIDI Box:
// unit by order of appearance, then the slot's place in it; the bare number otherwise). `pos` is a
// position in `slots`.
std::string slot_tile_label(const std::vector<Slot>& slots, int pos);

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
// The model has QIDIStudio's "Polar cooler" (its profiles' is_support_polar_cooler: the Q2 / Q2C,
// the X 4 series and the X-Plus 5). Used to offer the switch when the printer did not answer.
bool qidi_polar_cooler_model(const std::string& printer_model);

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
    int  mapped_to_empty { 0 };        // ... whose picked slot the printer says is empty
    int  type_mismatches { 0 };        // ... whose slot holds another material (or an unknown one)
    bool box_inconsistent { false };   // the Box answer contradicts itself (Status::qidi_box_inconsistent)
};

// ---- The slot send policy: one rule for every printer whose slots feed the tools -----------------
//
// A QIDI Box, an Elegoo CANVAS: the printer feeds each tool from a slot, and a print started on the
// wrong slots runs another material at the plate's settings. Every path that starts such a print
// with nobody at a dialog (the hub, the phone, an archive reprint) and the desktop send dialog ask
// slot_send_policy(); qidi_send_policy() and ElegooCanvasMapping::plan_unattended_send() are its two
// readings of what the printer said.

struct SlotSendFacts
{
    bool slots_required { false }; // a print is never started without reading the slots (a QIDI Box model)
    bool probe_ok { false };       // the printer answered
    bool slot_unit { false };      // ... as one whose slots can be read (QIDI firmware; an SDCP status)
    bool unit_in_use { false };    // the slots feed this print (a Box present and switched on; a CANVAS with trays)
    bool busy { false };           // printing or paused
    bool slots_inconsistent { false }; // the slot answer contradicts itself (Status::qidi_box_inconsistent)
    int  used_filaments { 0 };     // filaments this plate prints with
    int  mapped_filaments { 0 };   // ... that have a loaded slot picked
    int  mapped_to_empty { 0 };    // ... whose picked slot the printer says is empty
    int  type_mismatches { 0 };    // ... whose slot holds another material
    // Nothing is mapped, and the printer's own choice is known to be the right one: the CANVAS
    // "as sliced" start (design D4, an empty slot_map) of a one-filament plate whose material is
    // in the tray already in the extruder.
    bool as_sliced_ok { false };
};

enum class SlotSendPolicy
{
    NotApplicable,       // no slots to map (or not this kind of printer): the send goes on exactly as before
    Start,               // mapping complete (or the slots are not in use): start the print
    UploadOnlyBusy,      // the printer is printing or paused: never touch its slots under a print
    UploadOnlyNoProbe,   // a slot printer that could not be read: upload only unless the person chooses to print
    NeedsMapping,        // a used filament has no loaded slot: the printer would feed whatever it used last
    ConfirmTypeMismatch, // a filament goes to a slot holding another material: ask first (an unattended send refuses)
    StartAsSliced,       // nothing mapped, but as_sliced_ok: start with no mapping and let the printer feed
    MappedToEmpty,       // a filament goes to a slot the printer says is empty: never started
    UploadOnlyInconsistent, // the slot answer contradicts itself: upload only, nothing mapped
};

SlotSendPolicy slot_send_policy(const SlotSendFacts& facts);

// The QIDI Box reading of the slot send policy (StartAsSliced never comes out of it).
using QidiSendPolicy = SlotSendPolicy;

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

// One line for the log at each QIDI send: every Box slot as the send saw it - "1A loaded PLA
// (PLA Rapido) #FFFFFF; 1B empty; ..." - and, when the answer did not add up, why. No address,
// no serial.
std::string describe_qidi_box(const Status& st);

// ---- QIDIStudio's own LAN read of the Box -------------------------------------------------------
//
// QIDIStudio 2.07.02.60 (the release) does not read a `multi_color_controller` object at all. Over
// the LAN it reads Klipper's save_variables (`/printer/objects/query?save_variables=variables`):
// box_count, and per Box slot n (box * 4 + slot, 0-based: Box 1 is slots 0-3, the firmware's
// `box_config box0` / `box_stepper slot0..3`) filament_slot<n> / color_slot<n> / vendor_slot<n>,
// which are indexes into the printer's own `config/officiall_filas_list.cfg` ([fila<id>] type =
// PLA ..., [colordict] <id> = #RRGGBB). Whether slot n has filament it takes from the
// `box_stepper slot<n>` object (runout_button 0), not from those ids: the firmware's saved
// variables default filament_slot<n> to 1 (a PLA) on every slot. The same variables hold
// value_t<tool> and enable_box, which is what the start-of-print script writes and what is read
// back to confirm it took.

struct QidiFilaList
{
    std::map<int, std::string> type;   // fila id -> "PLA"
    std::map<int, std::string> name;   // fila id -> "PLA Rapido"
    std::map<int, std::string> colour; // colordict id -> "#RRGGBB"
};
// The INI text of officiall_filas_list.cfg.
QidiFilaList parse_qidi_filas_cfg(const std::string& text);
// save_variables' `variables` object (JSON text) -> the Box slots it describes (box_count * 4,
// Box 1 slot A first); empty when it names no Box. A slot counts as loaded only when its own state
// word `slot<n>` says so (0 empty, 1 / 2 / 3 filament there, negative blocked) - never from
// filament_slot<n>, which the firmware defaults to 1 (a PLA) on every slot and keeps after the
// spool is gone. A loaded slot's material is filament_slot<n> in the list; unknown when 0.
// (The probe also reads each slot's `box_stepper slot<n>` sensor, as QIDIStudio 2.07.02.60 does.)
std::vector<Slot> slots_from_qidi_variables(const std::string& variables_json, const QidiFilaList& list, bool& have_filaments);
// After the start-of-print script: do the printer's variables now say what was sent? enable_box
// as `use_box` (1 / 0, -1 = not sent) and value_t<tool> == 'slot<n>' for every pair of `mapping`.
// False with `why` otherwise - the print must not start on a mapping that did not take.
bool qidi_mapping_confirmed(const std::string& variables_json, const std::string& mapping, int use_box, std::string& why);
// The network-free half of probe() for a QIDI: a Moonraker `result.status` object (with
// save_variables and the box_stepper objects when the printer has them) and the printer's
// filament list -> the Box as the send dialog sees it. For the tests.
Status parse_qidi_status(const std::string& status_json, const QidiFilaList& filas, const std::string& host_type_key = "octoprint");
// Blocking, off the GUI thread: the `variables` object of save_variables, as JSON text.
bool query_qidi_variables(const std::string& base_url, std::string& variables_json, std::string& error, int timeout_s = 10);
// The probe timeout for a model: a QIDI Box printer's Moonraker is slow to answer (3 s timed out
// on a real one), every other printer keeps the short wait.
int probe_timeout_for(const std::string& printer_model);

// Blocking, off the GUI thread. `timeout_s` is per HTTP call.
Status probe(const Device& d, int timeout_s = 3);

// The half of probe() that has no network in it: a Moonraker `result.status` object turned
// into slots and a state. Exposed so the parsing is exercised by the Catch2 suite instead of
// only by a printer on somebody's desk. `status_json` is the JSON text of that object.
Status parse_moonraker_status(const std::string& status_json, const std::string& host_type_key);

// Happy Hare: GET /printer/objects/query?mmu (full body), a status object that contains `mmu`,
// or the `mmu` object itself. Network-free, for Catch2.
Status parse_happy_hare_mmu(const std::string& json_text, const std::string& host_type_key);
// AFC / BoxTurtle: GET /server/database/item?namespace=lane_data (full body), or the `value`
// object of lane keys. Network-free, for Catch2.
Status parse_afc_lane_data(const std::string& json_text, const std::string& host_type_key);

// Slot.color contract: "#RRGGBB" or empty. Hex, 0x, and known CSS names map to hex; "#None"
// and unparsable names are empty (the readable name is Slot::color_name, not this helper).
std::string print_host_slot_colour(const std::string& raw);

// The half of the Elegoo probe that has no network in it: a CANVAS query result as slots.
Status status_from_canvas(const CanvasQueryResult& result);

// The half of the Creality probe that has no network in it: a boxsInfo body as slots.
Status status_from_cfs(const std::string& boxs_info_json);

// Material-first auto-match (research X3 / #610), with Creality's CFS-over-external ranking
// when `cfs_order` is true: CFS type+colour > CFS type > ext type+colour > ext type.
// Returns an index into `slots`, or -1.
int cfs_auto_match(const std::string& type, const std::string& colour, const std::vector<Slot>& slots, bool cfs_order);

// What can be said before asking: false for every host type this fork has no status client for.
bool   can_probe(const std::string& host_type_key);
// The send dialog runs probe() on a worker thread only for these host types (CrealityPrint: /info
// plus a port-9999 WebSocket, two deadlines). Every other host is probed on the UI thread with a
// busy cursor, as it always was, so its dialog behaves exactly as before.
bool   probe_on_worker(const std::string& host_type_key);
// The sentence the dialog shows when `probe()` came back with nothing to map.
std::string no_filament_note(const std::string& host_type_key);

} // namespace PrintHostDevices
} // namespace Slic3r

#endif // slic3r_PrintHostDeviceStatus_hpp_
