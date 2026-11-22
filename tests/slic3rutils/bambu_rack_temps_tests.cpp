#include <catch2/catch.hpp>

#include "slic3r/GUI/BambuRackTemps.hpp"

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::BambuRackTemps;
using nlohmann::json;

// Fixtures: device.nozzle blocks in the layout Bambu Studio's DevNozzleSystemParser reads (v02.09.01.70,
// DevNozzleSystem.cpp). No captured H3C report with "temp" on rack entries exists yet, so these are
// derived from the parser, not recorded from a printer; the H3C hand test confirms them.

static int temp_word(int current, int target) { return current | (target << 16); }

// What MachineObject::parse_new_info keeps of each entry (id, addr_id, the exist bit, then
// parse_nozzle_temp), in the same order and with the same tolerance.
static std::vector<NozzleSample> samples_of(const json& nozzle)
{
    std::vector<NozzleSample> out;
    for (const json& e : nozzle["info"]) {
        NozzleSample s;
        s.id      = e.value("id", 0);
        s.addr_id = e.contains("addr_id") && e["addr_id"].is_number_integer() ? e["addr_id"].get<int>() : -1;
        if (nozzle.contains("exist") && nozzle["exist"].is_number_integer() && s.id >= 0 && s.id < 31)
            s.exist = ((nozzle["exist"].get<long long>() >> s.id) & 1) != 0;
        parse_nozzle_temp(e, s.temp, s.target, s.has_temp, s.temp_state);
        out.push_back(s);
    }
    return out;
}

// An H3C: the extruder holds the nozzle from rack position 2; position 0 heats to 200 C, 1 is idle,
// 3 is empty (its exist bit is clear).
static json h3c_nozzle()
{
    json n;
    n["exist"] = (1 << 0) | (1 << 16) | (1 << 17) | (1 << 18);
    n["state"] = 0;
    n["info"]  = json::array({
        { { "id", 0 }, { "type", "HS01" }, { "diameter", 0.4 }, { "addr_id", 2 }, { "temp", temp_word(220, 220) }, { "stat", 0 } },
        { { "id", 16 }, { "type", "HS01" }, { "diameter", 0.4 }, { "addr_id", 0 }, { "temp", temp_word(181, 200) }, { "stat", 1 << 3 } },
        { { "id", 17 }, { "type", "HH01" }, { "diameter", 0.6 }, { "addr_id", 1 }, { "temp", temp_word(26, 0) }, { "stat", 0 } },
        { { "id", 19 }, { "type", "" }, { "addr_id", 3 } },
    });
    return n;
}

TEST_CASE("H3C rack: one entry per position, in position order, read-only", "[BambuRackTemps][ToolTemps]")
{
    const auto rack = rack_heaters(samples_of(h3c_nozzle()), true, 350);
    REQUIRE(rack.size() == 4);

    CHECK(rack[0].id == "rack0");
    CHECK(rack[0].label == "Rack 1");
    CHECK(rack[0].temp == 181);
    CHECK(rack[0].target == 200);
    CHECK(rack[0].state == "heating");
    CHECK(rack[0].max == 350);
    CHECK(rack[0].present);
    CHECK_FALSE(rack[0].mounted);

    CHECK(rack[1].id == "rack1");
    CHECK(rack[1].temp == 26);
    CHECK(rack[1].state == "idle");

    SECTION("the position whose nozzle is on the extruder is mounted, with no reading of its own")
    {
        CHECK(rack[2].id == "rack2");
        CHECK(rack[2].mounted);
        CHECK(rack[2].present);
        CHECK_FALSE(rack[2].has_temp);
    }
    SECTION("an empty position is listed as empty, with no reading")
    {
        CHECK(rack[3].id == "rack3");
        CHECK_FALSE(rack[3].present);
        CHECK_FALSE(rack[3].has_temp);
        CHECK(rack[3].state.empty());
    }
    for (const auto& h : rack) {
        CHECK_FALSE(h.settable); // D4: no rack target until the H3C hand test
        CHECK(h.group == "rack");
    }
}

