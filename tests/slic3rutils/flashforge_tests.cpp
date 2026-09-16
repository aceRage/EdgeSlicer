// The Flashforge port-8898 HTTP local API: the parsing and the one decision that routes a printer
// to it.
//
// A Creator 5 / Adventurer 5M-series printer does not speak the old TCP-8899 M-code console at all,
// so a user who fills the Physical Printer dialog in and presses Test gets one of two very
// different conversations depending on a single predicate. That predicate, and the shapes of the
// JSON the printer answers with, are the contract worth pinning:
//
//  1. uses_local_api(). Serial number AND check code, both present, is what selects the HTTP path.
//     Miss either and the printer stays on the legacy serial console - which is exactly how an
//     Adventurer 3/4 owner keeps working after this change. Getting this backwards is the reported
//     bug ("target machine actively refused it"), so it gets its own tests.
//
//  2. validate_response(). The printer answers HTTP 200 with a JSON body that carries its own
//     result code; a non-zero `code`/`err` is a refusal (a wrong check code lands here) and the
//     message it carries is the only thing that tells the user which field is wrong.
//
//  3. parse_detail_material_slots(). The IFS (multi-filament station) mapping dialog is built from
//     /detail. The firmware has shipped both camelCase and PascalCase spellings of these keys and
//     reports "no station" by several different routes, so the reader tolerates all of them.
//
//  4. sanitize_filename() / url_for(). The printer rejects '=' and friends in an upload name, and
//     the URL is assembled from whatever the user typed in Hostname (bare IP, or a full URL).
//
// Nothing here touches a printer, a socket, wx or the filesystem.

#include <catch2/catch.hpp>

#include <string>
#include <vector>

#include "slic3r/Utils/Flashforge.hpp"

using Slic3r::FlashforgeMaterialSlot;
namespace ff = Slic3r::FlashforgeLocalApi;

// ---------------------------------------------------------------- the legacy/HTTP decision ----

TEST_CASE("Flashforge local API needs both the serial number and the check code", "[Flashforge]")
{
    // The AD5-series case: both fields filled in, so the HTTP local API is used.
    CHECK(ff::uses_local_api("SNMOCK123456", "abcd1234"));

    // Every way of being incomplete keeps the printer on the legacy TCP-8899 console. This is what
    // preserves the Adventurer 3/4 workflow, which has no serial/check-code fields to fill.
    CHECK_FALSE(ff::uses_local_api("", ""));
    CHECK_FALSE(ff::uses_local_api("SNMOCK123456", ""));
    CHECK_FALSE(ff::uses_local_api("", "abcd1234"));
}

// ------------------------------------------------------------------------ the URL it talks to ----

TEST_CASE("Flashforge local API URLs are built on port 8898", "[Flashforge]")
{
    SECTION("a bare address is used as-is")
    {
        CHECK(ff::url_for("192.168.1.50", "detail") == "http://192.168.1.50:8898/detail");
        CHECK(ff::url_for("192.168.1.50", "product") == "http://192.168.1.50:8898/product");
        CHECK(ff::url_for("192.168.1.50", "uploadGcode") == "http://192.168.1.50:8898/uploadGcode");
    }

    SECTION("a host with a path keeps only the host")
    {
        CHECK(ff::host_name_of("192.168.1.50/whatever") == "192.168.1.50");
    }

    SECTION("a full URL is reduced to its host, so the port is ours and not the user's")
    {
        CHECK(ff::host_name_of("http://192.168.1.50:80/") == "192.168.1.50");
        CHECK(ff::url_for("http://printer.local:8080/", "detail") == "http://printer.local:8898/detail");
    }
}

// ------------------------------------------------------------------------- the upload filename ----

TEST_CASE("Flashforge upload names drop characters the printer rejects", "[Flashforge]")
{
    // '=' is the one the printer is known to refuse; spaces and the rest go the same way.
    CHECK(ff::sanitize_filename("plate=1.gcode.3mf") == "plate_1.gcode.3mf");
    CHECK(ff::sanitize_filename("my model v2.3mf") == "my_model_v2.3mf");

    // Dots, underscores and hyphens survive - they are how the extension and most names are spelled.
    CHECK(ff::sanitize_filename("Benchy_v1-2.gcode") == "Benchy_v1-2.gcode");

    // Only the basename is uploaded, never the directories above it.
    CHECK(ff::sanitize_filename("C:/tmp/sub dir/part.3mf") == "part.3mf");

    // An empty name still has to be something, and the fallback extension rides along.
    CHECK(ff::sanitize_filename("", ".3mf") == "print.3mf");
    CHECK(ff::sanitize_filename("") == "print");
}

// ------------------------------------------------------------- the result code in every reply ----

