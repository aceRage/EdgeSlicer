// The hub's Elegoo SDCP backend (Utils/ElegooSdcp, Utils/ElegooSdcpMonitor): a Centauri Carbon 1 in
// the same printer row a Bambu printer or a U1 fills.
//
// The [ElegooSdcp] cases drive the pure half with frames shaped like the owner's capture (firmware
// V1.4.49, SDCP V3.0.0) - every id, name and colour here is made up. The hidden [.ElegooSdcpE2E]
// cases run the real persistent connection against tests/phone/mock_elegoo_sdcp.py on 127.0.0.1
// (run_elegoo_sdcp_e2e.py starts it and passes the port in ELEGOO_SDCP_MOCK_PORT). No real printer.

#include <catch2/catch.hpp>

#include "slic3r/GUI/RemoteHub.hpp"
#include "slic3r/Utils/ElegooCanvas.hpp"
#include "slic3r/Utils/ElegooSdcp.hpp"
#include "slic3r/Utils/ElegooSdcpMonitor.hpp"
#include "slic3r/Utils/Http.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

using namespace Slic3r;
using nlohmann::json;

namespace {

// The status push of a CC1 mid-print, in the capture's shape.
const char* const PRINTING = R"({
  "Status": {
    "CurrentStatus": [1], "PreviousStatus": 0,
    "TempOfNozzle": 219.6, "TempTargetNozzle": 220, "TempOfHotbed": 60.1, "TempTargetHotbed": 60,
    "TempOfBox": 31.5, "TempTargetBox": 0,
    "CurrenCoord": "120.00,110.00,3.20",
    "CurrentFanSpeed": {"ModelFan": 100, "AuxiliaryFan": 50, "BoxFan": 30},
    "ZOffset": 0.05,
    "LightStatus": {"SecondLight": 1, "RgbLight": [255, 255, 255]},
    "TimeLapseStatus": 0, "AmsConnectStatus": 1,
    "PrintInfo": {"Status": 13, "CurrentLayer": 42, "TotalLayer": 150, "CurrentTicks": 1200, "TotalTicks": 3600,
                  "Progress": 33, "Filename": "Cube_PLA_mock.gcode", "ErrorNumber": 0, "TaskId": "mock-task",
                  "PrintSpeedPct": 100}
  },
  "MainboardID": "MOCKBOARD00000001", "TimeStamp": 1760000000, "Topic": "sdcp/status/MOCKBOARD00000001"
})";

const char* const ATTRIBUTES = R"({
  "Attributes": {"Name": "Mock CC1", "MachineName": "Centauri Carbon", "ProtocolVersion": "V3.0.0",
                 "FirmwareVersion": "V1.4.49", "MainboardID": "MOCKBOARD00000001",
                 "Capabilities": ["FILE_TRANSFER", "PRINT_CONTROL", "VIDEO_STREAM"], "CameraStatus": 1,
                 "MaximumVideoStreamAllowed": 4},
  "MainboardID": "MOCKBOARD00000001", "TimeStamp": 1760000000, "Topic": "sdcp/attributes/MOCKBOARD00000001"
})";

// Cmd 324's payload frame: tray 0 an Elegoo spool, 1 and 2 empty, 3 an untagged spool (brand of dashes)
// in the toolhead.
const char* const CANVAS = R"({"Id":"","Data":{"Cmd":324,"Data":{"Ack":0,"active_canvas_id":0,"active_tray_id":3,"auto_refill":0,
  "canvas_list":[{"canvas_id":0,"connected":1,"tray_list":[
   {"tray_id":0,"brand":"ELEGOO","filament_type":"PLA","filament_name":"ELEGOO PLA","filament_code":"0x0001","filament_color":"#1E7A3C","status":1},
   {"tray_id":1,"brand":"","filament_type":"","filament_name":"","filament_code":"0x00000","filament_color":"","status":0},
   {"tray_id":2,"brand":"","filament_type":"","filament_name":"","filament_code":"0x00000","filament_color":"","status":0},
   {"tray_id":3,"brand":"\u2014\u2014 \u2014\u2014","filament_type":"PLA","filament_name":"PLA PRO","filament_code":"0x00000","filament_color":"#D02A2A","status":2}]}]},
  "RequestID":"abc","MainboardID":"MOCKBOARD00000001","TimeStamp":1760000000},"Topic":"sdcp/response/MOCKBOARD00000001"})";

