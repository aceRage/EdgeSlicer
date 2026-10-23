// slice_v1, the hub contract of the companion app's native Slice tab (EdgeSlicerApp
// docs/design_native_slice.md): the JSON producer of GET /api/plates/{i}/result, the cache headers of
// the plate images, the hub's allow-list entries for the new instance routes and the /pair flags.
//
// The fixtures in tests/slic3rutils/slice_v1/ are what the producer answers for two hand-built
// processor results; the app's own fixtures (tools/slice_v1/fixtures in EdgeSlicerApp) can be
// copied from them, and a change to the shape fails here first.

#include <catch2/catch.hpp>

#include "slic3r/GUI/RemoteHub.hpp"
#include "slic3r/GUI/RemoteSliceV1.hpp"

#include "libslic3r/CostEstimate.hpp"
#include "libslic3r/CostPricing.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"

#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>

using namespace Slic3r;
using namespace Slic3r::GUI;
using json = nlohmann::json;

namespace {

using Mode = PrintEstimatedStatistics::ETimeMode;

json read_fixture(const std::string& name)
{
    boost::nowide::ifstream in(std::string(SLICE_V1_FIXTURES_DIR) + "/" + name);
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    return json::parse(ss.str());
}

// Structural equality: the same keys (no extra, none missing), the same array lengths, strings and
// booleans equal, numbers within a hundredth of a cent.
void require_same(const json& got, const json& want, const std::string& where = "$")
{
    INFO(where << ": got " << got.dump() << ", want " << want.dump());
    if (want.is_number()) {
        REQUIRE(got.is_number());
        REQUIRE(std::abs(got.get<double>() - want.get<double>()) < 1e-6);
        return;
    }
    REQUIRE(got.type() == want.type());
    if (want.is_object()) {
        for (auto it = want.begin(); it != want.end(); ++it) {
            INFO("missing key " << it.key());
            REQUIRE(got.contains(it.key()));
            require_same(got.at(it.key()), it.value(), where + "." + it.key());
        }
        for (auto it = got.begin(); it != got.end(); ++it) {
            INFO("unexpected key " << it.key());
            REQUIRE(want.contains(it.key()));
        }
    } else if (want.is_array()) {
        REQUIRE(got.size() == want.size());
        for (size_t i = 0; i < want.size(); ++i)
            require_same(got.at(i), want.at(i), where + "[" + std::to_string(i) + "]");
    } else {
        REQUIRE(got == want);
    }
}

// Two filaments with prices and a machine rate; PLA prints model, support, flush and tower, PETG
// model and flush; a third slot exists in the project but extrudes nothing.
void fill_two_filaments(GCodeProcessorResult& r)
{
    r.reset();
    r.extruders_count    = 3;
    r.filament_densities = { 1.24f, 1.27f, 1.24f };
    r.filament_diameters = { 1.75f, 1.75f, 1.75f };
    r.filament_costs     = { 20.f, 25.f, 0.f };
    r.has_filament_costs = true;
    r.time_cost          = 2.0;
    r.has_time_cost      = true;
    auto& ps = r.print_statistics;
    ps.modes[size_t(Mode::Normal)].time         = 9000.f;
    ps.modes[size_t(Mode::Normal)].prepare_time = 120.f;
    ps.modes[size_t(Mode::Stealth)].time        = 12000.f;
    ps.model_volumes_per_extruder      = { { 0, 36000. }, { 1, 9000. } };
    ps.support_volumes_per_extruder    = { { 0, 2000. } };
    ps.flush_per_filament              = { { 0, 1500. }, { 1, 1000. } };
    ps.wipe_tower_volumes_per_extruder = { { 0, 500. } };
    ps.total_volumes_per_extruder      = { { 0, 40000. }, { 1, 10000. } };
    ps.total_filamentchanges           = 24;
    GCodeProcessorResult::SliceWarning w;
    w.level = 1;
    w.msg   = BED_TEMP_TOO_HIGH_THAN_FILAMENT;
    r.warnings.push_back(w);
    r.conflict_result = ConflictResult("Hex planter", "Drip ring", 41.2, nullptr, nullptr);
    r.conflict_result->layer = 205;
    r.toolpath_outside       = false;
}

SliceV1::ResultInput two_filament_input()
{
    SliceV1::ResultInput in;
    in.index       = 0;
    in.result_id   = 1842;
    in.gcode_bytes = 7130112;
    in.slots       = { { "Bambu PLA Basic @BBL X1C", "PLA", "#E6EBE3" },
                       { "Generic PETG @BBL X1C", "PETG", "#F5822F" },
                       { "Generic TPU @BBL X1C", "TPU", "#000000" } };
    SliceV1::PlateWarning pw;
    pw.code     = "slicing";
    pw.severity = "warning";
    pw.text     = "It seems object Hex planter has floating regions.";
    pw.object   = "Hex planter";
    in.warnings.push_back(pw);
    return in;
}

} // namespace

