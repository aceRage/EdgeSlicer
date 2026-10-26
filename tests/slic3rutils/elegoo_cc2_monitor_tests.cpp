#include <catch2/catch.hpp>

#include "slic3r/Utils/ElegooCc2.hpp"
#include "slic3r/Utils/ElegooCc2Monitor.hpp"
#include "slic3r/Utils/ElegooCc2Status.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace Slic3r;
using json = nlohmann::json;

// The Centauri Carbon 2's status for the hub (src/slic3r/Utils/ElegooCc2Status.hpp: the whole 1002
// answer, the 6000 pushes merged into it, the row every printer card reads) and the hub's connection
// to it (ElegooCc2Monitor.hpp: on demand, check-ins, heartbeat, controls), against a scripted printer.
// The serial number and the ids are made up.

namespace {

const char* MON_SN = "TESTSN0002";

json whole_status(int status, int sub, int progress = 0)
{
    return json::parse(R"({
        "machine_status": {"status": )" + std::to_string(status) + R"(, "sub_status": )" + std::to_string(sub) +
                       R"(, "progress": )" + std::to_string(progress) + R"(, "exception_status": []},
        "print_status": {"filename": "benchy.gcode", "current_layer": 12, "total_layer": 120, "print_duration": 300,
                         "total_duration": 3600, "remaining_time_sec": 3300},
        "extruder": {"temperature": 219.6, "target": 220},
        "heater_bed": {"temperature": 59.8, "target": 60},
        "ztemperature_sensor": {"temperature": 31.5, "measured_max_temperature": 33, "measured_min_temperature": 22},
        "fans": {"fan": {"speed": 100, "rpm": 6000}, "aux_fan": {"speed": 40, "rpm": 2000}, "box_fan": {"speed": 20, "rpm": 900},
                 "heater_fan": {"speed": 100, "rpm": 7000}},
        "led": {"status": 1}
    })");
}

std::string push_msg(int id, const json& delta) { return json { { "id", id }, { "method", 6000 }, { "result", delta } }.dump(); }

ElegooCc2::Status status_of(int status, int sub, int progress = 0)
{
    ElegooCc2::Status s;
    ElegooCc2::merge_full(s, whole_status(status, sub, progress));
    return s;
}

} // namespace

TEST_CASE("CC2 status: the whole answer and the pushes", "[ElegooCc2Monitor]")
{
    ElegooCc2::Status s;
    SECTION("a push before the whole status is not merged, and asks for it")
    {
        CHECK(ElegooCc2::merge_push(s, push_msg(5, json { { "machine_status", { { "progress", 9 } } } })) == ElegooCc2::PushResult::NeedFull);
        CHECK_FALSE(s.valid);
        CHECK(ElegooCc2::hub_state(s).empty());
    }
    SECTION("pushes merge key by key, objects recursively")
    {
        ElegooCc2::merge_full(s, whole_status(2, 2075, 10));
        CHECK(ElegooCc2::merge_push(s, push_msg(7, json { { "machine_status", { { "progress", 11 } } },
                                                      { "extruder", { { "temperature", 221.0 } } } })) == ElegooCc2::PushResult::Merged);
        CHECK(ElegooCc2::progress(s) == Approx(11));
        CHECK(s.machine() == 2);
        CHECK(s.sub() == 2075);                                  // kept from the whole status
        CHECK(s.raw["extruder"]["target"] == 220);               // kept: only temperature was pushed
        CHECK(s.raw["extruder"]["temperature"] == Approx(221.0));
        CHECK(ElegooCc2::filename(s) == "benchy.gcode");
    }
    SECTION("not a status push: ignored")
    {
        ElegooCc2::merge_full(s, whole_status(1, 0));
        CHECK(ElegooCc2::merge_push(s, R"({"id":3,"method":1002,"result":{}})") == ElegooCc2::PushResult::Ignored);
        CHECK(ElegooCc2::merge_push(s, "garbage") == ElegooCc2::PushResult::Ignored);
    }
    SECTION("five gaps in a row ask for the whole status again; a continuous id resets the count")
    {
        ElegooCc2::merge_full(s, whole_status(2, 2075));
        CHECK(ElegooCc2::merge_push(s, push_msg(10, json::object())) == ElegooCc2::PushResult::Merged);
        CHECK(ElegooCc2::merge_push(s, push_msg(12, json::object())) == ElegooCc2::PushResult::Merged); // gap 1
        CHECK(ElegooCc2::merge_push(s, push_msg(13, json::object())) == ElegooCc2::PushResult::Merged); // continuous: 0
        int id = 13;
        for (int i = 0; i < 4; ++i) {
            id += 2;
            CHECK(ElegooCc2::merge_push(s, push_msg(id, json::object())) == ElegooCc2::PushResult::Merged);
        }
        id += 2;
        CHECK(ElegooCc2::merge_push(s, push_msg(id, json::object())) == ElegooCc2::PushResult::NeedFull);
        CHECK(s.valid); // still shown while the whole status is asked for
    }
    SECTION("exception_code is replaced, never merged")
    {
        json w = whole_status(2, 2075);
        w["exception"] = { { "exception_code", { { "101", 1 }, { "102", 1 } } } };
        ElegooCc2::merge_full(s, w);
        ElegooCc2::merge_push(s, push_msg(1, json { { "exception", { { "exception_code", { { "103", 1 } } } } } }));
        CHECK(s.raw["exception"]["exception_code"].size() == 1);
        CHECK(s.raw["exception"]["exception_code"].contains("103"));
    }
}

