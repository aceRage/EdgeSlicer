#include <catch2/catch.hpp>

#include "slic3r/Utils/ElegooCanvas.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using namespace Slic3r;

// CANVAS is Elegoo's multi-material unit for the Centauri Carbon 1.
// src/slic3r/Utils/ElegooCanvas.cpp parses what command 324 reports, maps it onto the filament
// families this tree knows, and runs the Cmd 0 -> Cmd 324 conversation over an abstract transport.
// Every case below runs without a printer: the SDCP frames are scripted (MockSdcp), and the ids and
// addresses in them are made up.
//
// Started from upstream OrcaSlicer PR #14885, which ships no tests of its own. Design:
// tests/design_elegoo_canvas_sync_device_page.md.

namespace {

// A scripted SDCP printer: each open() starts the next session, whose frames receive() hands out in
// order and then times out.
struct MockSdcp : SdcpTransport
{
    std::vector<std::vector<std::string>> sessions;
    std::vector<std::string>              sent;
    int                                   opens   = 0;
    bool                                  refuse  = false;
    bool                                  is_open = false;
    size_t                                current = 0;
    size_t                                next    = 0;

    bool open(std::string& error) override
    {
        if (refuse) {
            error = "connect: refused";
            return false;
        }
        current = static_cast<size_t>(opens++);
        next    = 0;
        is_open = true;
        return true;
    }
    bool send(const std::string& text, std::string& error) override
    {
        if (!is_open) {
            error = "not connected";
            return false;
        }
        sent.push_back(text);
        return true;
    }
    bool receive(std::string& text, int, std::string& error) override
    {
        if (!is_open || current >= sessions.size() || next >= sessions[current].size()) {
            error = "receive: timed out";
            return false;
        }
        text = sessions[current][next++];
        return true;
    }
    void close() override { is_open = false; }
};

int sent_cmd(const std::string& request)
{
    return nlohmann::json::parse(request)["Data"]["Cmd"].get<int>();
}

// Made-up identifiers, shaped like the real ones.
const char* FAKE_MAINBOARD = "0123456789abcdef0123456789abcdef";

std::string status_frame(int ams)
{
    return std::string(R"({"Status":{"CurrentStatus":[0],"AmsConnectStatus":)") + std::to_string(ams) +
           R"(,"PrintInfo":{"Status":0,"Filename":"my_benchy.gcode","TaskId":"t-1"}},"MainboardID":")" + FAKE_MAINBOARD +
           R"(","TimeStamp":1,"Topic":"sdcp/status/)" + FAKE_MAINBOARD + R"("})";
}

std::string ack_frame(int cmd, int ack)
{
    return std::string(R"({"Id":"979d4c788a4a78bc777a870f1a02867a","Data":{"Cmd":)") + std::to_string(cmd) +
           R"(,"Data":{"Ack":)" + std::to_string(ack) + R"(},"RequestID":"r","MainboardID":")" + FAKE_MAINBOARD +
           R"(","TimeStamp":1},"Topic":"sdcp/response/)" + FAKE_MAINBOARD + R"("})";
}

// The shape of the public Cmd 324 capture (design doc section 1), with one empty tray.
const std::string CANVAS_PAYLOAD = R"({
    "active_canvas_id": 0, "active_tray_id": 1, "auto_refill": 1,
    "canvas_list": [{
        "canvas_id": 0, "connected": 1,
        "tray_list": [
            {"tray_id": 0, "brand": "ELEGOO", "filament_type": "PETG", "filament_name": "PETG",
             "filament_code": "0x00000", "filament_color": "#000000", "min_nozzle_temp": 220, "max_nozzle_temp": 250, "status": 0},
            {"tray_id": 1, "brand": "ELEGOO", "filament_type": "PLA", "filament_name": "PLA Silk",
             "filament_code": "0x00000", "filament_color": "#44F1FF", "min_nozzle_temp": 190, "max_nozzle_temp": 230, "status": 2},
            {"tray_id": 2, "brand": "", "filament_type": "PLA", "filament_name": "",
             "filament_code": "0x00000", "filament_color": "#FF0000", "min_nozzle_temp": 190, "max_nozzle_temp": 230, "status": 1},
            {"tray_id": 3, "brand": "", "filament_type": "", "filament_name": "",
             "filament_code": "", "filament_color": "", "min_nozzle_temp": 0, "max_nozzle_temp": 0, "status": 0}
        ]}],
    "Ack": 0
})";

} // namespace

