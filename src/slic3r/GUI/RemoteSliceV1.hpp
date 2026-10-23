#pragma once

// The companion app's native Slice tab (`slice_v1`, EdgeSlicerApp docs/design_native_slice.md
// section 3): the JSON the new instance routes answer with, and the cache headers of the routes it
// reads images from. wx-free on purpose: everything here is built from a GCodeProcessorResult, the
// slot names / types / colours the GUI read for it and the pricing that applies, so the tests in
// tests/slic3rutils/slice_v1_tests.cpp drive it without a Plater, and the app's fixtures can be
// compared against exactly what the hub sends.

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <vector>

namespace Slic3r {

struct GCodeProcessorResult;
struct PlateCounts;
struct PricingSettings;

namespace GUI {
namespace SliceV1 {

// What /pair `features` gains for the native Slice tab. The app decides native vs the web page on
// "slice_v1" alone and degrades inside the native flow on the others (design section 4).
const std::vector<std::string>& pair_features();

// One filament slot of the project, as the sidebar shows it.
struct SlotInfo
{
    std::string preset; // the filament preset's name
    std::string type;   // filament_type of that preset (PLA, PETG-HF, ...), "" when unknown
    std::string color;  // #RRGGBB as the project's filament_colour holds it
};

// One slicing warning of a plate.
struct PlateWarning
{
    std::string code;     // stable, machine-readable: "slicing", "gcode_path_conflict", "toolpath_outside", a processor enum string ...
    std::string severity; // info | warning | error
    std::string text;     // what the desktop says
    std::string object;   // the object it is about, "" for the plate
    double      z_min { -1. };
    double      z_max { -1. }; // both >= 0: "z": [z_min, z_max]
};

struct ResultInput
{
    int                       index { 0 };
    unsigned int              result_id { 0 };
    long long                 gcode_bytes { -1 }; // < 0: unknown, left out
    std::vector<SlotInfo>     slots;              // by slot index
    std::vector<PlateWarning> warnings;           // the plate's print / object step warnings (GUI-collected)
};

// The warnings a processor result carries by itself: its own slice warnings (nozzle hardness, bed
// temperature, timelapse), a G-code path conflict and a toolpath outside the plate.
std::vector<PlateWarning> processor_warnings(const GCodeProcessorResult& result);

// GET /api/plates/{index}/result. Cost is the desktop's: Slic3r::compute_cost() over the result
// (filament_costs and time_cost there are the effective ones - CostOverrides applies the user's
// price and rate overrides to the config the plate was sliced with), priced with price_plate() for
// the fees and markup the cost panel shows. `warnings` = in.warnings followed by
// processor_warnings(result).
nlohmann::json result_json(const GCodeProcessorResult& result, const ResultInput& in, const PlateCounts& counts,
                           const PricingSettings& pricing);

// Two decimals, half away from zero (grams, metres).
double round2(double v);

// ---- cache headers ----
// A strong ETag over the body ("\"<16 hex>\"", FNV-1a 64).
std::string etag_of(const std::string& body);
// Whether an If-None-Match header value (a list, W/ prefixes, "*") names `etag`.
bool etag_matches(const std::string& if_none_match, const std::string& etag);
// Cache-Control for a plate image. `version` is the client's ?v= (the plate's result_id): a
// non-zero one names exactly one rendering, so it may be kept for a day; without one (or v=0, an
// unsliced plate whose thumbnail changes with every edit) the client revalidates with the ETag.
std::string image_cache_headers(const std::string& version, const std::string& etag);
// For /result: always revalidate (prices can change without a re-slice), ETag to make that free.
std::string result_cache_headers(const std::string& etag);

// ---- POST /api/plates/{index}/send/preview (PR B, feature "slice_send_preview") ----
// The mapping sheet of a plate send, one shape for every kind of printer: which of the plate's
// filaments goes where (an AMS tray, a U1 toolhead, a CANVAS tray, a QIDI Box slot), what the send
// would refuse and the wire form of the mapping to send back. Nothing is sent by a preview.

struct SheetFilament
{
    int         index { 0 };  // 0-based project filament (the G-code's T number)
    std::string type;
    std::string color;        // #RRGGBB
    double      grams { 0. };
};

struct SheetTarget
{
    std::string id;           // what the wire form names: "<ams>-<slot>" (AMS), else the position ("0", "1", ...)
    std::string label;        // what the printer calls it: "A1", "T1", "1A"
    std::string type;
    std::string color;        // #RRGGBB, "" when unknown or empty
    bool        ready { false }; // something usable is loaded
    std::string side;         // "L" / "R" on a two-extruder Bambu printer, else ""
};

struct Sheet
{
    std::string                mapping_kind { "none" }; // ams | toolheads | canvas | qidi_box | none
    bool                       editable { false };      // the send takes this sheet's mapping back
    std::vector<SheetFilament> filaments;               // the ones the plate prints with
    std::vector<SheetTarget>   targets;
    std::map<int, int>         choice;                  // filament index -> position in targets, -1 = none
    std::map<int, int>         automatic;               // what the automatic mapping picks
    std::map<int, std::vector<std::string>> problems;   // per filament: why the send would refuse it
    std::vector<std::string>   warnings;                // shown, never refused for
    std::string                policy;                  // the slot send policy (CANVAS, QIDI Box), "" otherwise
    std::string                mapping;                 // the wire form to send back ("" for none / qidi_box)
    bool                       can_send { true };       // a print would go ahead with this mapping
};

// The sheet's part of the answer: mapping_kind, editable, filaments [{index, type, color, grams,
// target (a target id or null), auto, problems []}], targets [{id, label, type, color, ready, side}],
// mapping, policy, warnings [], can_send.
nlohmann::json sheet_json(const Sheet& sheet);

// "<filament>:<position>,..." of a choice map (only the filaments with a target): what
// /api/plates/{i}/send takes as `mapping` for a Snapmaker U1 and an Elegoo CANVAS.
std::string positions_wire(const std::map<int, int>& choice);

// A Snapmaker U1 (toolheads): the send's own refusals as problems - a used filament with no
// toolhead, a toolhead the printer does not have, an empty toolhead (unless `force`) - and a
// material other than the filament's as a warning (the send does not refuse that). Sets can_send
// and mapping.
void check_toolheads(Sheet& sheet, bool force);

// The Bambu reprint preview (BambuReprint::evaluate) as a plate sheet: adds mapping_kind "ams",
// editable, targets (from trays) and per filament color and target. The reprint's own fields
// (colour, tray, trays, problems, job, options...) stay, so the app's reprint mapping model reads it.
void decorate_ams_preview(nlohmann::json& preview);

} // namespace SliceV1
} // namespace GUI
} // namespace Slic3r