TEST_CASE("CC2 status: states, steps and buttons", "[ElegooCc2Monitor]")
{
    using namespace ElegooCc2;
    struct Case { int status, sub; const char* state; const char* phase; bool pause, resume, stop; };
    const Case cases[] = {
        { 1, 0, "standby", "", false, false, false },
        { 2, 2075, "printing", "", true, false, true },
        { 2, 0, "printing", "", true, false, true },
        { 2, 1045, "printing", "Preheating the nozzle", true, false, true },
        { 2, 1405, "printing", "Preheating the bed", true, false, true },
        { 2, 2901, "printing", "Auto-leveling", true, false, true },
        { 2, 2801, "printing", "Homing", true, false, true },
        { 2, 2501, "printing", "Pausing", false, false, true },
        { 2, 2502, "paused", "", false, true, true },
        { 2, 2505, "paused", "", false, true, true },
        { 2, 2401, "printing", "Resuming", false, false, true },
        { 2, 2503, "printing", "Stopping", false, false, false },
        { 2, 2504, "cancelled", "", false, false, false },
        { 2, 2077, "complete", "", false, false, false },
        { 14, 0, "error", "Emergency stop", false, false, false },
        { 11, 3000, "standby", "Receiving a file", false, false, false },
        { 5, 2901, "standby", "Auto-leveling", false, false, false },
    };
    for (const Case& c : cases) {
        INFO("status " << c.status << " / " << c.sub);
        const Status s = status_of(c.status, c.sub);
        CHECK(hub_state(s) == c.state);
        CHECK(phase_text(s) == c.phase);
        CHECK(can_pause(s) == c.pause);
        CHECK(can_resume(s) == c.resume);
        CHECK(can_stop(s) == c.stop);
    }
}

