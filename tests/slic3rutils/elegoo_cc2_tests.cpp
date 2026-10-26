#include <catch2/catch.hpp>

#include "slic3r/Utils/ElegooCc2.hpp"
#include "slic3r/Utils/ElegooCanvas.hpp"
#include "slic3r/Utils/ElegooStartPrint.hpp"

#include <nlohmann/json.hpp>

#include <deque>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;
using json = nlohmann::json;

// The Elegoo Centauri Carbon 2 on the LAN (src/slic3r/Utils/ElegooCc2.hpp): HTTP /system/info and
// PUT /upload, MQTT registration and the 1002 / 2005 / 1020 requests. Every case runs without a
// printer: the MQTT side is a scripted printer (MockCc2) that answers what it is sent. Serial
// numbers and ids below are made up.

namespace {

const char* SN = "TESTSN0001";

// A scripted CC2: answers the registration, then each request through `answer` (method, params) ->
// result object, or "" to stay silent.
struct MockCc2 : ElegooCc2::MqttTransport
{
    std::deque<std::pair<std::string, std::string>>    inbox;
    std::vector<std::pair<std::string, std::string>>   published;
    std::vector<std::string>                           subscribed;
    std::function<std::string(int, const json&)>       answer;
    std::string                                        register_error = "ok";
    bool                                               refuse_connect = false;
    bool                                               closed         = false;
    std::string                                        client_id;

    bool connect(const std::string& id, std::string& error) override
    {
        if (refuse_connect) {
            error = "could not connect to the printer's MQTT port";
            return false;
        }
        client_id = id;
        return true;
    }
    bool subscribe(const std::string& topic, std::string&) override
    {
        subscribed.push_back(topic);
        return true;
    }
    bool publish(const std::string& topic, const std::string& payload, std::string&) override
    {
        published.emplace_back(topic, payload);
        const json j = json::parse(payload, nullptr, false);
        if (topic == ElegooCc2::register_topic(SN)) {
            // Something for another client first: the session must skip it.
            inbox.emplace_back(ElegooCc2::register_response_topic(SN, j["request_id"].get<std::string>()),
                               R"({"client_id":"1_PC_0000","error":"ok"})");
            json r;
            r["client_id"] = j["client_id"];
            r["error"]     = register_error;
            inbox.emplace_back(ElegooCc2::register_response_topic(SN, j["request_id"].get<std::string>()), r.dump());
            return true;
        }
        if (topic == ElegooCc2::request_topic(SN, client_id) && answer) {
            const int         method = j["method"].get<int>();
            const std::string result = answer(method, j["params"]);
            if (result.empty())
                return true;
            // A status push and an answer to another id arrive first; the session must skip both.
            inbox.emplace_back("elegoo/" + std::string(SN) + "/api_status", R"({"id":1,"method":6000,"result":{}})");
            inbox.emplace_back(ElegooCc2::response_topic(SN, client_id),
                               R"({"id":)" + std::to_string(j["id"].get<int>() + 7777) + R"(,"method":1002,"result":{"error_code":0}})");
            json r;
            r["id"]     = j["id"];
            r["method"] = method;
            r["result"] = json::parse(result);
            inbox.emplace_back(ElegooCc2::response_topic(SN, client_id), r.dump());
        }
        return true;
    }
    bool receive(std::string& topic, std::string& payload, int) override
    {
        if (inbox.empty())
            return false;
        topic   = inbox.front().first;
        payload = inbox.front().second;
        inbox.pop_front();
        return true;
    }
    void close() override { closed = true; }

