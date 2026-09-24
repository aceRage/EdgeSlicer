#pragma once

// Printer-state sync and per-plate filament arrangement for Bambu's two-extruder printers
// (H2D, H2D Pro, H2C, X2D: any printer profile with two extruders of distinct variants).
//
// Everything here is GUI-free so it can be unit tested with mock printer reports. The GUI side
// (Plater, the pre-slice confirmation dialog, SelectMachine) converts the live MachineObject
// into a PrinterState and calls these functions.
//
// Numbering: "logical" extruders are the slicer's (0 = left, 1 = right; filament_map is the
// 1-based form, 1 = left, 2 = right). The printer report numbers "physical" extruders
// (0 = main = RIGHT on every current Bambu two-extruder machine); physical_extruder_map
// (physical = map[logical], [1, 0] on these machines) converts between the two. See
// BambuExtruderMap.hpp.

#include "PrintConfig.hpp"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace Slic3r { namespace DualNozzleSync {

// ---------------------------------------------------------------------------------------------
// Printer state (what "Sync" reads from the printer report)
// ---------------------------------------------------------------------------------------------

struct Tray
{
    int         ams_id{ -1 };
    int         slot_id{ -1 };
    std::string color;       // as reported: "RRGGBBAA" or "RRGGBB" (with or without '#')
    std::string type;        // filament_type, e.g. "PLA"
    std::string filament_id; // tray_info_idx, e.g. "GFA00"
};

struct AmsUnit
{
    int               ams_id{ -1 };            // 0..3 four-slot units, 128.. AMS HT, 254/255 external
    int               physical_extruder{ -1 }; // Ams::nozzle from the report
    int               slot_count{ 4 };         // 1 for AMS HT / N3S, 4 otherwise (Bambu's ams_cnt_map key)
    std::vector<Tray> trays;                   // loaded trays only
};

struct PrinterNozzle
{
    int              pos{ 0 };               // report id: 0/1 = extruder nozzle, 0x10 + n = rack slot n
    std::string      diameter;               // "0.4"
    NozzleVolumeType volume{ nvtStandard };
    bool             normal{ true };         // Bambu DevNozzle::IsNormal(); abnormal/unknown rack nozzles are not usable
    int              wear{ -1 };             // -1 = not reported
    std::string      fila_id;                // filament last used in the nozzle ("fila_id"), may be empty
    std::string      color;                  // its colour ("color_m"), may be empty

    bool on_rack() const { return pos >= 0x10; }
    int  rack_slot() const { return pos - 0x10; }
};

struct PrinterState
{
    std::string                dev_id;
    std::string                dev_name;
    std::string                printer_type;          // e.g. "O1C2"
    bool                       has_report{ false };   // false: offline / no push yet
    std::vector<int>           physical_extruder_map; // from the printer profile
    std::vector<AmsUnit>       ams;
    std::vector<PrinterNozzle> nozzles;

    // Logical extruder an AMS unit feeds; -1 when unknown.
    int logical_extruder_of(const AmsUnit &unit) const;
    // Logical extruder a nozzle belongs to: rack nozzles belong to the main (physical 0) extruder,
    // as in BambuStudio Sidebar::priv::get_nozzle_options.
    int logical_extruder_of(const PrinterNozzle &nozzle) const;
    // Every tray of every AMS unit with its logical extruder.
    struct TrayOnSide { Tray tray; int logical_extruder; int slot_count; };
    std::vector<TrayOnSide> all_trays() const;
};

// ---------------------------------------------------------------------------------------------
// Synced project state (BambuStudio PresetBundle::extruder_ams_counts / extruder_nozzle_stat)
// ---------------------------------------------------------------------------------------------

// Port of BambuStudio save_extruder_ams_count_to_string (PrintConfig.cpp:786): one string per
// extruder, "<slots>#<count>" joined by '|', keys sorted.
std::vector<std::string> save_extruder_ams_count_to_string(const std::vector<std::map<int, int>> &extruder_ams_count);
// Port of BambuStudio save_extruder_nozzle_stats_to_string (PrintConfig.cpp:896).
std::vector<std::string> save_extruder_nozzle_stats_to_string(const std::vector<std::map<NozzleVolumeType, int>> &stats);

