#include <catch2/catch.hpp>

#include <nlohmann/json.hpp>

#include <cstdio>

#include "slic3r/GUI/BambuCloudWatch.hpp"

using namespace Slic3r::BambuCloudWatch;
using nlohmann::json;

// Synthetic, masked data only: no real serials. The shapes are what the slicer parses - the account's
// printer list (DeviceManager::parse_user_print_info, task_status) and a printer's push_status
// (MachineObject::parse_json, gcode_state / mc_percent / layer_num).

namespace {

const char* CLOUD_LIST = R"({"devices":[
  {"dev_id":"00M00A000000001","dev_name":"H2D","dev_online":true,"dev_model_name":"O1D","task_status":"RUNNING"},
  {"dev_id":"00M00A000000002","dev_name":"H2C","dev_online":true,"dev_model_name":"O1C2","task_status":"RUNNING"},
  {"dev_id":"00M00A000000003","dev_name":"X1C","dev_online":true,"dev_model_name":"BL-P001","task_status":"FINISH"},
  {"dev_id":"00M00A000000004","dev_name":"P1S","dev_online":true,"dev_model_name":"C12","task_status":null}
]})";

const char* PUSH_RUNNING = R"({"print":{"command":"push_status","sequence_id":"1","gcode_state":"RUNNING",
  "mc_percent":22,"layer_num":73,"total_layer_num":415,"mc_remaining_time":476,"mc_print_stage":"2"}})";

// A newer firmware's delta: what changed since the last report, no gcode_state.
const char* PUSH_DELTA = R"({"print":{"command":"push_status","sequence_id":"2","mc_percent":23,"layer_num":76}})";

// The slicer's MachineObject in miniature: print_status only ever comes from a push's gcode_state,
// and a delta without it leaves it as it was.
struct Printer
{
    std::string print_status;
    std::string iot_print_status;
    int         percent { 0 };
    int         pushes { 0 };

    void apply_list_entry(const json& e)
    {
        if (e.contains("task_status") && !e["task_status"].is_null()) iot_print_status = e["task_status"].get<std::string>();
    }
    void apply_push(const char* text)
    {
        const json j  = json::parse(text);
        const json& p = j["print"];
        if (p.value("command", std::string()) != "push_status") return;
        ++pushes;
        if (p.contains("gcode_state")) print_status = p["gcode_state"].get<std::string>();
        if (p.contains("mc_percent")) percent = p["mc_percent"].get<int>();
    }
};

Printer from_list(int index)
{
    Printer p;
    p.apply_list_entry(json::parse(CLOUD_LIST)["devices"][index]);
    return p;
}

} // namespace

TEST_CASE("Cloud row: a printer that has not reported says what the cloud list says, and printing agrees", "[BambuCloudWatch]")
{
    // The H2D / H2C rows of 2026-10-25: before this, status "" next to printing: true.
    for (int i : { 0, 1 }) {
        const Printer   p  = from_list(i);
        const RowStatus rs = row_status(p.print_status, p.iot_print_status);
        CHECK(rs.status == "RUNNING");
        CHECK(rs.printing);
        CHECK(rs.source == "cloud_list");
        CHECK(p.pushes == 0);
    }
}

TEST_CASE("Cloud row: the status is never empty while printing is true", "[BambuCloudWatch]")
{
    const char* words[] = { "", "IDLE", "RUNNING", "PAUSE", "PREPARE", "SLICING", "FINISH", "FAILED", "UNKNOWN" };
    for (const char* report : words)
        for (const char* list : words) {
            const RowStatus rs = row_status(report, list);
            if (rs.printing) CHECK_FALSE(rs.status.empty());
            CHECK(rs.printing == is_printing_status(rs.status));
        }
}

TEST_CASE("Cloud row: the printer's own report wins over the list", "[BambuCloudWatch]")
{
    Printer p = from_list(2); // the list still says FINISH
    p.apply_push(PUSH_RUNNING);
    RowStatus rs = row_status(p.print_status, p.iot_print_status);
    CHECK(rs.status == "RUNNING");
    CHECK(rs.printing);
    CHECK(rs.source == "report");
    CHECK(p.percent == 22);

    // A delta without gcode_state keeps the state the full report set.
    p.apply_push(PUSH_DELTA);
    rs = row_status(p.print_status, p.iot_print_status);
    CHECK(rs.status == "RUNNING");
    CHECK(rs.source == "report");
    CHECK(p.percent == 23);
}

TEST_CASE("Cloud row: nothing known at all", "[BambuCloudWatch]")
{
    const Printer   p  = from_list(3); // task_status null
    const RowStatus rs = row_status(p.print_status, p.iot_print_status);
    CHECK(rs.status.empty());
    CHECK_FALSE(rs.printing);
    CHECK(rs.source == "none");
}

