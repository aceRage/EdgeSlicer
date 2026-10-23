#pragma once

// The companion app's native Slice tab (`slice_v1`, EdgeSlicerApp docs/design_native_slice.md
// section 3): the JSON the new instance routes answer with, and the cache headers of the routes it
// reads images from. wx-free on purpose: everything here is built from a GCodeProcessorResult, the
// slot names / types / colours the GUI read for it and the pricing that applies, so the tests in
// tests/slic3rutils/slice_v1_tests.cpp drive it without a Plater, and the app's fixtures can be
// compared against exactly what the hub sends.

#include <nlohmann/json.hpp>

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

} // namespace SliceV1
} // namespace GUI
} // namespace Slic3r
