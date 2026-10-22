// The network half of EdgeSlicer's printer protocols, end to end against the printer emulator
// (tools/printer_emulator, design tests/design_printer_emulator.md section 5): the same calls the
// send dialogs and the hub make - PrintHostDevices::probe, query_qidi_variables,
// ElegooCanvasQuery's canvas_query, SnapmakerLan::run_script / status_now - with no GUI and no
// printer. Every case is hidden ([.EmulatorE2E]): run_emulator_e2e.py starts a private emulator
// with a QIDI X-Max 4, an Elegoo CC1 and a Snapmaker U1 on loopback addresses and passes them in
//   EMU_CONTROL   http://127.0.0.1:<port>   (the emulator's control API, to set faults)
//   EMU_QIDI      <ip>:<port>
//   EMU_CC1       <ip>                       (SDCP is on the fixed port 3030)
//   EMU_U1        <ip>:<port>
// Nothing here contacts a real printer: every address is checked to be 127.x before it is used.

#include <catch2/catch.hpp>

#include "slic3r/GUI/SnapmakerLan.hpp"
#include "slic3r/Utils/ElegooCanvas.hpp"
#include "slic3r/Utils/ElegooSdcpMonitor.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/PrintHostDeviceStatus.hpp"
#include "slic3r/Utils/PrintHostDevices.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

using namespace Slic3r;
using nlohmann::json;

namespace {

std::string env(const char* name)
{
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

// "127.0.0.61:7125" / "127.0.0.61" -> true only for loopback.
bool loopback(const std::string& address) { return address.rfind("127.", 0) == 0; }

std::string host_part(const std::string& address)
{
    const size_t colon = address.find(':');
    return colon == std::string::npos ? address : address.substr(0, colon);
}

int port_part(const std::string& address, int fallback)
{
    const size_t colon = address.find(':');
    return colon == std::string::npos ? fallback : std::atoi(address.c_str() + colon + 1);
}

// The emulator's control API: patch a printer's config / reset it / set its print state.
json control(const std::string& path, const json& body)
{
    const std::string base = env("EMU_CONTROL");
    REQUIRE(base.rfind("http://127.0.0.1:", 0) == 0);
    json out;
    bool ok = false;
    Http::post(base + path)
        .header("Content-Type", "application/json")
        .set_post_body(body.dump())
        .timeout_connect(3)
        .timeout_max(10)
        .on_complete([&](std::string b, unsigned) {
            ok = true;
            try {
                out = json::parse(b);
            } catch (...) {}
        })
        .perform_sync();
    REQUIRE(ok);
    return out;
}

void reset(const std::string& id, const json& patch = json::object()) { control("/emu/printers/" + id + "/reset", patch); }
void set(const std::string& id, const json& patch) { control("/emu/printers/" + id + "/set", patch); }

json log_since(long long mark, const std::string& id, const std::string& kind)
{
    const std::string base = env("EMU_CONTROL");
    json out;
    Http::get(base + "/emu/log?printer=" + id + "&kind=" + kind + "&since=" + std::to_string(mark))
        .timeout_connect(3)
        .timeout_max(10)
        .on_complete([&](std::string b, unsigned) {
            try {
                out = json::parse(b);
            } catch (...) {}
        })
        .perform_sync();
    return out.value("entries", json::array());
}

long long mark_now()
{
    json out;
    Http::get(env("EMU_CONTROL") + "/emu/mark")
        .timeout_max(10)
        .on_complete([&](std::string b, unsigned) {
            try {
                out = json::parse(b);
            } catch (...) {}
        })
        .perform_sync();
    return out.value("mark", 0LL);
}

} // namespace

TEST_CASE("emulator QIDI: probe reads the Box, the variables and the polar cooler", "[.][EmulatorE2E]")
{
    const std::string qidi = env("EMU_QIDI");
    REQUIRE(loopback(qidi));
    reset("e2e-qidi");

    PrintHostDevices::Device d;
    d.address       = qidi;
    d.host_type     = "octoprint";
    d.printer_model = "Qidi X-Max 4";
    const PrintHostDevices::Status st = PrintHostDevices::probe(d, PrintHostDevices::probe_timeout_for(d.printer_model));
    REQUIRE(st.online);
    CHECK(st.qidi);
    CHECK(st.qidi_box);
    CHECK(st.qidi_polar_cooler);
    CHECK(st.state == "standby");
    REQUIRE(st.slots.size() == 4);
    CHECK(st.slots[0].type == "PLA");
    CHECK(st.slots[0].color == "#E03030");
    CHECK(st.slots[1].type == "ASA");
    CHECK_FALSE(st.slots[2].loaded);

    std::string vars, error;
    REQUIRE(PrintHostDevices::query_qidi_variables("http://" + qidi, vars, error, 5));
    const json v = json::parse(vars);
    CHECK(v.value("value_t0", std::string()) == "slot3"); // the stale mapping the emulator seeds
    CHECK(v.value("enable_box", 0) == 1);
    CHECK(v.value("box_count", 0) == 1);

    INFO("the Box read only through save_variables + the filament list (QIDIStudio's way)");
    reset("e2e-qidi", { { "qidi", { { "box_report", "vars_only" } } } });
    const PrintHostDevices::Status st2 = PrintHostDevices::probe(d, 10);
    REQUIRE(st2.online);
    CHECK(st2.qidi_box);
    REQUIRE(st2.slots.size() == 4);
    CHECK(st2.slots[1].type == "ASA");
    CHECK(st2.slots_have_filaments);

    INFO("a probe slower than its timeout answers offline, quickly");
    reset("e2e-qidi", { { "qidi", { { "box_report", "mcc+vars" } } }, { "faults", { { "probe_delay_s", 5 } } } });
    const auto t0 = std::chrono::steady_clock::now();
    const PrintHostDevices::Status slow = PrintHostDevices::probe(d, 2);
    CHECK_FALSE(slow.online);
    CHECK(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count() < 5);
    set("e2e-qidi", { { "faults", { { "probe_delay_s", 0 } } } });
}

TEST_CASE("emulator CC1: canvas_query reads the trays, the untagged brand, and a detached CANVAS", "[.][EmulatorE2E]")
{
    const std::string cc1 = env("EMU_CC1");
    REQUIRE(loopback(cc1));
    SdcpMonitor::reset_for_tests();
    reset("e2e-cc1");

    CanvasQueryResult r = canvas_query(cc1);
    REQUIRE(r.ok);
    CHECK(r.reached);
    CHECK(r.ams_connect_status == 1);
    REQUIRE(r.info.trays.size() == 4);
    CHECK(r.info.trays[0].tray_type == "PLA");
    CHECK(r.info.trays[0].colour_hex() == "#D02A2A");
    CHECK(r.info.trays[1].brand == "Generic"); // "— — — —" on the wire
    CHECK((r.info.trays[1].filament_code.empty() || r.info.trays[1].filament_code == "0x00000")); // "no tag"
    CHECK(r.info.trays[2].tray_type == "ASA");
    CHECK_FALSE(r.info.trays[3].has_filament);
    CHECK(r.info.active_tray_id == 0);

    INFO("the send dialog's probe of the same printer");
    PrintHostDevices::Device d;
    d.address       = cc1;
    d.host_type     = "elegoolink";
    d.printer_model = "Elegoo Centauri Carbon";
    PrintHostDevices::Status st = PrintHostDevices::probe(d, 3);
    REQUIRE(st.online);
    CHECK(st.canvas);
    REQUIRE(st.slots.size() == 4);
    CHECK(st.slots[2].type == "ASA");
    CHECK(st.slots[2].unit == 0);
    CHECK(st.slots[2].tray == 2);

    INFO("CANVAS detached");
    set("e2e-cc1", { { "canvas", { { "attached", false } } } });
    r = canvas_query(cc1);
    CHECK_FALSE(r.ok);
    CHECK(r.no_canvas);
    CHECK(r.ams_connect_status == 0);
    st = PrintHostDevices::probe(d, 3);
    CHECK_FALSE(st.canvas);
    CHECK(st.slots.empty());

    INFO("every viewer slot taken: a new connection is refused and the query says so");
    // The hub's shared SDCP connection, if one is open, keeps working (the printer only refuses NEW
    // clients); drop it so the query has to connect.
    SdcpMonitor::reset_for_tests();
    set("e2e-cc1", { { "canvas", { { "attached", true } } }, { "faults", { { "viewers_full", true } } } });
    r = canvas_query(cc1);
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.error.empty());
    set("e2e-cc1", { { "faults", { { "viewers_full", false } } } });
    SdcpMonitor::reset_for_tests();
}

