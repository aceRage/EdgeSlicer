#include <catch2/catch.hpp>

#include "slic3r/Utils/PrintersMonitor.hpp"

#include <boost/filesystem.hpp>

#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::PrintersMonitor;
using json = nlohmann::json;

// The Printers tab (GUI/PrintersPanel.cpp + resources/web/orca/monitor.html): what the page may ask
// for, what it is given, and where "Open Device page" goes.

TEST_CASE("printers monitor: the page's messages", "[PrintersMonitor]")
{
    CHECK(parse_message("printers_get").kind == Message::Kind::Get);
    CHECK(parse_message("printers_groups_get").kind == Message::Kind::Groups);
    CHECK(parse_message("printers_tasks").kind == Message::Kind::Tasks);

    Message m = parse_message("printers_control:pause:0:01P00A123456789");
    CHECK(m.kind == Message::Kind::Control);
    CHECK(m.action == "pause");
    CHECK_FALSE(m.confirm);
    CHECK(m.id == "01P00A123456789");

    // Ids carry colons: everything after the second separator is the id.
    m = parse_message("printers_control:stop:1:sm:U1-0042");
    CHECK(m.kind == Message::Kind::Control);
    CHECK(m.action == "stop");
    CHECK(m.confirm);
    CHECK(m.id == "sm:U1-0042");

    m = parse_message("printers_open:ph:abc-1");
    CHECK(m.kind == Message::Kind::Open);
    CHECK(m.id == "ph:abc-1");

    m = parse_message("printers_job:17");
    CHECK(m.kind == Message::Kind::Job);
    CHECK(m.job == 17);
}

TEST_CASE("printers monitor: anything else is refused", "[PrintersMonitor]")
{
    for (const char* bad : { "", "printers_get ", "printers", "hub_start", "printers_control:pause:0:",
                             "printers_control:set_temp:0:abc", "printers_control:stop:yes:abc", "printers_control:pause",
                             "printers_control:pause:0:a b", "printers_control:pause:0:a\"b", "printers_control:pause:0:<x>",
                             "printers_open:", "printers_open:a'b", "printers_job:", "printers_job:0", "printers_job:-1",
                             "printers_job:1x", "printers_job:12345678901" })
        CHECK(parse_message(bad).kind == Message::Kind::Invalid);
    // The settings verbs exist on the phone, not on this tab (owner decision: Pause / Resume / Stop).
    CHECK_FALSE(valid_action("set_fan"));
    CHECK_FALSE(valid_action("resume_error"));
    CHECK(valid_action("resume"));
    CHECK_FALSE(valid_printer_id(std::string(201, 'a')));
    CHECK(valid_printer_id(std::string(200, 'a')));
    CHECK_FALSE(valid_printer_id("caf\xc3\xa9"));
}

TEST_CASE("printers monitor: answers are a script call with escaped JSON", "[PrintersMonitor]")
{
    CHECK(script_call("__printers", json { { "ok", true } }) == "if (window.__printers) window.__printers({\"ok\":true});");
    // A name that tries to end the script, and non-ASCII (U+2028 among it), stay inside the string.
    const std::string js = script_call("__printers", json { { "name", "a\"); alert(1); (\"\xe2\x80\xa8\xc3\xa9" } });
    CHECK(js.find("\\\"); alert(1)") != std::string::npos);
    CHECK(js.find("\\u2028") != std::string::npos);
    CHECK(js.find("\\u00e9") != std::string::npos);
    for (unsigned char c : js) CHECK(c < 0x80);
    // Broken UTF-8 does not throw.
    CHECK_FALSE(script_call("__printers", json { { "name", std::string("\xff\xfe") } }).empty());
    // Only identifiers are called.
    CHECK(script_call("alert(1);x", json()).empty());
    CHECK(script_call("", json()).empty());
    CHECK(script_call("1abc", json()).empty());
}