Sdcp::Status status_of(const std::string& frame)
{
    Sdcp::Status     s;
    Sdcp::Attributes a;
    Sdcp::read_frame(frame, s, a);
    return s;
}

Sdcp::Status with_print(std::vector<int> machine, int print_status)
{
    Sdcp::Status s  = status_of(PRINTING);
    s.current_status = std::move(machine);
    s.print_status   = print_status;
    return s;
}

Sdcp::Snapshot connected_snapshot()
{
    Sdcp::Snapshot snap;
    snap.exists = snap.connected = snap.ever_connected = true;
    Sdcp::read_frame(PRINTING, snap.status, snap.attrs);
    Sdcp::read_frame(ATTRIBUTES, snap.status, snap.attrs);
    std::string err;
    snap.has_canvas = canvas_parse_payload(CANVAS, snap.canvas, err);
    return snap;
}

} // namespace

TEST_CASE("SDCP frames: a status push fills every field the row needs", "[ElegooSdcp]")
{
    Sdcp::Status     s;
    Sdcp::Attributes a;
    const Sdcp::Frame f = Sdcp::read_frame(PRINTING, s, a);
    REQUIRE(f.kind == Sdcp::FrameKind::Status);
    CHECK(f.ams_connect_seen);
    CHECK(s.valid);
    CHECK(s.machine() == 1);
    CHECK(s.print_status == 13);
    CHECK(s.current_layer == 42);
    CHECK(s.total_layer == 150);
    CHECK(s.current_ticks == 1200);
    CHECK(s.total_ticks == 3600);
    CHECK(s.progress == Approx(33));
    CHECK(s.filename == "Cube_PLA_mock.gcode");
    CHECK(s.speed_pct == 100);
    CHECK(s.has_nozzle);
    CHECK(s.nozzle == Approx(219.6));
    CHECK(s.nozzle_target == Approx(220));
    CHECK(s.bed == Approx(60.1));
    CHECK(s.box == Approx(31.5));
    CHECK(s.model_fan == 100);
    CHECK(s.aux_fan == 50);
    CHECK(s.box_fan == 30);
    CHECK(s.second_light == 1);
    CHECK(s.rgb_light.size() == 3);
    CHECK(s.ams_connect == 1);
    CHECK(s.timelapse == 0);
    CHECK(s.has_z_offset);
    CHECK(Sdcp::seconds_left(s) == 2400);
}

TEST_CASE("SDCP frames: a push that carries only what changed keeps the rest", "[ElegooSdcp]")
{
    Sdcp::Status     s = status_of(PRINTING);
    Sdcp::Attributes a;
    Sdcp::read_frame(R"({"Status":{"TempOfNozzle":221.0,"PrintInfo":{"CurrentLayer":43}}})", s, a);
    CHECK(s.nozzle == Approx(221.0));
    CHECK(s.current_layer == 43);
    CHECK(s.total_layer == 150);           // untouched
    CHECK(s.filename == "Cube_PLA_mock.gcode");
    CHECK(s.model_fan == 100);
    CHECK(s.machine() == 1);
}

TEST_CASE("SDCP frames: attributes, pong, responses and the CANVAS payload are told apart", "[ElegooSdcp]")
{
    Sdcp::Status     s;
    Sdcp::Attributes a;
    REQUIRE(Sdcp::read_frame(ATTRIBUTES, s, a).kind == Sdcp::FrameKind::Attributes);
    CHECK(a.firmware == "V1.4.49");
    CHECK(a.machine_name == "Centauri Carbon");
    CHECK(a.camera_status == 1);
    CHECK(a.max_video_streams == 4);
    CHECK(a.capabilities.size() == 3);
    CHECK(a.mainboard_id == "MOCKBOARD00000001");

    CHECK(Sdcp::read_frame("pong", s, a).kind == Sdcp::FrameKind::Pong);
    CHECK(Sdcp::read_frame("not json", s, a).kind == Sdcp::FrameKind::Unknown);

    const Sdcp::Frame ack = Sdcp::read_frame(R"({"Data":{"Cmd":129,"Data":{"Ack":0},"RequestID":"r1"},"Topic":"sdcp/response/x"})", s, a);
    CHECK(ack.kind == Sdcp::FrameKind::Response);
    CHECK(ack.cmd == 129);
    CHECK(ack.ack == 0);
    CHECK(ack.request_id == "r1");
    CHECK_FALSE(ack.has_canvas);

    const Sdcp::Frame cv = Sdcp::read_frame(CANVAS, s, a);
    CHECK(cv.kind == Sdcp::FrameKind::Response);
    CHECK(cv.cmd == 324);
    CHECK(cv.has_canvas);
}