TEST_CASE("slice_v1 result: the desktop's figures for a two-filament plate", "[SliceV1]")
{
    GCodeProcessorResult r;
    fill_two_filaments(r);
    const json got = SliceV1::result_json(r, two_filament_input(), PlateCounts{ 2, 2 }, PricingSettings());
    require_same(got, read_fixture("result_two_filaments.json"));
}

TEST_CASE("slice_v1 result: total cost is CostEstimate's total, filament plus machine time", "[SliceV1]")
{
    GCodeProcessorResult r;
    fill_two_filaments(r);
    const json got = SliceV1::result_json(r, two_filament_input(), PlateCounts{ 2, 2 }, PricingSettings());
    // The same number the cost panel's "Total" and PrintStatistics::total_cost come from.
    REQUIRE(std::abs(got["total"]["cost"].get<double>() - round_money(compute_cost(r).display_total)) < 1e-9);
    double sum = 0.;
    for (const auto& f : got["filaments"]) sum += f["cost"].get<double>();
    REQUIRE(std::abs(got["total"]["material_cost"].get<double>() - sum) < 1e-9);
}

TEST_CASE("slice_v1 result: unknown prices, fees and markup", "[SliceV1]")
{
    GCodeProcessorResult r;
    r.reset();
    r.extruders_count    = 1;
    r.filament_densities = { 1.24f };
    r.filament_diameters = { 1.75f };
    r.filament_costs     = { 0.f };
    r.has_filament_costs = false; // a G-code exported without prices
    r.time_cost          = 1.5;
    r.has_time_cost      = true;
    // PrintEstimatedStatistics::reset() does not clear the modes; say every time used here.
    r.print_statistics.modes[size_t(Mode::Normal)].time         = 3600.f;
    r.print_statistics.modes[size_t(Mode::Normal)].prepare_time = 0.f;
    r.print_statistics.modes[size_t(Mode::Stealth)].time        = 0.f;
    r.print_statistics.total_filamentchanges                    = 0;
    r.print_statistics.model_volumes_per_extruder = { { 0, 10000. } };
    r.print_statistics.total_volumes_per_extruder = { { 0, 10000. } };
    r.toolpath_outside = true;

    SliceV1::ResultInput in;
    in.index     = 2;
    in.result_id = 7;
    in.slots     = { { "Generic PLA", "PLA", "#FFFFFF" } };

    PricingSettings pricing;
    pricing.fee_per_plate = 2.5;
    pricing.markup_type   = MarkupType::Percent;
    pricing.markup_basis  = MarkupBasis::Overall;
    pricing.markup_value  = 25.;
    const json got = SliceV1::result_json(r, in, PlateCounts{ 1, 1 }, pricing);
    require_same(got, read_fixture("result_unpriced_markup.json"));
}