TEST_CASE("printers monitor: api_printers rows become the page's payload", "[PrintersMonitor]")
{
    const std::string body = R"({"printers":[
        {"id":"01P00A1","kind":"bambu","name":"X1C","percent":42,"ams":[{"id":"0","trays":[{"color":"#FF0000","type":"PLA"}]}],
         "controls":{"heaters":[{"id":"chamber","label":"Chamber","temp":31,"target":0}],"fans":[{"id":"part","label":"Part","percent":60}]}},
        {"id":"sm:U1-1","kind":"snapmaker","name":"U1","url":"http://192.168.1.20","toolheads":[{"index":0,"color":"#00FF00","loaded":true}]},
        {"id":"bad id","kind":"bambu"},
        {"name":"no id"},
        7
    ]})";
    const json p = page_payload(200, body, 1234);
    CHECK(p["ok"] == true);
    CHECK(p["at"] == 1234);
    REQUIRE(p["printers"].size() == 2);
    // The rows go through unchanged: the same schema as the hub's /summary.
    CHECK(p["printers"][0]["ams"][0]["trays"][0]["color"] == "#FF0000");
    CHECK(p["printers"][0]["controls"]["fans"][0]["percent"] == 60);
    CHECK(p["printers"][1]["toolheads"][0]["loaded"] == true);

    const json busy = page_payload(503, R"({"error":"the slicer is busy"})", 5);
    CHECK(busy["ok"] == false);
    CHECK(busy["error"] == "the slicer is busy");
    CHECK(busy["printers"].empty());
    CHECK(page_payload(500, "garbage", 5)["error"] == "the printer list is not available right now");
    CHECK(page_payload(200, "garbage", 5)["ok"] == false);
    CHECK(page_payload(200, R"({"printers":{}})", 5)["ok"] == false);
    CHECK(page_payload(200, R"({"printers":[]})", 5)["ok"] == true);
}

TEST_CASE("printers monitor: Open Device page, per vendor", "[PrintersMonitor]")
{
    const json rows = json::parse(R"J([
        {"id":"01P00A1","kind":"bambu"},
        {"id":"sm:U1-1","kind":"snapmaker","url":"http://192.168.1.20"},
        {"id":"ph:k1","kind":"printhost","url":"https://klipper.local:7125"},
        {"id":"host","kind":"printhost","url":"javascript:alert(1)"},
        {"id":"01P00A1","kind":"snapmaker","url":"http://dup"}
    ])J");
    const auto t = targets_of(rows);
    REQUIRE(t.size() == 4);
    CHECK(t.at("01P00A1").kind == "bambu"); // the first row with an id wins

    // Bambu: the Device tab while it is the Bambu one, else the page says why.
    CHECK(open_way(t.at("01P00A1"), true, false) == OpenWay::BambuMonitor);
    CHECK(open_way(t.at("01P00A1"), false, true) == OpenWay::NotHere);
    CHECK(not_here_reason(t.at("01P00A1")).find("Bambu Lab printer preset") != std::string::npos);
    // Snapmaker / Moonraker: the Device tab's web view when it is showing, else the browser.
    CHECK(open_way(t.at("sm:U1-1"), false, true) == OpenWay::PrinterWebView);
    CHECK(open_way(t.at("sm:U1-1"), true, false) == OpenWay::Browser);
    CHECK(open_way(t.at("ph:k1"), false, false) == OpenWay::Browser);
    // Never a non-web address.
    CHECK(open_way(t.at("host"), false, true) == OpenWay::NotHere);

    CHECK(is_web_url("http://192.168.1.20"));
    CHECK(is_web_url("https://printer.local:7125/"));
    CHECK_FALSE(is_web_url("http://"));
    CHECK_FALSE(is_web_url("file:///c:/x"));
    CHECK_FALSE(is_web_url("http://a b"));
    CHECK_FALSE(is_web_url("http://a\"onload=x"));
}

TEST_CASE("printers monitor: groups", "[PrintersMonitor]")
{
    // The old Multi-device pick list seeds the first group.
    auto seeded = seed_from_multi_devices({ "01P00A1", "", " 01P00A2 ", "01P00A1", "", "" });
    REQUIRE(seeded.size() == 1);
    CHECK(seeded[0].name == "Multi-device");
    CHECK(seeded[0].printers == std::vector<std::string> { "01P00A1", "01P00A2" });
    CHECK(seed_from_multi_devices({ "", "", "" }).empty());

    // Round trip.
    const std::string saved = save_groups(seeded);
    auto back = parse_groups(saved);
    REQUIRE(back.size() == 1);
    CHECK(back[0].id == seeded[0].id);
    CHECK(back[0].printers == seeded[0].printers);

    // Bad entries are skipped; "all" is reserved; duplicates keep the first; a missing name is the id.
    back = parse_groups(R"([{"id":"all","name":"x"},{"id":"farm","printers":["a","a","b c","b"]},{"id":"farm","name":"dup"},{"name":"no id"},3])");
    REQUIRE(back.size() == 1);
    CHECK(back[0].name == "farm");
    CHECK(back[0].printers == std::vector<std::string> { "a", "b" });
    CHECK(parse_groups("").empty());
    CHECK(parse_groups("not json").empty());

    const json g = groups_json(back);
    REQUIRE(g.size() == 2);
    CHECK(g[0]["id"] == "all");
    CHECK(g[0]["all"] == true);
    CHECK(g[1]["id"] == "farm");
}