SCENARIO("CANVAS colours are normalised to RRGGBBAA", "[ElegooCanvas]")
{
    GIVEN("the shapes a printer may report a colour in")
    {
        // Alpha is appended when the printer omits it.
        CHECK(canvas_normalize_color("#FF8800")   == "FF8800FF");
        CHECK(canvas_normalize_color("0xFF8800")  == "FF8800FF");
        CHECK(canvas_normalize_color("0XFF8800")  == "FF8800FF");
        CHECK(canvas_normalize_color("FF8800")    == "FF8800FF");
        // An explicit alpha is kept as given.
        CHECK(canvas_normalize_color("FF880080")  == "FF880080");
        // Case and surrounding whitespace do not matter.
        CHECK(canvas_normalize_color("  ff8800 ") == "FF8800FF");
    }

    GIVEN("input that is not a colour")
    {
        THEN("the slot gets a transparent placeholder rather than an exception")
        {
            CHECK(canvas_normalize_color("")        == "00000000");
            CHECK(canvas_normalize_color("nope")    == "00000000");
            CHECK(canvas_normalize_color("FF88")    == "00000000");
            CHECK(canvas_normalize_color("FF8800A") == "00000000");
        }
    }
}

SCENARIO("CANVAS material names collapse onto a filament family", "[ElegooCanvas]")
{
    GIVEN("the variants Elegoo sells")
    {
        CHECK(canvas_normalize_filament_type("PLA")      == "PLA");
        CHECK(canvas_normalize_filament_type("PLA Silk") == "PLA");
        CHECK(canvas_normalize_filament_type("PLA-CF")   == "PLA");
        CHECK(canvas_normalize_filament_type("PLA Matte")== "PLA");
        CHECK(canvas_normalize_filament_type("PETG")     == "PETG");
        CHECK(canvas_normalize_filament_type("PETG-CF")  == "PETG");
        CHECK(canvas_normalize_filament_type("ABS")      == "ABS");
        CHECK(canvas_normalize_filament_type("ASA-CF")   == "ASA");
        CHECK(canvas_normalize_filament_type("TPU 95A")  == "TPU");
    }

    GIVEN("names where one family's letters contain another's")
    {
        THEN("the longer name wins")
        {
            // "PET-CF" must not stop at PET and become a different family, and
            // "PC-FR" must not be read as a nylon on the strength of its P.
            CHECK(canvas_normalize_filament_type("PET-CF")  == "PETG");
            CHECK(canvas_normalize_filament_type("PC")      == "PC");
            CHECK(canvas_normalize_filament_type("PC-FR")   == "PC");
            CHECK(canvas_normalize_filament_type("PAHT-CF") == "PA");
            CHECK(canvas_normalize_filament_type("Nylon")   == "PA");
        }
    }

    GIVEN("a material this tree has no family for")
    {
        THEN("the name is passed through upper-cased rather than guessed at")
        {
            CHECK(canvas_normalize_filament_type("pps-cf") == "PPS-CF");
        }
    }
}