TEST_CASE("slice_v1 result: a slot the project no longer has keeps its numbers", "[SliceV1]")
{
    GCodeProcessorResult r;
    fill_two_filaments(r);
    SliceV1::ResultInput in = two_filament_input();
    in.slots.resize(1); // the PETG slot was removed after slicing
    r.extruder_colors = { "#E6EBE3", "#F5822F" };
    const json got = SliceV1::result_json(r, in, PlateCounts{}, PricingSettings());
    REQUIRE(got["filaments"].size() == 2);
    REQUIRE(got["filaments"][1]["preset"] == "");
    REQUIRE(got["filaments"][1]["color"] == "#F5822F"); // from the result itself
    REQUIRE(got["filaments"][1]["grams"].get<double>() > 0.);
}

TEST_CASE("slice_v1 processor warnings: codes and severities", "[SliceV1]")
{
    GCodeProcessorResult r;
    r.reset();
    GCodeProcessorResult::SliceWarning tip;
    tip.level = 0;
    tip.msg   = NOT_GENERATE_TIMELAPSE;
    GCodeProcessorResult::SliceWarning err;
    err.level = 2;
    err.msg   = NOZZLE_HRC_CHECKER;
    r.warnings = { tip, err };
    r.toolpath_outside = true;
    const auto w = SliceV1::processor_warnings(r);
    REQUIRE(w.size() == 3);
    CHECK(w[0].code == NOT_GENERATE_TIMELAPSE);
    CHECK(w[0].severity == "info");
    CHECK(w[1].code == NOZZLE_HRC_CHECKER);
    CHECK(w[1].severity == "error");
    CHECK(w[2].code == "toolpath_outside");
    CHECK(w[2].severity == "error");
    for (const auto& x : w) CHECK(!x.text.empty());
}

TEST_CASE("slice_v1 ETag: stable, content-bound and matched as If-None-Match does", "[SliceV1]")
{
    const std::string a = SliceV1::etag_of("{\"a\":1}");
    REQUIRE(a == SliceV1::etag_of("{\"a\":1}"));
    REQUIRE(a != SliceV1::etag_of("{\"a\":2}"));
    REQUIRE(a.size() == 18);
    REQUIRE(a.front() == '"');
    REQUIRE(a.back() == '"');
    CHECK(SliceV1::etag_matches(a, a));
    CHECK(SliceV1::etag_matches(" " + a, a));
    CHECK(SliceV1::etag_matches("\"x\", W/" + a, a));
    CHECK(SliceV1::etag_matches("*", a));
    CHECK_FALSE(SliceV1::etag_matches("\"x\"", a));
    CHECK_FALSE(SliceV1::etag_matches("", a));
    CHECK_FALSE(SliceV1::etag_matches(a, ""));
}

TEST_CASE("slice_v1 cache headers: a versioned image is kept, everything else revalidates", "[SliceV1]")
{
    const std::string etag = SliceV1::etag_of("png");
    const std::string v    = SliceV1::image_cache_headers("1842", etag);
    CHECK(v.find("Cache-Control: private, max-age=86400\r\n") != std::string::npos);
    CHECK(v.find("ETag: " + etag + "\r\n") != std::string::npos);
    // no ?v=, or v=0 (an unsliced plate: its thumbnail changes with every edit)
    CHECK(SliceV1::image_cache_headers("", etag).find("Cache-Control: private, no-cache\r\n") != std::string::npos);
    CHECK(SliceV1::image_cache_headers("0", etag).find("Cache-Control: private, no-cache\r\n") != std::string::npos);
    const std::string r = SliceV1::result_cache_headers(etag);
    CHECK(r.find("Cache-Control: private, no-cache\r\n") != std::string::npos);
    CHECK(r.find("ETag: " + etag + "\r\n") != std::string::npos);
}