TEST_CASE("printers monitor: printer pictures and the model line", "[PrintersMonitor]")
{
    const CoverIndex idx = index_covers({ { "BBL", "Bambu Lab H2C_cover.png" },
                                          { "BBL", "Bambu Lab H2C_bed.stl" },
                                          { "Snapmaker", "Snapmaker U1_cover.png" },
                                          { "Snapmaker", "Snapmaker A350 QS+B Kit_cover.png" },
                                          { "Other", "Snapmaker U1_cover.png" } });
    CHECK(idx.size() == 3);
    CHECK(cover_for(idx, "Bambu Lab H2C") == "/profiles/BBL/Bambu%20Lab%20H2C_cover.png");
    CHECK(cover_for(idx, "snapmaker u1") == "/profiles/Snapmaker/Snapmaker%20U1_cover.png"); // first vendor wins, any case
    CHECK(cover_for(idx, "Snapmaker A350 QS+B Kit") == "/profiles/Snapmaker/Snapmaker%20A350%20QS%2BB%20Kit_cover.png");
    CHECK(cover_for(idx, "Voron 2.4").empty());
    CHECK(cover_for(idx, "").empty());

    CHECK(short_model_name("bambu", "Bambu Lab H2C", "O1C2") == "H2C");
    CHECK(short_model_name("bambu", "", "O1C2") == "O1C2"); // unknown code: the code
    CHECK(short_model_name("snapmaker", "Snapmaker U1", "Snapmaker U1") == "Snapmaker U1");

    json rows = json::parse(R"J([
        {"id":"01P00A1","kind":"bambu","model":"O1C2","model_name":"Bambu Lab H2C","task":"Benchy"},
        {"id":"01P00A2","kind":"bambu","model":"O9Z9"},
        {"id":"sm:U1-1","kind":"snapmaker","model":"Snapmaker U1","task":"gear.gcode"}
    ])J");
    enrich_rows(rows, idx, { { "sm:U1-1", "20261007-gear" } });
    CHECK(rows[0]["model_short"] == "H2C");
    CHECK(rows[0]["picture"] == "/profiles/BBL/Bambu%20Lab%20H2C_cover.png");
    CHECK_FALSE(rows[0].contains("thumb_id"));
    CHECK(rows[1]["model_short"] == "O9Z9");
    CHECK_FALSE(rows[1].contains("picture")); // a Bambu code is never looked up as a name
    CHECK(rows[2]["picture"] == "/profiles/Snapmaker/Snapmaker%20U1_cover.png");
    CHECK(rows[2]["thumb_id"] == "20261007-gear");
}

TEST_CASE("printers monitor: the running job's plate picture from the archive", "[PrintersMonitor]")
{
    const std::vector<ArchiveEntry> entries = {
        { "01P00A1", "a-old", "Benchy", "benchy.gcode.3mf", 100 },
        { "01P00A1", "a-new", "Benchy", "benchy.gcode.3mf", 200 },
        { "01P00A1", "a-other", "Bracket", "bracket.3mf", 300 },     // newer, but not the running job
        { "sm:U1-1", "u-1", "", "gear.gcode", 50 },                // matched by file name
        { "ph:k1", "../etc", "x", "x", 1 },                        // not an archive id
    };
    const auto picks = pick_thumbnails(entries, { { "01P00A1", "Benchy" }, { "sm:U1-1", "gear.gcode" }, { "ph:k1", "x" }, { "idle", "" } });
    CHECK(picks.size() == 2);
    CHECK(picks.at("01P00A1") == "a-new");
    CHECK(picks.at("sm:U1-1") == "u-1");
    CHECK(pick_thumbnails(entries, {}).empty());

    CHECK(row_job(json { { "task", "A" }, { "subtask_name", "B" } }) == "A");
    CHECK(row_job(json { { "subtask_name", "B" } }) == "B");
    CHECK(row_job(json::array()) == "");

    CHECK(parse_message("printers_thumb:20261007-101500-benchy").kind == Message::Kind::Thumb);
    CHECK(parse_message("printers_thumb:20261007-101500-benchy").id == "20261007-101500-benchy");
    for (const char* bad : { "printers_thumb:", "printers_thumb:../x", "printers_thumb:.hidden", "printers_thumb:a/b", "printers_thumb:a b" })
        CHECK(parse_message(bad).kind == Message::Kind::Invalid);
}