TEST_CASE("Cloud row freshness: the hub's poll age is not the printer's", "[BambuCloudWatch]")
{
    const long long POLL_STALE = 30000;

    // A window from before report_age_s: the poll decides, as before.
    Freshness f = row_freshness(3000, POLL_STALE, false, 0, true);
    CHECK(f.age_ms == 3000);
    CHECK_FALSE(f.stale);
    CHECK(row_freshness(31000, POLL_STALE, false, 0, true).stale);

    // Polled three seconds ago, but the printer has never reported: stale (the H2D / H2C rows).
    f = row_freshness(3000, POLL_STALE, true, -1, true);
    CHECK(f.stale);
    CHECK(f.age_ms == 3000);

    // Reporting: fresh, and as old as the report plus the poll.
    f = row_freshness(3000, POLL_STALE, true, 2, true);
    CHECK_FALSE(f.stale);
    CHECK(f.age_ms == 5000);

    // Printing, but the reports stopped two minutes ago: stale, with the true age.
    f = row_freshness(3000, POLL_STALE, true, 130, true);
    CHECK(f.stale);
    CHECK(f.age_ms == 133000);

    // An idle P1 that has only been quiet for five minutes is not stale; a quarter of an hour is.
    CHECK_FALSE(row_freshness(3000, POLL_STALE, true, 300, false).stale);
    CHECK(row_freshness(3000, POLL_STALE, true, 16 * 60, false).stale);

    // A window that went away still makes the row stale, however fresh its last report was.
    CHECK(row_freshness(60000, POLL_STALE, true, 1, true).stale);
}

TEST_CASE("Cloud watch: every online cloud printer is subscribed once, at once", "[BambuCloudWatch]")
{
    Asked asked;
    std::vector<Candidate> cands = { { "00M00A000000002", -1 }, { "00M00A000000001", -1 } };
    std::vector<std::string> due = subscribe_due(asked, cands, 1000);
    REQUIRE(due.size() == 2);
    CHECK(due[0] == "00M00A000000001"); // by id
    CHECK(due[1] == "00M00A000000002");

    // Five seconds later, still no report: not again yet.
    CHECK(subscribe_due(asked, cands, 6000).empty());

    // Reporting now: never again while the reports flow.
    cands = { { "00M00A000000001", 1000 }, { "00M00A000000002", 500 } };
    CHECK(subscribe_due(asked, cands, 1000 + RETRY_MS + 1).empty());
}

TEST_CASE("Cloud watch: a silent printer is asked again, at the slow pace", "[BambuCloudWatch]")
{
    Asked asked;
    subscribe_due(asked, { { "A", -1 } }, 0);
    // Quiet for longer than SILENT_AFTER_MS, but RETRY_MS has not passed.
    CHECK(subscribe_due(asked, { { "A", SILENT_AFTER_MS + 1 } }, RETRY_MS - 1).empty());
    // Now it has.
    std::vector<std::string> due = subscribe_due(asked, { { "A", SILENT_AFTER_MS + 1 } }, RETRY_MS);
    REQUIRE(due.size() == 1);
    CHECK(due[0] == "A");
    // And the clock starts again from that request.
    CHECK(subscribe_due(asked, { { "A", SILENT_AFTER_MS + 1 } }, RETRY_MS + 1000).empty());
    // Never reported at all counts as silent.
    CHECK(subscribe_due(asked, { { "A", -1 } }, 2 * RETRY_MS).size() == 1);
}

TEST_CASE("Cloud watch: a printer that leaves and comes back is subscribed at once", "[BambuCloudWatch]")
{
    Asked asked;
    subscribe_due(asked, { { "A", -1 }, { "B", -1 } }, 0);
    // B went offline (or into LAN mode): forgotten.
    CHECK(subscribe_due(asked, { { "A", 100 } }, 1000).empty());
    CHECK(asked.count("B") == 0);
    // Back online: subscribed straight away, not after RETRY_MS.
    const std::vector<std::string> due = subscribe_due(asked, { { "A", 100 }, { "B", -1 } }, 2000);
    REQUIRE(due.size() == 1);
    CHECK(due[0] == "B");
}

TEST_CASE("Cloud watch: duplicates, empty ids and the cap", "[BambuCloudWatch]")
{
    Asked asked;
    CHECK(subscribe_due(asked, { { "A", -1 }, { "A", -1 }, { "", -1 } }, 0) == std::vector<std::string>{ "A" });

    Asked many;
    std::vector<Candidate> cands;
    for (int i = 0; i < 20; ++i) {
        char id[32];
        std::snprintf(id, sizeof(id), "00M00A0000000%02d", i);
        cands.push_back({ id, -1 });
    }
    CHECK(subscribe_due(many, cands, 0).size() == MAX_PRINTERS);
}