// Per logical extruder, number of AMS units by slot count, both keys (1 and 4) always present -
// what BambuStudio's AMSCountPopupWindow::SetAMSCount stores and check_ams_status_impl compares
// (Plater.cpp:17076-17111). External spool holders are not counted.
std::vector<std::map<int, int>> extruder_ams_counts(const PrinterState &state, size_t extruder_count);
// Bambu string form, e.g. H2C with an AMS HT on the left and two AMS on the right:
// ["1#1|4#0", "1#0|4#2"].
std::vector<std::string> extruder_ams_count_strings(const PrinterState &state, size_t extruder_count);

// Per logical extruder, usable nozzles by volume type (the extruder's own nozzle plus, for the
// main extruder, the rack), counting only nozzles whose diameter matches the preset's diameter
// for that extruder (when given) and that the printer reports as normal. H2C with a 0.4
// standard nozzle on the left and 0.4 standard on the right plus five in the rack:
// ["Standard#1", "Standard#6"].
std::vector<std::map<NozzleVolumeType, int>> extruder_nozzle_stats(const PrinterState &state, size_t extruder_count,
                                                                   const std::vector<double> &preset_diameters = {});
std::vector<std::string> extruder_nozzle_stats_strings(const PrinterState &state, size_t extruder_count,
                                                       const std::vector<double> &preset_diameters = {});

// Stable fingerprint of the parts of the printer state that matter for an arrangement: which
// printer, the AMS layout per extruder, what is loaded in each tray (colour + type) and the
// usable nozzles. Report noise (humidity, temperatures, remaining %) is not part of it.
// Empty when the state has no report.
std::string state_fingerprint(const PrinterState &state);

// ---------------------------------------------------------------------------------------------
// Per-plate arrangement (filament -> extruder, filament -> tray)
// ---------------------------------------------------------------------------------------------

struct ProjectFilament
{
    int         index{ -1 }; // 0-based filament index
    std::string color;       // "#RRGGBB"
    std::string type;        // "PLA"
    std::string filament_id; // "GFA00"
};

struct TrayRef
{
    int ams_id{ -1 };
    int slot_id{ -1 };
    bool valid() const { return ams_id >= 0 && slot_id >= 0; }
    bool operator==(const TrayRef &o) const { return ams_id == o.ams_id && slot_id == o.slot_id; }
    bool operator!=(const TrayRef &o) const { return !(*this == o); }
};

struct Arrangement
{
    // 1-based logical extruder per filament (1 = left, 2 = right), sized to the project's filament
    // count; filaments the plate does not use keep their previous value.
    std::vector<int>       filament_map;
    // Tray per used filament (0-based filament index). Missing / invalid = not mapped.
    std::map<int, TrayRef> trays;
};

// Colour distance used for the pre-fill (0 = identical; ~765 = black vs white). Weighted
// "redmean" RGB, the same family as the send dialog's colour match.
double color_distance(const std::string &a, const std::string &b);

// A reasonable pre-fill, not a replacement for the user's confirmation: each used filament is
// matched to the closest loaded tray (colour, with a penalty for a different filament type), a
// tray used at most once while free trays remain, and the filament goes to the extruder that
// tray feeds. Filaments that get no tray keep prior_map's side (else the extruder with more free
// trays). With no printer state at all the prior map is returned unchanged.
// prior_map: 1-based, may be empty or short (missing entries default to 1).
Arrangement propose_arrangement(const PrinterState &state, const std::vector<ProjectFilament> &used,
                                size_t filament_count, const std::vector<int> &prior_map);

// True when every used filament sits on one extruder although the printer has loaded trays
// feeding both - worth a hint in the dialog (Bambu's own auto grouping did exactly this on the
// 2026-09-23 H2C job).
bool all_on_one_side_with_trays_on_both(const Arrangement &arr, const PrinterState &state,
                                        const std::vector<ProjectFilament> &used);