    std::vector<json> requests(int method) const
    {
        std::vector<json> out;
        for (const auto& p : published) {
            if (p.first != ElegooCc2::request_topic(SN, client_id))
                continue;
            const json j = json::parse(p.second, nullptr, false);
            if (j.is_object() && j.value("method", -1) == method)
                out.push_back(j);
        }
        return out;
    }
};

ElegooCc2::SessionOptions quick()
{
    ElegooCc2::SessionOptions o;
    o.client_id           = "1_PC_4242";
    o.first_id            = 100;
    o.register_timeout_ms = 200;
    o.reply_timeout_ms    = 200;
    return o;
}

std::string status_result(int status, int sub_status = 0)
{
    return R"({"error_code":0,"machine_status":{"status":)" + std::to_string(status) + R"(,"sub_status":)" +
           std::to_string(sub_status) + "}}";
}

// The CANVAS answer of a CC2 Combo: one connected unit with four trays (red PLA in the toolhead,
// an untagged white PLA, black PETG and an empty tray) and a second, disconnected unit.
const char* COMBO_CANVAS = R"({"error_code":0,"canvas_info":{"active_canvas_id":0,"active_tray_id":0,"auto_refill":false,
 "canvas_list":[{"canvas_id":0,"connected":1,"tray_list":[
   {"tray_id":0,"brand":"ELEGOO","filament_type":"PLA","filament_name":"PLA","filament_code":"0x0000","filament_color":"#FF0000","min_nozzle_temp":190,"max_nozzle_temp":230,"status":2},
   {"tray_id":1,"brand":"— —","filament_type":"PLA","filament_name":"PLA","filament_code":"0x00000","filament_color":"#FFFFFF","min_nozzle_temp":190,"max_nozzle_temp":230,"status":1},
   {"tray_id":2,"brand":"ELEGOO","filament_type":"PETG","filament_name":"PETG","filament_code":"","filament_color":"#000000","min_nozzle_temp":230,"max_nozzle_temp":260,"status":1},
   {"tray_id":3,"brand":"","filament_type":"","filament_name":"","filament_code":"","filament_color":"","min_nozzle_temp":0,"max_nozzle_temp":0,"status":0}]},
  {"canvas_id":1,"connected":0,"tray_list":[]}]}})";

} // namespace

TEST_CASE("Centauri Carbon 2 models are told apart from the CC1", "[ElegooCc2]")
{
    CHECK(ElegooCc2::is_cc2_model("Elegoo Centauri Carbon 2"));
    CHECK(ElegooCc2::is_cc2_model("  elegoo centauri carbon 2 "));
    CHECK(ElegooCc2::is_cc2_model("Elegoo Centauri 2"));
    CHECK_FALSE(ElegooCc2::is_cc2_model("Elegoo Centauri Carbon"));
    CHECK_FALSE(ElegooCc2::is_cc2_model("Elegoo Centauri"));
    CHECK_FALSE(ElegooCc2::is_cc2_model("Elegoo Neptune 4 Pro"));
    CHECK_FALSE(ElegooCc2::is_cc2_model(""));

    // The CC1 keeps its SDCP-only features; the CANVAS readers include both.
    CHECK(canvas_capable_model("Elegoo Centauri Carbon"));
    CHECK_FALSE(canvas_capable_model("Elegoo Centauri Carbon 2"));
    CHECK(canvas_reader_model("Elegoo Centauri Carbon"));
    CHECK(canvas_reader_model("Elegoo Centauri Carbon 2"));
    CHECK(canvas_reader_model("Elegoo Centauri 2"));
    CHECK_FALSE(canvas_reader_model("Elegoo Neptune 4 Pro"));
}

TEST_CASE("CC2 access code and host", "[ElegooCc2]")
{
    CHECK(ElegooCc2::token_or_default("") == "123456");
    CHECK(ElegooCc2::token_or_default("  ") == "123456");
    CHECK(ElegooCc2::token_or_default(" 9f3a ") == "9f3a");
    CHECK(ElegooCc2::host_of("http://10.0.0.5:80/") == "10.0.0.5");
    CHECK(ElegooCc2::host_of("10.0.0.5") == "10.0.0.5");
}

