// U1 local-first, Plan B (GUI/U1WsRoute): the Device page's JSON-RPC to a cloud-bound U1 goes over
// the printer's own Moonraker WebSocket while it answers, through the Snapmaker cloud for whatever
// the LAN cannot do, and entirely through the cloud while the LAN is gone.
//
// The unit cases cover the JSON-RPC helpers (Utils/U1LocalFirst). The [U1LocalFirstE2E] cases are
// hidden: run_u1_local_first_e2e.py starts mock_u1_mqtt.py (the fake cloud broker) and
// fake_moonraker_ws.py (the fake printer's Moonraker) on 127.0.0.1. No real printer or cloud.

#include <catch2/catch.hpp>

#include "slic3r/GUI/U1WsRoute.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/U1LocalFirst.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::U1LocalFirst;
using nlohmann::json;

// ------------------------------------------------------------------ units ----

TEST_CASE("U1 WebSocket route: matching answers to requests", "[U1LocalFirst]")
{
    const json num = { { "id", 12 } };
    const json str = { { "id", "edge-ws-3" } };
    CHECK(rpc_id_key(num) == "12");
    CHECK(rpc_id_key(str) == "\"edge-ws-3\"");
    CHECK(rpc_id_key(json::object()).empty());
    const json null_id = { { "id", nullptr } };
    CHECK(rpc_id_key(null_id).empty());
}

TEST_CASE("U1 WebSocket route: what counts as a method the LAN does not have", "[U1LocalFirst]")
{
    const json missing = { { "error", { { "code", -32601 }, { "message", "Method not found" } } } };
    const json by_text = { { "error", { { "code", 400 }, { "message", "Method Not Found: custom.x" } } } };
    const json busy    = { { "error", { { "code", 400 }, { "message", "Printer is busy" } } } };
    const json ok      = { { "result", "ok" } };
    CHECK(is_method_not_found(missing));
    CHECK(is_method_not_found(by_text));
    CHECK_FALSE(is_method_not_found(busy)); // the printer said no: the page hears it
    CHECK_FALSE(is_method_not_found(ok));
}

TEST_CASE("U1 WebSocket route: only reads are asked twice", "[U1LocalFirst]")
{
    for (const char* m : { "server.info", "printer.objects.query", "server.files.list", "server.files.metadata", "machine.system_info",
                           "system.get_device_info", "machine.heartbeat", "custom.file.filament.objects.get_mapping" }) {
        INFO(m);
        CHECK(is_idempotent_method(m));
    }
    for (const char* m : { "printer.gcode.script", "printer.print.start", "printer.print.pause", "printer.print.cancel",
                           "server.files.delete_file", "printer.control.bed_temp", "machine.set_device_name", "" }) {
        INFO(m);
        CHECK_FALSE(is_idempotent_method(m));
    }
}

TEST_CASE("U1 WebSocket route: notifications land where the page listens", "[U1LocalFirst]")
{
    CHECK(topic_for_notification("SN", "notify_status_update") == "SN/status");
    CHECK(topic_for_notification("SN", "notify_gcode_response") == "SN/notification");
    CHECK(topic_for_notification("SN", "notify_proc_stat_update") == "SN/notification");
}

// ------------------------------------------------------------- end to end ----

namespace {

struct Ports
{
    int cloud { 0 }, control { 0 }, moonraker { 0 };
    bool ok() const { return cloud && control && moonraker; }
};

Ports ports()
{
    Ports       p;
    int         lan = 0, pairing = 0, a = 0, b = 0;
    const char* v   = std::getenv("U1MOCK_PORTS");
    if (v)
        std::sscanf(v, "%d:%d:%d:%d", &p.cloud, &lan, &pairing, &p.control);
    const char* w = std::getenv("U1MOCK_MOONRAKER");
    if (w)
        std::sscanf(w, "%d:%d:%d", &a, &b, &p.moonraker);
    return p;
}

json get(int port, const std::string& path)
{
    json out;
    Http::get("http://127.0.0.1:" + std::to_string(port) + path)
        .timeout_max(5)
        .on_complete([&out](std::string body, unsigned) { out = json::parse(body, nullptr, false); })
        .perform_sync();
    return out;
}

template<class Pred> bool wait_until(Pred pred, int timeout_ms)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < until) {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return pred();
}

