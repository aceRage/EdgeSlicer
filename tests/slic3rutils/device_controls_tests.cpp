#include <catch2/catch.hpp>

#include "slic3r/GUI/DeviceControls.hpp"

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::DeviceControls;
using nlohmann::json;

// A U1's /printer/objects/query status, as mock_printhost / a real U1 (firmware 1.5.2) answer it,
// with the controls objects the phone's printer screen reads.
static json u1_status()
{
    return json::parse(R"({
        "heater_bed": {"temperature": 24.5, "target": 0.0},
        "extruder":  {"temperature": 25.0, "target": 0.0},
        "extruder1": {"temperature": 26.0, "target": 210.0},
        "extruder2": {"temperature": 25.0, "target": 0.0},
        "extruder3": {"temperature": 25.0, "target": 0.0},
        "gcode_move": {"speed_factor": 1.25, "speed": 1500.0, "extrude_factor": 1.0},
        "fan": {"speed": 0.5},
        "led cavity_led": {"color_data": [[0.0, 0.0, 0.0, 1.0]]},
        "fan_generic cavity_fan": {"speed": 0.0}
    })");
}

TEST_CASE("A U1's objects become its controls: bed, four toolheads, speed factor, light, two fans", "[DeviceControls]")
{
    const Caps caps = moonraker_caps(u1_status(), 100, 300, 4);
    REQUIRE(caps.heaters.size() == 5);
    CHECK(caps.heaters[0].id == "bed");
    CHECK(caps.heaters[0].max == 100);
    CHECK(caps.heaters[1].id == "nozzle0");
    CHECK(caps.heaters[1].label == "Nozzle 1");
    CHECK(caps.heaters[2].id == "nozzle1");
    CHECK(caps.heaters[2].target == Approx(210.0));
    CHECK(caps.heaters[4].label == "Nozzle 4");
    CHECK(caps.heaters[4].max == 300);
    for (const Heater& h : caps.heaters) CHECK(h.settable);

    REQUIRE(caps.has_speed);
    CHECK(caps.speed.kind == "factor");
    CHECK(caps.speed.value == 125);
    CHECK(caps.speed.min == SPEED_FACTOR_MIN);
    CHECK(caps.speed.max == SPEED_FACTOR_MAX);

    CHECK(caps.has_light);
    CHECK(caps.light_on);

    REQUIRE(caps.fans.size() == 2);
    CHECK(caps.fans[0].id == "part");
    CHECK(caps.fans[0].percent == 50);
    CHECK(caps.fans[1].id == "cavity");
    CHECK(caps.fans[1].percent == 0);
}

TEST_CASE("Only what the printer answered is a control", "[DeviceControls]")
{
    SECTION("a one-nozzle Klipper printer without light or speed report")
    {
        const Caps caps = moonraker_caps(json::parse(R"({"heater_bed":{"temperature":20,"target":0},
                                                          "extruder":{"temperature":21,"target":0},
                                                          "extruder1":{}})"));
        REQUIRE(caps.heaters.size() == 2);
        CHECK(caps.heaters[1].label == "Nozzle"); // a single nozzle is not numbered
        CHECK_FALSE(caps.has_speed);
        CHECK_FALSE(caps.has_light);
        CHECK(caps.fans.empty());
        const json j = to_json(caps);
        CHECK_FALSE(j.contains("speed"));
        CHECK_FALSE(j.contains("light"));
        CHECK(j["fans"].empty());
    }
    SECTION("an LED that is dark reads as off")
    {
        json st = u1_status();
        st["led cavity_led"]["color_data"] = json::parse("[[0,0,0,0]]");
        CHECK_FALSE(moonraker_caps(st).light_on);
    }
    SECTION("not an object at all")
    {
        CHECK(moonraker_caps(json()).heaters.empty());
    }
}