TEST_CASE("CC2 /system/info and /upload answers", "[ElegooCc2]")
{
    std::string sn, error;
    CHECK(ElegooCc2::parse_system_info(R"({"error_code":0,"system_info":{"sn":"ABC123","hostname":"cc2"}})", sn, error));
    CHECK(sn == "ABC123");
    CHECK_FALSE(ElegooCc2::parse_system_info(R"({"error_code":1000})", sn, error));
    CHECK(error == "the printer refused the access code");
    CHECK_FALSE(ElegooCc2::parse_system_info(R"({"error_code":0,"system_info":{}})", sn, error));
    CHECK_FALSE(ElegooCc2::parse_system_info("<html>ELEGOO</html>", sn, error));
    CHECK(sn.empty());

    CHECK(ElegooCc2::parse_upload_reply(R"({"error_code":0})", error));
    CHECK_FALSE(ElegooCc2::parse_upload_reply(R"({"error_code":1000})", error));
    CHECK_FALSE(ElegooCc2::parse_upload_reply(R"({"code":"000000"})", error)); // the CC1's answer is not the CC2's
    CHECK_FALSE(ElegooCc2::parse_upload_reply("", error));

    CHECK(ElegooCc2::content_range(0, 1048576, 2500000) == "bytes 0-1048575/2500000");
    CHECK(ElegooCc2::content_range(2097152, 402848, 2500000) == "bytes 2097152-2499999/2500000");
}

TEST_CASE("CC2 MQTT topics and registration", "[ElegooCc2]")
{
    CHECK(ElegooCc2::make_client_id(1234) == "1_PC_1234");
    CHECK(ElegooCc2::request_id_for("1_PC_1234") == "1_PC_1234_req");
    CHECK(ElegooCc2::register_topic("S1") == "elegoo/S1/api_register");
    CHECK(ElegooCc2::register_response_topic("S1", "1_PC_1234_req") == "elegoo/S1/1_PC_1234_req/register_response");
    CHECK(ElegooCc2::request_topic("S1", "1_PC_1234") == "elegoo/S1/1_PC_1234/api_request");
    CHECK(ElegooCc2::response_topic("S1", "1_PC_1234") == "elegoo/S1/1_PC_1234/api_response");
    CHECK(json::parse(ElegooCc2::register_payload("1_PC_1234", "1_PC_1234_req")) ==
          json::parse(R"({"client_id":"1_PC_1234","request_id":"1_PC_1234_req"})"));

    std::string error;
    using RR = ElegooCc2::RegisterResult;
    CHECK(ElegooCc2::parse_register_response(R"({"client_id":"1_PC_1","error":"ok"})", "1_PC_1", error) == RR::Ok);
    CHECK(ElegooCc2::parse_register_response(R"({"client_id":"1_PC_2","error":"ok"})", "1_PC_1", error) == RR::NotForUs);
    CHECK(ElegooCc2::parse_register_response(R"({"client_id":"1_PC_1","error":"too many clients"})", "1_PC_1", error) == RR::TooManyClients);
    CHECK(ElegooCc2::parse_register_response(R"({"client_id":"1_PC_1","error":"denied"})", "1_PC_1", error) == RR::Refused);
    CHECK(error == "denied");
    CHECK(ElegooCc2::parse_register_response("nope", "1_PC_1", error) == RR::Invalid);
}

TEST_CASE("CC2 request and reply envelopes", "[ElegooCc2]")
{
    CHECK(ElegooCc2::request_payload(7, 1002) == R"({"id":7,"method":1002,"params":{}})");
    CHECK(ElegooCc2::request_payload(8, 2005, "not json") == R"({"id":8,"method":2005,"params":{}})");

    const ElegooCc2::Reply r = ElegooCc2::parse_reply(R"({"id":9,"method":1002,"result":{"error_code":0,"machine_status":{"status":2,"sub_status":2077}}})");
    CHECK(r.valid);
    CHECK(r.id == 9);
    CHECK(r.method == 1002);
    CHECK(r.error_code == 0);
    CHECK(ElegooCc2::machine_status(r.result) == 2);
    CHECK(ElegooCc2::machine_sub_status(r.result) == 2077);
    CHECK_FALSE(ElegooCc2::parse_reply(R"({"method":6000})").valid);

    CHECK(ElegooCc2::state_name(1) == "standby");
    CHECK(ElegooCc2::state_name(2) == "printing");
    CHECK(ElegooCc2::state_name(-1).empty());
    CHECK_FALSE(ElegooCc2::status_is_busy(1));
    CHECK_FALSE(ElegooCc2::status_is_busy(-1));
    CHECK(ElegooCc2::status_is_busy(2, 0));
    CHECK_FALSE(ElegooCc2::status_is_busy(2, ElegooCc2::SUB_PRINT_COMPLETED)); // a finished print on the screen
    CHECK(ElegooCc2::status_is_busy(11));                                       // still taking in a file
}