TEST_CASE("slice_v1 allow-list: the new instance routes and nothing around them", "[SliceV1][RemoteHub]")
{
    using RemoteHub::Testing::instance_api_allowed;
    CHECK(instance_api_allowed("GET", "/api/plates/0/result"));
    CHECK(instance_api_allowed("GET", "/api/plates/12/result"));
    CHECK_FALSE(instance_api_allowed("POST", "/api/plates/0/result"));
    CHECK_FALSE(instance_api_allowed("GET", "/api/plates/x/result"));
    CHECK_FALSE(instance_api_allowed("GET", "/api/plates/0/result/x"));

    CHECK(instance_api_allowed("POST", "/api/jobs/3/cancel"));
    CHECK_FALSE(instance_api_allowed("GET", "/api/jobs/3/cancel"));
    CHECK_FALSE(instance_api_allowed("DELETE", "/api/jobs/3/cancel"));
    CHECK_FALSE(instance_api_allowed("POST", "/api/jobs/x/cancel"));
    CHECK_FALSE(instance_api_allowed("POST", "/api/jobs//cancel"));
    CHECK_FALSE(instance_api_allowed("POST", "/api/jobs/3/cancel/more"));
    CHECK_FALSE(instance_api_allowed("POST", "/api/jobs/3"));

    // what was there before is unchanged
    CHECK(instance_api_allowed("GET", "/api/jobs/3"));
    CHECK(instance_api_allowed("GET", "/api/jobs"));
    CHECK(instance_api_allowed("GET", "/api/plates/0/thumbnail.png"));
    CHECK(instance_api_allowed("POST", "/api/plates/0/send"));
    CHECK(instance_api_allowed("GET", "/api/presets"));
    CHECK_FALSE(instance_api_allowed("POST", "/api/debug/anything"));
}

TEST_CASE("slice_v1 /pair features: the native Slice tab's flags are listed", "[SliceV1][RemoteHub]")
{
    const auto f   = RemoteHub::Testing::pair_features();
    auto       has = [&f](const char* s) { return std::find(f.begin(), f.end(), s) != f.end(); };
    CHECK(has("slice_v1"));
    CHECK(has("slice_result"));
    CHECK(has("slice_cancel"));
    // the earlier flags stay
    for (const char* s : { "events", "control", "send", "summary", "thumbnail", "webrtc", "quality", "apppush", "hubid", "push_levels", "live_activity" })
        CHECK(has(s));
}

// ---- PR B: the send preview sheet ----------------------------------------------------------

namespace {

SliceV1::Sheet u1_sheet()
{
    SliceV1::Sheet s;
    s.mapping_kind = "toolheads";
    s.editable     = true;
    s.filaments    = { { 0, "PLA", "#FFFFFF", 20.5 }, { 1, "PLA", "#101010", 3.25 }, { 3, "PLA", "#FF0000", 1.0 } };
    s.targets      = { { "0", "T1", "PLA", "#FFFFFF", true, "" },
                       { "1", "T2", "PETG", "#000000", true, "" },
                       { "2", "T3", "", "", false, "" },
                       { "3", "T4", "PLA", "#FF0000", true, "" } };
    s.choice       = { { 0, 0 }, { 1, 1 }, { 3, 2 } };
    s.automatic    = { { 0, 0 }, { 1, 3 }, { 3, 3 } };
    return s;
}

} // namespace

TEST_CASE("slice_v1 send preview: a U1 sheet with an empty toolhead and a material warning", "[SliceV1]")
{
    SliceV1::Sheet s = u1_sheet();
    SliceV1::check_toolheads(s, false);
    require_same(SliceV1::sheet_json(s), read_fixture("send_preview_u1.json"));
}

TEST_CASE("slice_v1 send preview: force lets an empty toolhead through, as the send does", "[SliceV1]")
{
    SliceV1::Sheet s = u1_sheet();
    SliceV1::check_toolheads(s, true);
    CHECK(s.can_send);
    CHECK(s.problems.empty());
}

TEST_CASE("slice_v1 send preview: a filament with no toolhead or one past the last is refused", "[SliceV1]")
{
    SliceV1::Sheet s = u1_sheet();
    s.choice[0] = -1;
    s.choice[1] = 7;
    SliceV1::check_toolheads(s, true);
    CHECK_FALSE(s.can_send);
    REQUIRE(s.problems.count(0) == 1);
    REQUIRE(s.problems.count(1) == 1);
    CHECK(s.problems[0].front().find("has no toolhead") != std::string::npos);
    CHECK(s.problems[1].front().find("no toolhead 8") != std::string::npos);
    CHECK(s.mapping == "1:7,3:2"); // filament 0 has none, so it is left out of the wire form
}