TEST_CASE("printers monitor: the shared device page (owner D6)", "[PrintersMonitor]")
{
    // A Centauri Carbon 2 row (it carries "cc2") is drawn by the shared device page; others keep theirs.
    const json rows = json::parse(R"([
        {"id":"ph:cc2a","kind":"printhost","url":"http://192.168.1.40","cc2":{"protocol":"mqtt"}},
        {"id":"ph:k1","kind":"printhost","url":"http://192.168.1.41"},
        {"id":"ph:new","kind":"printhost","device_page":true},
        {"id":"01P00A1","kind":"bambu"}])");
    CHECK(uses_device_page(rows[0]));
    CHECK_FALSE(uses_device_page(rows[1]));
    CHECK(uses_device_page(rows[2]));
    CHECK_FALSE(uses_device_page(json::parse(R"({"id":"x","cc2":"not an object"})")));
    const auto t = targets_of(rows);
    CHECK(t.at("ph:cc2a").device_page);
    CHECK_FALSE(t.at("ph:k1").device_page);
    // Opened in the Device tab's web view; with the view not there, the old rules (its address).
    CHECK(open_way(t.at("ph:cc2a"), false, true) == OpenWay::DevicePage);
    CHECK(open_way(t.at("ph:cc2a"), true, false) == OpenWay::Browser);
    CHECK(open_way(t.at("ph:new"), false, true) == OpenWay::DevicePage);
    CHECK(open_way(t.at("ph:new"), false, false) == OpenWay::NotHere);
    CHECK(open_way(t.at("ph:k1"), false, true) == OpenWay::PrinterWebView);

    // The page's address names the row, percent-encoded, and reads back the same id.
    CHECK(device_page_path("ph:cc2a", true) == "/web/orca/device.html?embed=1&theme=dark&id=ph%3Acc2a");
    CHECK(device_page_path("host", false) == "/web/orca/device.html?embed=1&theme=light&id=host");
    CHECK(device_page_path("bad id", true).empty());
    CHECK(device_page_path("", true).empty());
    CHECK(device_page_id("http://127.0.0.1:13618/web/orca/device.html?embed=1&theme=dark&id=ph%3Acc2a") == "ph:cc2a");
    // __edgeTheme rewrites ?theme= in place; the id is still found wherever it is.
    CHECK(device_page_id("http://127.0.0.1:1/web/orca/device.html?id=host&embed=1&theme=light#x") == "host");
    CHECK(device_page_id("http://127.0.0.1:1/web/orca/monitor.html?id=host").empty());
    CHECK(device_page_id("http://192.168.1.40/web/orca/device.html?id=%22%3E").empty()); // decodes to a refused id
    CHECK(device_page_id("http://127.0.0.1:1/web/orca/device.html?embed=1").empty());
}

TEST_CASE("printers monitor: printers_forget names one printer", "[PrintersMonitor]")
{
    Message m = parse_message("printers_forget:sm:U1-0042");
    CHECK(m.kind == Message::Kind::Forget);
    CHECK(m.id == "sm:U1-0042");
    CHECK(parse_message("printers_forget:").kind == Message::Kind::Invalid);
    CHECK(parse_message("printers_forget:a b").kind == Message::Kind::Invalid);
    CHECK(parse_message("printers_forget:<x>").kind == Message::Kind::Invalid);
}