TEST_CASE("H3C rack in the controls JSON: heaters[] untouched, rack[] beside it", "[BambuRackTemps][ToolTemps]")
{
    using namespace DeviceControls;
    Caps caps;
    caps.heaters.push_back(Heater { "bed", "Bed", 25, 0, 120, true, false });
    for (Heater& h : bambu_nozzle_heaters({ { 0, 220, 220 } }, 1, 0, 350)) caps.heaters.push_back(h);
    const json before = to_json(caps);
    caps.rack = rack_heaters(samples_of(h3c_nozzle()), true, 350);
    const json j = to_json(caps);

    CHECK(j["heaters"] == before["heaters"]);
    REQUIRE(j["rack"].size() == 4);
    CHECK(j["rack"][0]["temp"] == 181.0);
    CHECK(j["rack"][0]["settable"] == false);
    CHECK(j["rack"][0]["kind"] == "nozzle");
    CHECK(j["rack"][0]["group"] == "rack");
    CHECK(j["rack"][0]["state"] == "heating");
    CHECK(j["rack"][2]["mounted"] == true);
    CHECK(j["rack"][2]["temp"].is_null());
    CHECK(j["rack"][3]["present"] == false);
    CHECK(j["rack"][3]["temp"].is_null());

    int         t = -1;
    std::string why;
    CHECK(check_set_temp(caps, "rack0", "60", true, false, t, why) == 404);
    CHECK(check_set_temp(caps, "nozzle0", "60", true, false, t, why) == 0); // the extruder row is as before
}

TEST_CASE("Printers that send no rack temperature keep the JSON they had", "[BambuRackTemps][ToolTemps]")
{
    SECTION("H2C: rack entries without temp")
    {
        json n;
        n["exist"] = 0x3F0003;
        n["info"]  = json::array();
        n["info"].push_back({ { "id", 0 }, { "type", "HS01" }, { "diameter", 0.4 } });
        n["info"].push_back({ { "id", 1 }, { "type", "HS01" }, { "diameter", 0.4 } });
        for (int i = 0; i < 6; ++i) n["info"].push_back({ { "id", 0x10 + i }, { "type", "HS01" }, { "diameter", 0.4 }, { "stat", 0 } });
        CHECK(rack_heaters(samples_of(n), false, 350).empty());
        DeviceControls::Caps caps;
        caps.rack = rack_heaters(samples_of(n), false, 350);
        CHECK_FALSE(DeviceControls::to_json(caps).contains("rack"));
    }
    SECTION("H2D: two extruder nozzles, even if they carried a temp field")
    {
        json n;
        n["info"] = json::array({ { { "id", 0 }, { "type", "HS01" }, { "temp", temp_word(200, 200) } },
                                  { { "id", 1 }, { "type", "HS01" }, { "temp", temp_word(30, 0) } } });
        CHECK(rack_heaters(samples_of(n), false, 350).empty());
    }
    SECTION("a single-nozzle printer with no nozzle block entries")
    {
        CHECK(rack_heaters({}, false, -1).empty());
    }
}

TEST_CASE("Rack state bits are read only on a direct-connected rack", "[BambuRackTemps][ToolTemps]")
{
    const auto rack = rack_heaters(samples_of(h3c_nozzle()), false, 350);
    REQUIRE(rack.size() == 4);
    CHECK(rack[0].temp == 181);
    CHECK(rack[0].state.empty());
    CHECK(state_name(0) == "idle");
    CHECK(state_name(1) == "heating");
    CHECK(state_name(2) == "holding");
    CHECK(state_name(3) == "cooling");
    CHECK(state_name(-1).empty());
}

TEST_CASE("parse_nozzle_temp never throws and ignores odd values", "[BambuRackTemps][ToolTemps]")
{
    const char* odd[] = {
        R"({"temp": "220"})", R"({"temp": null})", R"({"temp": -5})", R"({"temp": 12.5})", R"({"temp": [1]})",
        R"({"stat": "8"})",   R"({})",             R"([])",            R"(7)",              R"(null)",
    };
    for (const char* text : odd) {
        DYNAMIC_SECTION("entry " << text)
        {
            int  temp = 7, target = 8, state = -1;
            bool has  = false;
            REQUIRE_NOTHROW(parse_nozzle_temp(json::parse(text), temp, target, has, state));
            CHECK_FALSE(has);
            CHECK(temp == 7);
            CHECK(target == 8);
            CHECK(state == -1);
        }
    }
    SECTION("the 16 / 16 split and stat bits 3-4")
    {
        int  temp = 0, target = 0, state = -1;
        bool has  = false;
        parse_nozzle_temp(json::parse(R"({"temp": 13107381, "stat": 26})"), temp, target, has, state); // 181 | 200 << 16; stat 0b11010
        CHECK(has);
        CHECK(temp == 181);
        CHECK(target == 200);
        CHECK(state == 3);
    }
    SECTION("a rack position outside 0..15 is ignored")
    {
        std::vector<NozzleSample> s(1);
        s[0].id       = 0x10;
        s[0].addr_id  = 40;
        s[0].has_temp = true;
        CHECK(rack_heaters(s, true, 350).empty());
    }
}
