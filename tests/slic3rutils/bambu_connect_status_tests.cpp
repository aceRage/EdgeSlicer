#include <catch2/catch.hpp>

#include <ctime>
#include <string>

#include "libslic3r/AppConfig.hpp"
#include "slic3r/Utils/BambuConnectStatus.hpp"

using namespace Slic3r;
using namespace Slic3r::BambuConnectStatus;

namespace {

DynamicPrintConfig tray(const std::string &filament_id, const std::string &colour, const std::string &name, bool exists = true)
{
    DynamicPrintConfig c;
    c.set_key_value("filament_id", new ConfigOptionStrings{ filament_id });
    c.set_key_value("tag_uid", new ConfigOptionStrings{ "0000000000000000" });
    c.set_key_value("filament_type", new ConfigOptionStrings{ "PLA" });
    c.set_key_value("tray_name", new ConfigOptionStrings{ name });
    c.set_key_value("filament_colour", new ConfigOptionStrings{ colour });
    c.set_key_value("filament_exist", new ConfigOptionBools{ exists });
    c.set_key_value("filament_multi_colors", new ConfigOptionStrings{});
    return c;
}

Snapshot sample_snapshot(std::time_t at)
{
    Snapshot s;
    s.saved_at      = at;
    s.printer_model = "Bambu Lab H2D";
    s.printer_name  = "Workshop H2D";
    s.trays.emplace(0, tray("GFA00", "#FF0000", "A1"));
    s.trays.emplace(1, tray("GFL99", "#00FF00", "A2", false));
    s.trays.emplace(254, tray("GFG02", "#0000FF", "Ext"));
    s.extruders.push_back({ 0, true, 1, 0.4f });  // right: High Flow
    s.extruders.push_back({ 1, true, 0, 0.6f });  // left: Standard
    return s;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Status-only gate
// ---------------------------------------------------------------------------------------------

TEST_CASE("status_only_allows lets only pushall and get_version through", "[BambuConnectStatus]")
{
    CHECK(status_only_allows(R"({"pushing":{"sequence_id":"1","command":"pushall","version":1,"push_target":1}})"));
    CHECK(status_only_allows(R"({"info":{"sequence_id":"2","command":"get_version"}})"));

    // Every control command is refused, whatever its namespace.
    CHECK_FALSE(status_only_allows(R"({"print":{"sequence_id":"3","command":"project_file","url":"ftp://x"}})"));
    CHECK_FALSE(status_only_allows(R"({"print":{"sequence_id":"4","command":"pause"}})"));
    CHECK_FALSE(status_only_allows(R"({"system":{"sequence_id":"5","command":"ledctrl","led_node":"chamber_light"}})"));
    CHECK_FALSE(status_only_allows(R"({"system":{"sequence_id":"6","command":"get_access_code"}})"));
    CHECK_FALSE(status_only_allows(R"({"xcam":{"command":"xcam_control_set"}})"));
    CHECK_FALSE(status_only_allows(R"({"camera":{"command":"ipcam_record_set"}})"));
    CHECK_FALSE(status_only_allows(R"({"pushing":{"command":"start"}})"));
    CHECK_FALSE(status_only_allows(R"({"info":{"command":"get_accessories"}})"));
    // The right command in the wrong namespace.
    CHECK_FALSE(status_only_allows(R"({"print":{"command":"pushall"}})"));
    CHECK_FALSE(status_only_allows(R"({"print":{"command":"get_version"}})"));
    // A status request carrying a second block is refused as a whole.
    CHECK_FALSE(status_only_allows(R"({"pushing":{"command":"pushall"},"print":{"command":"stop"}})"));
    // Malformed payloads.
    CHECK_FALSE(status_only_allows(""));
    CHECK_FALSE(status_only_allows("not json"));
    CHECK_FALSE(status_only_allows(R"({"pushing":"pushall"})"));
    CHECK_FALSE(status_only_allows(R"({"pushing":{"command":1}})"));
    CHECK_FALSE(status_only_allows(R"([{"pushing":{"command":"pushall"}}])"));
}

TEST_CASE("command_label names the command and nothing else", "[BambuConnectStatus]")
{
    CHECK(command_label(R"({"print":{"sequence_id":"7","command":"project_file","url":"ftp://192.168.1.5/x"}})") == "print/project_file");
    CHECK(command_label(R"({"system":{"command":"ledctrl"}})") == "system/ledctrl");
    CHECK(command_label("garbage") == "?");
    CHECK(command_label(R"({"print":{}})") == "?");
}

TEST_CASE("status-only flag round-trips through the app config", "[BambuConnectStatus]")
{
    AppConfig cfg;
    CHECK_FALSE(is_status_only(cfg, "01P00A000000001"));
    set_status_only(cfg, "01P00A000000001", true);
    CHECK(is_status_only(cfg, "01P00A000000001"));
    CHECK_FALSE(is_status_only(cfg, "01P00A000000002"));
    CHECK_FALSE(is_status_only(cfg, ""));
    set_status_only(cfg, "01P00A000000001", false);
    CHECK_FALSE(is_status_only(cfg, "01P00A000000001"));
    // Clearing a printer that was never set is harmless; an empty serial is ignored.
    set_status_only(cfg, "01P00A000000003", false);
    set_status_only(cfg, "", true);
    CHECK_FALSE(is_status_only(cfg, ""));
}

// ---------------------------------------------------------------------------------------------
// Snapshot
// ---------------------------------------------------------------------------------------------

TEST_CASE("snapshot JSON round-trips trays and nozzles", "[BambuConnectStatus]")
{
    const Snapshot in = sample_snapshot(1'800'000'000);
    Snapshot       out;
    REQUIRE(from_json(to_json(in), out));
    CHECK(out.saved_at == in.saved_at);
    CHECK(out.printer_model == in.printer_model);
    CHECK(out.printer_name == in.printer_name);
    REQUIRE(out.trays.size() == 3);
    CHECK(out.trays == in.trays);
    CHECK(out.trays.at(1).opt<ConfigOptionBools>("filament_exist")->values == std::vector<unsigned char>{ 0 });
    CHECK(out.trays.at(0).opt<ConfigOptionStrings>("filament_multi_colors")->values.empty());
    CHECK(out.trays.at(254).opt<ConfigOptionStrings>("tray_name")->values == std::vector<std::string>{ "Ext" });
    CHECK(out.extruders == in.extruders);
    CHECK(same_content(in, out));
}

TEST_CASE("snapshot keeps other option types and survives odd input", "[BambuConnectStatus]")
{
    Snapshot s;
    DynamicPrintConfig c;
    c.set_key_value("ints", new ConfigOptionInts{ 1, 2, 3 });
    c.set_key_value("floats", new ConfigOptionFloats{ 0.4, 0.6 });
    c.set_key_value("text", new ConfigOptionString("Caf\xC3\xA9 \"quoted\""));
    c.set_key_value("flag", new ConfigOptionBool(true));
    c.set_key_value("count", new ConfigOptionInt(7));
    c.set_key_value("ratio", new ConfigOptionFloat(1.5));
    s.trays.emplace(3, c);
    Snapshot out;
    REQUIRE(from_json(to_json(s), out));
    CHECK(out.trays == s.trays);

    CHECK_FALSE(from_json("", out));
    CHECK_FALSE(from_json("{}", out));
    CHECK_FALSE(from_json(R"({"trays":7})", out));
    // Unknown option types and broken trays are skipped, the rest is kept.
    REQUIRE(from_json(R"({"trays":[{"index":0,"options":{"a":{"t":"zz","v":1},"b":{"t":"ss","v":["x"]}}},{"options":{}}],"saved_at":5})", out));
    REQUIRE(out.trays.size() == 1);
    CHECK(out.trays.at(0).has("b"));
    CHECK_FALSE(out.trays.at(0).has("a"));
    CHECK(out.saved_at == 5);
}

TEST_CASE("same_content ignores the time and the names", "[BambuConnectStatus]")
{
    Snapshot a = sample_snapshot(100);
    Snapshot b = sample_snapshot(900);
    b.printer_name = "renamed";
    CHECK(same_content(a, b));
    b.trays.at(0).opt<ConfigOptionStrings>("filament_colour")->values = { "#123456" };
    CHECK_FALSE(same_content(a, b));
    Snapshot c = sample_snapshot(100);
    c.extruders[0].volume_type = 0;
    CHECK_FALSE(same_content(a, c));
}

TEST_CASE("store_snapshot writes on change and refreshes the time only now and then", "[BambuConnectStatus]")
{
    AppConfig cfg;
    const std::string dev = "0948AD000000001";
    Snapshot s = sample_snapshot(1000);
    CHECK(store_snapshot(cfg, dev, s));
    // Same content a minute later: not written again.
    s.saved_at = 1060;
    CHECK_FALSE(store_snapshot(cfg, dev, s));
    // Same content past the refresh interval: written, so "as of" moves on.
    s.saved_at = 1000 + 601;
    CHECK(store_snapshot(cfg, dev, s));
    // A changed tray is written at once.
    s.saved_at = 1000 + 602;
    s.trays.at(1).opt<ConfigOptionStrings>("filament_colour")->values = { "#ABCDEF" };
    CHECK(store_snapshot(cfg, dev, s));

    const auto all = load_snapshots(cfg);
    REQUIRE(all.count(dev) == 1);
    CHECK(all.at(dev).saved_at == 1602);
    CHECK(all.at(dev).trays.at(1).opt<ConfigOptionStrings>("filament_colour")->values == std::vector<std::string>{ "#ABCDEF" });

    // Nothing to keep: no serial, or no trays.
    CHECK_FALSE(store_snapshot(cfg, "", s));
    Snapshot empty;
    CHECK_FALSE(store_snapshot(cfg, "other", empty));
}

TEST_CASE("pick_snapshot prefers the selected printer, then the newest of the model", "[BambuConnectStatus]")
{
    std::map<std::string, Snapshot> all;
    Snapshot h2d_old = sample_snapshot(100);
    Snapshot h2d_new = sample_snapshot(500);
    Snapshot x1c     = sample_snapshot(900);
    x1c.printer_model = "Bambu Lab X1 Carbon";
    Snapshot no_trays = sample_snapshot(1000);
    no_trays.trays.clear();
    all["H2D-OLD"] = h2d_old;
    all["H2D-NEW"] = h2d_new;
    all["X1C"]     = x1c;
    all["EMPTY"]   = no_trays;

    CHECK(pick_snapshot(all, "H2D-OLD", "Bambu Lab H2D") == "H2D-OLD");
    // The selected printer is another model: the newest of the model being sliced for instead.
    CHECK(pick_snapshot(all, "X1C", "Bambu Lab H2D") == "H2D-NEW");
    CHECK(pick_snapshot(all, "X1C", "") == "X1C");
    CHECK(pick_snapshot(all, "", "Bambu Lab H2D") == "H2D-NEW");
    CHECK(pick_snapshot(all, "UNKNOWN", "Bambu Lab H2D") == "H2D-NEW");
    CHECK(pick_snapshot(all, "EMPTY", "Bambu Lab X1 Carbon") == "X1C");
    CHECK(pick_snapshot(all, "", "Bambu Lab A1") == "");
    CHECK(pick_snapshot(all, "", "") == "");
    Snapshot unknown_model = sample_snapshot(50);
    unknown_model.printer_model.clear();
    all["NOMODEL"] = unknown_model;
    CHECK(pick_snapshot(all, "NOMODEL", "Bambu Lab H2D") == "NOMODEL");
    CHECK(pick_snapshot({}, "H2D-OLD", "Bambu Lab H2D") == "");
}

TEST_CASE("time_label shows the time today and the date otherwise", "[BambuConnectStatus]")
{
    std::tm noon{};
    noon.tm_year  = 2026 - 1900;
    noon.tm_mon   = 9;
    noon.tm_mday  = 15;
    noon.tm_hour  = 12;
    noon.tm_min   = 0;
    noon.tm_isdst = -1;
    const std::time_t now = std::mktime(&noon);

    CHECK(time_label(now - 2 * 60 * 60 + 5 * 60, now) == "10:05");
    CHECK(time_label(now - 26 * 60 * 60, now) == "2026-10-14 10:00");
}

TEST_CASE("nozzle_volume_types_for maps physical nozzles to logical extruders", "[BambuConnectStatus]")
{
    const std::vector<ExtruderSnap> nozzles = { { 0, true, 1, 0.4f }, { 1, true, 0, 0.4f } };
    // H2D: logical 0 is the left (physical 1), logical 1 the right (physical 0).
    CHECK(nozzle_volume_types_for(nozzles, { 1, 0 }, { 0, 0 }) == std::vector<int>{ 0, 1 });
    // Identity map.
    CHECK(nozzle_volume_types_for(nozzles, { 0, 1 }, { 0, 0 }) == std::vector<int>{ 1, 0 });
    // A nozzle the printer did not report keeps the preset's value.
    const std::vector<ExtruderSnap> one_missing = { { 0, false, 1, 0.4f }, { 1, true, 1, 0.4f } };
    CHECK(nozzle_volume_types_for(one_missing, { 0, 1 }, { 0, 0 }) == std::vector<int>{ 0, 1 });
    // Single extruder; a short `current` is padded.
    CHECK(nozzle_volume_types_for({ { 0, true, 1, 0.4f } }, { 0 }, {}) == std::vector<int>{ 1 });
}