TEST_CASE("CC2 row: what the hub, the Printers tab and the device page read", "[ElegooCc2Monitor]")
{
    ElegooCc2::Snapshot snap;
    snap.exists       = true;
    snap.connected    = true;
    snap.status       = status_of(2, 2075, 42);
    snap.status_at_ms = 1000000;
    snap.now_ms       = 1005000;
    snap.firmware     = "1.0.0.37";
    json row          = { { "id", "ph:cc2" }, { "kind", "printhost" } };

    SECTION("a printing plain CC2")
    {
        snap.canvas_known = true; // it said: no CANVAS
        ElegooCc2::fill_row(snap, row);
        CHECK(row["status"] == "printing");
        CHECK(row["print_status"] == "printing");
        CHECK(row["online"] == true);
        CHECK(row["printing"] == true);
        CHECK(row["percent"] == 42);
        CHECK(row["layer"] == 12);
        CHECK(row["total_layers"] == 120);
        CHECK(row["left_time_s"] == 3300);
        CHECK(row["task"] == "benchy.gcode");
        CHECK(row["stage"] == "benchy.gcode");
        CHECK(row["can_pause"] == true);
        CHECK(row["can_resume"] == false);
        CHECK(row["can_stop"] == true);
        CHECK(row["bed_temp"] == Approx(59.8));
        CHECK(row["bed_target"] == Approx(60));
        CHECK(row["chamber_temp"] == Approx(31.5));
        CHECK(row["nozzles"][0]["temp"] == Approx(219.6));
        CHECK(row["nozzles"][0]["target"] == Approx(220));
        // The controls: readings only (elegoo-link enables no CC2 set method), the light and three fans.
        const json& c = row["controls"];
        REQUIRE(c["heaters"].size() == 3);
        for (const json& h : c["heaters"]) CHECK(h["settable"] == false);
        CHECK(c["light"]["on"] == true);
        REQUIRE(c["fans"].size() == 3);
        CHECK(c["fans"][0]["id"] == "part");
        CHECK(c["fans"][0]["percent"] == 100);
        CHECK(c["fans"][2]["id"] == "chamber");
        CHECK_FALSE(row.contains("ams"));
        CHECK(row["cc2"]["protocol"] == "mqtt");
        CHECK(row["cc2"]["live"] == true);
        CHECK(row["cc2"]["age_s"] == 5);
        CHECK(row["cc2"]["canvas_attached"] == false);
        CHECK(row["cc2"]["firmware"] == "1.0.0.37");
        CHECK(row["cc2_connected"] == true);
        CHECK(row["print_error"].is_null());
    }
    SECTION("a Combo: its CANVAS in the shared ams shape")
    {
        snap.canvas_known = true;
        snap.has_canvas   = true;
        std::string why;
        REQUIRE(canvas_parse_payload(R"({"active_canvas_id":0,"active_tray_id":1,"auto_refill":true,"canvas_list":[{"canvas_id":0,"connected":1,
            "tray_list":[{"tray_id":0,"brand":"ELEGOO","filament_type":"PLA","filament_name":"PLA","filament_color":"#FF0000","status":1},
                         {"tray_id":1,"brand":"ELEGOO","filament_type":"PETG","filament_name":"PETG","filament_color":"#000000","status":2},
                         {"tray_id":2,"brand":"","filament_type":"","filament_name":"","filament_color":"","status":0},
                         {"tray_id":3,"brand":"","filament_type":"","filament_name":"","filament_color":"","status":0}]}]})",
                                     snap.canvas, why));
        ElegooCc2::fill_row(snap, row);
        REQUIRE(row.contains("ams"));
        REQUIRE(row["ams"].size() == 1);
        CHECK(row["ams"][0]["type"] == "canvas");
        REQUIRE(row["ams"][0]["trays"].size() == 4);
        CHECK(row["ams"][0]["trays"][1]["type"] == "PETG");
        CHECK(row["ams"][0]["trays"][1]["active"] == true);
        CHECK(row["ams"][0]["trays"][2]["exists"] == false);
        CHECK(row["cc2"]["canvas_attached"] == true);
        CHECK(row["cc2"]["auto_refill"] == true);
    }
    SECTION("paused, finished, cancelled")
    {
        snap.status = status_of(2, 2502, 50);
        ElegooCc2::fill_row(snap, row);
        CHECK(row["status"] == "paused");
        CHECK(row["can_resume"] == true);
        CHECK(row["can_pause"] == false);
        snap.status = status_of(2, 2077, 99);
        json done   = { { "id", "ph:cc2" } };
        ElegooCc2::fill_row(snap, done);
        CHECK(done["status"] == "complete");
        CHECK(done["percent"] == 100);
        CHECK(done["left_time_s"] == 0);
        CHECK(done["task"] == "benchy.gcode");
        snap.status = status_of(2, 2504, 30);
        json stopped = { { "id", "ph:cc2" } };
        ElegooCc2::fill_row(snap, stopped);
        CHECK(stopped["status"] == "cancelled");
        CHECK(stopped["printing"] == false);
    }
    SECTION("an emergency stop is an error with the printer's exception code")
    {
        json w = whole_status(14, 0);
        w["machine_status"]["exception_status"] = json::array({ 812 });
        ElegooCc2::merge_full(snap.status, w);
        ElegooCc2::fill_row(snap, row);
        CHECK(row["status"] == "error");
        CHECK(row["print_error"]["code"] == "812");
    }
    SECTION("a status that stopped coming: shown, without buttons")
    {
        snap.now_ms = snap.status_at_ms + 200000;
        ElegooCc2::fill_row(snap, row, 120);
        CHECK(row["status"] == "printing");
        CHECK(row["can_pause"] == false);
        CHECK(row["can_stop"] == false);
        CHECK(row["cc2"]["stale"] == true);
    }
    SECTION("closed on purpose (nobody looking): the last status, no buttons")
    {
        snap.connected = false;
        ElegooCc2::fill_row(snap, row);
        CHECK(row["status"] == "printing");
        CHECK(row["online"] == true);
        CHECK(row["can_pause"] == false);
        CHECK(row["cc2"]["live"] == false);
    }
    SECTION("off: offline with the reason")
    {
        snap.connected = false;
        snap.failures  = 2;
        snap.error     = "the printer does not answer";
        ElegooCc2::fill_row(snap, row);
        CHECK(row["status"] == "offline");
        CHECK(row["online"] == false);
        CHECK(row["status_error"] == "the printer does not answer");
    }
    SECTION("another window holds it")
    {
        snap.connected    = false;
        snap.other_window = true;
        ElegooCc2::fill_row(snap, row);
        CHECK(row["status_error"] == "another EdgeSlicer window is connected to this printer");
        CHECK_FALSE(row.contains("status"));
    }
    SECTION("never heard: nothing but the button defaults")
    {
        ElegooCc2::Snapshot none;
        json                r = { { "id", "ph:cc2" } };
        ElegooCc2::fill_row(none, r);
        CHECK_FALSE(r.contains("status"));
        CHECK(r["can_pause"] == false);
    }
}