TEST_CASE("SDCP requests: the envelope, with the mainboard id once it is known", "[ElegooSdcp]")
{
    const json before = json::parse(Sdcp::request_text(129, json::object(), "rid", 1760000000000LL, ""));
    CHECK(before["Id"] == "");
    CHECK(before["Data"]["Cmd"] == 129);
    CHECK(before["Data"]["Data"] == json::object());
    CHECK(before["Data"]["RequestID"] == "rid");
    CHECK(before["Data"]["MainboardID"] == "");
    CHECK(before["Data"]["From"] == 1);
    CHECK_FALSE(before.contains("Topic"));
    const json after = json::parse(Sdcp::request_text(403, Sdcp::fan_data("aux", 40), "rid", 1, "BOARD"));
    CHECK(after["Data"]["MainboardID"] == "BOARD");
    CHECK(after["Topic"] == "sdcp/request/BOARD");
    CHECK(after["Data"]["Data"]["TargetFanSpeed"]["AuxiliaryFan"] == 40);
}

TEST_CASE("SDCP controls: Cmd 403 data for the light and each fan", "[ElegooSdcp]")
{
    CHECK(Sdcp::light_data(true) == json::parse(R"({"LightStatus":{"SecondLight":1,"RgbLight":[255,255,255]}})"));
    CHECK(Sdcp::light_data(false) == json::parse(R"({"LightStatus":{"SecondLight":0,"RgbLight":[0,0,0]}})"));
    CHECK(Sdcp::fan_data("part", 70) == json::parse(R"({"TargetFanSpeed":{"ModelFan":70}})"));
    CHECK(Sdcp::fan_data("chamber", 150) == json::parse(R"({"TargetFanSpeed":{"BoxFan":100}})"));
    CHECK(Sdcp::fan_key("cavity").empty());
    CHECK(Sdcp::ack_text(129, 0).empty());
    CHECK_FALSE(Sdcp::ack_text(129, 1).empty());
}

TEST_CASE("SDCP state words: what the hub, the events and the app call each status", "[ElegooSdcp]")
{
    using namespace Sdcp;
    CHECK(hub_state(Status()).empty()); // nothing heard yet
    CHECK(hub_state(with_print({ 1 }, PRINT_PRINTING)) == "printing");
    CHECK(hub_state(with_print({ 1 }, PRINT_PREHEATING)) == "printing");
    CHECK(phase_text(with_print({ 1 }, PRINT_PREHEATING)) == "Preheating");
    CHECK(hub_state(with_print({ 1 }, PRINT_AUTO_LEVELING)) == "printing");
    CHECK(phase_text(with_print({ 1 }, PRINT_AUTO_LEVELING)) == "Auto-leveling");
    CHECK(hub_state(with_print({ 1 }, PRINT_PAUSING)) == "printing");
    CHECK(phase_text(with_print({ 1 }, PRINT_PAUSING)) == "Pausing");
    CHECK(hub_state(with_print({ 1 }, PRINT_PAUSED)) == "paused");
    CHECK(hub_state(with_print({ 1 }, PRINT_STOPPING)) == "printing");
    CHECK(hub_state(with_print({ 1 }, PRINT_STOPPED)) == "cancelled");
    // After the print the sub-status keeps the outcome while the machine is idle (spec).
    CHECK(hub_state(with_print({ 0 }, PRINT_COMPLETE)) == "complete");
    CHECK(hub_state(with_print({ 0 }, PRINT_STOPPED)) == "cancelled");
    CHECK(hub_state(with_print({ 0 }, PRINT_ERROR)) == "error");
    CHECK(hub_state(with_print({ 0 }, PRINT_IDLE)) == "standby");
    // CurrentStatus 8 (file checking, the capture's third value) is not a print.
    CHECK(hub_state(with_print({ 8 }, PRINT_IDLE)) == "standby");
    CHECK(phase_text(with_print({ 8 }, PRINT_IDLE)) == "Checking the file");
    // A file transfer first in the list, a print behind it: still printing.
    CHECK(hub_state(with_print({ 2, 1 }, PRINT_PRINTING)) == "printing");
}