SCENARIO("A CANVAS family maps to an OrcaFilamentLibrary preset id", "[ElegooCanvas]")
{
    GIVEN("the families the normaliser produces")
    {
        // These ids exist in resources/profiles/OrcaFilamentLibrary.
        CHECK(canvas_generic_filament_id("PLA")  == "OGFL99");
        CHECK(canvas_generic_filament_id("PETG") == "OGFG99");
        CHECK(canvas_generic_filament_id("ABS")  == "OGFB99");
        CHECK(canvas_generic_filament_id("ASA")  == "OGFB98");
        CHECK(canvas_generic_filament_id("TPU")  == "OGFU99");
    }

    GIVEN("a family with no generic counterpart")
    {
        THEN("the slot is left unmapped instead of pointing at the wrong preset")
        {
            CHECK(canvas_generic_filament_id("PPS-CF").empty());
            CHECK(canvas_generic_filament_id("").empty());
        }
    }
}

SCENARIO("A command-324 payload becomes a slot list", "[ElegooCanvas]")
{
    GIVEN("one connected unit with a loaded and an empty slot")
    {
        const std::string payload = R"({
            "canvas_list": [
                {
                    "connected": 1,
                    "tray_list": [
                        {"tray_id": 0, "status": 1, "filament_type": "PLA Silk",
                         "filament_color": "#FF8800", "max_nozzle_temp": 240},
                        {"tray_id": 1, "status": 0, "filament_type": ""}
                    ]
                }
            ]
        })";

        CanvasInfo info;
        std::string error;
        REQUIRE(canvas_parse_payload(payload, info, error));

        THEN("both slots are reported, only one carrying filament")
        {
            CHECK(info.unit_count == 1);
            REQUIRE(info.trays.size() == 2);

            CHECK(info.trays[0].slot_index == 0);
            CHECK(info.trays[0].has_filament);
            CHECK(info.trays[0].tray_type == "PLA");
            CHECK(info.trays[0].filament_type == "PLA Silk");
            CHECK(info.trays[0].tray_color == "FF8800FF");
            CHECK(info.trays[0].colour_hex() == "#FF8800");
            CHECK(info.trays[0].tray_info_idx == "OGFL99");
            CHECK(info.trays[0].nozzle_temp == 240);

            CHECK(info.trays[1].slot_index == 1);
            CHECK(! info.trays[1].has_filament);
            CHECK(info.trays[1].tray_type.empty());
            CHECK(info.trays[1].colour_hex().empty());
            CHECK(info.loaded_count() == 1);
        }
    }

    GIVEN("a disconnected unit in front of a connected one")
    {
        const std::string payload = R"({
            "canvas_list": [
                {"canvas_id": 0, "connected": 0, "tray_list": [
                    {"tray_id": 0}, {"tray_id": 1}, {"tray_id": 2}, {"tray_id": 3}]},
                {"canvas_id": 1, "connected": 1, "tray_list": [
                    {"tray_id": 0, "status": 1, "filament_type": "PETG",
                     "filament_color": "00FF00"}]}
            ]
        })";

        CanvasInfo info;
        std::string error;
        REQUIRE(canvas_parse_payload(payload, info, error));

        THEN("the absent unit still consumes its slot numbers, and the raw ids are kept")
        {
            // Otherwise unplugging the first unit would renumber every slot
            // behind it and remap the whole print.
            CHECK(info.unit_count == 2);
            REQUIRE(info.units.size() == 2);
            CHECK(! info.units[0].connected);
            CHECK(info.units[0].tray_count == 4);
            CHECK(info.units[1].connected);
            REQUIRE(info.trays.size() == 1);
            CHECK(info.trays[0].slot_index == 4);
            CHECK(info.trays[0].canvas_id == 1);
            CHECK(info.trays[0].tray_id == 0);
            CHECK(info.trays[0].tray_type == "PETG");
            CHECK(info.trays[0].tray_info_idx == "OGFG99");
            CHECK(info.find(1, 0) == &info.trays[0]);
            CHECK(info.find(0, 0) == nullptr);
        }
    }

    GIVEN("the payload wrapped the way the firmware nests it")
    {
        const std::string once = R"({"Data": {"canvas_list": [
            {"connected": 1, "tray_list": [
                {"tray_id": 0, "status": 1, "filament_type": "ABS", "filament_color": "#112233"}]}]}})";
        const std::string twice = R"({"Data": {"Data": {"canvas_list": [
            {"connected": 1, "tray_list": [
                {"tray_id": 0, "status": 1, "filament_type": "ABS", "filament_color": "#112233"}]}]}}})";

        THEN("either nesting is unwrapped")
        {
            for (const std::string &p : {once, twice}) {
                CanvasInfo info;
                std::string error;
                REQUIRE(canvas_parse_payload(p, info, error));
                REQUIRE(info.trays.size() == 1);
                CHECK(info.trays[0].tray_type == "ABS");
            }
        }
    }

    GIVEN("replies that carry no usable slot list")
    {
        THEN("the parse fails with a reason instead of inventing slots")
        {
            CanvasInfo info;
            std::string error;

            CHECK(! canvas_parse_payload("not json", info, error));
            CHECK(! error.empty());

            // the ACK frame the printer sends before the payload
            CHECK(! canvas_parse_payload(R"({"Status": "ok"})", info, error));
            CHECK(! error.empty());

            CHECK(! canvas_parse_payload(R"({"canvas_list": []})", info, error));
            CHECK(! error.empty());
        }
    }
}