// ---------------------------------------------------------------- the monitor ----

namespace {

// A scripted CC2 shared between the test and the monitor's worker thread.
struct FakePrinter
{
    std::mutex                                       m;
    std::condition_variable                          cv;
    std::deque<std::pair<std::string, std::string>>  inbox;
    std::vector<std::pair<std::string, std::string>> published;
    std::string                                      client_id;
    bool                                             connected = false;
    int                                              connects  = 0;
    int                                              closes    = 0;
    int                                              status = 1, sub = 0, progress = 0;
    bool                                             answer_pause = true;
    std::atomic<bool>                                reachable { true };

    void deliver(const std::string& topic, const std::string& payload)
    {
        {
            std::lock_guard<std::mutex> lock(m);
            inbox.emplace_back(topic, payload);
        }
        cv.notify_all();
    }
    int count(const std::function<bool(const std::string&, const std::string&)>& pred)
    {
        std::lock_guard<std::mutex> lock(m);
        int                         n = 0;
        for (const auto& p : published) n += pred(p.first, p.second) ? 1 : 0;
        return n;
    }
    int requests(int method)
    {
        return count([&](const std::string& t, const std::string& p) {
            const json j = json::parse(p, nullptr, false);
            return t.find("/api_request") != std::string::npos && j.is_object() && j.value("method", -1) == method;
        });
    }
};

struct FakeTransport : ElegooCc2::MqttTransport
{
    std::shared_ptr<FakePrinter> p;
    explicit FakeTransport(std::shared_ptr<FakePrinter> printer) : p(std::move(printer)) {}

    bool connect(const std::string& id, std::string& error) override
    {
        std::lock_guard<std::mutex> lock(p->m);
        if (!p->reachable) {
            error = "could not connect to the printer's MQTT port";
            return false;
        }
        p->client_id = id;
        p->connected = true;
        ++p->connects;
        return true;
    }
    bool subscribe(const std::string&, std::string&) override { return true; }
    bool publish(const std::string& topic, const std::string& payload, std::string&) override
    {
        std::string cid;
        int         status, sub, progress;
        bool        answer_pause;
        {
            std::lock_guard<std::mutex> lock(p->m);
            p->published.emplace_back(topic, payload);
            cid          = p->client_id;
            status       = p->status;
            sub          = p->sub;
            progress     = p->progress;
            answer_pause = p->answer_pause;
        }
        const json j = json::parse(payload, nullptr, false);
        if (topic == ElegooCc2::register_topic(MON_SN)) {
            p->deliver(ElegooCc2::register_response_topic(MON_SN, j["request_id"].get<std::string>()),
                       json { { "client_id", j["client_id"] }, { "error", "ok" } }.dump());
            return true;
        }
        if (topic != ElegooCc2::request_topic(MON_SN, cid) || !j.is_object()) return true;
        if (j.value("type", "") == "PING") {
            p->deliver(ElegooCc2::response_topic(MON_SN, cid), R"({"type":"PONG"})");
            return true;
        }
        const int method = j.value("method", -1);
        json      result = { { "error_code", 0 } };
        if (method == ElegooCc2::METHOD_GET_STATUS) {
            result = whole_status(status, sub, progress);
            result["error_code"] = 0;
        } else if (method == ElegooCc2::METHOD_GET_ATTRIBUTES) {
            result["software_version"] = { { "ota_version", "9.9.9" } };
        } else if (method == ElegooCc2::METHOD_GET_CANVAS) {
            result["canvas_info"] = json::parse(R"({"active_canvas_id":-1,"active_tray_id":-1,"auto_refill":false,"canvas_list":[]})");
        } else if (method == ElegooCc2::METHOD_PAUSE_PRINT) {
            if (!answer_pause) return true;
            {
                std::lock_guard<std::mutex> lock(p->m);
                p->sub = 2502;
            }
            p->deliver(ElegooCc2::status_topic(MON_SN), push_msg(1000, json { { "machine_status", { { "sub_status", 2502 } } } }));
        }
        p->deliver(ElegooCc2::response_topic(MON_SN, cid), json { { "id", j["id"] }, { "method", method }, { "result", result } }.dump());
        return true;
    }
    bool receive(std::string& topic, std::string& payload, int timeout_ms) override
    {
        std::unique_lock<std::mutex> lock(p->m);
        if (!p->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&]() { return !p->inbox.empty(); })) return false;
        topic   = p->inbox.front().first;
        payload = p->inbox.front().second;
        p->inbox.pop_front();
        return true;
    }
    void close() override
    {
        std::lock_guard<std::mutex> lock(p->m);
        if (p->connected) ++p->closes;
        p->connected = false;
    }
    bool alive() const override
    {
        std::lock_guard<std::mutex> lock(p->m);
        return p->connected;
    }
};