TEST_CASE("emulator U1: status, the mapping script, and an unload refused while printing", "[.][EmulatorE2E]")
{
    using namespace Slic3r::GUI;
    const std::string u1 = env("EMU_U1");
    REQUIRE(loopback(u1));
    reset("e2e-u1");

    SnapmakerLan::Device d;
    d.id    = "EMU-U1-E2EU1";
    d.name  = "EMU e2e U1";
    d.model = "Snapmaker U1";
    d.ip    = host_part(u1);
    d.port  = port_part(u1, 80);

    const SnapmakerLan::Status s = SnapmakerLan::status_now(d);
    REQUIRE(s.online);
    CHECK(s.state == "standby");
    CHECK(s.nozzles.size() == 4);
    const std::vector<SnapmakerLan::Toolhead> heads = SnapmakerLan::toolheads(d);
    REQUIRE(heads.size() == 4);
    CHECK(heads[0].type == "PLA");
    CHECK(heads[0].color == "#D93A2B");
    CHECK(heads[2].type == "PETG");
    CHECK_FALSE(heads[3].loaded);

    INFO("the mapped start's macros, as start_print_mapped sends them");
    const long long mark   = mark_now();
    const std::string script = SnapmakerLan::mapping_script({ 2, 0 }, true);
    std::string       error;
    REQUIRE(SnapmakerLan::run_script(d, script, error));
    const json maps = log_since(mark, "e2e-u1", "extruder_map");
    REQUIRE(maps.size() == 2);
    CHECK(maps[0]["payload"]["config"] == 0);
    CHECK(maps[0]["payload"]["map"] == 2);
    CHECK(maps[1]["payload"]["map"] == 0);
    const json used = log_since(mark, "e2e-u1", "used_extruders");
    REQUIRE(used.size() == 1);
    CHECK(used[0]["payload"]["extruders"] == json::array({ 2, 0 }));
    const json prefs = log_since(mark, "e2e-u1", "preferences");
    REQUIRE(prefs.size() == 1);
    CHECK(prefs[0]["payload"]["end_unload_filament"] == json::array({ 1, 0, 1, 0 }));

    INFO("the firmware refuses SET_PRINT_PREFERENCES while printing; run_script reports its words");
    control("/emu/printers/e2e-u1/state", { { "state", "printing" } });
    error.clear();
    CHECK_FALSE(SnapmakerLan::run_script(d, "SET_PRINT_PREFERENCES BED_LEVEL=0 END_UNLOAD_FILAMENT=[1,0,0,0]", error));
    CHECK(error.find("not allowed while printing") != std::string::npos);
    reset("e2e-u1");
}
