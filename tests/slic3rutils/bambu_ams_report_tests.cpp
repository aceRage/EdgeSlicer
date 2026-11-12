#include <catch2/catch.hpp>

// The wx / Windows headers first, as every GUI source has them (GUI_App.hpp): a libslic3r header
// read before them leaves std::byte and the SDK's byte ambiguous (see bambu_reprint_tests.cpp).
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/DeviceManager.hpp"

#include <stdexcept>

using namespace Slic3r;
using nlohmann::json;

// MachineObject::parse_json reads a push_status print report in this order: print.ams
// (parse_ams_report, in its own try since the H3C fix), then vt_tray / vir_slot, then the
// new-protocol fields (parse_new_info: extruders, rack nozzles, chamber, SD card). parse_json itself
// needs the app (presets), so these tests call the two parts it runs, in the same order.
//
// The payloads are made up in the shape of the real reports. No real printer's serial or names.

namespace {

// One tray of an AMS unit; loaded trays carry a Bambu PLA.
json tray(int id, bool loaded)
{
    json t;
    t["id"] = std::to_string(id);
    if (loaded) {
        t["tag_uid"]        = "0000000000000000";
        t["tray_info_idx"]  = "GFA00";
        t["tray_type"]      = "PLA";
        t["tray_sub_brands"] = "PLA Basic";
        t["tray_color"]     = "FFFFFFFF";
        t["tray_weight"]    = "1000";
        t["tray_diameter"]  = "1.75";
        t["nozzle_temp_max"] = "230";
        t["nozzle_temp_min"] = "190";
        t["remain"]         = 80;
        t["k"]              = 0.02f;
        t["n"]              = 1.0f;
        t["cali_idx"]       = -1;
        t["cols"]           = json::array({ "FFFFFFFF" });
        t["ctype"]          = 0;
    }
    return t;
}

json unit(const std::string& id, const std::string& info, int trays, int loaded)
{
    json u;
    u["id"]           = id;
    u["info"]         = info;
    u["humidity"]     = "5";
    u["humidity_raw"] = "30";
    u["temp"]         = "25.0";
    u["dry_time"]     = 0;
    u["tray"]         = json::array();
    for (int i = 0; i < trays; ++i)
        u["tray"].push_back(tray(i, i < loaded));
    return u;
}

// The new-protocol flag strings and device block. aux bits 12..13 = 1: an SD card, normal.
// device.nozzle: the toolhead nozzle (id 0) and nine rack nozzles (ids 0x10 + n), as an H3C sends.
json n10_print()
{
    json p;
    p["cfg"]  = "";
    p["fun"]  = "";
    p["aux"]  = "1000";
    p["stat"] = "";

    json nozzles = json::array();
    nozzles.push_back({ { "id", 0 }, { "type", "HS01" }, { "diameter", 0.4 } });
    for (int n = 0; n < 9; ++n)
        nozzles.push_back({ { "id", 0x10 + n }, { "type", "HS01" }, { "diameter", 0.4 } });

    json extruder_info = json::array();
    extruder_info.push_back({ { "id", 0 }, { "info", 8 }, { "temp", 29 }, { "spre", 0xFFFF }, { "snow", 0xFFFF }, { "star", 0xFFFF },
                              { "hnow", 0 }, { "htar", 0 }, { "stat", 0 } });

    p["device"] = {
        { "nozzle", { { "exist", 1 }, { "state", 0 }, { "info", nozzles } } },
        { "extruder", { { "state", 1 }, { "info", extruder_info } } },
        { "ctc", { { "state", 0 }, { "info", { { "temp", 35 } } } } },
    };

    // The AMS 3H: type 6 in info bits 0..3, extruder 0xE in bits 8..11 (Bambu Studio DevAmsType
    // ZMULTI_A; ams_exist_bits bit 13, tray bits 28 + slot). Nine slots, slots 1-4 loaded, so
    // tray_exist_bits has bit 31 set: past a 32-bit long.
    p["ams"] = {
        { "ams", json::array({ unit("0", "E06", 9, 4) }) },
        { "ams_exist_bits", "2000" },
        { "tray_exist_bits", "F0000000" },
        { "tray_is_bbl_bits", "F0000000" },
        { "tray_read_done_bits", "F0000000" },
        { "tray_reading_bits", "0" },
        { "tray_now", "255" },
        { "tray_tar", "255" },
        { "version", 4 },
        { "insert_flag", true },
        { "power_on_flag", false },
    };
    return p;
}

} // namespace

TEST_CASE("Report bitmasks are read as 64-bit hex and bad values never throw", "[BambuAmsReport]")
{
    long long bits = 7;
    CHECK(MachineObject::parse_hex_bits(json("F0000000"), bits));
    CHECK(bits == 0xF0000000LL);
    CHECK(MachineObject::hex_bit_set(bits, 31));
    CHECK_FALSE(MachineObject::hex_bit_set(bits, 27));

    // All nine AMS 3H slots: bits 28..36.
    CHECK(MachineObject::parse_hex_bits(json("1FF0000000"), bits));
    CHECK(MachineObject::hex_bit_set(bits, 36));
    CHECK_FALSE(MachineObject::hex_bit_set(bits, 37));

    CHECK(MachineObject::parse_hex_bits(json("0x10"), bits));
    CHECK(bits == 16);
    CHECK(MachineObject::parse_hex_bits(json(5), bits));
    CHECK(bits == 5);

    // Not hex, too long, empty, or the wrong JSON type: false, the old value stays.
    CHECK_FALSE(MachineObject::parse_hex_bits(json("zz"), bits));
    CHECK_FALSE(MachineObject::parse_hex_bits(json("11112222333344445"), bits));
    CHECK_FALSE(MachineObject::parse_hex_bits(json(""), bits));
    CHECK_FALSE(MachineObject::parse_hex_bits(json(true), bits));
    CHECK_FALSE(MachineObject::parse_hex_bits(json(), bits));
    CHECK(bits == 5);

    CHECK_FALSE(MachineObject::hex_bit_set(-1, 64));
    CHECK_FALSE(MachineObject::hex_bit_set(-1, -1));

    // What the old stol did with the same mask where long is 32 bits (MSVC).
    if (sizeof(long) == 4)
        CHECK_THROWS_AS(std::stol("F0000000", nullptr, 16), std::out_of_range);
}

