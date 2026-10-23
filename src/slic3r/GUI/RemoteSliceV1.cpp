#include "RemoteSliceV1.hpp"

#include "libslic3r/CostEstimate.hpp"
#include "libslic3r/CostPricing.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/libslic3r.h"

#include <boost/format.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace Slic3r {
namespace GUI {
namespace SliceV1 {

const std::vector<std::string>& pair_features()
{
    // slice_v1: the contract (result + cancel + presets type + instances progress + cache headers);
    // slice_result / slice_cancel: the two sub-features the app checks one by one.
    static const std::vector<std::string> f = { "slice_v1", "slice_result", "slice_cancel" };
    return f;
}

double round2(double v)
{
    const double scaled = v * 100.;
    return std::round(scaled + (scaled >= 0. ? 1e-9 : -1e-9)) / 100.;
}

static const char* severity_of(int level)
{
    // GCodeProcessorResult::SliceWarning::level: 0 tip, 1 warning, 2 error.
    return level >= 2 ? "error" : level == 1 ? "warning" : "info";
}

// The processor's slice warnings are enum strings; the desktop shows them in the Bambu send dialog
// with its own wording. The same sentences here, in English (the phone shows them as they come).
static std::string processor_warning_text(const std::string& code)
{
    if (code == NOZZLE_HRC_CHECKER)
        return "A filament needs a harder nozzle than the one this printer has.";
    if (code == BED_TEMP_TOO_HIGH_THAN_FILAMENT)
        return "The bed temperature is above a filament's softening temperature, which may clog the nozzle.";
    if (code == NOT_SUPPORT_TRADITIONAL_TIMELAPSE)
        return "This print cannot use the traditional timelapse mode.";
    if (code == NOT_GENERATE_TIMELAPSE)
        return "No timelapse will be recorded for this print.";
    if (code == LONG_RETRACTION_WHEN_CUT)
        return "Long retraction when the filament is cut is on.";
    return code;
}

std::vector<PlateWarning> processor_warnings(const GCodeProcessorResult& result)
{
    std::vector<PlateWarning> out;
    for (const auto& w : result.warnings) {
        PlateWarning pw;
        pw.code     = w.msg.empty() ? std::string("processor") : w.msg;
        pw.severity = severity_of(w.level);
        pw.text     = processor_warning_text(w.msg);
        out.emplace_back(std::move(pw));
    }
    if (result.conflict_result) {
        const auto& c = *result.conflict_result;
        PlateWarning pw;
        pw.code     = "gcode_path_conflict";
        pw.severity = "warning"; // the desktop shows it as a serious warning
        // GLCanvas3D's own sentence (EWarning::GCodeConflict), untranslated.
        pw.text = (boost::format("Conflicts of G-code paths have been found at layer %d, z = %.2lf mm. Please separate the conflicted "
                                 "objects farther (%s <-> %s).") %
                   c.layer % c._height % c._objName1 % c._objName2)
                      .str();
        pw.z_min = pw.z_max = c._height;
        out.emplace_back(std::move(pw));
    }
    if (result.toolpath_outside) {
        PlateWarning pw;
        pw.code     = "toolpath_outside";
        pw.severity = "error";
        pw.text     = "A G-code path goes beyond the plate boundaries.";
        out.emplace_back(std::move(pw));
    }
    return out;
}

static nlohmann::json warning_json(const PlateWarning& w)
{
    nlohmann::json j;
    j["code"]     = w.code;
    j["severity"] = w.severity;
    j["text"]     = w.text;
    if (!w.object.empty())
        j["object"] = w.object;
    if (w.z_min >= 0. && w.z_max >= 0.)
        j["z"] = { round2(w.z_min), round2(w.z_max) };
    return j;
}

nlohmann::json result_json(const GCodeProcessorResult& result, const ResultInput& in, const PlateCounts& counts,
                           const PricingSettings& pricing)
{
    using ETimeMode = PrintEstimatedStatistics::ETimeMode;
    const PrintEstimatedStatistics& ps = result.print_statistics;
    const CostBreakdown             cost   = compute_cost(result);
    const PricedCost                priced = price_plate(cost, counts, pricing);

    nlohmann::json j;
    j["index"]     = in.index;
    j["result_id"] = in.result_id;
    j["time_s"]    = (long long) std::llround(ps.modes[size_t(ETimeMode::Normal)].time);
    const float silent = ps.modes[size_t(ETimeMode::Stealth)].time;
    if (silent > 0.f)
        j["time_silent_s"] = (long long) std::llround(silent);
    j["prepare_time_s"] = (long long) std::llround(ps.modes[size_t(ETimeMode::Normal)].prepare_time);
    if (in.gcode_bytes >= 0)
        j["gcode_bytes"] = in.gcode_bytes;

    // metres as the desktop's statistics compute them: volume / filament cross-section.
    auto meters_of = [&result](size_t slot, double volume_mm3) {
        const double d = slot < result.filament_diameters.size() ? double(result.filament_diameters[slot]) : 0.;
        return d > 0. ? 0.001 * volume_mm3 / (PI * 0.25 * d * d) : 0.;
    };
    auto total_volume = [&ps](size_t slot) {
        auto it = ps.total_volumes_per_extruder.find(slot);
        return it == ps.total_volumes_per_extruder.end() ? 0. : it->second;
    };

    double total_g = 0., total_m = 0., flush_g = 0.;
    j["filaments"] = nlohmann::json::array();
    for (const FilamentCostLine& line : cost.lines) {
        nlohmann::json f;
        f["index"] = line.slot;
        const SlotInfo* s = line.slot < in.slots.size() ? &in.slots[line.slot] : nullptr;
        f["preset"] = s ? s->preset : std::string();
        f["type"]   = s ? s->type : std::string();
        // the colour the plate was sliced with when the result carries it, else the project's
        std::string color = s ? s->color : std::string();
        if (color.empty() && line.slot < result.extruder_colors.size())
            color = result.extruder_colors[line.slot];
        f["color"] = color;
        double vol = total_volume(line.slot);
        if (vol <= 0. && line.slot < result.filament_densities.size() && result.filament_densities[line.slot] > 0.f)
            vol = line.total_g / (0.001 * result.filament_densities[line.slot]);
        const double m = meters_of(line.slot, vol);
        f["grams"]        = round2(line.total_g);
        f["meters"]       = round2(m);
        f["cost"]         = round_money(line.total_cost);
        f["price_per_kg"] = round2(line.price_per_kg);
        f["model_g"]      = round2(line.model_g);
        f["support_g"]    = round2(line.support_g);
        f["flush_g"]      = round2(line.flush_g);
        f["tower_g"]      = round2(line.tower_g);
        j["filaments"].push_back(f);
        total_g += line.total_g;
        total_m += m;
        flush_g += line.flush_g;
    }

    nlohmann::json t;
    t["grams"]            = round2(total_g);
    t["meters"]           = round2(total_m);
    // The leaves are cent-rounded already; rounding the sums again only drops float noise (6.300000000000001).
    t["cost"]             = round_money(cost.display_total); // material + machine (printer time_cost), as the cost panel's "Total"
    t["material_cost"]    = round_money(cost.display_material);
    t["machine_cost"]     = round_money(cost.display_machine);
    t["flush_grams"]      = round2(flush_g);
    t["filament_changes"] = ps.total_filamentchanges;
    j["total"]            = t;

    nlohmann::json c;
    c["prices_known"]        = cost.prices_known;
    c["machine_rate_known"]  = cost.machine_rate_known;
    c["machine_rate_per_h"]  = cost.machine_rate_per_h;
    c["material"]            = round_money(priced.material);
    c["machine"]             = round_money(priced.machine);
    c["fees"]                = round_money(priced.fees);
    c["total_cost"]          = round_money(priced.total_cost);
    c["markup"]              = round_money(priced.markup);
    c["selling_price"]       = round_money(priced.selling_price);
    c["has_markup"]          = priced.has_markup();
    j["cost"]                = c;

    j["warnings"] = nlohmann::json::array();
    for (const PlateWarning& w : in.warnings)
        j["warnings"].push_back(warning_json(w));
    for (const PlateWarning& w : processor_warnings(result))
        j["warnings"].push_back(warning_json(w));
    return j;
}

// ---- cache headers ----

std::string etag_of(const std::string& body)
{
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char ch : body) {
        h ^= ch;
        h *= 1099511628211ULL;
    }
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long) h);
    return std::string("\"") + buf + "\"";
}