TEST_CASE("Flashforge replies are accepted or refused by their own result code", "[Flashforge]")
{
    std::string error;

    SECTION("code 0 is success")
    {
        REQUIRE(ff::validate_response(R"({"code":0,"message":"Success"})", error));
        CHECK(error.empty());
    }

    SECTION("a reply with no code at all is success")
    {
        // /product answers with the payload and no envelope code on some firmware.
        REQUIRE(ff::validate_response(R"({"product":{"nozzleTempCtrlState":1}})", error));
        CHECK(error.empty());
    }

    SECTION("a non-zero code is a refusal and carries the printer's own words")
    {
        // This is what a wrong check code looks like coming back over HTTP 200.
        REQUIRE_FALSE(ff::validate_response(R"({"code":5,"message":"token is invalid"})", error));
        CHECK(error.find("5") != std::string::npos);
        CHECK(error.find("token is invalid") != std::string::npos);
    }

    SECTION("the older err/msg spelling is understood too")
    {
        REQUIRE_FALSE(ff::validate_response(R"({"err":3,"msg":"check code error"})", error));
        CHECK(error.find("check code error") != std::string::npos);
    }

    SECTION("a code carried as a string still counts")
    {
        REQUIRE_FALSE(ff::validate_response(R"({"code":"7","message":"busy"})", error));
        CHECK(error.find("busy") != std::string::npos);
    }

    SECTION("a refusal with no message still says something")
    {
        REQUIRE_FALSE(ff::validate_response(R"({"code":9})", error));
        CHECK_FALSE(error.empty());
    }

    SECTION("a body that is not JSON, or not an object, is a failure and not a crash")
    {
        // What a printer that is not a Flashforge answers with - or an HTML error page.
        REQUIRE_FALSE(ff::validate_response("<html>404</html>", error));
        CHECK_FALSE(error.empty());

        REQUIRE_FALSE(ff::validate_response("[1,2,3]", error));
        CHECK_FALSE(error.empty());

        REQUIRE_FALSE(ff::validate_response("", error));
        CHECK_FALSE(error.empty());
    }
}

// ------------------------------------------------------------------------ the IFS slot mapping ----

TEST_CASE("Flashforge /detail yields the material station slots", "[Flashforge]")
{
    std::vector<FlashforgeMaterialSlot> slots;
    bool                                has_station = false;

    SECTION("a four-slot AD5X station is read whole")
    {
        const std::string body = R"({
            "code": 0,
            "detail": {
                "hasMatlStation": 1,
                "matlStationInfo": {
                    "slotCnt": 4,
                    "slotInfos": [
                        {"slotId": 1, "hasFilament": true,  "materialName": "PLA",     "materialColor": "#FF0000"},
                        {"slotId": 2, "hasFilament": true,  "materialName": "PETG",    "materialColor": "#00FF00"},
                        {"slotId": 3, "hasFilament": false, "materialName": "",        "materialColor": ""},
                        {"slotId": 4, "hasFilament": true,  "materialName": "PLA-CF",  "materialColor": "#0000FF"}
                    ]
                }
            }
        })";

        REQUIRE(ff::parse_detail_material_slots(body, slots, has_station));
        CHECK(has_station);
        REQUIRE(slots.size() == 4);

        CHECK(slots[0].slot_id == 1);
        CHECK(slots[0].has_filament);
        CHECK(slots[0].material_name == "PLA");
        CHECK(slots[0].material_color == "#FF0000");

        // An empty slot is reported, not skipped: the mapping dialog has to show it as empty.
        CHECK(slots[2].slot_id == 3);
        CHECK_FALSE(slots[2].has_filament);
        CHECK(slots[2].material_name.empty());

        CHECK(slots[3].material_name == "PLA-CF");
    }

    SECTION("the PascalCase spelling of the same reply reads identically")
    {
        const std::string body = R"({
            "detail": {
                "HasMatlStation": 1,
                "MatlStationInfo": {
                    "SlotCnt": 2,
                    "SlotInfos": [
                        {"slotId": 1, "hasFilament": true, "materialName": "ABS", "materialColor": "#101010"},
                        {"slotId": 2, "hasFilament": false}
                    ]
                }
            }
        })";

        REQUIRE(ff::parse_detail_material_slots(body, slots, has_station));
        CHECK(has_station);
        REQUIRE(slots.size() == 2);
        CHECK(slots[0].material_name == "ABS");
        CHECK_FALSE(slots[1].has_filament);
    }

    SECTION("a reply with the detail inlined rather than nested still reads")
    {
        const std::string body = R"({
            "hasMatlStation": 1,
            "matlStationInfo": {"slotCnt": 1, "slotInfos": [{"slotId": 1, "hasFilament": true, "materialName": "PLA"}]}
        })";

        REQUIRE(ff::parse_detail_material_slots(body, slots, has_station));
        CHECK(has_station);
        REQUIRE(slots.size() == 1);
    }

    SECTION("a printer with no station reports none and no slots")
    {
        // A plain Creator 5 / AD5M: single extruder, no IFS. The send dialog must not offer mapping.
        const std::string body = R"({"code":0,"detail":{"hasMatlStation":0,"firmwareVersion":"3.1.3"}})";

        REQUIRE(ff::parse_detail_material_slots(body, slots, has_station));
        CHECK_FALSE(has_station);
        CHECK(slots.empty());
    }

    SECTION("slots present without the flag still mean a station")
    {
        const std::string body = R"({"detail":{"matlStationInfo":{"slotInfos":[{"slotId":1,"hasFilament":true}]}}})";

        REQUIRE(ff::parse_detail_material_slots(body, slots, has_station));
        CHECK(has_station);
        REQUIRE(slots.size() == 1);
    }

    SECTION("a missing slotId falls back to the slot's position, 1-based")
    {
        const std::string body = R"({"detail":{"matlStationInfo":{"slotInfos":[
            {"hasFilament":true,"materialName":"PLA"},
            {"hasFilament":true,"materialName":"PETG"}
        ]}}})";

        REQUIRE(ff::parse_detail_material_slots(body, slots, has_station));
        REQUIRE(slots.size() == 2);
        CHECK(slots[0].slot_id == 1);
        CHECK(slots[1].slot_id == 2);
    }

    SECTION("a body that will not parse is a failure, not a crash")
    {
        CHECK_FALSE(ff::parse_detail_material_slots("not json at all", slots, has_station));
        CHECK(slots.empty());
    }
}