TEST_CASE("Bambu nozzles follow the Device tab's rows: one Nozzle, or L above R", "[DeviceControls]")
{
    SECTION("single nozzle (X1C / P1 / A1)")
    {
        const auto hs = bambu_nozzle_heaters({ { 0, 215, 220 } }, 1, 0, -1);
        REQUIRE(hs.size() == 1);
        CHECK(hs[0].id == "nozzle0");
        CHECK(hs[0].label == "Nozzle");
        CHECK(hs[0].max == DEFAULT_NOZZLE_MAX); // the printer reported no limit
    }
    SECTION("H2D / H2C: left (extruder 1) listed first, right (extruder 0) second, with its own limit")
    {
        const auto hs = bambu_nozzle_heaters({ { 0, 30, 0 }, { 1, 220, 220 } }, 2, 1, 350);
        REQUIRE(hs.size() == 2);
        CHECK(hs[0].id == "nozzle1");
        CHECK(hs[0].label == "L");
        CHECK(hs[0].active);
        CHECK(hs[1].id == "nozzle0");
        CHECK(hs[1].label == "R");
        CHECK(hs[1].max == 350);
    }
}

TEST_CASE("to_json is the documented `controls` shape", "[DeviceControls]")
{
    Caps caps;
    caps.heaters.push_back(Heater { "bed", "Bed", 60.2, 60, 110, true, false });
    caps.heaters.push_back(Heater { "chamber", "Chamber", 31, 0, 60, false, false });
    caps.has_speed = true;
    caps.speed     = Speed { "level", 2, 1, 4, false };
    caps.has_light = true;
    caps.light_on  = false;
    caps.fans.push_back(Fan { "aux", "Aux", 40 });
    const json j = to_json(caps);
    REQUIRE(j["heaters"].size() == 2);
    CHECK(j["heaters"][0]["id"] == "bed");
    CHECK(j["heaters"][0]["min"] == 0);
    CHECK(j["heaters"][0]["max"] == 110);
    CHECK(j["heaters"][1]["settable"] == false);
    CHECK(j["speed"]["kind"] == "level");
    CHECK(j["speed"]["settable"] == false);
    REQUIRE(j["speed"]["levels"].size() == 4);
    CHECK(j["speed"]["levels"][3]["label"] == "Ludicrous");
    CHECK(j["light"]["on"] == false);
    CHECK(j["fans"][0]["id"] == "aux");
    CHECK(j["fans"][0]["percent"] == 40);
}

TEST_CASE("set_temp: heater, limit, whole degrees, and the idle confirmation above 50 C", "[DeviceControls]")
{
    const Caps caps = moonraker_caps(u1_status(), 100, 300, 4);
    int         t   = -1;
    std::string why;

    CHECK(check_set_temp(caps, "bed", "60", true, false, t, why) == 0);
    CHECK(t == 60);
    CHECK(check_set_temp(caps, "nozzle3", "0", false, false, t, why) == 0); // off needs nothing
    CHECK(t == 0);
    CHECK(check_set_temp(caps, "nozzle1", "50", false, false, t, why) == 0); // at the line: no confirm
    CHECK(check_set_temp(caps, "bed", "101", true, false, t, why) == 400);
    CHECK(why.find("100") != std::string::npos);
    CHECK(check_set_temp(caps, "nozzle0", "301", true, true, t, why) == 400);
    CHECK(check_set_temp(caps, "bed", "60.5", true, false, t, why) == 400);
    CHECK(check_set_temp(caps, "bed", "-5", true, false, t, why) == 400);
    CHECK(check_set_temp(caps, "bed", "", true, false, t, why) == 400);
    CHECK(check_set_temp(caps, "chamber", "40", true, false, t, why) == 404); // the U1 has none
    CHECK(check_set_temp(caps, "", "40", true, false, t, why) == 400);

    SECTION("idle above 50 C: refused without confirm, accepted with it")
    {
        CHECK(check_set_temp(caps, "nozzle0", "210", false, false, t, why) == 400);
        CHECK(why.find("confirm") != std::string::npos);
        CHECK(check_set_temp(caps, "nozzle0", "210", true, false, t, why) == 0);
    }
    SECTION("printing: a change needs no confirm")
    {
        CHECK(check_set_temp(caps, "nozzle0", "215", false, true, t, why) == 0);
    }
    SECTION("a read-only heater is 409")
    {
        Caps x1;
        x1.heaters.push_back(Heater { "chamber", "Chamber", 30, 0, 60, false, false });
        CHECK(check_set_temp(x1, "chamber", "40", true, false, t, why) == 409);
    }
}