SCENARIO("CANVAS tray presence and status semantics", "[ElegooCanvas]")
{
    CanvasInfo  info;
    std::string error;
    REQUIRE(canvas_parse_payload(CANVAS_PAYLOAD, info, error));
    REQUIRE(info.trays.size() == 4);

    GIVEN("a tray with a material and a colour but status 0, as in the public capture")
    {
        THEN("it is loaded: presence is the type and the colour, the status is kept as data")
        {
            CHECK(info.trays[0].has_filament);
            CHECK(info.trays[0].status == 0);
            CHECK(info.trays[0].state() == CanvasTrayState::Empty);
            CHECK(info.trays[0].colour_hex() == "#000000");
        }
    }

    GIVEN("the status numbers")
    {
        CHECK(canvas_tray_state(-1) == CanvasTrayState::Unknown);
        CHECK(canvas_tray_state(0) == CanvasTrayState::Empty);
        CHECK(canvas_tray_state(1) == CanvasTrayState::Preloaded);
        CHECK(canvas_tray_state(2) == CanvasTrayState::Loaded);
        CHECK(canvas_tray_state(5) == CanvasTrayState::Loaded);
        CHECK(info.trays[1].state() == CanvasTrayState::Loaded);
        CHECK(info.trays[2].state() == CanvasTrayState::Preloaded);
    }

    GIVEN("a tray with no material")
    {
        CHECK(! info.trays[3].has_filament);
        CHECK(info.loaded_count() == 3);
    }

    GIVEN("a tray that names a material but no colour")
    {
        CanvasInfo  i2;
        REQUIRE(canvas_parse_payload(R"({"canvas_list":[{"connected":1,"tray_list":[
            {"tray_id":0,"filament_type":"PLA","filament_color":"","status":2}]}]})", i2, error));
        REQUIRE(i2.trays.size() == 1);
        CHECK(! i2.trays[0].has_filament);
    }

    GIVEN("the top-level fields")
    {
        CHECK(info.active_canvas_id == 0);
        CHECK(info.active_tray_id == 1);
        CHECK(info.auto_refill == 1);
        CHECK(info.trays[1].min_nozzle_temp == 190);
        CHECK(info.trays[1].nozzle_temp == 230);
        CHECK(info.trays[1].filament_code == "0x00000");
    }

    GIVEN("tray ids counted from 1, as one elegoo-link comment says some firmware does")
    {
        CanvasInfo i2;
        REQUIRE(canvas_parse_payload(R"({"canvas_list":[{"canvas_id":0,"connected":1,"tray_list":[
            {"tray_id":1,"filament_type":"PLA","filament_color":"#FFFFFF"},
            {"tray_id":2,"filament_type":"PLA","filament_color":"#000000"}]}]})", i2, error));
        THEN("the raw tray_id is kept for the printer, the slot numbers stay 0-based by position")
        {
            REQUIRE(i2.trays.size() == 2);
            CHECK(i2.trays[0].tray_id == 1);
            CHECK(i2.trays[0].position == 0);
            CHECK(i2.trays[0].slot_index == 0);
            CHECK(i2.trays[1].tray_id == 2);
            CHECK(i2.trays[1].slot_index == 1);
        }
    }

    GIVEN("numbers sent as strings")
    {
        CanvasInfo i2;
        REQUIRE(canvas_parse_payload(R"({"active_tray_id":"2","canvas_list":[{"canvas_id":"0","connected":"1","tray_list":[
            {"tray_id":"3","filament_type":"PETG","filament_color":"#00FF00","status":"1"}]}]})", i2, error));
        REQUIRE(i2.trays.size() == 1);
        CHECK(i2.active_tray_id == 2);
        CHECK(i2.trays[0].tray_id == 3);
        CHECK(i2.trays[0].status == 1);
    }
}