TEST_CASE("SDCP state words: which of pause, resume and stop the printer would take", "[ElegooSdcp]")
{
    using namespace Sdcp;
    const Status printing = with_print({ 1 }, PRINT_PRINTING);
    CHECK(can_pause(printing));
    CHECK_FALSE(can_resume(printing));
    CHECK(can_stop(printing));
    const Status paused = with_print({ 1 }, PRINT_PAUSED);
    CHECK_FALSE(can_pause(paused));
    CHECK(can_resume(paused));
    CHECK(can_stop(paused));
    CHECK(can_pause(with_print({ 1 }, PRINT_PREHEATING)));
    CHECK_FALSE(can_pause(with_print({ 1 }, PRINT_PAUSING)));
    CHECK_FALSE(can_stop(with_print({ 1 }, PRINT_STOPPING)));
    const Status idle = with_print({ 0 }, PRINT_COMPLETE);
    CHECK_FALSE(can_pause(idle));
    CHECK_FALSE(can_resume(idle));
    CHECK_FALSE(can_stop(idle));
    CHECK(is_printing(paused));
    CHECK_FALSE(is_printing(idle));
}

TEST_CASE("SDCP row: a CC1 mid-print fills the row a Bambu printer does", "[ElegooSdcp]")
{
    json row = { { "id", "ph:dev1" }, { "kind", "printhost" }, { "status", "unknown" }, { "can_upload", true } };
    Sdcp::fill_row(connected_snapshot(), "printer.local", row);
    CHECK(row["online"] == true);
    CHECK(row["status"] == "printing");
    CHECK(row["print_status"] == "printing");
    CHECK(row["printing"] == true);
    CHECK(row["can_pause"] == true);
    CHECK(row["can_resume"] == false);
    CHECK(row["can_stop"] == true);
    CHECK(row["can_upload"] == true); // what list_hosts set stays
    CHECK(row["percent"] == 33);
    CHECK(row["left_time_s"] == 2400);
    CHECK(row["layer"] == 42);
    CHECK(row["total_layers"] == 150);
    CHECK(row["task"] == "Cube_PLA_mock.gcode");
    CHECK(row["stage"] == "Cube_PLA_mock.gcode");
    CHECK(row["print_error"].is_null());
    CHECK(row["bed_temp"].get<double>() == Approx(60.1));
    CHECK(row["bed_target"].get<double>() == Approx(60));
    REQUIRE(row["nozzles"].size() == 1);
    CHECK(row["nozzles"][0]["temp"].get<double>() == Approx(219.6));
    CHECK(row["chamber_temp"].get<double>() == Approx(31.5));

    const json& c = row["controls"];
    REQUIRE(c["heaters"].size() == 3);
    CHECK(c["heaters"][0]["id"] == "bed");
    CHECK(c["heaters"][1]["id"] == "nozzle0");
    CHECK(c["heaters"][2]["id"] == "chamber");
    CHECK(c["heaters"][2]["label"] == "Chamber");
    for (const json& h : c["heaters"]) CHECK(h["settable"] == false); // readings only
    CHECK(c["speed"]["kind"] == "factor");
    CHECK(c["speed"]["value"] == 100);
    CHECK(c["speed"]["settable"] == false);
    CHECK(c["light"]["on"] == true);
    REQUIRE(c["fans"].size() == 3);
    CHECK(c["fans"][0]["id"] == "part");
    CHECK(c["fans"][0]["percent"] == 100);
    CHECK(c["fans"][1]["id"] == "aux");
    CHECK(c["fans"][2]["id"] == "chamber");
    CHECK_FALSE(c.contains("filament"));

    // The CANVAS in the Bambu AMS shape: one unit, four trays, the untagged spool named Generic.
    REQUIRE(row["ams"].size() == 1);
    const json& u = row["ams"][0];
    CHECK(u["id"] == "0");
    CHECK(u["name"] == "CANVAS 1");
    CHECK(u["type"] == "canvas");
    REQUIRE(u["trays"].size() == 4);
    CHECK(u["trays"][0]["exists"] == true);
    CHECK(u["trays"][0]["type"] == "PLA");
    CHECK(u["trays"][0]["sub_type"] == "ELEGOO PLA");
    CHECK(u["trays"][0]["color"] == "#1E7A3C");
    CHECK(u["trays"][0]["active"] == false);
    CHECK(u["trays"][1]["exists"] == false);
    CHECK(u["trays"][1]["color"] == "");
    CHECK(u["trays"][3]["id"] == "3");
    CHECK(u["trays"][3]["sub_type"] == "Generic PLA PRO");
    CHECK(u["trays"][3]["active"] == true);
    CHECK_FALSE(u["trays"][0].contains("remain"));   // the printer does not know
    CHECK_FALSE(u["trays"][0].contains("can_load")); // nothing is offered

    CHECK(row["camera_hint"]["available"] == true);
    CHECK(row["camera_hint"]["max_streams"] == 4);
    CHECK(row["camera_hint"]["rkind"] == "elegoo");
    CHECK(row["camera_hint"]["route"] == "/elegoo?id=");
    CHECK(row["camera_hint"]["ip"] == "printer.local");
    CHECK(row["sdcp"]["firmware"] == "V1.4.49");
    CHECK(row["sdcp"]["canvas_attached"] == true);
    CHECK(row["sdcp_connected"] == true);
    // Nothing identifying.
    CHECK(row.dump().find("MOCKBOARD") == std::string::npos);
}