TEST_CASE("set_speed / set_light / set_fan checks", "[DeviceControls]")
{
    const Caps  u1 = moonraker_caps(u1_status(), 100, 300, 4);
    int         v  = 0;
    bool        on = false;
    std::string why;

    CHECK(check_set_speed(u1, "150", v, why) == 0);
    CHECK(v == 150);
    CHECK(check_set_speed(u1, "5", v, why) == 400);
    CHECK(check_set_speed(u1, "301", v, why) == 400);

    Caps bambu;
    bambu.has_speed = true;
    bambu.speed     = Speed { "level", 2, 1, 4, true };
    CHECK(check_set_speed(bambu, "4", v, why) == 0);
    CHECK(check_set_speed(bambu, "5", v, why) == 400);
    CHECK(check_set_speed(bambu, "0", v, why) == 400);
    bambu.speed.settable = false; // not printing
    CHECK(check_set_speed(bambu, "3", v, why) == 409);
    CHECK(check_set_speed(Caps(), "3", v, why) == 409);

    CHECK(check_set_light(u1, "1", on, why) == 0);
    CHECK(on);
    CHECK(check_set_light(u1, "0", on, why) == 0);
    CHECK_FALSE(on);
    CHECK(check_set_light(u1, "yes", on, why) == 400);
    CHECK(check_set_light(Caps(), "1", on, why) == 409);

    CHECK(check_set_fan(u1, "part", "70", v, why) == 0);
    CHECK(v == 70);
    CHECK(check_set_fan(u1, "part", "101", v, why) == 400);
    CHECK(check_set_fan(u1, "aux", "50", v, why) == 404); // the U1 has no aux fan
    CHECK(check_set_fan(u1, "", "50", v, why) == 400);
}

TEST_CASE("Klipper G-code for each control", "[DeviceControls]")
{
    // What the Snapmaker app's own device page sends for a toolhead.
    CHECK(moonraker_temp_script("nozzle0", 210) == "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=210");
    CHECK(moonraker_temp_script("nozzle3", 0) == "SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=0");
    CHECK(moonraker_temp_script("bed", 60) == "SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=60");
    CHECK(moonraker_temp_script("chamber", 40).empty());
    CHECK(moonraker_temp_script("nozzle", 40).empty());
    CHECK(moonraker_speed_script(125) == "M220 S125");
    CHECK(moonraker_light_script(true) == "SET_LED LED=cavity_led WHITE=1");
    CHECK(moonraker_light_script(false) == "SET_LED LED=cavity_led WHITE=0");
    CHECK(moonraker_fan_script("part", 100) == "M106 S255");
    CHECK(moonraker_fan_script("part", 50) == "M106 S128");
    CHECK(moonraker_fan_script("part", 0) == "M106 S0");
    CHECK(moonraker_fan_script("cavity", 40) == "SET_FAN_SPEED FAN=cavity_fan SPEED=0.40");
    CHECK(moonraker_fan_script("aux", 40).empty());
    CHECK(moonraker_controls_query() == "gcode_move&fan&led%20cavity_led&fan_generic%20cavity_fan");
}

TEST_CASE("Bambu fan values are the Device tab popup's ten steps", "[DeviceControls]")
{
    // FanControl::command_control_fan: floor(step * 25.5) for step 0..10.
    CHECK(bambu_fan_value(0) == 0);
    CHECK(bambu_fan_value(10) == 25);
    CHECK(bambu_fan_value(50) == 127);
    CHECK(bambu_fan_value(54) == 127); // rounds to step 5
    CHECK(bambu_fan_value(56) == 153); // rounds to step 6
    CHECK(bambu_fan_value(100) == 255);
    CHECK(bambu_fan_type("part") == 1);
    CHECK(bambu_fan_type("aux") == 2);
    CHECK(bambu_fan_type("chamber") == 3);
    CHECK(bambu_fan_type("cavity") == 0);
    CHECK(percent_of_byte(255) == 100);
    CHECK(percent_of_byte(127) == 50);
    CHECK(percent_of_byte(0) == 0);
}

TEST_CASE("Heater ids", "[DeviceControls]")
{
    CHECK(nozzle_index_of("nozzle0") == 0);
    CHECK(nozzle_index_of("nozzle12") == 12);
    CHECK(nozzle_index_of("nozzle") == -1);
    CHECK(nozzle_index_of("nozzleX") == -1);
    CHECK(nozzle_index_of("bed") == -1);
    CHECK(klipper_heater("nozzle1") == "extruder1");
    CHECK(klipper_heater("nozzle0") == "extruder");
    CHECK(klipper_heater("bed") == "heater_bed");
    CHECK(klipper_heater("chamber").empty());
    CHECK(rack_index_of("rack0") == 0);
    CHECK(rack_index_of("rack8") == 8);
    CHECK(rack_index_of("rack") == -1);
    CHECK(rack_index_of("nozzle0") == -1);
    CHECK(rack_id(3) == "rack3");
    CHECK(klipper_heater("rack0").empty()); // a rack position is never a Klipper heater
}