TEST_CASE("printers monitor: leftovers from the hub are added, marked and stripped of controls", "[PrintersMonitor]")
{
    json payload = page_payload(200, R"({"printers":[{"id":"sm:U1-0042","kind":"snapmaker","online":false}]})", 1);
    const std::string hub = R"({"printers":[
        {"id":"sm:U1-0042","kind":"snapmaker","configured":true,"age_s":5},
        {"id":"sm:TEST-MOCK","kind":"snapmaker","name":"Mock U1","configured":false,"stale":true,"age_s":7200,
         "can_pause":true,"controls":{"light":{"on":true}}},
        {"id":"ph:old","kind":"printhost"},
        {"id":"bad id","kind":"printhost","configured":false}
    ]})";
    add_leftovers(payload, 200, hub);
    REQUIRE(payload["printers"].size() == 2);
    const json& p = payload["printers"][1];
    CHECK(p["id"] == "sm:TEST-MOCK");
    CHECK(p["leftover"] == true);
    CHECK(p["online"] == false);
    CHECK(p["seen_s"] == 7200);
    CHECK_FALSE(p.contains("can_pause"));
    CHECK_FALSE(p.contains("controls"));
    CHECK_FALSE(payload["printers"][0].contains("leftover")); // the slicer's own row is untouched
}

TEST_CASE("printers monitor: no leftovers without an ok payload or an ok hub", "[PrintersMonitor]")
{
    const std::string hub = R"({"printers":[{"id":"sm:TEST-MOCK","configured":false}]})";
    json failed = page_payload(503, "", 1);
    add_leftovers(failed, 200, hub);
    CHECK(failed["printers"].empty());
    json ok = page_payload(200, R"({"printers":[]})", 1);
    add_leftovers(ok, 503, hub);
    CHECK(ok["printers"].empty());
    add_leftovers(ok, 200, "not json");
    CHECK(ok["printers"].empty());
}

// ---- "Remove printer": which stores a row id touches --------------------------------------------

TEST_CASE("printers monitor: Remove plan per kind of row", "[PrintersMonitor][remove]")
{
    using S = std::vector<std::string>;
    // A saved print-host device (Qidi / Elegoo / Creality / FlashForge / Klipper).
    RemovalPlan ph = removal_plan(json{ { "id", "ph:abc123" }, { "kind", "printhost" }, { "model_key", "Qidi X-Max 4" },
                                        { "device_id", "abc123" }, { "ip", "127.0.0.32" } });
    CHECK(ph.mode == RemovalPlan::Mode::Remove);
    CHECK(ph.ph_model_key == "Qidi X-Max 4");
    CHECK(ph.ph_device_id == "abc123");
    CHECK(ph.stores() == S{ "print_host_devices", "tombstone", "hub_printers" });
    // An id with no device_id field still names the device.
    CHECK(removal_plan(json{ { "id", "ph:zzz" }, { "kind", "printhost" } }).ph_device_id == "zzz");

    // A LAN Snapmaker: its list, the Device tab's saved devices, a tombstone, the hub row.
    RemovalPlan sm = removal_plan(json{ { "id", "sm:U1-8110026" }, { "kind", "snapmaker" }, { "ip", "192.168.1.40" } });
    CHECK(sm.mode == RemovalPlan::Mode::Remove);
    CHECK(sm.sm_id == "U1-8110026");
    CHECK(sm.sm_ip == "192.168.1.40");
    CHECK(sm.stores() == S{ "snapmaker_lan", "app_config_devices", "tombstone", "hub_printers" });

    // Bambu in LAN mode: Log out. Bound to the cloud: explained, nothing touched.
    RemovalPlan lan = removal_plan(json{ { "id", "01P00A000000001" }, { "kind", "bambu" }, { "lan_mode", true } });
    CHECK(lan.mode == RemovalPlan::Mode::Logout);
    CHECK(lan.stores() == S{ "bambu_lan", "tombstone", "hub_printers" });
    RemovalPlan cloud = removal_plan(json{ { "id", "01P00A000000002" }, { "kind", "bambu" }, { "lan_mode", false } });
    CHECK(cloud.mode == RemovalPlan::Mode::Explain);
    CHECK(cloud.stores().empty());
    CHECK(cloud.reason.find("Bambu Lab account") != std::string::npos);

    // The preset's own host and the Device tab connection are explained, not removed.
    CHECK(removal_plan(json{ { "id", "host" }, { "kind", "printhost" } }).mode == RemovalPlan::Mode::Explain);
    CHECK(removal_plan(json{ { "id", "connect" }, { "kind", "connect" } }).mode == RemovalPlan::Mode::Explain);

    // A leftover (only the hub remembers it): the hub row and nothing else, whatever its kind says.
    RemovalPlan left = removal_plan(json{ { "id", "sm:GONE" }, { "kind", "snapmaker" }, { "leftover", true } });
    CHECK(left.mode == RemovalPlan::Mode::Remove);
    CHECK(left.stores() == S{ "hub_printers" });

    // No id: nothing at all.
    CHECK(removal_plan(json{ { "kind", "snapmaker" } }).stores().empty());
}