TEST_CASE("SDCP row: paused, preheating, finished and a print error", "[ElegooSdcp]")
{
    Sdcp::Snapshot snap = connected_snapshot();
    snap.status.print_status = Sdcp::PRINT_PAUSED;
    json row = json::object();
    Sdcp::fill_row(snap, "", row);
    CHECK(row["status"] == "paused");
    CHECK(row["can_resume"] == true);
    CHECK(row["can_pause"] == false);
    CHECK_FALSE(row["camera_hint"].contains("ip")); // no host given

    snap.status.print_status = Sdcp::PRINT_PREHEATING;
    row = json::object();
    Sdcp::fill_row(snap, "", row);
    CHECK(row["status"] == "printing");
    CHECK(row["stage"] == "Preheating");
    CHECK(row["task"] == "Cube_PLA_mock.gcode");

    snap.status.current_status = { 0 };
    snap.status.print_status   = Sdcp::PRINT_COMPLETE;
    row = json::object();
    Sdcp::fill_row(snap, "", row);
    CHECK(row["status"] == "complete");
    CHECK(row["printing"] == false);
    CHECK(row["percent"] == 100);
    CHECK(row["left_time_s"] == 0);
    CHECK(row["task"] == "Cube_PLA_mock.gcode");
    CHECK(row["can_stop"] == false);

    snap.status.print_status = Sdcp::PRINT_ERROR;
    snap.status.error_number = 2;
    row = json::object();
    Sdcp::fill_row(snap, "", row);
    CHECK(row["status"] == "error");
    REQUIRE(row["print_error"].is_object());
    CHECK(row["print_error"]["code"] == "2");

    snap.status.print_status = Sdcp::PRINT_IDLE;
    snap.status.ams_connect  = 0; // the CANVAS was unplugged
    row = json::object();
    Sdcp::fill_row(snap, "", row);
    CHECK(row["status"] == "standby");
    CHECK_FALSE(row.contains("task"));
    CHECK_FALSE(row.contains("percent"));
    CHECK_FALSE(row.contains("ams"));
}