struct WsPage
{
    std::string                                              sn;
    std::shared_ptr<MqttClient>                              cloud;
    std::shared_ptr<GUI::U1WsRoute>                          route;
    std::shared_ptr<std::mutex>                              mtx = std::make_shared<std::mutex>();
    std::shared_ptr<std::vector<std::pair<std::string, json>>> got = std::make_shared<std::vector<std::pair<std::string, json>>>();

    bool ask(const std::string& method, long long id)
    {
        std::string msg;
        return cloud->Publish(sn + "/request", json({ { "jsonrpc", "2.0" }, { "method", method }, { "id", id } }).dump(), 1, msg);
    }
    std::vector<json> answers(long long id) const
    {
        std::lock_guard<std::mutex> lk(*mtx);
        std::vector<json>           out;
        for (const auto& m : *got)
            if (m.first == sn + "/response" && m.second.is_object() && m.second.contains("id") && m.second["id"].is_number_integer() &&
                m.second["id"].get<long long>() == id)
                out.push_back(m.second);
        return out;
    }
    std::string via(long long id, int timeout_ms = 6000)
    {
        wait_until([&] { return !answers(id).empty(); }, timeout_ms);
        const std::vector<json> a = answers(id);
        if (a.empty())
            return "none";
        return a[0].contains("result") && a[0]["result"].is_object() ? a[0]["result"].value("via", "") : "error";
    }
    int count_on(const std::string& suffix, const std::string& needle) const
    {
        std::lock_guard<std::mutex> lk(*mtx);
        int                         n = 0;
        for (const auto& m : *got)
            n += m.first == sn + suffix && m.second.dump().find(needle) != std::string::npos;
        return n;
    }
};

WsPage open_ws_page(const Ports& p, const std::string& sn)
{
    WsPage page;
    page.sn    = sn;
    page.cloud = MqttClient::create("mqtt://127.0.0.1:" + std::to_string(p.cloud), "cloud-ws-" + sn);
    auto got   = page.got;
    auto mtx   = page.mtx;
    page.cloud->SetMessageCallback([got, mtx](const std::string& topic, const std::string& payload, void*) {
        std::lock_guard<std::mutex> lk(*mtx);
        got->emplace_back(topic, json::parse(payload, nullptr, false));
    });
    std::string msg;
    REQUIRE(page.cloud->Connect(msg));

    GUI::U1WsDeps d;
    d.timing.retry_ms    = { 300, 600, 1000 };
    d.request_timeout_ms = 3000;
    d.connect_timeout_ms = 2000;
    d.idle_timeout_ms    = 2000;
    d.ws_port            = p.moonraker;
    d.known_addresses    = [](const std::string&) { return std::vector<std::string> { "127.0.0.1" }; };
    d.is_that_printer    = [sn](const std::string&, const std::string& asked) { return asked == sn; };
    page.route           = GUI::U1WsRoute::attach(page.cloud, d);
    // What the page does: subscribe its device topics on the cloud session, then set_engine.
    for (const char* t : { "/response", "/notification", "/status" })
        REQUIRE(page.cloud->Subscribe(sn + t, 1, msg));
    page.route->set_device(sn);
    return page;
}

void close_ws_page(WsPage& page)
{
    CHECK(page.route->stop_and_wait(15000));
    std::string msg;
    page.cloud->Disconnect(msg);
}

int method_count(const Ports& p, const std::string& method)
{
    const json j = get(p.moonraker, "/fake/methods");
    if (!j.is_object() || !j.contains("methods"))
        return -1;
    return (int) std::count(j["methods"].begin(), j["methods"].end(), json(method));
}

} // namespace