// ---- multi-tool temperatures, phase 1: the heater model (tests/design_device_multi_tool_temps.md) ----

// heaters[] as an installed phone app reads it: only the eight keys the controls JSON always had.
// The keys added later are taken off, so what is left can be compared with what main produced.
static json old_shape(const json& heaters)
{
    json out = json::array();
    for (json h : heaters) {
        for (const char* k : { "kind", "group", "present", "mounted", "state" }) h.erase(k);
        out.push_back(h);
    }
    return out;
}

// Klipper status answers, one per shape a printer can send.
static json klipper_fixture(int extruders)
{
    json st = json::object();
    st["heater_bed"] = { { "temperature", 60.5 }, { "target", 60.0 } };
    for (int i = 0; i < extruders; ++i)
        st[klipper_extruder_name(i)] = { { "temperature", 20.0 + i }, { "target", i == 1 ? 215.0 : 0.0 } };
    return st;
}

TEST_CASE("heaters[] keeps its old keys and values for every printer that had them", "[DeviceControls][ToolTemps]")
{
    SECTION("U1, four toolheads (main produced exactly this)")
    {
        const json expected = json::parse(R"([
            {"id":"bed","label":"Bed","temp":24.5,"target":0.0,"min":0,"max":100,"settable":true,"active":false},
            {"id":"nozzle0","label":"Nozzle 1","temp":25.0,"target":0.0,"min":0,"max":300,"settable":true,"active":false},
            {"id":"nozzle1","label":"Nozzle 2","temp":26.0,"target":210.0,"min":0,"max":300,"settable":true,"active":false},
            {"id":"nozzle2","label":"Nozzle 3","temp":25.0,"target":0.0,"min":0,"max":300,"settable":true,"active":false},
            {"id":"nozzle3","label":"Nozzle 4","temp":25.0,"target":0.0,"min":0,"max":300,"settable":true,"active":false}
        ])");
        const json j = to_json(moonraker_caps(u1_status(), 100, 300, 4));
        CHECK(old_shape(j["heaters"]) == expected);
        CHECK_FALSE(j.contains("rack"));
    }
    SECTION("one-nozzle Klipper printer (extruder1 answered empty)")
    {
        const json expected = json::parse(R"([
            {"id":"bed","label":"Bed","temp":20,"target":0,"min":0,"max":120,"settable":true,"active":false},
            {"id":"nozzle0","label":"Nozzle","temp":21,"target":0,"min":0,"max":300,"settable":true,"active":false}
        ])");
        const json j = to_json(moonraker_caps(json::parse(R"({"heater_bed":{"temperature":20,"target":0},
                                                              "extruder":{"temperature":21,"target":0},
                                                              "extruder1":{}})")));
        CHECK(old_shape(j["heaters"]) == expected);
    }
    SECTION("two-extruder Klipper printer (IDEX)")
    {
        const json expected = json::parse(R"([
            {"id":"bed","label":"Bed","temp":60.5,"target":60.0,"min":0,"max":120,"settable":true,"active":false},
            {"id":"nozzle0","label":"Nozzle 1","temp":20.0,"target":0.0,"min":0,"max":300,"settable":true,"active":false},
            {"id":"nozzle1","label":"Nozzle 2","temp":21.0,"target":215.0,"min":0,"max":300,"settable":true,"active":false}
        ])");
        CHECK(old_shape(to_json(moonraker_caps(klipper_fixture(2)))["heaters"]) == expected);
    }
    SECTION("four-extruder Klipper printer")
    {
        const json hs = old_shape(to_json(moonraker_caps(klipper_fixture(4)))["heaters"]);
        REQUIRE(hs.size() == 5);
        CHECK(hs[4] == json::parse(R"({"id":"nozzle3","label":"Nozzle 4","temp":23.0,"target":0.0,"min":0,"max":300,
                                       "settable":true,"active":false})"));
    }
    SECTION("Bambu X1C: bed, one nozzle, chamber sensor (rows as RemoteControl::bambu_caps builds them)")
    {
        const json expected = json::parse(R"([
            {"id":"bed","label":"Bed","temp":55.0,"target":55.0,"min":0,"max":110,"settable":true,"active":false},
            {"id":"nozzle0","label":"Nozzle","temp":220.0,"target":220.0,"min":0,"max":300,"settable":true,"active":false},
            {"id":"chamber","label":"Chamber","temp":35.0,"target":0.0,"min":0,"max":60,"settable":false,"active":false}
        ])");
        Caps caps;
        caps.heaters.push_back(Heater { "bed", "Bed", 55, 55, 110, true, false });
        for (Heater& h : bambu_nozzle_heaters({ { 0, 220, 220 } }, 1, 0, -1)) caps.heaters.push_back(h);
        caps.heaters.push_back(Heater { "chamber", "Chamber", 35, 0, DEFAULT_CHAMBER_MAX, false, false });
        const json j = to_json(caps);
        CHECK(old_shape(j["heaters"]) == expected);
        CHECK_FALSE(j.contains("rack"));
    }
    SECTION("Bambu H2D: L above R, the active one marked, settable chamber")
    {
        const json expected = json::parse(R"([
            {"id":"bed","label":"Bed","temp":60.0,"target":60.0,"min":0,"max":120,"settable":true,"active":false},
            {"id":"nozzle1","label":"L","temp":220.0,"target":220.0,"min":0,"max":350,"settable":true,"active":true},
            {"id":"nozzle0","label":"R","temp":30.0,"target":0.0,"min":0,"max":350,"settable":true,"active":false},
            {"id":"chamber","label":"Chamber","temp":40.0,"target":45.0,"min":0,"max":60,"settable":true,"active":false}
        ])");
        Caps caps;
        caps.heaters.push_back(Heater { "bed", "Bed", 60, 60, 120, true, false });
        for (Heater& h : bambu_nozzle_heaters({ { 0, 30, 0 }, { 1, 220, 220 } }, 2, 1, 350)) caps.heaters.push_back(h);
        caps.heaters.push_back(Heater { "chamber", "Chamber", 40, 45, DEFAULT_CHAMBER_MAX, true, false });
        CHECK(old_shape(to_json(caps)["heaters"]) == expected);
    }
}