TEST_CASE("SDCP row: off, still connecting, and held by another window", "[ElegooSdcp]")
{
    // The very first attempt is still running: the row stays as list_hosts made it ("unknown").
    Sdcp::Snapshot first;
    first.exists = first.connecting = true;
    json row = { { "status", "unknown" }, { "online", true } };
    Sdcp::fill_row(first, "h", row);
    CHECK(row["status"] == "unknown");
    CHECK(row["online"] == true);
    CHECK(row["can_pause"] == false);

    // Off: a clean offline row - no temperatures, no job, no controls, the reason in status_error.
    Sdcp::Snapshot off;
    off.exists   = true;
    off.failures = 2;
    off.error    = "the printer does not answer (connect timed out)";
    off.status   = status_of(PRINTING); // what it said before it went away is not shown
    row          = { { "status", "unknown" }, { "online", true } };
    Sdcp::fill_row(off, "h", row);
    CHECK(row["online"] == false);
    CHECK(row["status"] == "offline");
    CHECK(row["printing"] == false);
    CHECK(row["status_error"] == off.error);
    for (const char* k : { "bed_temp", "nozzles", "controls", "ams", "percent", "task", "print_status" })
        CHECK_FALSE(row.contains(k));

    Sdcp::Snapshot other;
    other.exists       = true;
    other.other_window = true;
    row                = { { "status", "unknown" }, { "online", true } };
    Sdcp::fill_row(other, "h", row);
    CHECK(row["status"] == "unknown");
    CHECK(row["sdcp_connected"] == false);
    CHECK(row.contains("status_error"));
}

TEST_CASE("SDCP row: the hub keeps the window that holds the connection", "[ElegooSdcp]")
{
    using namespace Slic3r::GUI::RemoteHub::Testing;
    const std::string live  = R"({"id":"ph:d","kind":"printhost","sdcp_connected":true,"status":"printing"})";
    const std::string other = R"({"id":"ph:d","kind":"printhost","sdcp_connected":false,"status":"unknown"})";
    CHECK(merge_printer_row(live, 100, true, other, 200).keep_cached);
    CHECK_FALSE(merge_printer_row(live, 100, false, other, 200).keep_cached); // the holder went quiet
    CHECK_FALSE(merge_printer_row(other, 200, true, live, 100).keep_cached);  // the holder's row wins
    CHECK_FALSE(merge_printer_row(live, 100, true, other, 100).keep_cached);  // the same window
}

// ------------------------------------------------------------------- end to end ----

namespace {

int mock_port()
{
    const char* p = std::getenv("ELEGOO_SDCP_MOCK_PORT");
    return p ? std::atoi(p) : 0;
}

json mock(const std::string& path)
{
    std::string body;
    Http::get("http://127.0.0.1:" + std::to_string(mock_port()) + path)
        .timeout_connect(2)
        .timeout_max(5)
        .on_complete([&](std::string b, unsigned) { body = b; })
        .perform_sync();
    try {
        return json::parse(body);
    } catch (...) {
        return json::object();
    }
}

template<class Pred> bool wait_for(Pred pred, int ms)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return pred();
}

const std::string HOST = "127.0.0.1";

} // namespace