TEST_CASE("CC2 start command parameters", "[ElegooCc2]")
{
    const int PC = 1; // the pc_bed_type the caller passes (BedType::btPC)

    SECTION("a plain CC2: no CANVAS, an empty slot_map, plate side A")
    {
        std::map<std::string, std::string> info { { "timeLapse", "1" }, { "heatedBedLeveling", "0" }, { "bedType", "5" } };
        const json p = json::parse(ElegooCc2::start_print_params(info, PC, "cube.gcode", false));
        CHECK(p == json::parse(R"({"storage_media":"local","filename":"cube.gcode","config":{"delay_video":true,
              "printer_check":false,"print_layout":"A","bedlevel_force":false,"slot_map":[]}})"));
        // Key order as elegoo-link builds it.
        CHECK(ElegooCc2::start_print_params(info, PC, "cube.gcode", false).rfind(R"({"storage_media":"local","filename":"cube.gcode","config":{)", 0) == 0);
    }
    SECTION("a Combo with two filaments mapped onto CANVAS trays, PC plate")
    {
        std::map<std::string, std::string> info { { "heatedBedLeveling", "1" }, { "bedType", "1" }, { "canvasAttached", "1" },
                                                  { "slotMap", "0:0:2,1:0:0" } };
        const json p = json::parse(ElegooCc2::start_print_params(info, PC, "two colour.gcode", true));
        CHECK(p["filename"] == "two colour.gcode");
        CHECK(p["config"]["print_layout"] == "B");
        CHECK(p["config"]["printer_check"] == true);
        CHECK(p["config"]["bedlevel_force"] == true);
        CHECK(p["config"]["delay_video"] == false);
        CHECK(p["config"]["slot_map"] == json::parse(R"([{"t":0,"canvas_id":0,"tray_id":2},{"t":1,"canvas_id":0,"tray_id":0}])"));
    }
    SECTION("a slotMap without a CANVAS read is not sent")
    {
        std::map<std::string, std::string> info { { "slotMap", "0:0:2" } };
        const json p = json::parse(ElegooCc2::start_print_params(info, PC, "a.gcode", false));
        CHECK(p["config"]["slot_map"].empty());
    }
    SECTION("a quote in the file name stays valid JSON")
    {
        const json p = json::parse(ElegooCc2::start_print_params({}, PC, "a \"b\".gcode", false));
        CHECK(p["filename"] == "a \"b\".gcode");
    }
}

TEST_CASE("CC2 session: registration", "[ElegooCc2]")
{
    MockCc2 printer;
    SECTION("registers, skipping another client's answer")
    {
        ElegooCc2::Session s(printer, SN, quick());
        std::string        error;
        REQUIRE(s.open(error));
        CHECK(printer.client_id == "1_PC_4242");
        CHECK(printer.subscribed == std::vector<std::string> { "elegoo/TESTSN0001/1_PC_4242/api_response",
                                                               "elegoo/TESTSN0001/1_PC_4242_req/register_response" });
        REQUIRE(!printer.published.empty());
        CHECK(printer.published[0].first == "elegoo/TESTSN0001/api_register");
    }
    SECTION("too many clients")
    {
        printer.register_error = "too many clients";
        ElegooCc2::Session s(printer, SN, quick());
        std::string        error;
        CHECK_FALSE(s.open(error));
        CHECK(error == "the printer has too many connections open");
        CHECK(printer.closed);
    }
    SECTION("no MQTT")
    {
        printer.refuse_connect = true;
        ElegooCc2::Session s(printer, SN, quick());
        std::string        error;
        CHECK_FALSE(s.open(error));
        CHECK_FALSE(error.empty());
    }
    SECTION("no serial number")
    {
        ElegooCc2::Session s(printer, "", quick());
        std::string        error;
        CHECK_FALSE(s.open(error));
        CHECK(printer.published.empty());
    }
}