TEST_CASE("The added heater keys: kind, group, present, mounted, state", "[DeviceControls][ToolTemps]")
{
    const json hs = to_json(moonraker_caps(u1_status(), 100, 300, 4))["heaters"];
    CHECK(hs[0]["kind"] == "bed");
    CHECK(hs[0]["group"] == "env");
    CHECK(hs[1]["kind"] == "nozzle");
    CHECK(hs[1]["group"] == "tool");
    for (const json& h : hs) {
        CHECK(h["present"] == true);
        CHECK(h["mounted"] == false);
        CHECK_FALSE(h.contains("state")); // only when the printer says so
    }

    Caps caps;
    caps.heaters.push_back(Heater { "chamber", "Chamber", 30, 0, 60, false, false });
    Heater cutter { "nozzle0", "Cutter", 0, 0, 300, true, true };
    cutter.kind = "cutter";
    caps.heaters.push_back(cutter);
    Heater warm { "rack2", "Rack 3", 180, 200, 350, false, false };
    warm.state = "heating";
    caps.rack.push_back(warm);
    const json j = to_json(caps);
    CHECK(j["heaters"][0]["kind"] == "chamber");
    CHECK(j["heaters"][0]["group"] == "env");
    SECTION("a tool that is not a heater has no reading and is never settable")
    {
        CHECK(j["heaters"][1]["kind"] == "cutter");
        CHECK(j["heaters"][1]["temp"].is_null());
        CHECK(j["heaters"][1]["target"].is_null());
        CHECK(j["heaters"][1]["settable"] == false);
    }
    SECTION("rack positions are their own array, with the rack group and the printer's state")
    {
        REQUIRE(j["rack"].size() == 1);
        CHECK(j["rack"][0]["id"] == "rack2");
        CHECK(j["rack"][0]["kind"] == "nozzle");
        CHECK(j["rack"][0]["group"] == "rack");
        CHECK(j["rack"][0]["state"] == "heating");
        CHECK(j["rack"][0]["temp"] == 180.0);
        CHECK(j["heaters"].size() == 2);
    }
    SECTION("an empty slot is not settable")
    {
        Caps e;
        Heater slot { "nozzle0", "Nozzle", 0, 0, 300, true, false };
        slot.present = false;
        e.heaters.push_back(slot);
        CHECK(to_json(e)["heaters"][0]["settable"] == false);
    }
}

