// The SDCP start-print command ElegooLink sends after "Upload and print".
//
// Reported bug: on a Centauri Carbon the log showed the command going out as
//     "Calibration_switch":,   "Tlp_Switch":
// - not JSON - and the printer never answered. The send dialog that fills the options in was not
// being created, so the extended-info map was empty, and the old code read the missing keys with
// operator[] (an empty string) and pasted that into the text. The builder now works from a JSON
// writer and treats a missing option as off, so these tests pin both halves: the text is always
// valid JSON, and what the dialog chose arrives in the right fields.
//
// Nothing here touches a printer, a socket, wx or the filesystem.

#include <catch2/catch.hpp>

#include <map>
#include <string>

#include <nlohmann/json.hpp>

#include "slic3r/Utils/ElegooStartPrint.hpp"

using Slic3r::ElegooStartPrint::build_payload;
using Slic3r::ElegooStartPrint::flag;
using Slic3r::ElegooStartPrint::platform_type;

namespace {
constexpr int PC_PLATE = 1; // the BedType value of the smooth plate; any value the dialog sends works here

using Info = std::map<std::string, std::string>;

nlohmann::json parse_payload(const Info& info, const std::string& filename = "cube.gcode.3mf")
{
    const std::string text = build_payload(info, PC_PLATE, filename, "req-1", 1735032553000LL);
    // Throws on invalid JSON, which fails the test case with the parser's message.
    return nlohmann::json::parse(text);
}
} // namespace

TEST_CASE("Elegoo start print: a missing extended info still gives valid JSON with options off", "[ElegooLink]")
{
    const nlohmann::json j = parse_payload(Info{});
    const auto&          d = j.at("Data").at("Data");

    CHECK(d.at("Calibration_switch").get<int>() == 0);
    CHECK(d.at("Tlp_Switch").get<int>() == 0);
    CHECK(d.at("PrintPlatformType").get<int>() == 0);
    CHECK(d.at("StartLayer").get<int>() == 0);
}

TEST_CASE("Elegoo start print: empty option values are off, never blank", "[ElegooLink]")
{
    // What operator[] used to hand back for a key the dialog never set.
    const nlohmann::json j = parse_payload(Info{{"timeLapse", ""}, {"heatedBedLeveling", ""}, {"bedType", ""}});
    const auto&          d = j.at("Data").at("Data");

    CHECK(d.at("Calibration_switch").get<int>() == 0);
    CHECK(d.at("Tlp_Switch").get<int>() == 0);
    CHECK(d.at("PrintPlatformType").get<int>() == 0);

    const std::string text = build_payload(Info{}, PC_PLATE, "a.gcode", "r", 1);
    CHECK(text.find("Calibration_switch\":,") == std::string::npos);
    CHECK(text.find("Tlp_Switch\":}") == std::string::npos);
}

TEST_CASE("Elegoo start print: the dialog's choices reach the right fields", "[ElegooLink]")
{
    SECTION("leveling and time-lapse on")
    {
        const auto j = parse_payload(Info{{"timeLapse", "1"}, {"heatedBedLeveling", "1"}, {"bedType", "0"}});
        const auto& d = j.at("Data").at("Data");
        CHECK(d.at("Calibration_switch").get<int>() == 1);
        CHECK(d.at("Tlp_Switch").get<int>() == 1);
        CHECK(d.at("PrintPlatformType").get<int>() == 0);
    }
    SECTION("leveling off, time-lapse on")
    {
        const auto j = parse_payload(Info{{"timeLapse", "1"}, {"heatedBedLeveling", "0"}});
        const auto& d = j.at("Data").at("Data");
        CHECK(d.at("Calibration_switch").get<int>() == 0);
        CHECK(d.at("Tlp_Switch").get<int>() == 1);
    }
    SECTION("the smooth plate is 1, anything else 0")
    {
        CHECK(platform_type(Info{{"bedType", std::to_string(PC_PLATE)}}, PC_PLATE) == 1);
        CHECK(platform_type(Info{{"bedType", "0"}}, PC_PLATE) == 0);
        CHECK(platform_type(Info{{"bedType", "5"}}, PC_PLATE) == 0);
        CHECK(platform_type(Info{}, PC_PLATE) == 0);
    }
}

TEST_CASE("Elegoo start print: flags are 1 only for \"1\"", "[ElegooLink]")
{
    CHECK(flag(Info{{"timeLapse", "1"}}, "timeLapse") == 1);
    CHECK(flag(Info{{"timeLapse", "0"}}, "timeLapse") == 0);
    CHECK(flag(Info{{"timeLapse", ""}}, "timeLapse") == 0);
    CHECK(flag(Info{{"timeLapse", "true"}}, "timeLapse") == 0);
    CHECK(flag(Info{}, "timeLapse") == 0);
}

TEST_CASE("Elegoo start print: envelope fields and the file name", "[ElegooLink]")
{
    const nlohmann::json j = parse_payload(Info{{"timeLapse", "1"}}, "cube.gcode.3mf");

    CHECK(j.at("Id").get<std::string>().empty());
    const auto& inner = j.at("Data");
    CHECK(inner.at("Cmd").get<int>() == 128);
    CHECK(inner.at("RequestID").get<std::string>() == "req-1");
    CHECK(inner.at("MainboardID").get<std::string>().empty());
    CHECK(inner.at("TimeStamp").get<long long>() == 1735032553000LL);
    CHECK(inner.at("From").get<int>() == 1);
    CHECK(inner.at("Data").at("Filename").get<std::string>() == "/local/cube.gcode.3mf");
}

TEST_CASE("Elegoo start print: a quote or backslash in the file name cannot break the JSON", "[ElegooLink]")
{
    const std::string name = "my \"part\" a\\b.gcode.3mf";
    const nlohmann::json j = parse_payload(Info{}, name);
    CHECK(j.at("Data").at("Data").at("Filename").get<std::string>() == "/local/" + name);
}