TEST_CASE("CC2 CANVAS read: a Combo and a plain CC2", "[ElegooCc2]")
{
    MockCc2 printer;
    std::string canvas_answer;
    printer.answer = [&](int method, const json&) -> std::string {
        if (method == ElegooCc2::METHOD_GET_STATUS)
            return status_result(1);
        if (method == ElegooCc2::METHOD_GET_CANVAS)
            return canvas_answer;
        return "";
    };
    auto run = [&]() {
        ElegooCc2::Session s(printer, SN, quick());
        std::string        error;
        REQUIRE(s.open(error));
        return ElegooCc2::canvas_query_over(s);
    };

    SECTION("Combo: the connected unit's trays, the disconnected unit still numbered")
    {
        canvas_answer               = COMBO_CANVAS;
        const CanvasQueryResult r = run();
        REQUIRE(r.ok);
        CHECK_FALSE(r.no_canvas);
        CHECK(r.print_state == "standby");
        CHECK(r.ams_connect_status == 1);
        CHECK(r.info.units.size() == 2);
        REQUIRE(r.info.trays.size() == 4);
        CHECK(r.info.loaded_count() == 3);
        CHECK(r.info.trays[0].colour_hex() == "#FF0000");
        CHECK(r.info.trays[1].brand == "Generic"); // the dash-only brand
        CHECK(r.info.trays[2].tray_type == "PETG");
        CHECK_FALSE(r.info.trays[3].has_filament);
        CHECK(r.info.active_canvas_id == 0);
        CHECK(r.info.active_tray_id == 0);
    }
    SECTION("plain CC2: an empty canvas_list")
    {
        canvas_answer = R"({"error_code":0,"canvas_info":{"active_canvas_id":-1,"active_tray_id":-1,"canvas_list":[]}})";
        const CanvasQueryResult r = run();
        CHECK_FALSE(r.ok);
        CHECK(r.no_canvas);
        CHECK(r.reached);
        CHECK(r.print_state == "standby");
    }
    SECTION("plain CC2: only a disconnected unit")
    {
        canvas_answer = R"({"error_code":0,"canvas_info":{"canvas_list":[{"canvas_id":0,"connected":0,"tray_list":[{"tray_id":0}]}]}})";
        const CanvasQueryResult r = run();
        CHECK_FALSE(r.ok);
        CHECK(r.no_canvas);
    }
    SECTION("plain CC2: no canvas_info at all")
    {
        canvas_answer = R"({"error_code":0})";
        const CanvasQueryResult r = run();
        CHECK(r.no_canvas);
    }
    SECTION("the printer refuses the request")
    {
        canvas_answer = R"({"error_code":1001})";
        const CanvasQueryResult r = run();
        CHECK_FALSE(r.ok);
        CHECK_FALSE(r.no_canvas);
        CHECK(r.error == "the printer refused the request as invalid");
    }
    SECTION("the printer stays silent")
    {
        canvas_answer.clear();
        const CanvasQueryResult r = run();
        CHECK_FALSE(r.ok);
        CHECK_FALSE(r.no_canvas);
        CHECK_FALSE(r.error.empty());
    }
}