TEST_CASE("An H3C report with the AMS 3H loaded is read to the end", "[BambuAmsReport]")
{
    MachineObject obj { nullptr, "fake-h3c", "FAKEN100000001", "127.0.0.1" };
    obj.printer_type = "N10";
    const json print = n10_print();

    REQUIRE_NOTHROW(obj.parse_ams_report(print, false));
    CHECK(obj.ams_exist_bits == 0x2000);
    CHECK(obj.tray_exist_bits == 0xF0000000LL);
    CHECK(obj.tray_is_bbl_bits == 0xF0000000LL);
    CHECK(obj.tray_read_done_bits == 0xF0000000LL);
    CHECK(obj.ams_version == 4);
    // Phase 0 keeps the AMS 3H hidden (extruder 0xE is still skipped): its slots must not reach
    // the mapping or the filament sync until the tray numbering for it is ported.
    CHECK(obj.amsList.empty());

    REQUIRE_NOTHROW(obj.parse_new_info(print));
    CHECK(obj.is_enable_np);
    CHECK(obj.m_extder_data.total_extder_count == 1);
    CHECK(obj.m_extder_data.extders.size() == 1);
    CHECK(obj.m_nozzle_data.nozzles.size() == 10);
    CHECK(obj.has_nozzle_rack());
    CHECK(obj.chamber_temp == Approx(35.0f));
    CHECK(obj.sdcard_state == MachineObject::SdcardState::HAS_SDCARD_NORMAL);
}

TEST_CASE("An H2D-style AMS report parses as before", "[BambuAmsReport]")
{
    MachineObject obj { nullptr, "fake-h2d", "FAKEH2D0000001", "127.0.0.1" };
    obj.printer_type = "O1D";

    // An AMS 2 Pro (type 3) on the right extruder (0) with slots 1-3 loaded, and an AMS HT
    // (type 4, id 128) on the left extruder (1) with its slot loaded.
    json print;
    print["ams"] = {
        { "ams", json::array({ unit("0", "3", 4, 3), unit("128", "104", 1, 1) }) },
        { "ams_exist_bits", "11" },
        { "tray_exist_bits", "10007" },
        { "tray_is_bbl_bits", "10007" },
        { "tray_read_done_bits", "10007" },
        { "tray_reading_bits", "0" },
        { "version", 2 },
    };

    REQUIRE_NOTHROW(obj.parse_ams_report(print, false));
    CHECK(obj.ams_exist_bits == 0x11);
    CHECK(obj.tray_exist_bits == 0x10007);
    CHECK(obj.is_ams_need_update);
    REQUIRE(obj.amsList.size() == 2);

    Ams* ams = obj.amsList["0"];
    REQUIRE(ams != nullptr);
    CHECK(ams->type == 3);
    CHECK(ams->nozzle == 0);
    CHECK(ams->is_exists);
    CHECK(ams->humidity == 4); // humidity_raw 30 -> level 4
    REQUIRE(ams->trayList.size() == 4);
    CHECK(ams->trayList["0"]->is_exists);
    CHECK(ams->trayList["2"]->is_exists);
    CHECK_FALSE(ams->trayList["3"]->is_exists);
    CHECK(ams->trayList["0"]->type == "PLA");
    CHECK(ams->trayList["0"]->setting_id == "GFA00");
    CHECK(ams->trayList["0"]->color == "FFFFFFFF");
    CHECK(ams->trayList["0"]->remain == 80);
    CHECK(ams->trayList["3"]->type.empty());

    Ams* ht = obj.amsList["128"];
    REQUIRE(ht != nullptr);
    CHECK(ht->type == 4);
    CHECK(ht->nozzle == 1);
    CHECK(ht->is_exists);
    REQUIRE(ht->trayList.size() == 1);
    CHECK(ht->trayList["0"]->is_exists);

    // The same report again changes nothing, and a unit that disappears is removed.
    REQUIRE_NOTHROW(obj.parse_ams_report(print, false));
    CHECK_FALSE(obj.is_ams_need_update);
    print["ams"]["ams"]       = json::array({ unit("0", "3", 4, 3) });
    print["ams"]["ams_exist_bits"] = "1";
    REQUIRE_NOTHROW(obj.parse_ams_report(print, false));
    CHECK(obj.amsList.size() == 1);
    CHECK(obj.amsList.count("128") == 0);
}

TEST_CASE("A report without an ams block leaves the AMS state alone", "[BambuAmsReport]")
{
    MachineObject obj { nullptr, "fake-x1c", "FAKEX1C0000001", "127.0.0.1" };
    obj.ams_exist_bits = 1;
    REQUIRE_NOTHROW(obj.parse_ams_report(json::object(), false));
    REQUIRE_NOTHROW(obj.parse_ams_report(json { { "ams", "x" } }, false));
    CHECK(obj.ams_exist_bits == 1);
}