SCENARIO("A CANVAS tray's name and vendor for the filament sync", "[ElegooCanvas]")
{
    CanvasInfo  info;
    std::string error;
    REQUIRE(canvas_parse_payload(CANVAS_PAYLOAD, info, error));

    // "ELEGOO PLA Silk" finds "Elegoo PLA Silk @ECC" by its base name (case-blind).
    CHECK(canvas_tray_name(info.trays[1]) == "ELEGOO PLA Silk");
    CHECK(canvas_tray_vendor(info.trays[1]) == "ELEGOO");
    // The brand is not doubled when the name already carries it.
    CanvasTray t = info.trays[1];
    t.filament_name = "Elegoo PLA+";
    CHECK(canvas_tray_name(t) == "Elegoo PLA+");
    // No brand: a hand-entered or untagged spool is the generic preset of its family.
    CHECK(canvas_tray_name(info.trays[2]) == "Generic PLA");
    CHECK(canvas_tray_vendor(info.trays[2]) == "Generic");
    // No name: the brand and the reported material.
    t.filament_name.clear();
    t.filament_type = "PETG";
    t.tray_type     = "PETG";
    CHECK(canvas_tray_name(t) == "ELEGOO PETG");
    // An empty tray has no name.
    CHECK(canvas_tray_name(info.trays[3]).empty());
}

SCENARIO("Which printers read a CANVAS, and where", "[ElegooCanvas]")
{
    GIVEN("printer models")
    {
        CHECK(canvas_capable_model("Elegoo Centauri Carbon"));
        CHECK(canvas_capable_model(" elegoo centauri carbon "));
        // CC2 speaks MQTT, not SDCP (design D10); the Centauri without Carbon is unverified.
        CHECK(! canvas_capable_model("Elegoo Centauri Carbon 2"));
        CHECK(! canvas_capable_model("Elegoo Centauri"));
        CHECK(! canvas_capable_model("Snapmaker U1"));
        CHECK(! canvas_capable_model(""));
    }

    GIVEN("print_host values")
    {
        CHECK(sdcp_host_of("192.168.1.20") == "192.168.1.20");
        CHECK(sdcp_host_of("http://192.168.1.20") == "192.168.1.20");
        CHECK(sdcp_host_of("http://192.168.1.20:80/") == "192.168.1.20");
        CHECK(sdcp_host_of(" cc1.local:3030/websocket ") == "cc1.local");
        CHECK(sdcp_host_of("https://user@printer.lan/x?y") == "printer.lan");
        CHECK(sdcp_host_of("[fe80::1]:3030") == "fe80::1");
        CHECK(sdcp_host_of("fe80::1") == "fe80::1");
        CHECK(sdcp_host_of("").empty());
    }
}