bool etag_matches(const std::string& if_none_match, const std::string& etag)
{
    if (etag.empty())
        return false;
    size_t pos = 0;
    while (pos < if_none_match.size()) {
        size_t comma = if_none_match.find(',', pos);
        if (comma == std::string::npos)
            comma = if_none_match.size();
        std::string tag = if_none_match.substr(pos, comma - pos);
        const size_t b  = tag.find_first_not_of(" \t");
        const size_t e  = tag.find_last_not_of(" \t");
        tag             = b == std::string::npos ? std::string() : tag.substr(b, e - b + 1);
        if (tag == "*")
            return true;
        if (tag.compare(0, 2, "W/") == 0)
            tag = tag.substr(2); // weak comparison, as If-None-Match uses
        if (tag == etag)
            return true;
        pos = comma + 1;
    }
    return false;
}

std::string image_cache_headers(const std::string& version, const std::string& etag)
{
    const bool versioned = !version.empty() && version != "0";
    return std::string("Cache-Control: ") + (versioned ? "private, max-age=86400" : "private, no-cache") + "\r\nETag: " + etag + "\r\n";
}

std::string result_cache_headers(const std::string& etag)
{
    return "Cache-Control: private, no-cache\r\nETag: " + etag + "\r\n";
}

} // namespace SliceV1
} // namespace GUI
} // namespace Slic3r