// Waits (bounded) until pred holds.
bool eventually(const std::function<bool()>& pred, int ms = 5000)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

struct MonitorFixture
{
    std::shared_ptr<FakePrinter> printer = std::make_shared<FakePrinter>();
    std::atomic<int>             connector_calls { 0 };

    MonitorFixture()
    {
        ElegooCc2Monitor::reset_for_tests();
        auto p = printer;
        ElegooCc2Monitor::set_connector_for_tests([p, this](const std::string&, const std::string& token, std::string& sn,
                                                            std::unique_ptr<ElegooCc2::MqttTransport>& t, std::string& error) {
            ++connector_calls;
            if (!p->reachable) {
                error = "the printer does not answer";
                return false;
            }
            if (token == "wrong") {
                error = "the printer refused the access code";
                return false;
            }
            sn = MON_SN;
            t  = std::make_unique<FakeTransport>(p);
            return true;
        });
    }
    ~MonitorFixture()
    {
        ElegooCc2Monitor::reset_for_tests();
        ElegooCc2Monitor::set_connector_for_tests(nullptr);
        ElegooCc2Monitor::set_timing_for_tests(0, 0);
    }
};

} // namespace

TEST_CASE("CC2 monitor: on demand, pushes and controls", "[ElegooCc2Monitor]")
{
    MonitorFixture f;
    ElegooCc2Monitor::set_timing_for_tests(1500, 600000, 200, 0);
    const std::string host = "cc2.test";

    // The first look never waits; the connection comes up behind it.
    const ElegooCc2::Snapshot first = ElegooCc2Monitor::demand(host, "");
    CHECK(first.exists);
    REQUIRE(eventually([&]() { return ElegooCc2Monitor::demand(host, "").status.valid; }));
    ElegooCc2::Snapshot s = ElegooCc2Monitor::demand(host, "");
    CHECK(s.connected);
    CHECK(ElegooCc2::hub_state(s.status) == "standby");
    CHECK(eventually([&]() { return ElegooCc2Monitor::demand(host, "").firmware == "9.9.9"; }));
    // The status topic is read: a push lands in the snapshot.
    {
        std::lock_guard<std::mutex> lock(f.printer->m);
        f.printer->status = 2;
        f.printer->sub    = 2075;
    }
    f.printer->deliver(ElegooCc2::status_topic(MON_SN), push_msg(1, json { { "machine_status", { { "status", 2 }, { "sub_status", 2075 }, { "progress", 33 } } } }));
    REQUIRE(eventually([&]() { return ElegooCc2::progress(ElegooCc2Monitor::demand(host, "").status) == 33; }));
    CHECK(ElegooCc2Monitor::connected(host));
    // The heartbeat goes out on the request topic.
    CHECK(eventually([&]() {
        return f.printer->count([](const std::string& t, const std::string& p) {
                   return t.find("/api_request") != std::string::npos && p.find("PING") != std::string::npos;
               }) >= 2;
    }));
    // While somebody looks, the CANVAS is read; this printer has none.
    CHECK(eventually([&]() { return ElegooCc2Monitor::demand(host, "").canvas_known; }));
    CHECK_FALSE(ElegooCc2Monitor::demand(host, "").has_canvas);

    // A control: 1021, answered, and the push after it says paused.
    const ElegooCc2Monitor::CallResult r = ElegooCc2Monitor::call(host, "", ElegooCc2::METHOD_PAUSE_PRINT, "{}", 3000);
    CHECK(r.sent);
    CHECK(r.answered);
    CHECK(r.error_code == 0);
    CHECK(eventually([&]() { return ElegooCc2::hub_state(ElegooCc2Monitor::demand(host, "").status) == "paused"; }));

    // A control the printer never answers: bounded by the timeout.
    {
        std::lock_guard<std::mutex> lock(f.printer->m);
        f.printer->answer_pause = false;
    }
    const auto                         t0 = std::chrono::steady_clock::now();
    const ElegooCc2Monitor::CallResult silent = ElegooCc2Monitor::call(host, "", ElegooCc2::METHOD_PAUSE_PRINT, "{}", 400);
    CHECK_FALSE(silent.answered);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3));

    // Nobody asks any more: after KEEP the connection closes, and the row keeps the last status.
    REQUIRE(eventually([&]() { return !ElegooCc2Monitor::touch(host, "").connected; }, 6000));
    s = ElegooCc2Monitor::touch(host, "");
    CHECK(s.status.valid);
    CHECK(s.failures == 0);
    CHECK(f.printer->closes >= 1);
}