TEST_CASE("printers monitor: every card is told what Remove will do", "[PrintersMonitor][remove]")
{
    json rows = json::array({ json{ { "id", "sm:U1" }, { "kind", "snapmaker" } },
                              json{ { "id", "01P00A000000001" }, { "kind", "bambu" }, { "lan_mode", true } },
                              json{ { "id", "01P00A000000002" }, { "kind", "bambu" } } });
    annotate_removal(rows);
    CHECK(rows[0]["remove"]["mode"] == "remove");
    CHECK(rows[1]["remove"]["mode"] == "logout");
    CHECK(rows[2]["remove"]["mode"] == "explain");
    CHECK(rows[2]["remove"].contains("reason"));
}

TEST_CASE("printers monitor: printers_remove and printers_rename messages", "[PrintersMonitor][remove]")
{
    Message m = parse_message("printers_remove:sm:U1-0042");
    CHECK(m.kind == Message::Kind::Remove);
    CHECK(m.id == "sm:U1-0042");
    CHECK(parse_message("printers_remove:").kind == Message::Kind::Invalid);
    m = parse_message(R"(printers_rename:{"id":"sm:U1-0042","name":"  Left U1  "})");
    CHECK(m.kind == Message::Kind::Rename);
    CHECK(m.id == "sm:U1-0042");
    CHECK(m.name == "Left U1");
    m = parse_message(R"(printers_rename:{"id":"sm:U1-0042","name":""})");
    CHECK(m.kind == Message::Kind::Rename);
    CHECK(m.name.empty());
    CHECK(parse_message(R"(printers_rename:{"id":"a b","name":"x"})").kind == Message::Kind::Invalid);
    CHECK(parse_message("printers_rename:not json").kind == Message::Kind::Invalid);
}

// ---- the tombstone ---------------------------------------------------------------------------

TEST_CASE("printers monitor: a removed printer is tombstoned by id or address until it is added again", "[PrintersMonitor][remove]")
{
    json store = json::object();
    CHECK_FALSE(tombstoned(store, "sm:U1-0042", "192.168.1.40"));
    add_tombstone(store, "sm:U1-0042", "192.168.1.40", 1000);
    CHECK(tombstoned(store, "sm:U1-0042", ""));            // discovery by serial
    CHECK(tombstoned(store, "sm:OTHER", "192.168.1.40"));  // a source that only knows the address
    CHECK_FALSE(tombstoned(store, "sm:OTHER", "192.168.1.41"));
    add_tombstone(store, "sm:U1-0042", "192.168.1.40", 2000); // removed twice: still one entry
    CHECK(store["removed"].size() == 1);
    CHECK(clear_tombstone(store, "sm:U1-0042", "192.168.1.40"));
    CHECK_FALSE(tombstoned(store, "sm:U1-0042", "192.168.1.40"));
    CHECK_FALSE(clear_tombstone(store, "sm:U1-0042", ""));
    for (int i = 0; i < 300; ++i) add_tombstone(store, "sm:T" + std::to_string(i), "", i);
    CHECK(store["removed"].size() == 256);
    CHECK(tombstoned(store, "sm:T299", ""));
    CHECK_FALSE(tombstoned(store, "sm:T0", ""));
}

TEST_CASE("printers monitor: the tombstone and alias files survive a restart", "[PrintersMonitor][remove]")
{
    const std::string path = (boost::filesystem::temp_directory_path() /
                              boost::filesystem::unique_path("pm_store_%%%%%%%%.json")).string();
    CHECK(load_tombstones(path).empty()); // missing file: no tombstones
    json store = json::object();
    add_tombstone(store, "sm:U1-0042", "192.168.1.40", 1000);
    REQUIRE(save_tombstones(path, store));
    REQUIRE(save_tombstones(path, store)); // over an existing file
    CHECK(tombstoned(load_tombstones(path), "sm:U1-0042", ""));
    json aliases = json::object();
    REQUIRE(set_alias(aliases, "sm:U1-0042", "Left U1"));
    REQUIRE(save_store_file(path, aliases));
    CHECK(alias_of(load_store_file(path), "sm:U1-0042") == "Left U1");
    boost::system::error_code ec;
    boost::filesystem::remove(path, ec);
}

// ---- "Rename" --------------------------------------------------------------------------------