// Trays offered for a filament on the given logical extruder (0 = left, 1 = right).
std::vector<Tray> trays_for_extruder(const PrinterState &state, int logical_extruder);

// Problems that make an arrangement unusable as confirmed (empty = fine). The GUI words them.
struct Issue
{
    enum class Kind {
        TrayOnOtherExtruder, // a filament's tray feeds the other extruder (the printer would re-arrange or refuse)
        TrayMissing,         // the chosen tray is no longer loaded (checked only with a report)
        NoNozzleOnExtruder,  // an extruder gets filaments but has no usable nozzle (checked only when nozzles are reported)
    };
    Kind    kind;
    int     filament{ -1 }; // 0-based, for the tray issues
    TrayRef tray;
    int     extruder{ -1 }; // logical 0/1
};
std::vector<Issue> validate_arrangement(const Arrangement &arr, const PrinterState &state,
                                        const std::vector<ProjectFilament> &used, size_t extruder_count,
                                        const std::vector<double> &preset_diameters = {});

// ---------------------------------------------------------------------------------------------
// Confirmation record (saved per plate in the project) and invalidation
// ---------------------------------------------------------------------------------------------

struct Confirmation
{
    std::string            dev_id;          // printer the arrangement was confirmed for ("" = offline)
    std::string            state_fp;        // state_fingerprint() at confirmation ("" = not synced)
    std::string            filaments_fp;    // filaments_fingerprint() of the plate at confirmation
    std::vector<int>       filament_map;    // 1-based, as confirmed
    std::map<int, TrayRef> trays;           // confirmed trays per 0-based filament
    bool                   synced{ false }; // confirmed with live printer data

    bool empty() const { return filament_map.empty(); }
    std::string serialize() const;                 // compact JSON
    static Confirmation deserialize(const std::string &s); // empty on error
};

// Fingerprint of what the plate prints with: used filament indices + colour + type.
std::string filaments_fingerprint(const std::vector<ProjectFilament> &used);

enum class ConfirmReason {
    None,             // stored confirmation still valid: slice without asking
    NeverConfirmed,
    PrinterChanged,   // a different printer is selected
    PrinterStateChanged, // same printer, AMS/nozzle/tray contents changed
    NowSynced,        // confirmed offline, the printer is online now
    FilamentsChanged, // plate uses different filaments / colours / types
    MapChanged,       // plate map no longer the confirmed one
    UserRequested,    // opened from the Slice menu to change the arrangement
};

// Whether the pre-slice confirmation must be shown for this plate. current_map is the plate's
// current 1-based map (empty = not manual).
ConfirmReason needs_confirmation(const Confirmation &stored, const PrinterState &state,
                                 const std::vector<ProjectFilament> &used, const std::vector<int> &current_map);

// Who is starting a slice of a plate whose arrangement may be unconfirmed.
enum class SliceTrigger {
    User,       // Slice / Slice all / Ctrl+R / Preview tab / plate thumbnail / any direct reslice
    Background, // automatic re-slice after an edit ("background processing"), or an export that must slice first
    Remote,     // phone / hub request or a hidden instance: nobody can answer a dialog
};

enum class SliceGate {
    Proceed,    // slice now
    ShowDialog, // show the arrangement confirmation first; slice after Confirm
    Defer,      // do not slice; tell the user the arrangement has to be confirmed first
};

// What to do with a slice of a plate given why it needs confirming (None = confirmed and still
// valid). Missing printer data never makes a plate "confirmed": an unconfirmed plate is asked
// about (User) or held back (Background) whether or not the printer is connected. Only a remote
// request, which nobody can answer, slices unconfirmed with the stored or automatic grouping.
SliceGate slice_gate(ConfirmReason reason, SliceTrigger trigger);

// Whether a plate sliced against sliced_dev/sliced_fp must be invalidated now that the selected
// printer is state. Going offline (no report) never invalidates; a different printer, a changed
// state fingerprint, or a printer appearing after an unsynced slice does.
bool slice_invalidated_by(const std::string &sliced_dev, const std::string &sliced_fp, const PrinterState &state);

}} // namespace Slic3r::DualNozzleSync