TEST_CASE("slice_v1 send preview: positions_wire is what /send takes as mapping", "[SliceV1]")
{
    CHECK(SliceV1::positions_wire({}) == "");
    CHECK(SliceV1::positions_wire({ { 0, 1 }, { 1, -1 }, { 2, 0 } }) == "0:1,2:0");
}

TEST_CASE("slice_v1 send preview: a plate-level problem (slot policy) is listed but on no filament", "[SliceV1]")
{
    SliceV1::Sheet s;
    s.mapping_kind = "canvas";
    s.filaments    = { { 0, "PLA", "#FFFFFF", 5. } };
    s.targets      = { { "0", "1A", "PETG", "#000000", true, "" } };
    s.choice       = { { 0, 0 } };
    s.problems[-1] = { "Filament 1 (PLA) is on tray 1A, which holds PETG." };
    s.policy       = "type_mismatch";
    s.can_send     = false;
    const json j   = SliceV1::sheet_json(s);
    REQUIRE(j["problems"].size() == 1);
    CHECK(j["problems"][0]["filament"] == -1);
    CHECK(j["filaments"][0]["problems"].empty());
    CHECK(j["policy"] == "type_mismatch");
    CHECK(j["can_send"] == false);
}

TEST_CASE("slice_v1 send preview: the Bambu reprint sheet gains the plate sheet's fields", "[SliceV1]")
{
    json p = json::parse(R"({
        "printer": {"id": "X", "name": "X1C", "model": "BL-P001"},
        "filaments": [
            {"index": 0, "type": "PLA", "colour": "#E6EBE3", "grams": 58.3, "side": "", "tray": "0-0", "auto": true, "problems": []},
            {"index": 2, "type": "PETG", "colour": "#000000", "grams": 2.0, "side": "", "tray": null, "auto": false, "problems": ["no tray"]}
        ],
        "trays": [
            {"ams_id": 0, "slot_id": 0, "id": "0-0", "name": "A1", "side": "", "exists": true, "ready": true, "type": "PLA", "match_type": "PLA", "colour": "#E6EBE3"}
        ],
        "mapping": "0:0-0", "problems": [], "options": {}, "warnings": [], "can_send": false
    })");
    SliceV1::decorate_ams_preview(p);
    CHECK(p["mapping_kind"] == "ams");
    CHECK(p["editable"] == true);
    REQUIRE(p["targets"].size() == 1);
    CHECK(p["targets"][0] == json::parse(R"({"id": "0-0", "label": "A1", "type": "PLA", "color": "#E6EBE3", "ready": true, "side": ""})"));
    CHECK(p["filaments"][0]["color"] == "#E6EBE3");
    CHECK(p["filaments"][0]["target"] == "0-0");
    CHECK(p["filaments"][1]["target"].is_null());
    // the reprint's own fields are still there for the app's BambuMapping model
    CHECK(p["filaments"][0]["colour"] == "#E6EBE3");
    CHECK(p["filaments"][0]["tray"] == "0-0");
    CHECK(p["mapping"] == "0:0-0");
    CHECK(p["policy"] == "");
}

TEST_CASE("slice_v1 send preview: the hub proxies the preview route and /pair says so", "[SliceV1][RemoteHub]")
{
    using RemoteHub::Testing::instance_api_allowed;
    CHECK(instance_api_allowed("POST", "/api/plates/0/send/preview"));
    CHECK_FALSE(instance_api_allowed("GET", "/api/plates/0/send/preview"));
    CHECK_FALSE(instance_api_allowed("POST", "/api/plates/x/send/preview"));
    CHECK_FALSE(instance_api_allowed("POST", "/api/plates/0/send/preview/x"));
    const auto f = RemoteHub::Testing::pair_features();
    CHECK(std::find(f.begin(), f.end(), "slice_send_preview") != f.end());
}
