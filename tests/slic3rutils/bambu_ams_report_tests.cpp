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

// An H3C (N10) with the AMS 3H attached: print.ams, print.vir_slot and part of print.device from a
// real push_status, with every serial, tag, uuid and name replaced by a fake value. The AMS 3H is
// unit "32", info "240002006" (type 6 in bits 0..3, extruder 0 in bits 8..11), nine trays with
// slots 1-4 loaded: ams_exist_bits bit 13 and tray_exist_bits bits 28..31, past a 32-bit long. Its
// humidity and temperature are only in zone_info, not on the unit. One extruder (snow 0x2001 = AMS
// 0x20 slot 1) and nine rack nozzles (ids 0x10..0x18).
const char* const H3C_REPORT = R"JSON(
{"ams":{"ams":[{"dry_info":"1","id":"32","info":"240002006","tray":[{"bed_temp":"0","bed_temp_type":"0",
"cali_idx":-1,"cols":["000000FF"],"ctype":2,"drying_temp":"0","drying_time":"0","id":"0","ins_cyc":3,
"nozzle_temp_max":"240","nozzle_temp_min":"190","print_status":0,"remain":-1,"remain_g":-1,"state":27,
"tag_uid":"0000000000000000","total_len":330000,"tray_color":"000000FF","tray_diameter":"1.75",
"tray_id_name":"FAKE-00","tray_info_idx":"GFA00","tray_sub_brands":"","tray_type":"PLA",
"tray_uuid":"00000000000000000000000000000000","tray_weight":"-1",
"xcam_info":"00000000000000000000000000000000"},{"bed_temp":"0","bed_temp_type":"0","cali_idx":-1,
"cols":["DE4343FF"],"ctype":2,"drying_temp":"0","drying_time":"0","id":"1","ins_cyc":4,
"nozzle_temp_max":"240","nozzle_temp_min":"190","print_status":0,"remain":-1,"remain_g":-1,"state":27,
"tag_uid":"0000000000000000","total_len":330000,"tray_color":"DE4343FF","tray_diameter":"1.75",
"tray_id_name":"FAKE-00","tray_info_idx":"GFA00","tray_sub_brands":"","tray_type":"PLA",
"tray_uuid":"00000000000000000000000000000000","tray_weight":"-1",
"xcam_info":"00000000000000000000000000000000"},{"bed_temp":"0","bed_temp_type":"0","cali_idx":-1,
"cols":["00000000"],"ctype":0,"drying_temp":"0","drying_time":"0","id":"2","ins_cyc":1,"nozzle_temp_max":"0",
"nozzle_temp_min":"0","print_status":0,"remain":-1,"remain_g":-1,"state":27,"tag_uid":"0000000000000000",
"total_len":330000,"tray_color":"00000000","tray_diameter":"1.75","tray_id_name":"FAKE-00","tray_info_idx":"",
"tray_sub_brands":"","tray_type":"","tray_uuid":"00000000000000000000000000000000","tray_weight":"-1",
"xcam_info":"00000000000000000000000000000000"},{"bed_temp":"0","bed_temp_type":"0","cali_idx":-1,
"cols":["00000000"],"ctype":0,"drying_temp":"0","drying_time":"0","id":"3","ins_cyc":3,"nozzle_temp_max":"0",
"nozzle_temp_min":"0","print_status":0,"remain":-1,"remain_g":-1,"state":27,"tag_uid":"0000000000000000",
"total_len":330000,"tray_color":"00000000","tray_diameter":"1.75","tray_id_name":"FAKE-00","tray_info_idx":"",
"tray_sub_brands":"","tray_type":"","tray_uuid":"00000000000000000000000000000000","tray_weight":"-1",
"xcam_info":"00000000000000000000000000000000"},{"id":"4","print_status":0,"state":160},{"id":"5",
"print_status":0,"state":160},{"id":"6","print_status":0,"state":160},{"id":"7","print_status":0,"state":160},
{"id":"8","print_status":0,"state":160}],"zone_info":[{"dry_setting":{"dry_duration":-1,"dry_filament":"",
"dry_temperature":-1},"dry_sf_reason":[],"dry_time":0,"humidity":"1","humidity_raw":"50","info":"240002006",
"temp":"25.7","zone_dry_info":"1","zone_id":0,"zone_slots":[5,7]},{"dry_setting":{"dry_duration":-1,
"dry_filament":"","dry_temperature":-1},"dry_sf_reason":[],"dry_time":0,"humidity":"1","humidity_raw":"47",
"info":"240002006","temp":"25.2","zone_dry_info":"0","zone_id":1,"zone_slots":[0,1,2,3,4,6,8]}]}],
"ams_exist_bits":"2000","ams_exist_bits_raw":"2000","cali_id":255,"cali_stat":0,"cfs":[2,10,5,7,16],
"cfs_m":[[2,10,5,7],[2,10,5,7,16]],"csl":[8192,8193],"insert_flag":true,"power_on_flag":false,"stat":0,
"tray_exist_bits":"f0000000","tray_hall_out_bits":"f0000000","tray_is_bbl_bits":"f0000000","tray_now":"255",
"tray_pre":"255","tray_read_done_bits":"f0000000","tray_reading_bits":"0","tray_tar":"1","unbind_ams_stat":0,
"version":9340},"device":{"bed":{"info":{"temp":26},"state":0},"bed_temp":26,"ctc":{"info":{"temp":24},
"state":0},"extruder":{"info":[{"hnow":0,"hpre":0,"htar":0,"id":0,"info":1547,"snow":8193,"spre":8193,
"star":8193,"stat":393984,"temp":27,"z_bias":0.0}],"state":1},"holder":{"info":1,"info2":16355,"job":0,
"pos":2,"stat":0,"step":[0]},"nozzle":{"exist":33488896,"hc":511,"info":[{"addr_id":0,"base_type":0,
"bind_ams":[],"c_t":1,"color_m":"000000FF","diameter":0.4,"fila_id":"GFA00","id":16,"p_t":0,"rfid":6,
"sn":"FAKESN00000000","snow":8192,"stat":161,"temp":27,"tm":350,"type":"HS01","wear":0.0},{"addr_id":1,
"base_type":0,"c_t":1,"color_m":"DE4343FF","diameter":0.4,"fila_id":"GFA00","id":17,"p_t":0,"rfid":6,
"sn":"FAKESN00000000","snow":8193,"stat":161,"temp":27,"tm":350,"type":"HS01","wear":0.0},{"addr_id":2,
"base_type":0,"c_t":0,"color_m":"00000000","diameter":0.4,"fila_id":"","id":18,"p_t":0,"rfid":0,
"sn":"FAKESN00000000","snow":65023,"stat":1,"temp":26,"tm":350,"type":"HS01","wear":0.0},{"addr_id":3,
"base_type":0,"c_t":0,"color_m":"00000000","diameter":0.4,"fila_id":"","id":19,"p_t":0,"rfid":0,
"sn":"FAKESN00000000","snow":64767,"stat":1,"temp":26,"tm":350,"type":"HS01","wear":0.0},{"addr_id":4,
"base_type":0,"c_t":1,"color_m":"00000000","diameter":0.4,"fila_id":"","id":20,"p_t":2016,"rfid":0,
"sn":"FAKESN00000000","snow":64511,"stat":1,"temp":27,"tm":350,"type":"HS01","wear":0.0},{"addr_id":5,
"base_type":0,"c_t":0,"color_m":"00000000","diameter":0.4,"fila_id":"","id":21,"p_t":0,"rfid":0,
"sn":"FAKESN00000000","snow":64255,"stat":1,"temp":26,"tm":350,"type":"HS01","wear":0.0},{"addr_id":6,
"base_type":0,"c_t":0,"color_m":"00000000","diameter":0.4,"fila_id":"","id":22,"p_t":0,"rfid":0,
"sn":"FAKESN00000000","snow":63999,"stat":1,"temp":26,"tm":350,"type":"HS01","wear":0.0},{"addr_id":7,
"base_type":1,"c_t":0,"color_m":"00000000","diameter":0.4,"fila_id":"","id":23,"p_t":0,"rfid":0,
"sn":"FAKESN00000000","snow":63488,"stat":1,"temp":26,"tm":350,"type":"HT01","wear":0.0},{"addr_id":8,
"base_type":0,"c_t":0,"color_m":"00000000","diameter":0.4,"fila_id":"","id":24,"p_t":0,"rfid":0,
"sn":"FAKESN00000000","snow":63487,"stat":1,"temp":26,"tm":350,"type":"HS01","wear":0.0}],"refresh_list":[],
"src_id":255,"state":4096,"tar_id":255},"storage":[{"name":"sdcard","total":"120112000","used":"64"}],
"type":1},"vir_slot":[{"bed_temp":"0","bed_temp_type":"0","cali_idx":-1,"cols":["00000000"],"ctype":0,
"drying_temp":"0","drying_time":"0","id":"247","ins_cyc":1,"nozzle_temp_max":"0","nozzle_temp_min":"0",
"print_status":0,"remain":-1,"remain_g":-1,"state":1,"tag_uid":"0000000000000000","total_len":330000,
"tray_color":"00000000","tray_diameter":"1.75","tray_id_name":"FAKE-00","tray_info_idx":"",
"tray_sub_brands":"","tray_type":"","tray_uuid":"00000000000000000000000000000000","tray_weight":"-1",
"xcam_info":"00000000000000000000000000000000"},{"bed_temp":"0","bed_temp_type":"0","cali_idx":-1,
"cols":["00000000"],"ctype":0,"drying_temp":"0","drying_time":"0","id":"248","ins_cyc":1,
"nozzle_temp_max":"0","nozzle_temp_min":"0","print_status":0,"remain":-1,"remain_g":-1,"state":1,
"tag_uid":"0000000000000000","total_len":330000,"tray_color":"00000000","tray_diameter":"1.75",
"tray_id_name":"FAKE-00","tray_info_idx":"","tray_sub_brands":"","tray_type":"",
"tray_uuid":"00000000000000000000000000000000","tray_weight":"-1",
"xcam_info":"00000000000000000000000000000000"},{"bed_temp":"0","bed_temp_type":"0","cali_idx":-1,
"cols":["00000000"],"ctype":0,"drying_temp":"0","drying_time":"0","id":"249","ins_cyc":1,
"nozzle_temp_max":"0","nozzle_temp_min":"0","print_status":0,"remain":-1,"remain_g":-1,"state":1,
"tag_uid":"0000000000000000","total_len":330000,"tray_color":"00000000","tray_diameter":"1.75",
"tray_id_name":"FAKE-00","tray_info_idx":"","tray_sub_brands":"","tray_type":"",
"tray_uuid":"00000000000000000000000000000000","tray_weight":"-1",
"xcam_info":"00000000000000000000000000000000"},{"bed_temp":"0","bed_temp_type":"0","cali_idx":-1,
"cols":["00000000"],"ctype":0,"drying_temp":"0","drying_time":"0","id":"250","ins_cyc":1,
"nozzle_temp_max":"0","nozzle_temp_min":"0","print_status":0,"remain":-1,"remain_g":-1,"state":1,
"tag_uid":"0000000000000000","total_len":330000,"tray_color":"00000000","tray_diameter":"1.75",
"tray_id_name":"FAKE-00","tray_info_idx":"","tray_sub_brands":"","tray_type":"",
"tray_uuid":"00000000000000000000000000000000","tray_weight":"-1",
"xcam_info":"00000000000000000000000000000000"},{"bed_temp":"0","bed_temp_type":"0","cali_idx":-1,
"cols":["00000000"],"ctype":0,"drying_temp":"0","drying_time":"0","id":"251","ins_cyc":1,
"nozzle_temp_max":"0","nozzle_temp_min":"0","print_status":0,"remain":-1,"remain_g":-1,"state":1,
"tag_uid":"0000000000000000","total_len":330000,"tray_color":"00000000","tray_diameter":"1.75",
"tray_id_name":"FAKE-00","tray_info_idx":"","tray_sub_brands":"","tray_type":"",
"tray_uuid":"00000000000000000000000000000000","tray_weight":"-1",
"xcam_info":"00000000000000000000000000000000"},{"bed_temp":"0","bed_temp_type":"0","cali_idx":-1,
"cols":["00000000"],"ctype":0,"drying_temp":"0","drying_time":"0","id":"252","ins_cyc":1,
"nozzle_temp_max":"0","nozzle_temp_min":"0","print_status":0,"remain":-1,"remain_g":-1,"state":1,
"tag_uid":"0000000000000000","total_len":330000,"tray_color":"00000000","tray_diameter":"1.75",
"tray_id_name":"FAKE-00","tray_info_idx":"","tray_sub_brands":"","tray_type":"",
"tray_uuid":"00000000000000000000000000000000","tray_weight":"-1",
"xcam_info":"00000000000000000000000000000000"},{"bed_temp":"0","bed_temp_type":"0","cali_idx":-1,
"cols":["00000000"],"ctype":0,"drying_temp":"0","drying_time":"0","id":"253","ins_cyc":1,
"nozzle_temp_max":"0","nozzle_temp_min":"0","print_status":0,"remain":-1,"remain_g":-1,"state":1,
"tag_uid":"0000000000000000","total_len":330000,"tray_color":"00000000","tray_diameter":"1.75",
"tray_id_name":"FAKE-00","tray_info_idx":"","tray_sub_brands":"","tray_type":"",
"tray_uuid":"00000000000000000000000000000000","tray_weight":"-1",
"xcam_info":"00000000000000000000000000000000"},{"bed_temp":"0","bed_temp_type":"0","cali_idx":-1,
"cols":["00000000"],"ctype":0,"drying_temp":"0","drying_time":"0","id":"254","ins_cyc":1,
"nozzle_temp_max":"0","nozzle_temp_min":"0","print_status":0,"remain":-1,"remain_g":-1,"state":1,
"tag_uid":"0000000000000000","total_len":330000,"tray_color":"00000000","tray_diameter":"1.75",
"tray_id_name":"FAKE-00","tray_info_idx":"","tray_sub_brands":"","tray_type":"",
"tray_uuid":"00000000000000000000000000000000","tray_weight":"-1",
"xcam_info":"00000000000000000000000000000000"},{"bed_temp":"0","bed_temp_type":"0","cali_idx":-1,
"cols":["00000000"],"ctype":0,"drying_temp":"0","drying_time":"0","id":"255","ins_cyc":1,
"nozzle_temp_max":"0","nozzle_temp_min":"0","print_status":0,"remain":-1,"remain_g":-1,"state":1,
"tag_uid":"0000000000000000","total_len":330000,"tray_color":"00000000","tray_diameter":"1.75",
"tray_id_name":"FAKE-00","tray_info_idx":"","tray_sub_brands":"","tray_type":"",
"tray_uuid":"00000000000000000000000000000000","tray_weight":"-1",
"xcam_info":"00000000000000000000000000000000"}]}
)JSON";

