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

// The instance API's manifest version (GET /api `version`). 3: slice_v1; 4: PR D, arrange (the
// layout lists overhanging instances, /plates/loose, `blocked`, arrange / orient / printable / undo).
constexpr int INSTANCE_API_VERSION = 4;

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
    std::string id;           // what a filament's `target` names: the label ("A1", "T1", "1A"); the wire form if labels repeat
    std::string label;        // what the printer calls it
    std::string type;         // "" = unknown (sent as null)
    std::string color;        // #RRGGBB, "" when unknown or empty (sent as null)
    bool        ready { false }; // something usable is loaded
    std::string side;         // "L" / "R" on a two-extruder Bambu printer, else ""
    std::string wire;         // the target in the mapping's wire form: "<ams>-<slot>" (AMS); "" = its position ("0", "1"...)
};

struct Sheet
{
    std::string                mapping_kind { "none" }; // ams | toolheads | canvas | none (a QIDI Box: none, read-only)
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

// The sheet's part of the answer (EdgeSlicerApp tools/slice_v1/schemas/send_preview.schema.json):
// mapping_kind, editable, filaments [{index, type, color, grams, target (a target id or null), auto,
// problems []}], targets [{id, label, type|null, color|null, ready, side, wire}], mapping, policy,
// problems [], warnings [], can_send. problems[] codes: "mapping" (one filament) or "policy"
// (filament -1, the plate as a whole).
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
// editable, targets (from trays: id = the tray's name, wire = "<ams>-<slot>") and per filament color
// and target (that id). The reprint's own fields
// (colour, tray, trays, problems, job, options...) stay, so the app's reprint mapping model reads it.
void decorate_ams_preview(nlohmann::json& preview);

// A send refusal ("... so the print was not started") as a preview says it ("... would not be
// started"). Text it does not know is returned unchanged.
std::string preview_tense(std::string text);

// ---- a Bambu preview without connecting (owner, 2026-10-23) ----
// Browsing printers on the phone's send sheet must not select / connect each Bambu printer it
// looks at. A preview is built from what the PC's DeviceManager already holds for that printer when
// that is current; otherwise the sheet says so (source "none") and the app asks again with live=1,
// which selects and connects the printer as a send does. A plate send itself is unchanged.

// What the PC knows about a Bambu printer right now, read off the MachineObject without touching it.
struct CachedPrinter
{
    bool   known { false };     // DeviceManager lists it
    bool   online { false };    // MachineObject::is_online
    bool   connected { false }; // MachineObject::is_connected: its pushes are arriving now
    double age_s { -1. };       // seconds since its last push (MachineObject::last_update_time), < 0 unknown
    std::string name;           // its display name (dev_name), "" when unknown
};

// The longest a printer's last push may be old for its AMS to count as current.
constexpr double CACHED_AMS_MAX_AGE_S = 120.;

// "" when the cached state can be used for a preview; otherwise why not:
// printer_unknown | printer_offline | printer_not_connected | printer_state_stale.
std::string cached_state_problem(const CachedPrinter& c, double max_age_s = CACHED_AMS_MAX_AGE_S);

// The sheet for a Bambu printer with no usable cached state: mapping_kind "ams", editable false, the
// plate's filaments with no target, no targets, the reason as a plate-level problem (code = the
// reason) and a hint, can_send false, source "none", needs_live true, ams_age_s (null unknown).
nlohmann::json no_state_sheet(const std::vector<SheetFilament>& filaments, const std::string& reason, double age_s);

// ---- PR D: arrange (features "slice_arrange", "slice_orient", "slice_undo") ----
// EdgeSlicerApp docs/design_native_slice_parity.md section 5: why a plate cannot slice, so the app
// can offer the fix (Arrange, Print them, Show on PC) instead of guessing.

// One instance registered on a plate (PartPlate's obj_to_instance_set), as that plate sees it.
struct PlateInstance
{
    bool outside { false };   // in the plate's instance_outside_set: not wholly on the printable area
    bool printable { true };  // ModelInstance::printable, the object list's Printable toggle
    bool off_plate { false }; // outside and not touching the plate at all (an arrange can leave one registered
                              // to the plate it was taken from): the slice leaves it out, it does not stop it
};

// Why the PC will not slice this plate, "" when nothing on the plate stops it:
//   "empty"          nothing is on the plate
//   "outside"        a printable instance hangs over the edge (the PC greys Slice out: "An object is
//                    laid over the plate boundaries", even when other objects are inside), or the only
//                    printable ones are off the plate; Arrange fixes it
//   "locked"         the same on a locked plate, which Arrange leaves alone: only the PC can unlock it
//   "not_printable"  every instance on the plate is set not to print
// Instances that are registered to no plate at all are counted apart (/plates/loose).
std::string plate_blocked(const std::vector<PlateInstance>& instances, bool locked);
// "" as JSON null, anything else as the string.
nlohmann::json blocked_json(const std::string& reason);

// "Place loose objects" (POST /api/plates/{i}/arrange?loose=1, /api/plates place_count): how many
// instances it places - the printable ones not wholly on their unlocked plate, and the printable ones
// on no plate (loose_printable). Everything else stays where it is, so 0 means there is nothing to do.
int place_count(const std::vector<std::vector<PlateInstance>>& plates, const std::vector<bool>& locked, int loose_printable);

// What Place loose objects (ArrangeJob::prepare_loose) does with one instance, the same rule
// place_count counts: Place it into free space, leave it to Stay where it is (wholly on its plate,
// or on a plate the person locked), or Skip it (set not to print and not on a plate). `user_locked`
// is the person's lock only: the arrange also locks, for its own run, a plate whose print sequence
// is not the project's, and what hangs over that plate's edge is still placed (onto another plate).
enum class LooseRoute { Place, Stay, Skip };
LooseRoute loose_route(bool on_plate, bool wholly_on_plate, bool user_locked, bool printable);

// POST /api/undo?expect=: whether the PC's next undo step may be taken. An empty `expect` takes
// whatever it is; otherwise it must name the topmost undo snapshot exactly, so a phone's Undo after
// an Arrange never undoes something a person did at the PC in the meantime.
bool undo_matches(const std::string& expect, const std::string& top);

} // namespace SliceV1
} // namespace GUI
} // namespace Slic3r