TEST_CASE("CC2 monitor: check-ins, failures and the access code", "[ElegooCc2Monitor]")
{
    MonitorFixture f;
    SECTION("nobody looking: one check-in reads the status and closes")
    {
        ElegooCc2Monitor::set_timing_for_tests(60000, 600000, 0, 0);
        ElegooCc2Monitor::touch("cc2.checkin", "");
        REQUIRE(eventually([&]() { return ElegooCc2Monitor::touch("cc2.checkin", "").status.valid; }));
        REQUIRE(eventually([&]() { return !ElegooCc2Monitor::touch("cc2.checkin", "").connected; }));
        CHECK(f.printer->requests(ElegooCc2::METHOD_GET_STATUS) >= 1);
        CHECK(f.printer->requests(ElegooCc2::METHOD_GET_CANVAS) == 0); // nobody looking: no CANVAS read
        const int calls = f.connector_calls.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        CHECK(f.connector_calls.load() == calls); // the next check-in is minutes away
    }
    SECTION("a printer that is off: offline with the reason, and backs off")
    {
        ElegooCc2Monitor::set_timing_for_tests(60000, 600000, 0, 0);
        f.printer->reachable = false;
        ElegooCc2Monitor::demand("cc2.off", "");
        REQUIRE(eventually([&]() { return ElegooCc2Monitor::demand("cc2.off", "").failures > 0; }));
        const ElegooCc2::Snapshot s = ElegooCc2Monitor::demand("cc2.off", "");
        CHECK_FALSE(s.connected);
        CHECK(s.error == "the printer does not answer");
        json row = { { "id", "ph:off" } };
        ElegooCc2::fill_row(s, row);
        CHECK(row["status"] == "offline");
        // Backing off: no new attempt within the first second (the first back-off is 2 s).
        const int calls = f.connector_calls.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        CHECK(f.connector_calls.load() == calls);
        // A control on it fails fast with the reason rather than hanging.
        const ElegooCc2Monitor::CallResult r = ElegooCc2Monitor::call("cc2.off", "", ElegooCc2::METHOD_STOP_PRINT, "{}", 5000);
        CHECK_FALSE(r.answered);
        // It comes back: the next attempt connects.
        f.printer->reachable = true;
        CHECK(eventually([&]() { return ElegooCc2Monitor::demand("cc2.off", "").connected; }, 8000));
    }
    SECTION("a wrong access code says so")
    {
        ElegooCc2Monitor::set_timing_for_tests(60000, 600000, 0, 0);
        ElegooCc2Monitor::demand("cc2.code", "wrong");
        REQUIRE(eventually([&]() { return ElegooCc2Monitor::demand("cc2.code", "wrong").failures > 0; }));
        CHECK(ElegooCc2Monitor::demand("cc2.code", "wrong").error == "the printer refused the access code");
    }
    SECTION("a host that could smuggle anything is refused outright")
    {
        CHECK_FALSE(ElegooCc2Monitor::demand("cc2/../x", "").exists);
        CHECK_FALSE(ElegooCc2Monitor::demand("", "").exists);
    }
}
