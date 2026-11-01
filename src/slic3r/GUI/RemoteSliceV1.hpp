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
// layout lists overhanging instances, /plates/loose, `blocked`, arrange / orient / printable / undo);
// 5: PR F, filament settings (settings/filament and its revert / save, presets/filament_remove);
// 6: PR E, preview v2 (colour by, options, from, hidden features, /preview/legend, /preview/layers).
constexpr int INSTANCE_API_VERSION = 6;

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

// ---- PR F: filament settings (feature "slice_filament_settings") ----
// EdgeSlicerApp docs/design_native_slice_parity.md section 4: a slot's filament preset edited like
// the Process tab, saved, reverted, and a slot removed, from the phone.

// The slots whose preset is the same as `slot`'s, `slot` included, in order. The Filament tab edits
// one copy of a preset, so an edit made for one of them changes all of them ("also changes slot 3").
std::vector<int> slots_sharing(const std::vector<std::string>& slot_presets, int slot);

// A request that would make the Filament tab edit `slot`: HTTP status 200 when it may, otherwise the
// status and the sentence. The tab holds one edited copy and is bound to one slot (`bound`, the slot
// the sidebar last edited, 0 before any). Unsaved edits (`dirty`) belong to `edited`, the preset the
// tab has open; the desktop asks Save / Discard before it opens another preset, which a phone cannot
// answer, so that is refused (409) until the edits are saved or reverted. A slot using the same
// preset shares the edits and may be opened.
struct Refusal
{
    int         status { 200 };
    std::string error;
    bool        ok() const { return status == 200; }
};
Refusal bind_filament_slot(int slot, int slots, int bound, bool dirty, const std::string& slot_preset, const std::string& edited);

// The name POST /settings/filament/save saves under: `requested` (Save as) when given, else the
// preset's own name, or "<name> - Custom" for a system preset (the fork's no-prompt rule, as the
// process save). Surrounding blanks are dropped from `requested`.
std::string filament_save_name(const std::string& requested, const std::string& edited, bool is_system);

// Save as names a preset for one slot: after the save the desktop moves every slot that used the old
// preset to the new one (Sidebar::update_presets_from_to); the other slots go back to the old preset.
std::vector<std::string> slots_after_save_as(std::vector<std::string> before, int slot, const std::string& saved);

// POST /presets/filament_remove?index=[&replace=][&confirm=1]: whether slot `slot` of `slots`
// physical slots may go. `replace` (-1: none, objects using it fall back to the first filament) is
// the slot whose filament they take instead; `mixed_dependents` mixed filaments are built from it,
// which the desktop asks about, so they need `confirm`.
Refusal filament_remove_check(int slot, int slots, int replace, int mixed_dependents, bool confirm);

// ---- PR E: preview v2 (feature "slice_preview_v2") ----
// EdgeSlicerApp docs/design_native_slice_parity.md section 2.3: the phone's preview colours the
// toolpaths by any of the desktop's view types, hides features and moves (travel, seams...), starts
// at a lower layer, and reads the legend and the per-layer times the PC already holds.

// Stable ids of the G-code viewer's extrusion roles, indexed like libvgcode::EGCodeExtrusionRole
// (RemoteAccess static_asserts the count), so a legend row and a `hide=` list mean the same feature
// in every language.
const std::vector<std::string>& preview_role_ids();
// The role index of an id, or -1.
int preview_role_index(const std::string& id);

// Stable ids of the toggleable moves, indexed like libvgcode::EOptionType's first six.
const std::vector<std::string>& preview_option_ids();
int preview_option_index(const std::string& id);

// A comma list of ids ("seam,travel"), blanks dropped, each once, in order.
std::vector<std::string> split_ids(const std::string& list);

// "#RRGGBB" of an 8-bit colour.
std::string hex_color(unsigned char r, unsigned char g, unsigned char b);

// The unit and the decimals the desktop's legend shows for a gradient view type (its config name):
// "" and 0 for the discrete ones.
std::string gradient_unit(const std::string& color_by);
int gradient_decimals(const std::string& color_by);

// The legend of a gradient: the colour range's bin values and palette (palette[i] colours
// values[i]; one value is a single colour, two are the palette's ends) as stops ordered by value,
// with min and max. Rounded to the legend's decimals (one more for layer times under 10 s).
nlohmann::json gradient_legend(const std::string& color_by, const std::vector<float>& values, const std::vector<std::string>& palette_hex);

// GET /plates/{i}/preview/layers: every layer's top Z, its height (from the layer below, the first
// from 0) and its estimated time (seconds, 2 decimals; null when the times do not match the layers).
nlohmann::json layer_rows(const std::vector<double>& zs, const std::vector<float>& times);

// The lower layer of a frame: `from` clamped to 0..top (anything below 0 is 0).
unsigned int preview_from_layer(int from, unsigned int top);

// How the phone wants one preview frame to look, from preview.png's query. Empty `color_by`: the
// PC's own; `options_given` false: the PC's own moves; `hide`: roles not drawn.
struct PreviewLook
{
    std::string              color_by;
    int                      from { 0 };
    bool                     options_given { false };
    std::vector<std::string> options;
    std::vector<std::string> hide;
};

} // namespace SliceV1
} // namespace GUI
} // namespace Slic3r