TEST_CASE("U1 WebSocket route end to end: requests, notifications and status go over the LAN", "[.][U1LocalFirstE2E]")
{
    GUI::U1WsRoute::reset_pacing();
    const Ports p = ports();
    REQUIRE(p.ok());
    get(p.moonraker, "/fake/up");
    WsPage page = open_ws_page(p, "U1WSLOCAL1");
    REQUIRE(wait_until([&] { return page.route->on_lan(); }, 10000));

    REQUIRE(page.ask("server.info", 11));
    CHECK(page.via(11) == "local");
    // The status stream the page starts comes from the LAN, on <sn>/status.
    std::string msg;
    const json  sub = { { "jsonrpc", "2.0" }, { "method", "printer.objects.subscribe" },
                        { "params", { { "objects", { { "print_stats", nullptr } } } } }, { "id", 12 } };
    REQUIRE(page.cloud->Publish(page.sn + "/request", sub.dump(), 1, msg));
    CHECK(page.via(12) == "local");
    CHECK(wait_until([&] { return page.count_on("/status", "notify_status_update") >= 1; }, 5000));
    CHECK(page.count_on("/notification", "notify_proc_stat_update") >= 1);
    // A cloud answer the route did not ask for does not reach the page (no duplicates).
    REQUIRE(page.cloud->PublishDirect(page.sn + "/request", json({ { "method", "server.info" }, { "id", 13 } }).dump(), 1, msg));
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    CHECK(page.answers(13).empty());
    CHECK(page.answers(11).size() == 1);
    close_ws_page(page);
}

TEST_CASE("U1 WebSocket route end to end: a method the LAN does not have goes through the cloud", "[.][U1LocalFirstE2E]")
{
    GUI::U1WsRoute::reset_pacing();
    const Ports p = ports();
    REQUIRE(p.ok());
    get(p.moonraker, "/fake/up");
    const int tried_before = method_count(p, "custom.cloud_only");
    WsPage    page         = open_ws_page(p, "U1WSCLOUD1");
    REQUIRE(wait_until([&] { return page.route->on_lan(); }, 10000));

    REQUIRE(page.ask("custom.cloud_only", 21));
    CHECK(page.via(21) == "cloud");
    CHECK(page.answers(21).size() == 1); // the LAN's "Method not found" never reaches the page
    const std::vector<std::string> cloud = page.route->cloud_methods();
    CHECK(std::find(cloud.begin(), cloud.end(), "custom.cloud_only") != cloud.end());
    // From now on straight to the cloud: the LAN is not asked again.
    REQUIRE(page.ask("custom.cloud_only", 22));
    CHECK(page.via(22) == "cloud");
    CHECK(method_count(p, "custom.cloud_only") == tried_before + 1);
    // Everything else still goes over the LAN.
    REQUIRE(page.ask("printer.info", 23));
    CHECK(page.via(23) == "local");
    CHECK(page.route->on_lan());
    close_ws_page(page);
}

TEST_CASE("U1 WebSocket route end to end: the LAN drops, the cloud takes over, the LAN comes back", "[.][U1LocalFirstE2E]")
{
    GUI::U1WsRoute::reset_pacing();
    const Ports p = ports();
    REQUIRE(p.ok());
    get(p.moonraker, "/fake/up");
    WsPage page = open_ws_page(p, "U1WSDROP01");
    REQUIRE(wait_until([&] { return page.route->on_lan(); }, 10000));
    REQUIRE(page.ask("server.info", 31));
    CHECK(page.via(31) == "local");

    get(p.moonraker, "/fake/drop");
    REQUIRE(wait_until([&] { return !page.route->on_lan(); }, 8000));
    REQUIRE(page.ask("server.info", 32));
    CHECK(page.via(32) == "cloud");
    CHECK(page.answers(32).size() == 1);

    get(p.moonraker, "/fake/up");
    REQUIRE(wait_until([&] { return page.route->on_lan(); }, 10000));
    REQUIRE(page.ask("server.info", 33));
    CHECK(page.via(33) == "local");
    close_ws_page(page);
}
