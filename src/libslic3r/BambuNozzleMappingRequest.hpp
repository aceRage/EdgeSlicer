#pragma once

// The get_auto_nozzle_mapping request a two-extruder Bambu printer answers before a print
// (BambuStudio DevNozzleMappingCtrl::CtrlGetAutoNozzleMappingV0, DeviceCore/DevMappingNozzle.cpp:38-179),
// built from plain data so it can be unit tested. SelectMachineDialog fills the input from the
// sliced plate and the printer report.

#include "BambuDualNozzleSync.hpp"

#include <string>
#include <vector>

namespace Slic3r { namespace BambuNozzleMapping {

struct MappedFilament
{
    int         id{ -1 };  // 0-based filament index
    std::string ams_id;    // "0", "128"; empty = unmapped
    std::string slot_id;   // "0".."3"
    int         tray_id{ -1 }; // fallback when ams_id/slot_id do not parse
    std::string filament_id; // "cate": filament preset id, e.g. "GFA00"
    std::string color;       // "color": the project filament colour as the dialog sends it
};

// One (filament, logical nozzle) pair of the slicing result.
struct FilamentNozzle
{
    int              filament{ -1 };         // 0-based
    int              logical_extruder{ 0 };  // 0 = left, 1 = right
    int              group_id{ 0 };
    std::string      diameter;               // as the grouping result names it ("0.4"); may be empty
    NozzleVolumeType volume{ nvtStandard };
};

struct RequestInput
{
    int calibration{ 0 };              // flow calibration option (1/0)
    int extrude_cali_manual_mode{ 1 }; // no PA switch in this fork: automatic
    std::vector<int>            filament_change_sequence; // 0-based filament ids in print order
    std::vector<MappedFilament> mapped;
    std::vector<FilamentNozzle> filament_nozzles;
    // Printer report nozzles (extruder nozzles and rack). Empty: fall back to the preset's two
    // extruder nozzles (preset_diameters / preset_volumes, logical order) as before.
    std::vector<DualNozzleSync::PrinterNozzle> printer_nozzles;
    std::vector<int>              physical_extruder_map;
    std::vector<double>           preset_diameters;
    std::vector<NozzleVolumeType> preset_volumes;
};

// "0.40": two decimals, as Bambu's s_get_diameter_str.
std::string diameter_str(double d);
// "0.4" / "0.40" -> "0.40"; returned unchanged when it does not parse.
std::string diameter_str(const std::string &d);

// V0 request JSON ({"print":{"command":"get_auto_nozzle_mapping", ...}}), sequence_id "0"
// (the network agent stamps its own).
//  - filament_seq: first index in filament_change_sequence per 1-based id, -1 when unused.
//  - ams_mapping: 33 entries, (ams_id << 8) | slot_id at the 1-based filament id, 0xFFFF unused.
//  - fila_info: one entry per FilamentNozzle of a mapped filament.
//  - nozzle_info: every usable (normal) nozzle the printer reports, extruder nozzles first and
//    then the rack, "pos" = report id (0x10 + n for rack slot n), with wear/cate/color when
//    reported (Bambu :138-176). Without a report: the preset's two extruder nozzles.
std::string build_v0_request(const RequestInput &in);

// The send dialog's own get_auto_nozzle_mapping query is ADVISORY. The network agent asks the
// printer again while sending and, on a refusal or no answer, sends the job without
// "nozzle_mapping" - the pre-#114 behaviour, which prints fine. A refusal therefore only warns:
// the H2D answered result=fail errno=1 to every query (2026-09-23), even for a one-filament job
// on the extruder it was sliced for, and blocking on it made the printer unusable from here.
// Only mismatches detected locally and with certainty (a tray on the wrong extruder's AMS) block.
enum class QueryState {
    None,     // no query for this job (single nozzle, right nozzle unused, not a sliced send)
    Waiting,  // query published, no answer yet
    Accepted, // printer returned a mapping
    Refused,  // printer answered result=fail
    NoAnswer, // no answer within the timeout, or the query could not be published
};

struct SendGate
{
    bool send_enabled{ true };
    bool warn{ false }; // amber note under the AMS mapping
    bool note{ false }; // grey (informational) note
};

// "fail" / "failed" / "FAIL" (BambuStudio CheckErrorSyncNozzleMappingResultV0).
bool reply_is_refusal(const std::string &result);
SendGate send_gate(QueryState state);

// Whether a job is sent through the get_auto_nozzle_mapping handshake at all. BambuStudio asks
// only printers with a nozzle rack (DevNozzleRack::IsSupported, fun bit 60 = H2C), only for sliced
// sends, and only when the right extruder is used; the H2D (no rack) is never asked.
struct QueryConditions
{
    bool sliced_send{ false };       // not a reprint from the printer's storage
    bool dual_nozzle_preset{ false };
    bool printer_has_rack{ false };
    bool right_nozzle_used{ true };  // unknown grouping counts as used
    bool has_ams_mapping{ false };
};
bool query_applies(const QueryConditions &c);

}} // namespace Slic3r::BambuNozzleMapping