TEST_CASE("set_temp refuses tools that are not heaters, empty slots and rack positions", "[DeviceControls][ToolTemps]")
{
    int         t = -1;
    std::string why;
    Caps        caps;
    Heater      laser { "nozzle0", "Laser", 0, 0, 300, true, true };
    laser.kind = "laser";
    caps.heaters.push_back(laser);
    Heater empty { "nozzle1", "L", 0, 0, 300, true, false };
    empty.present = false;
    caps.heaters.push_back(empty);
    caps.rack.push_back(Heater { "rack0", "Rack 1", 25, 0, 350, true, false });

    CHECK(check_set_temp(caps, "nozzle0", "40", true, false, t, why) == 409);
    CHECK(why.find("not a heater") != std::string::npos);
    CHECK(check_set_temp(caps, "nozzle1", "40", true, false, t, why) == 409);
    // Rack targets are read-only until they are proven on an H3C (D4): not a heater the route knows.
    CHECK(check_set_temp(caps, "rack0", "40", true, false, t, why) == 404);
}

TEST_CASE("One Klipper extruder walk: index order, gaps skipped, up to 16", "[DeviceControls][ToolTemps]")
{
    SECTION("a gap is skipped and later tools keep their numbers")
    {
        const json st = json::parse(R"({"extruder":{"temperature":30,"target":0},
                                        "extruder1":{},
                                        "extruder2":{"temperature":40,"target":200}})");
        const auto tools = klipper_extruders(st);
        REQUIRE(tools.size() == 3);
        CHECK(tools[1].index == 1);
        CHECK_FALSE(tools[1].has_temp);
        CHECK(tools[2].index == 2);
        CHECK(tools[2].target == 200.0);
        const Caps caps = moonraker_caps(st);
        REQUIRE(caps.heaters.size() == 2); // no bed in this answer
        CHECK(caps.heaters[0].id == "nozzle0");
        CHECK(caps.heaters[0].label == "Nozzle 1");
        CHECK(caps.heaters[1].id == "nozzle2");
        CHECK(caps.heaters[1].label == "Nozzle 3");
        CHECK(moonraker_temp_script(caps.heaters[1].id, 200) == "SET_HEATER_TEMPERATURE HEATER=extruder2 TARGET=200");
    }
    SECTION("six extruders are all listed (the old cap was four)")
    {
        const Caps caps = moonraker_caps(klipper_fixture(6));
        REQUIRE(caps.heaters.size() == 7);
        CHECK(caps.heaters[6].id == "nozzle5");
        CHECK(caps.heaters[6].label == "Nozzle 6");
    }
    SECTION("the sanity cap is 16, even when a caller asks for more")
    {
        const json st = klipper_fixture(20);
        CHECK(klipper_extruders(st).size() == 16);
        CHECK(klipper_extruders(st, 40).size() == 16);
        CHECK(klipper_extruders(st, 4).size() == 4);
        CHECK(moonraker_caps(st).heaters.size() == 17);
    }
    SECTION("a target that is not a number is reported as missing, not as zero")
    {
        const auto tools = klipper_extruders(json::parse(R"({"extruder":{"temperature":30,"target":"x"}})"));
        REQUIRE(tools.size() == 1);
        CHECK(tools[0].has_temp);
        CHECK_FALSE(tools[0].has_target);
    }
    SECTION("not an object, or no extruder at all")
    {
        CHECK(klipper_extruders(json()).empty());
        CHECK(klipper_extruders(json::parse(R"({"extruder":5})")).empty());
        CHECK(klipper_extruders(json::parse(R"({"heater_bed":{"temperature":1}})")).empty());
    }
    SECTION("the query names every extruder up to the cap")
    {
        CHECK(klipper_extruders_query(4) == "extruder&extruder1&extruder2&extruder3");
        const std::string q = klipper_extruders_query();
        CHECK(q.find("&extruder15") != std::string::npos);
        CHECK(q.find("extruder16") == std::string::npos);
    }
}