TEST_CASE("SDCP monitor: one connection, pushed status, CANVAS while watched, commands", "[.][ElegooSdcpE2E]")
{
    REQUIRE(mock_port() > 0);
    SdcpMonitor::reset_for_tests();
    SdcpMonitor::set_port_for_tests(mock_port());
    mock("/mock/reset");
    mock("/mock/print?state=printing");
    mock("/mock/canvas?attached=1");

    // touch() never waits for the network.
    const auto t0 = std::chrono::steady_clock::now();
    SdcpMonitor::touch(HOST);
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() < 200);

    REQUIRE(wait_for([] { return SdcpMonitor::touch(HOST).connected; }, 5000));
    REQUIRE(wait_for([] { return Sdcp::hub_state(SdcpMonitor::touch(HOST).status) == "printing"; }, 3000));
    REQUIRE(wait_for([] { return SdcpMonitor::touch(HOST).attrs.firmware == "V1.4.49"; }, 3000));
    Sdcp::Snapshot s = SdcpMonitor::touch(HOST);
    CHECK(s.status.ams_connect == 1);
    CHECK_FALSE(s.has_canvas); // nobody watches yet: no Cmd 324

    // The pushes keep coming without asking.
    const long long ticks = s.status.current_ticks;
    CHECK(wait_for([&] { return SdcpMonitor::touch(HOST).status.current_ticks > ticks; }, 4000));

    // Somebody looks: the CANVAS is read.
    SdcpMonitor::note_watching();
    REQUIRE(wait_for([] { return SdcpMonitor::touch(HOST).has_canvas; }, 4000));
    s = SdcpMonitor::touch(HOST);
    REQUIRE(s.canvas.trays.size() == 4);
    CHECK(s.canvas.active_tray_id == 3);

    // The desktop's own CANVAS read goes over the same connection.
    const CanvasQueryResult cq = canvas_query(HOST);
    CHECK(cq.ok);
    CHECK(cq.info.trays.size() == 4);

    // Pause, then resume, over the same connection; the printer's pushes say it happened.
    SdcpMonitor::CommandResult r = SdcpMonitor::command(HOST, Sdcp::CMD_PAUSE, json::object(), 5000);
    CHECK(r.answered);
    CHECK(r.ack == 0);
    CHECK(wait_for([] { return Sdcp::hub_state(SdcpMonitor::touch(HOST).status) == "paused"; }, 4000));
    r = SdcpMonitor::command(HOST, Sdcp::CMD_PAUSE, json::object(), 5000);
    CHECK(r.ack == 1); // already paused: the printer refuses
    r = SdcpMonitor::command(HOST, Sdcp::CMD_RESUME, json::object(), 5000);
    CHECK(r.ack == 0);
    CHECK(wait_for([] { return SdcpMonitor::touch(HOST).status.print_status == Sdcp::PRINT_PRINTING; }, 4000));

    // Light and fan.
    r = SdcpMonitor::command(HOST, Sdcp::CMD_CONTROL, Sdcp::light_data(false), 5000);
    CHECK(r.ack == 0);
    CHECK(wait_for([] { return SdcpMonitor::touch(HOST).status.second_light == 0; }, 3000));
    r = SdcpMonitor::command(HOST, Sdcp::CMD_CONTROL, Sdcp::fan_data("chamber", 60), 5000);
    CHECK(r.ack == 0);
    CHECK(wait_for([] { return SdcpMonitor::touch(HOST).status.box_fan == 60; }, 3000));

    // All of that over exactly one websocket.
    const json st = mock("/mock/state");
    CHECK(st["connections_total"] == 1);
    CHECK(st["max_open"] == 1);

    // Unplugging the CANVAS drops its trays from the snapshot.
    mock("/mock/canvas?attached=0");
    CHECK(wait_for([] { return !SdcpMonitor::touch(HOST).has_canvas; }, 3000));

    SdcpMonitor::reset_for_tests();
}

TEST_CASE("SDCP monitor: a dropped connection comes back, a refused one is reported", "[.][ElegooSdcpE2E]")
{
    REQUIRE(mock_port() > 0);
    SdcpMonitor::reset_for_tests();
    SdcpMonitor::set_port_for_tests(mock_port());
    mock("/mock/reset");
    mock("/mock/print?state=idle");
    REQUIRE(wait_for([] { return SdcpMonitor::touch(HOST).connected; }, 5000));
    CHECK(wait_for([] { return Sdcp::hub_state(SdcpMonitor::touch(HOST).status) == "standby"; }, 3000));

    mock("/mock/drop");
    CHECK(wait_for([] { return !SdcpMonitor::touch(HOST).connected; }, 3000));
    CHECK(wait_for([] { return SdcpMonitor::touch(HOST).connected; }, 8000)); // back after the 2 s retry
    CHECK(mock("/mock/state")["open"] == 1);

    // A printer that declines the handshake (too many clients): an error, no hang, and touch() stays fast.
    mock("/mock/refuse?on=1");
    mock("/mock/drop");
    CHECK(wait_for([] {
        const Sdcp::Snapshot s = SdcpMonitor::touch(HOST);
        return !s.connected && s.failures >= 1 && s.error.find("declined") != std::string::npos;
    }, 8000));
    const auto t0 = std::chrono::steady_clock::now();
    json row = json::object();
    Sdcp::fill_row(SdcpMonitor::touch(HOST), HOST, row);
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() < 200);
    CHECK(row["online"] == false);
    CHECK(row["status"] == "offline");
    // Commands fail at once when it is not connected.
    const SdcpMonitor::CommandResult r = SdcpMonitor::command(HOST, Sdcp::CMD_PAUSE, json::object(), 5000);
    CHECK_FALSE(r.answered);
    CHECK_FALSE(r.sent);
    mock("/mock/refuse?on=0");
    SdcpMonitor::reset_for_tests();
}