SCENARIO("SDCP requests and the CANVAS gate", "[ElegooCanvas]")
{
    GIVEN("a request")
    {
        const nlohmann::json j = nlohmann::json::parse(sdcp_request(324, "abc", 1234));
        CHECK(j["Id"] == "");
        CHECK(j["Data"]["Cmd"] == 324);
        CHECK(j["Data"]["Data"].is_object());
        CHECK(j["Data"]["Data"].empty());
        CHECK(j["Data"]["RequestID"] == "abc");
        CHECK(j["Data"]["MainboardID"] == "");
        CHECK(j["Data"]["TimeStamp"] == 1234);
        CHECK(j["Data"]["From"] == 1);
    }

    GIVEN("status frames")
    {
        CHECK(sdcp_ams_connect_status(status_frame(1)) == 1);
        CHECK(sdcp_ams_connect_status(status_frame(0)) == 0);
        CHECK(sdcp_ams_connect_status(ack_frame(0, 0)) == -1);
        CHECK(sdcp_ams_connect_status(R"({"Data":{"Status":{"AmsConnectStatus":"1"}}})") == 1);
        CHECK(sdcp_ams_connect_status("garbage") == -1);
    }
}

SCENARIO("The CANVAS query over a scripted printer", "[ElegooCanvas]")
{
    GIVEN("a CC1 with a CANVAS attached")
    {
        MockSdcp printer;
        printer.sessions = { { ack_frame(0, 0), status_frame(1) }, { ack_frame(324, 0), CANVAS_PAYLOAD } };
        const CanvasQueryResult r = canvas_query_over(printer);
        THEN("Cmd 0 then Cmd 324 on two short connections, and the trays come back")
        {
            CHECK(r.ok);
            CHECK(r.reached);
            CHECK(r.ams_connect_status == 1);
            CHECK(! r.no_canvas);
            CHECK(r.info.trays.size() == 4);
            CHECK(printer.opens == 2);
            REQUIRE(printer.sent.size() == 2);
            CHECK(sent_cmd(printer.sent[0]) == 0);
            CHECK(sent_cmd(printer.sent[1]) == 324);
            CHECK(! printer.is_open);
        }
    }

    GIVEN("a CC1 that reports no CANVAS")
    {
        MockSdcp printer;
        printer.sessions = { { status_frame(0) } };
        const CanvasQueryResult r = canvas_query_over(printer);
        THEN("Cmd 324 is not asked")
        {
            CHECK(! r.ok);
            CHECK(r.no_canvas);
            CHECK(r.ams_connect_status == 0);
            CHECK(printer.opens == 1);
            CHECK(printer.sent.size() == 1);
        }
    }

    GIVEN("a status reply without AmsConnectStatus (unknown firmware)")
    {
        MockSdcp printer;
        printer.sessions = { { ack_frame(0, 0) }, { CANVAS_PAYLOAD } };
        const CanvasQueryResult r = canvas_query_over(printer);
        THEN("the CANVAS is asked anyway: no firmware gate, a failure would only mean no data")
        {
            CHECK(r.ams_connect_status == -1);
            CHECK(r.ok);
            CHECK(printer.sent.size() == 2);
        }
    }

    GIVEN("a printer that is off")
    {
        MockSdcp printer;
        printer.refuse = true;
        const CanvasQueryResult r = canvas_query_over(printer);
        CHECK(! r.ok);
        CHECK(! r.reached);
        CHECK(! r.error.empty());
        CHECK(printer.sent.empty());
    }

    GIVEN("a printer that refuses Cmd 324")
    {
        MockSdcp printer;
        printer.sessions = { { status_frame(1) }, { ack_frame(324, 3) } };
        const CanvasQueryResult r = canvas_query_over(printer);
        CHECK(! r.ok);
        CHECK(r.error.find("Ack 3") != std::string::npos);
    }

    GIVEN("a printer that never sends the payload")
    {
        MockSdcp printer;
        printer.sessions = { { status_frame(1) }, { ack_frame(324, 0) } };
        const CanvasQueryResult r = canvas_query_over(printer);
        CHECK(! r.ok);
        CHECK(r.reached);
        CHECK(! r.error.empty());
        CHECK(r.info.trays.empty());
    }
}