TEST_CASE("printers monitor: aliases are cleaned, set, cleared and put on the rows", "[PrintersMonitor][rename]")
{
    CHECK(clean_alias("  Left U1  ") == "Left U1");
    CHECK(clean_alias(std::string("A") + char(7) + "B") == "AB");
    CHECK(clean_alias(std::string(100, 'x')).size() == 64);
    CHECK(clean_alias("   ").empty());

    json store;
    CHECK_FALSE(set_alias(store, "bad id", "x"));
    REQUIRE(set_alias(store, "sm:U1-8110026", "Left U1"));
    REQUIRE(set_alias(store, "ph:abc", "Qidi in the shed"));
    json rows = json::array({ json{ { "id", "sm:U1-8110026" }, { "name", "U1-8110026" } },
                              json{ { "id", "ph:abc" }, { "name", "Test" }, { "alias", "Test" } },
                              json{ { "id", "01P00A000000001" }, { "name", "X1C" } } });
    apply_aliases(rows, store);
    CHECK(rows[0]["name"] == "Left U1");
    CHECK(rows[0]["custom_name"] == "Left U1");
    CHECK(rows[0]["reported_name"] == "U1-8110026");
    CHECK(rows[1]["name"] == "Qidi in the shed");
    CHECK(rows[1]["alias"] == "Test"); // the device's own alias field is left alone
    CHECK(rows[2]["name"] == "X1C");
    CHECK_FALSE(rows[2].contains("custom_name"));

    // An empty name: back to the printer's own.
    REQUIRE(set_alias(store, "sm:U1-8110026", ""));
    CHECK(alias_of(store, "sm:U1-8110026").empty());
    json again = json::array({ json{ { "id", "sm:U1-8110026" }, { "name", "U1-8110026" } } });
    apply_aliases(again, store);
    CHECK(again[0]["name"] == "U1-8110026");
    CHECK_FALSE(again[0].contains("custom_name"));
}

TEST_CASE("printers monitor: the remove kind a client words its button by", "[PrintersMonitor][remove]")
{
    CHECK(removal_json(json{ { "id", "ph:a" }, { "kind", "printhost" } })["kind"] == "print_host_device");
    CHECK(removal_json(json{ { "id", "sm:a" }, { "kind", "snapmaker" } })["kind"] == "lan_snapmaker");
    CHECK(removal_json(json{ { "id", "01P00A000000001" }, { "kind", "bambu" }, { "lan_mode", true } })["kind"] == "bambu_lan");
    const json cloud = removal_json(json{ { "id", "01P00A000000002" }, { "kind", "bambu" } });
    CHECK(cloud["mode"] == "explain");
    CHECK(cloud["kind"] == "bambu_cloud");
    CHECK(removal_json(json{ { "id", "host" } })["kind"] == "preset_host");
    CHECK(removal_json(json{ { "id", "connect" }, { "kind", "connect" } })["kind"] == "device_connection");
    CHECK(removal_json(json{ { "id", "sm:x" }, { "leftover", true } })["kind"] == "leftover");
}

TEST_CASE("printers monitor: the report leaves tombstoned printers out", "[PrintersMonitor][remove]")
{
    json store = json::object();
    add_tombstone(store, "ph:qidi1", "", 1);
    add_tombstone(store, "sm:U1-8110026", "192.168.1.40", 1);
    add_tombstone(store, "01P00A000000001", "", 1);
    json rows = json::array({ json{ { "id", "ph:qidi1" }, { "ip", "127.0.0.32" } },
                              json{ { "id", "ph:keep" }, { "ip", "127.0.0.33" } },
                              json{ { "id", "sm:U1-8110026" }, { "ip", "192.168.1.40" } },
                              // the same U1 under an address-derived id (discovery without its serial)
                              json{ { "id", "sm:192.168.1.40" }, { "ip", "192.168.1.40" } },
                              json{ { "id", "01P00A000000001" }, { "kind", "bambu" } },
                              // a print host on the removed U1's old address is a different printer
                              json{ { "id", "ph:other" }, { "ip", "192.168.1.40" } } });
    CHECK(filter_tombstoned(rows, store) == 4);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0]["id"] == "ph:keep");
    CHECK(rows[1]["id"] == "ph:other");
    json none = json::array({ json{ { "id", "a" } } });
    CHECK(filter_tombstoned(none, json::object()) == 0);
    CHECK(none.size() == 1);
}