TEST_CASE("CC2 start print", "[ElegooCc2]")
{
    MockCc2          printer;
    std::deque<int>  statuses;       // successive machine_status.status answers (the last one repeats)
    int              sub_status = 0;
    std::deque<int>  start_codes;    // successive 1020 error codes (the last one repeats)
    printer.answer = [&](int method, const json&) -> std::string {
        if (method == ElegooCc2::METHOD_GET_STATUS) {
            const int st = statuses.front();
            if (statuses.size() > 1)
                statuses.pop_front();
            return status_result(st, sub_status);
        }
        if (method == ElegooCc2::METHOD_START_PRINT) {
            const int code = start_codes.front();
            if (start_codes.size() > 1)
                start_codes.pop_front();
            return R"({"error_code":)" + std::to_string(code) + "}";
        }
        return "";
    };
    auto run = [&](const std::map<std::string, std::string>& info) {
        ElegooCc2::Session s(printer, SN, quick());
        std::string        error;
        REQUIRE(s.open(error));
        return ElegooCc2::start_print_over(s, info, 1, "cube.gcode", 3, 0);
    };

    SECTION("an idle plain CC2 starts the file as sliced")
    {
        statuses    = { 1 };
        start_codes = { 0 };
        const ElegooCc2::StartResult r = run({ { "heatedBedLeveling", "1" } });
        CHECK(r.ok);
        const auto sent = printer.requests(ElegooCc2::METHOD_START_PRINT);
        REQUIRE(sent.size() == 1);
        CHECK(sent[0]["params"]["filename"] == "cube.gcode");
        CHECK(sent[0]["params"]["config"]["slot_map"].empty());
        CHECK(sent[0]["params"]["config"]["printer_check"] == true);
    }
    SECTION("a Combo start carries the dialog's mapping")
    {
        statuses    = { 1 };
        start_codes = { 0 };
        const ElegooCc2::StartResult r = run({ { "canvasAttached", "1" }, { "slotMap", "0:0:1,1:0:3" } });
        CHECK(r.ok);
        const auto sent = printer.requests(ElegooCc2::METHOD_START_PRINT);
        REQUIRE(sent.size() == 1);
        CHECK(sent[0]["params"]["config"]["slot_map"].size() == 2);
        CHECK(sent[0]["params"]["config"]["slot_map"][1]["tray_id"] == 3);
    }
    SECTION("a printer still taking in the file is asked again")
    {
        statuses    = { 11, 11, 1 };
        start_codes = { 0 };
        CHECK(run({}).ok);
        CHECK(printer.requests(ElegooCc2::METHOD_GET_STATUS).size() == 3);
    }
    SECTION("a finished print on the screen does not block the next one")
    {
        statuses    = { 2 };
        sub_status  = ElegooCc2::SUB_PRINT_COMPLETED;
        start_codes = { 0 };
        CHECK(run({}).ok);
    }
    SECTION("a printing printer is not sent the start")
    {
        statuses = { 2 };
        start_codes = { 0 };
        const ElegooCc2::StartResult r = run({});
        CHECK_FALSE(r.ok);
        CHECK(r.busy);
        CHECK(r.error == "the printer is printing, so the print was not started");
        CHECK(printer.requests(ElegooCc2::METHOD_START_PRINT).empty());
        CHECK(printer.requests(ElegooCc2::METHOD_GET_STATUS).size() == 3);
    }
    SECTION("missing leveling data: one retry with bedlevel_force when leveling was asked for")
    {
        statuses    = { 1 };
        start_codes = { ElegooCc2::ERR_MISSING_BED_LEVELING, 0 };
        CHECK(run({ { "heatedBedLeveling", "1" } }).ok);
        const auto sent = printer.requests(ElegooCc2::METHOD_START_PRINT);
        REQUIRE(sent.size() == 2);
        CHECK(sent[0]["params"]["config"]["bedlevel_force"] == false);
        CHECK(sent[1]["params"]["config"]["bedlevel_force"] == true);
    }
    SECTION("missing leveling data without leveling asked for: no retry")
    {
        statuses    = { 1 };
        start_codes = { ElegooCc2::ERR_MISSING_BED_LEVELING };
        const ElegooCc2::StartResult r = run({});
        CHECK_FALSE(r.ok);
        CHECK(r.error_code == ElegooCc2::ERR_MISSING_BED_LEVELING);
        CHECK(printer.requests(ElegooCc2::METHOD_START_PRINT).size() == 1);
    }
    SECTION("the printer cannot find the file")
    {
        statuses    = { 1 };
        start_codes = { ElegooCc2::ERR_FILE_NOT_FOUND };
        const ElegooCc2::StartResult r = run({});
        CHECK_FALSE(r.ok);
        CHECK(r.error == "the printer cannot find the uploaded file");
    }
}
