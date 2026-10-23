#ifndef slic3r_ElegooCanvasMapping_hpp_
#define slic3r_ElegooCanvasMapping_hpp_

// Elegoo CANVAS, phase 2 of tests/design_elegoo_canvas_sync_device_page.md: which tray each of the
// plate's filaments prints from, for the Upload and Print dialog of a Centauri Carbon 1.
//
//  * slot_map_for(): the dialog's choices -> the start command's slot_map entries (raw ids).
//  * warnings_for(): what is worth a warning but never a refusal.
//  * validate(): a mapped tray that has gone since the dialog read the CANVAS stops the send
//    before the upload.
//  * The remembered mapping (design D5): <datadir>/hub/elegoo_canvas_mapping.json, keyed by the
//    project filament's type + preset alias + colour, holding the type, name and colour of the tray
//    it went to. Matched by content, not by slot number, so moving a spool to another tray keeps
//    working. The idea follows ElegooSlicer's PrinterMmsManager (AGPL-3.0); the code is ours.
//
// wx-free and socket-free: tests/slic3rutils/elegoo_canvas_mapping_tests.cpp.

#include <map>
#include <string>
#include <vector>

#include "ElegooStartPrint.hpp"
#include "PrintHostDeviceStatus.hpp"

namespace Slic3r {
namespace ElegooCanvasMapping {

// One filament the plate uses, as the dialog knows it.
struct UsedFilament
{
    int         index { 0 };        // 0-based project filament: the T-number
    std::string type;               // "PLA"
    std::string colour;             // "#RRGGBB" or "#RRGGBBAA"
    std::string alias;              // the filament preset's alias, or its name
    int         nozzle_temp { 0 };  // the filament's nozzle temperature, 0 = unknown
};

// "#RRGGBB" upper case, "" for anything that is not a colour.
std::string normalize_colour(const std::string& colour);

// choice[i] is the position in `slots` that used[i] goes to, -1 = "As sliced". With `identity`
// (ElegooStartPrint::AS_SLICED_IDENTITY_MAP) an "As sliced" filament t goes to slots[t] when that is a
// CANVAS tray. Entries are ordered by filament and never name a slot that is not a CANVAS tray.
std::vector<ElegooStartPrint::SlotMapEntry> slot_map_for(const std::vector<UsedFilament>& used,
                                                         const std::vector<int>& choice,
                                                         const std::vector<PrintHostDevices::Slot>& slots,
                                                         bool identity);

enum class WarningKind
{
    MoreFilamentsThanTrays, // the plate uses more filaments than the CANVAS has loaded trays
    SharedTray,             // two filaments print from one tray
    TemperatureOutOfRange,  // the filament's nozzle temperature is outside the tray's range
    EmptyTray,              // a filament is mapped onto a tray with nothing in it
};

struct Warning
{
    WarningKind kind;
    int         filament { -1 }; // UsedFilament::index, -1 when it is about the whole plate
    int         slot { -1 };     // Slot::index, -1 when none
};

std::vector<Warning> warnings_for(const std::vector<UsedFilament>& used, const std::vector<int>& choice,
                                  const std::vector<PrintHostDevices::Slot>& slots);

// "" when every entry still names a loaded tray in `fresh` (a second read of the CANVAS); else a
// sentence for the log naming the first entry that does not.
std::string validate(const std::vector<ElegooStartPrint::SlotMapEntry>& entries, const std::vector<PrintHostDevices::Slot>& fresh);

// ---- remembered mappings ----

struct Remembered
{
    std::string type;
    std::string name;
    std::string colour; // "#RRGGBB"
};

std::string key_for(const UsedFilament& f);

// <datadir>/hub/elegoo_canvas_mapping.json, unless a test pointed it elsewhere.
std::string store_path();
void        set_store_path(const std::string& path);

std::map<std::string, Remembered> load();
void                              save(const std::map<std::string, Remembered>& memory);

// Records the choices that named a tray (not "As sliced") into `memory`.
void remember(std::map<std::string, Remembered>& memory, const std::vector<UsedFilament>& used,
              const std::vector<int>& choice, const std::vector<PrintHostDevices::Slot>& slots);

// The position in `slots` of the loaded tray that holds what `f` went to last time (same type, name
// and colour), -1 when there is none or more than one tray fits equally.
int recall(const std::map<std::string, Remembered>& memory, const UsedFilament& f, const std::vector<PrintHostDevices::Slot>& slots);

// ---- a send with nobody at the dialog (the hub, the phone, an archive reprint) ----
//
// The desktop dialog's mapping, made the dialog's way, and then the slot send policy
// (PrintHostDevices::slot_send_policy) instead of a question nobody can answer: every used filament
// on a loaded tray of its own material, or the print is not started (and nothing is uploaded).

struct UnattendedPlan
{
    PrintHostDevices::SlotSendPolicy            policy { PrintHostDevices::SlotSendPolicy::NotApplicable };
    PrintHostDevices::SlotSendFacts             facts;
    std::vector<int>                            choice;  // per `used`: its position in the slots, -1 = none
    std::vector<ElegooStartPrint::SlotMapEntry> entries; // Cmd 128's slot_map
    bool                                        canvas { false }; // a CANVAS answered: canvasAttached goes out
    bool                                        explicit_mapping { false }; // the choices came from the request
    std::vector<std::string>                    problems; // one sentence per filament that stops the start
    std::string                                 refusal;  // why the print is not started; "" when it may start

    // The print may start (Start, StartAsSliced, NotApplicable).
    bool may_start() const;
};

// `st`: what the printer said (PrintHostDevices::probe). `used`: the filaments the plate prints
// with. `mapping`: filament index -> position in st.slots, as the request named them; empty = the
// dialog's starting choice for each: the remembered tray (recall), else the closest colour among the
// loaded trays (PrintHostDevices::auto_match_slot). `printer` names the printer in the refusal.
UnattendedPlan plan_unattended_send(const PrintHostDevices::Status& st, const std::vector<UsedFilament>& used,
                                    const std::map<std::string, Remembered>& memory, const std::map<int, int>& mapping,
                                    const std::string& printer);

// The plan as ElegooLink's start command takes it (ElegooStartPrint.hpp): canvasAttached + slotMap
// when a CANVAS answered, exactly what the dialog's extendedInfo() adds; nothing otherwise.
void add_to_extended_info(const UnattendedPlan& plan, std::map<std::string, std::string>& info);

} // namespace ElegooCanvasMapping
} // namespace Slic3r

#endif