// The new-protocol flag strings are made up: empty, apart from aux bits 12..13 = 1 (an SD card, normal).
json n10_print()
{
    json p    = json::parse(H3C_REPORT);
    p["cfg"]  = "";
    p["fun"]  = "";
    p["aux"]  = "1000";
    p["stat"] = "";
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
    CHECK(obj.tray_reading_bits == 0);
    CHECK(obj.ams_version == 9340);
    CHECK(obj.m_tray_tar == "1");
    // Phase 0 keeps the AMS 3H out of the AMS list: its slots must not reach the mapping or the
    // filament sync until its slot numbering is ported.
    CHECK(obj.amsList.empty());

    // What parse_json reads after the AMS block, which the overflow used to skip.
    REQUIRE_NOTHROW(obj.parse_new_info(print));
    CHECK(obj.is_enable_np);
    CHECK(obj.m_extder_data.total_extder_count == 1);
    REQUIRE(obj.m_extder_data.extders.size() == 1);
    CHECK(obj.m_extder_data.extders[0].snow.ams_id == "32");
    CHECK(obj.m_extder_data.extders[0].snow.slot_id == "1");
    CHECK(obj.m_nozzle_data.nozzles.size() == 9);
    CHECK(obj.has_nozzle_rack());
    CHECK(obj.chamber_temp == Approx(24.0f));
    CHECK(obj.sdcard_state == MachineObject::SdcardState::HAS_SDCARD_NORMAL);

    // A second push changes nothing and still keeps the AMS 3H out.
    REQUIRE_NOTHROW(obj.parse_ams_report(print, false));
    CHECK_FALSE(obj.is_ams_need_update);
    CHECK(obj.amsList.empty());
}

TEST_CASE("A rack AMS that was listed earlier is dropped from the AMS list", "[BambuAmsReport]")
{
    MachineObject obj { nullptr, "fake-h3c", "FAKEN100000002", "127.0.0.1" };
    obj.amsList["32"] = new Ams("32", 0, 6);
    REQUIRE_NOTHROW(obj.parse_ams_report(n10_print(), false));
    CHECK(obj.amsList.empty());
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
