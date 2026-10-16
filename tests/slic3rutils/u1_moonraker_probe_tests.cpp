// The read-only U1 Moonraker probe (Utils/U1MoonrakerProbe): the owner runs it against a U1 in
// cloud mode to learn whether its Moonraker JSON-RPC answers on the LAN (Plan B of
// tests/design_u1_local_first.md). These cases check that it can only ever read, and - hidden,
// against fake_moonraker_ws.py, run by run_u1_local_first_e2e.py - that it reports what it finds.

#include <catch2/catch.hpp>

#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/U1MoonrakerProbe.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::U1MoonrakerProbe;
using nlohmann::json;

TEST_CASE("U1 Moonraker probe: only read-only methods can be sent", "[U1MoonrakerProbe]")
{
    CHECK(read_only_methods().size() == 3);
    for (const std::string& m : read_only_methods()) {
        INFO(m);
        CHECK(is_read_only_method(m));
        const json req = json::parse(request_for(m, 5));
        CHECK(req["method"] == m);
        CHECK(req["id"] == 5);
    }
    for (const char* m : { "printer.gcode.script", "printer.print.start", "printer.emergency_stop", "server.files.delete_file",
                           "machine.reboot", "printer.objects.subscribe", "" }) {
        INFO(m);
        CHECK_FALSE(is_read_only_method(m));
        CHECK(request_for(m, 1).empty());
    }
    const json q = json::parse(request_for("printer.objects.query", 9));
    CHECK(q["params"]["objects"].contains("print_stats"));
    CHECK(q["params"]["objects"].contains("extruder"));
    for (const std::string& p : http_paths()) {
        INFO(p);
        CHECK(p.find("/server/files") == std::string::npos);
        CHECK(p.find("gcode") == std::string::npos);
    }
}

TEST_CASE("U1 Moonraker probe: what it reports of an answer", "[U1MoonrakerProbe]")
{
    const std::string info = R"({"result": {"klippy_state": "ready", "hostname": "my-printer", "serial": "XYZ123"}})";
    const std::string text = describe_answer(info);
    CHECK(text.find("klippy_state=ready") != std::string::npos);
    // Keys, never values: no host name or serial number in the log.
    CHECK(text.find("my-printer") == std::string::npos);
    CHECK(text.find("XYZ123") == std::string::npos);
    const std::string err = R"({"error": {"code": 401, "message": "Unauthorized"}})";
    CHECK(describe_answer(err) == "error 401: Unauthorized");
    CHECK(describe_answer("<html>") == "not JSON (6 bytes)");
    CHECK(describe_answer("") == "empty answer");
    const std::string query = R"({"result": {"status": {"print_stats": {"state": "printing"}}}})";
    CHECK(describe_answer(query).find("print_stats.state=\"printing\"") != std::string::npos);
}

namespace {
std::vector<int> moonraker_ports()
{
    int         a = 0, b = 0;
    const char* v = std::getenv("U1MOCK_MOONRAKER");
    if (v)
        std::sscanf(v, "%d:%d", &a, &b);
    return { a, b };
}
std::vector<std::string> methods_seen(int port)
{
    std::vector<std::string> out;
    Http::get("http://127.0.0.1:" + std::to_string(port) + "/fake/methods")
        .timeout_max(5)
        .on_complete([&out](std::string body, unsigned) {
            const json j = json::parse(body, nullptr, false);
            if (j.is_object() && j.contains("methods"))
                for (const json& m : j["methods"])
                    out.push_back(m.get<std::string>());
        })
        .perform_sync();
    return out;
}
} // namespace

TEST_CASE("U1 Moonraker probe end to end: HTTP and WebSocket both answer", "[.][U1LocalFirstE2E]")
{
    const std::vector<int> ports = moonraker_ports();
    REQUIRE(ports[0] > 0);
    Options o;
    o.http_port = ports[0];
    o.ws_ports  = { ports[0] };
    o.timeout_s = 3;
    const std::vector<Step> steps = run("127.0.0.1", o);
    REQUIRE(steps.size() == 3 + 1 + 3); // three GETs, the upgrade, three methods
    for (const Step& s : steps) {
        INFO(s.name << ": " << s.detail);
        CHECK(s.ok);
    }
    CHECK(steps[3].status == 101);
    const std::string text = report(steps);
    CHECK(text.find("Local HTTP JSON answers: yes") != std::string::npos);
    CHECK(text.find("Local WebSocket JSON-RPC answers: yes") != std::string::npos);
    // Only the read-only methods reached the printer.
    const std::vector<std::string> seen = methods_seen(ports[0]);
    CHECK(seen.size() == 3);
    for (const std::string& m : seen) {
        INFO(m);
        CHECK(is_read_only_method(m));
    }
}

TEST_CASE("U1 Moonraker probe end to end: HTTP answers, the WebSocket does not", "[.][U1LocalFirstE2E]")
{
    const std::vector<int> ports = moonraker_ports();
    REQUIRE(ports[1] > 0);
    Options o;
    o.http_port = ports[1];
    o.ws_ports  = { ports[1] };
    o.timeout_s = 3;
    const std::vector<Step> steps = run("127.0.0.1", o);
    REQUIRE(steps.size() == 4); // three GETs and the refused upgrade
    CHECK(steps[0].ok);
    CHECK_FALSE(steps[3].ok);
    CHECK(steps[3].status == 404);
    const std::string text = report(steps);
    CHECK(text.find("Local HTTP JSON answers: yes") != std::string::npos);
    CHECK(text.find("Local WebSocket JSON-RPC answers: no") != std::string::npos);
}

TEST_CASE("U1 Moonraker probe end to end: nothing listening fails fast", "[.][U1LocalFirstE2E]")
{
    Options o;
    o.http_port = 1;
    o.ws_ports  = { 1 };
    o.timeout_s = 2;
    const std::vector<Step> steps = run("127.0.0.1", o);
    REQUIRE(steps.size() == 4);
    for (const Step& s : steps) {
        INFO(s.name << ": " << s.detail);
        CHECK_FALSE(s.ok);
        CHECK(s.ms < 8000);
    }
    CHECK(report(steps).find("Local WebSocket JSON-RPC answers: no") != std::string::npos);
}