SCENARIO("The Capture Canvas data diagnostic is redacted", "[ElegooCanvas]")
{
    const std::string attributes = std::string(R"({"Attributes":{"Name":"Workshop printer","MachineName":"Centauri Carbon",)") +
                                   R"("BrandName":"ELEGOO","FirmwareVersion":"V1.1.40.2","MainboardIP":"192.168.0.50",)" +
                                   R"("MainboardMAC":"AA:BB:CC:DD:EE:FF","MainboardID":")" + FAKE_MAINBOARD +
                                   R"(","SN":"F01SERIAL0001"},"MainboardID":")" + FAKE_MAINBOARD +
                                   R"(","TimeStamp":1,"Topic":"sdcp/attributes/)" + FAKE_MAINBOARD + R"("})";
    MockSdcp printer;
    printer.sessions = { { ack_frame(0, 0), status_frame(1) },
                         { ack_frame(1, 0), attributes },
                         { ack_frame(324, 0), CANVAS_PAYLOAD } };
    const std::string text = canvas_capture_over(printer, R"({"app":"EdgeSlicer test","printer_model":"Elegoo Centauri Carbon"})");
    const nlohmann::json doc = nlohmann::json::parse(text);

    THEN("nothing identifying the printer is in it")
    {
        CHECK(text.find(FAKE_MAINBOARD) == std::string::npos);
        CHECK(text.find("192.168.0.50") == std::string::npos);
        CHECK(text.find("AA:BB:CC:DD:EE:FF") == std::string::npos);
        CHECK(text.find("F01SERIAL0001") == std::string::npos);
        CHECK(text.find("Workshop printer") == std::string::npos);
        CHECK(text.find("my_benchy") == std::string::npos);
        CHECK(text.find("979d4c788a4a78bc777a870f1a02867a") == std::string::npos);
    }

    THEN("what the owner's capture is for is kept")
    {
        CHECK(doc["ams_connect_status"] == 1);
        CHECK(doc["firmware_version"] == "V1.1.40.2");
        CHECK(text.find("Centauri Carbon") != std::string::npos);
        REQUIRE(doc["steps"].size() == 3);
        CHECK(doc["steps"][0]["cmd"] == 0);
        CHECK(doc["steps"][1]["cmd"] == 1);
        CHECK(doc["steps"][2]["cmd"] == 324);
        CHECK(doc["steps"][2]["frames"].size() == 2);
        CHECK(doc["steps"][2]["frames"][1]["canvas_list"][0]["tray_list"][1]["filament_color"] == "#44F1FF");
        CHECK(doc["steps"][2]["frames"][1]["canvas_list"][0]["tray_list"][1]["filament_code"] == "0x00000");
        REQUIRE(doc.contains("parsed"));
        CHECK(doc["parsed"]["trays"].size() == 4);
        CHECK(doc["parsed"]["trays"][0]["state"] == "empty");
        CHECK(doc["parsed"]["trays"][0]["has_filament"] == true);
        CHECK(doc["context"]["printer_model"] == "Elegoo Centauri Carbon");
        CHECK(printer.opens == 3);
    }
}

SCENARIO("Frame redaction", "[ElegooCanvas]")
{
    CHECK(canvas_redact_frame("not json") == "<non-JSON frame, 8 bytes>");
    const std::string r = canvas_redact_frame(R"({"msg":"from 10.0.0.7 mac 01-23-45-67-89-ab id 0123456789ABCDEF01","filament_name":"PLA"})");
    CHECK(r.find("10.0.0.7") == std::string::npos);
    CHECK(r.find("01-23-45-67-89-ab") == std::string::npos);
    CHECK(r.find("0123456789ABCDEF01") == std::string::npos);
    CHECK(r.find("\"filament_name\":\"PLA\"") != std::string::npos);
}
