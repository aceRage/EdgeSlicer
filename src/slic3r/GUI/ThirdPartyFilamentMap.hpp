#ifndef slic3r_GUI_ThirdPartyFilamentMap_hpp_
#define slic3r_GUI_ThirdPartyFilamentMap_hpp_

// Vendor-neutral filament → device-slot mapping for third-party send/upload dialogs
// (Elegoo CANVAS, Creality CFS, K3 holders, Happy Hare / AFC / QIDI Box, Anycubic ACE).
//
// wx-free: the dialog fills this model (third_party_slots_from() turns the probe's slot list into
// tiles), the panel shows it, and encode_filament_mapping() returns the same "filament:slot"
// string PrintHostSendDialog already sends. Bambu's SelectMachineDialog is unchanged.

#include <string>
#include <vector>

namespace Slic3r {
namespace PrintHostDevices {
struct Slot;
}
namespace GUI {

// One filament of the plate being sent.
struct ThirdPartySlicerFilament
{
    int         index { 0 };     // 0-based slicer filament / T-number
    std::string colour;          // "#RRGGBB" or "#RRGGBBAA"; empty = unknown
    std::string type;            // "PLA"
    std::string name;            // preset alias or display name
    int         nozzle_id { -1 }; // 0-based nozzle / toolhead; -1 = none / single-nozzle
};

// One place on the device a filament can be fed from (AMS / CANVAS / MMU / CFS / ACE / box tray).
struct ThirdPartyDeviceSlot
{
    int         unit_id { -1 };  // 0-based unit (AMS 1, CANVAS 1, CFS box 1); -1 = ungrouped
    int         slot_id { -1 };  // 0-based slot inside the unit (A=0)
    std::string label;           // shown on the tile; empty = bambu_style_slot_label()
    std::string colour;          // "#RRGGBB"; empty = unknown
    std::string type;            // "PLA"; empty = the device did not say
    std::string name;            // spool / material name
    int         nozzle_id { -1 }; // which nozzle this unit feeds; -1 = unspecified
    bool        empty { false };
    int         index { 0 };     // protocol index (PrintHostDevices::Slot::index)
};

struct ThirdPartyFilamentMapModel
{
    std::vector<ThirdPartySlicerFilament> filaments; // one row each, already filtered to used
    std::vector<ThirdPartyDeviceSlot>     slots;
    // Parallel to filaments: position in `slots`, or -1 = "As sliced".
    std::vector<int>                      mapping;
    bool                                  read_only { false };
};

struct ThirdPartySlotGroup
{
    int              unit_id { -1 };
    int              nozzle_id { -1 };
    std::vector<int> slot_positions; // indices into model.slots
};

enum class ThirdPartyMapMismatch {
    None          = 0,
    Type          = 1,
    Colour        = 2,
    TypeAndColour = 3,
};

// "#RRGGBB" upper case from "#RRGGBB", "#RRGGBBAA", "RRGGBB"; empty for anything else.
std::string normalize_map_colour(const std::string& colour);

// Bambu-style unit+slot: unit 0 slot 0 → "1A", unit 0 slot 3 → "1D", unit 1 slot 0 → "2A".
// Both are positions (0-based), never raw protocol ids. A unit with more than 26 slots falls
// back to "1-27". unit_id < 0: 1-based slot numbers ("1", "2", ...).
std::string bambu_style_slot_label(int unit_id, int slot_id);

// The probe's slot list (PrintHostDevices::Status::slots) as tiles, labelled by position so a
// printer's own numbering (0- or 1-based tray ids) never shows:
//  * grouped trays (Elegoo CANVAS, Slot::unit >= 0): units numbered in order of appearance,
//    trays lettered by their position inside the unit: "1A".."1D", "2A"...;
//  * a QIDI Box (qidi_box): Box slot b * 4 + n is "<b+1><A+n>", the same "1A".."4D";
//  * tool slots (Snapmaker U1, Moonraker toolheads, Slot::unit < 0): "T1".."T4", as Slot::label()
//    and the Device card name them.
// `index` keeps Slot::index, which is what the mapping string and the remembered mapping use.
std::vector<ThirdPartyDeviceSlot> third_party_slots_from(const std::vector<PrintHostDevices::Slot>& slots, bool qidi_box);

// The slot a filament row starts on, as a position in `slots` (-1 = "As sliced"): the dialog's
// remembered slot (`remembered_pos`, Elegoo preselect_slot; -1 = none) when it is a position in
// `slots`, else PrintHostDevices::auto_match_slot - the closest colour, or a QIDI Box's
// material-first rule when `qidi_box`.
int initial_slot_position(const std::vector<PrintHostDevices::Slot>& slots, int remembered_pos, const std::string& colour,
                          const std::string& type, bool qidi_box);

// The label a tile should show: slot.label if set, else bambu_style_slot_label().
std::string slot_display_label(const ThirdPartyDeviceSlot& slot);

bool types_match(const std::string& a, const std::string& b);

// Redmean, same formula PrintHostDevices::auto_match_slot uses. 0 when either colour is empty.
double map_colour_distance(const std::string& a, const std::string& b);

// Noticeably different colours (redmean). False when either side has no colour.
bool colours_mismatch(const std::string& a, const std::string& b);

ThirdPartyMapMismatch mismatch_for(const ThirdPartySlicerFilament& filament, const ThirdPartyDeviceSlot& slot);
// Slot compared for a warning that describes what will actually print. Interactive: mapping[row].
// Read-only: the slot whose protocol index equals the filament T-number (identity); mapping[] may
// still hold a visual auto-match and is not sent.
int                   warning_slot_position(const ThirdPartyFilamentMapModel& model, size_t filament_row);
ThirdPartyMapMismatch mismatch_at(const ThirdPartyFilamentMapModel& model, size_t filament_row);

// Distinct nozzle ids on filaments or slots; more than one means a multi-nozzle / multi-toolhead printer.
std::vector<int> distinct_nozzle_ids(const ThirdPartyFilamentMapModel& model);
bool             has_multiple_nozzles(const ThirdPartyFilamentMapModel& model);

// Badge on the slicer-filament colour box: "L"/"R" when the printer has exactly two nozzles
// numbered 0/1 (Bambu H2D: 0 = right, 1 = left), else the 1-based nozzle number, else the
// 1-based filament index on a single nozzle. Nozzle badges need the device's slots to say which
// nozzle they feed: on a printer whose slots are the toolheads (U1) the project's filament_map is
// not a nozzle the popup groups by, so the badge stays the filament number.
std::string filament_badge_text(const ThirdPartySlicerFilament& filament, const ThirdPartyFilamentMapModel& model);

// Slots grouped by nozzle (when several) then unit, preserving first-seen order.
std::vector<ThirdPartySlotGroup> group_slots(const std::vector<ThirdPartyDeviceSlot>& slots);

// "0:1,1:2" — filament index to Slot::index. Empty when model.read_only (the cards may still
// show an auto-match) or when every row is "As sliced". Same shape as
// PrintHostSendDialog::filament_mapping().
std::string encode_filament_mapping(const ThirdPartyFilamentMapModel& model);

} // namespace GUI
} // namespace Slic3r

#endif