TEST_CASE("SDCP monitor: a second EdgeSlicer window leaves the printer to the first", "[.][ElegooSdcpE2E]")
{
    REQUIRE(mock_port() > 0);
    SdcpMonitor::reset_for_tests();
    SdcpMonitor::set_port_for_tests(mock_port());
    mock("/mock/reset");
    // A scratch data dir, so the lock file lands nowhere real.
    const std::string old_dir = data_dir();
    const boost::filesystem::path dir = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("sdcp_lock_%%%%%%%%");
    set_data_dir(dir.string());
    boost::filesystem::create_directories(dir / "hub" / "sdcp");
    // Another window holds the lock for this printer (the lock file is <datadir>/hub/sdcp/<host>[_port].lock).
    const boost::filesystem::path lock_path = dir / "hub" / "sdcp" / ("127_0_0_1_" + std::to_string(mock_port()) + ".lock");
    { boost::nowide::ofstream f(lock_path.string()); }
    {
        boost::interprocess::file_lock other(lock_path.string().c_str());
        REQUIRE(other.try_lock());
        SdcpMonitor::touch(HOST);
        CHECK(wait_for([] { return SdcpMonitor::touch(HOST).other_window; }, 3000));
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        CHECK_FALSE(SdcpMonitor::touch(HOST).connected);
        CHECK(mock("/mock/state")["open"] == 0);
        json row = { { "status", "unknown" } };
        Sdcp::fill_row(SdcpMonitor::touch(HOST), HOST, row);
        CHECK(row["status"] == "unknown");
        CHECK(row["sdcp_connected"] == false);
        other.unlock();
    }
    SdcpMonitor::reset_for_tests();
    // Free again: this window connects.
    CHECK(wait_for([] { return SdcpMonitor::touch(HOST).connected; }, 5000));
    CHECK_FALSE(SdcpMonitor::touch(HOST).other_window);
    SdcpMonitor::reset_for_tests();
    set_data_dir(old_dir);
    boost::system::error_code ec;
    boost::filesystem::remove_all(dir, ec);
}

TEST_CASE("SDCP monitor: a printer that stops answering is dropped, and a dead address costs no caller anything", "[.][ElegooSdcpE2E]")
{
    REQUIRE(mock_port() > 0);
    SdcpMonitor::reset_for_tests();
    SdcpMonitor::set_port_for_tests(mock_port());
    mock("/mock/reset");
    mock("/mock/print?state=idle");
    REQUIRE(wait_for([] { return SdcpMonitor::touch(HOST).connected; }, 5000));
    // Muted: no pushes, no pongs, no answers. A command times out at its own deadline.
    mock("/mock/mute?on=1");
    const auto t0 = std::chrono::steady_clock::now();
    const SdcpMonitor::CommandResult r = SdcpMonitor::command(HOST, Sdcp::CMD_STATUS, json::object(), 1500);
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK_FALSE(r.answered);
    CHECK(took < 3000);
    mock("/mock/mute?on=0");

    // Nothing listens on this port: the attempts fail in the background, quickly, and keep failing
    // quietly; touch() never blocks.
    SdcpMonitor::reset_for_tests();
    SdcpMonitor::set_port_for_tests(1); // nothing on 127.0.0.1:1
    const auto t1 = std::chrono::steady_clock::now();
    SdcpMonitor::touch(HOST);
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1).count() < 200);
    CHECK(wait_for([] { return SdcpMonitor::touch(HOST).failures >= 1; }, 6000));
    CHECK_FALSE(SdcpMonitor::touch(HOST).connected);
    SdcpMonitor::reset_for_tests();
    SdcpMonitor::set_port_for_tests(mock_port());
}
