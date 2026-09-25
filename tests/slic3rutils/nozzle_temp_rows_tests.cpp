#include <catch2/catch.hpp>

#include "slic3r/GUI/NozzleTempRows.hpp"
#include "slic3r/GUI/SnapmakerLan.hpp"

using namespace Slic3r::GUI;

TEST_CASE("Single-nozzle printers keep one unbadged row from the first extruder", "[NozzleTempRows]")
{
    SECTION("X1/P1 style report: one extruder")
    {
        const auto rows = nozzle_temp_rows({ { 0, 215, 220 } }, 1, 0);
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].badge.empty());
        CHECK(rows[0].extruder_id == 0);
        CHECK(rows[0].temp == 215);
        CHECK(rows[0].target == 220);
        CHECK(rows[0].heating);
        CHECK_FALSE(rows[0].active);
    }
    SECTION("total count says 1 even though two entries arrived: stay single")
    {
        const auto rows = nozzle_temp_rows({ { 0, 30, 0 }, { 1, 200, 200 } }, 1, 0);
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].temp == 30);
        CHECK(rows[0].badge.empty());
    }
    SECTION("count says 2 but both entries carry the same id: stay single")
    {
        const auto rows = nozzle_temp_rows({ { 0, 30, 0 }, { 0, 31, 0 } }, 2, 0);
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].temp == 30);
    }
    SECTION("no data")
    {
        CHECK(nozzle_temp_rows({}, 0, 0).empty());
        CHECK(nozzle_temp_rows({}, 2, 0).empty());
    }
}

TEST_CASE("Two-nozzle printers show L above R with the right values", "[NozzleTempRows]")
{
    // The owner's H2D screenshot: L 219 / 178, R 214 / 220, right nozzle heating.
    // DeviceManager stores the printer's order, which is id 0 (right) first.
    const auto rows = nozzle_temp_rows({ { 0, 214, 220 }, { 1, 219, 178 } }, 2, 0);
    REQUIRE(rows.size() == 2);

    CHECK(rows[0].badge == "L");
    CHECK(rows[0].extruder_id == 1);
    CHECK(rows[0].temp == 219);
    CHECK(rows[0].target == 178);
    CHECK_FALSE(rows[0].heating); // cooling down: grey icon
    CHECK_FALSE(rows[0].active);

    CHECK(rows[1].badge == "R");
    CHECK(rows[1].extruder_id == 0);
    CHECK(rows[1].temp == 214);
    CHECK(rows[1].target == 220);
    CHECK(rows[1].heating); // orange icon
    CHECK(rows[1].active);
}

TEST_CASE("Row order and badges do not depend on the report order", "[NozzleTempRows]")
{
    const auto rows = nozzle_temp_rows({ { 1, 25, 0 }, { 0, 26, 0 } }, 2, 1);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].badge == "L");
    CHECK(rows[0].temp == 25);
    CHECK(rows[0].active);
    CHECK(rows[1].badge == "R");
    CHECK(rows[1].temp == 26);
    CHECK_FALSE(rows[1].active);
}

TEST_CASE("Active highlight follows current_extder_id and clears when it is unknown", "[NozzleTempRows]")
{
    const std::vector<NozzleTempSample> ex{ { 0, 25, 0 }, { 1, 25, 0 } };
    for (int cur : { 0, 1 }) {
        const auto rows = nozzle_temp_rows(ex, 2, cur);
        REQUIRE(rows.size() == 2);
        for (const auto &r : rows)
            CHECK(r.active == (r.extruder_id == cur));
    }
    // 0xF is what the 4-bit state field holds while switching / when nothing is loaded.
    for (const auto &r : nozzle_temp_rows(ex, 2, 0xF))
        CHECK_FALSE(r.active);
}

TEST_CASE("Heating uses the panel's 2-degree threshold", "[NozzleTempRows]")
{
    CHECK_FALSE(nozzle_is_heating(219, 220));
    CHECK(nozzle_is_heating(218, 220));
    CHECK_FALSE(nozzle_is_heating(220, 0));
    CHECK_FALSE(nozzle_is_heating(200, 200));
}

TEST_CASE("More than two extruders get numbered badges, highest id first", "[NozzleTempRows]")
{
    const auto rows = nozzle_temp_rows({ { 0, 1, 0 }, { 1, 2, 0 }, { 2, 3, 0 } }, 3, 2);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].badge == "3");
    CHECK(rows[0].active);
    CHECK(rows[1].badge == "2");
    CHECK(rows[2].badge == "1");
}

// ---- Snapmaker over the LAN (the phone hub's U1 card) ----

TEST_CASE("Snapmaker LAN probe keeps every toolhead's temperature", "[NozzleTempRows][SnapmakerLan]")
{
    using Slic3r::GUI::SnapmakerLan::nozzle_temps_of;

    SECTION("U1: four toolheads, in toolhead order")
    {
        const auto st = nlohmann::json::parse(R"({
            "heater_bed": {"temperature": 60.1, "target": 60},
            "extruder":  {"temperature": 220.4, "target": 220, "nozzle_diameter": 0.4},
            "extruder1": {"temperature": 25.0,  "target": 0},
            "extruder2": {"temperature": 150.2, "target": 170},
            "extruder3": {"temperature": 26.5,  "target": 0}
        })");
        const auto n = nozzle_temps_of(st);
        REQUIRE(n.size() == 4);
        CHECK(n[0].first == Approx(220.4));
        CHECK(n[0].second == Approx(220));
        CHECK(n[2].first == Approx(150.2));
        CHECK(n[2].second == Approx(170));
        CHECK(n[3].first == Approx(26.5));
    }
    SECTION("single-extruder Moonraker answer")
    {
        const auto n = nozzle_temps_of(nlohmann::json::parse(R"({"extruder": {"temperature": 30, "target": 0}})"));
        REQUIRE(n.size() == 1);
        CHECK(n[0].first == Approx(30));
    }
    SECTION("a gap stops the list so indexes stay toolhead numbers")
    {
        const auto n = nozzle_temps_of(nlohmann::json::parse(R"({"extruder": {"temperature": 30, "target": 0},
                                                                 "extruder2": {"temperature": 40, "target": 0}})"));
        CHECK(n.size() == 1);
    }
    SECTION("no extruder at all")
    {
        CHECK(nozzle_temps_of(nlohmann::json::parse(R"({"heater_bed": {"temperature": 30, "target": 0}})")).empty());
        CHECK(nozzle_temps_of(nlohmann::json::object()).empty());
    }
}
