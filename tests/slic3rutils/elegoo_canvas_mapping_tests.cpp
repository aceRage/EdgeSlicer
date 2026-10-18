// Elegoo CANVAS phase 2: which tray each filament of the plate prints from on a Centauri Carbon 1.
// Pins the start command's slot_map (ElegooStartPrint.hpp), the CANVAS -> send dialog slots
// (PrintHostDevices::status_from_canvas), the dialog's choices -> slot_map entries, the warnings,
// the re-read check and the remembered mappings (ElegooCanvasMapping.hpp).
//
// Nothing here touches a printer, a socket or wx: the CANVAS payload is a mock, and the remembered
// mappings go to a temporary file of their own.

#include <catch2/catch.hpp>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <ctime>
#include <map>
#include <string>
#include <vector>

#include "slic3r/Utils/ElegooCanvas.hpp"
#include "slic3r/Utils/ElegooCanvasMapping.hpp"
#include "slic3r/Utils/ElegooStartPrint.hpp"
#include "slic3r/Utils/PrintHostDeviceStatus.hpp"

using namespace Slic3r;
using ElegooStartPrint::SlotMapEntry;
using ElegooCanvasMapping::UsedFilament;
using PrintHostDevices::Slot;

namespace {

using Info = std::map<std::string, std::string>;

nlohmann::json start_data(const Info& info)
{
    return nlohmann::json::parse(ElegooStartPrint::build_payload(info, 1, "cube.gcode", "req-1", 1735032553000LL)).at("Data").at("Data");
}

// One CANVAS unit: PLA red in tray 0, PETG black in tray 1, tray 2 empty, PLA white in tray 3.
CanvasQueryResult mock_canvas(int tray_base = 0)
{
    const std::string b0 = std::to_string(tray_base), b1 = std::to_string(tray_base + 1), b2 = std::to_string(tray_base + 2),
                      b3 = std::to_string(tray_base + 3);
    const std::string payload = R"({"active_canvas_id":0,"active_tray_id":-1,"canvas_list":[{"canvas_id":0,"connected":1,"tray_list":[
        {"tray_id":)" + b0 + R"(,"brand":"ELEGOO","filament_type":"PLA","filament_name":"PLA","filament_color":"#FF0000","min_nozzle_temp":190,"max_nozzle_temp":230,"status":2},
        {"tray_id":)" + b1 + R"(,"brand":"ELEGOO","filament_type":"PETG","filament_name":"PETG","filament_color":"#000000","min_nozzle_temp":220,"max_nozzle_temp":250,"status":2},
        {"tray_id":)" + b2 + R"(,"brand":"","filament_type":"","filament_name":"","filament_color":"","status":0},
        {"tray_id":)" + b3 + R"(,"brand":"","filament_type":"PLA","filament_name":"","filament_color":"#FFFFFF","min_nozzle_temp":190,"max_nozzle_temp":230,"status":1}]}]})";
    CanvasQueryResult r;
    std::string       error;
    r.reached = true;
    r.ams_connect_status = 1;
    r.ok      = canvas_parse_payload(payload, r.info, error);
    return r;
}

UsedFilament used(int index, const std::string& type, const std::string& colour, const std::string& alias = "Generic PLA", int temp = 210)
{
    UsedFilament u;
    u.index       = index;
    u.type        = type;
    u.colour      = colour;
    u.alias       = alias;
    u.nozzle_temp = temp;
    return u;
}

struct TempStore
{
    boost::filesystem::path path;
    explicit TempStore(const std::string& tag)
    {
        path = boost::filesystem::temp_directory_path() /
               ("elegoo_canvas_mapping_test_" + tag + "_" + std::to_string((long long) ::time(nullptr)) + ".json");
        boost::system::error_code ec;
        boost::filesystem::remove(path, ec);
        ElegooCanvasMapping::set_store_path(path.string());
    }
    ~TempStore()
    {
        boost::system::error_code ec;
        boost::filesystem::remove(path, ec);
        ElegooCanvasMapping::set_store_path("");
    }
};

} // namespace

TEST_CASE("Elegoo start print: no CANVAS, no slot_map - the command is byte-identical to before", "[ElegooCanvas]")
{
    const Info        plain{{"timeLapse", "1"}, {"heatedBedLeveling", "0"}, {"bedType", "1"}};
    const std::string before = ElegooStartPrint::build_payload(plain, 1, "cube.gcode", "r", 5);
    // A slotMap with no CANVAS read (canvasAttached missing) is ignored.
    Info with_stray = plain;
    with_stray["slotMap"] = "0:0:1";
    CHECK(ElegooStartPrint::build_payload(with_stray, 1, "cube.gcode", "r", 5) == before);
    CHECK(before.find("slot_map") == std::string::npos);
    // The field order the printer has always been sent.
    CHECK(before == R"({"Id":"","Data":{"Cmd":128,"Data":{"Filename":"/local/cube.gcode","StartLayer":0,"Calibration_switch":0,)"
                    R"("PrintPlatformType":1,"Tlp_Switch":1},"RequestID":"r","MainboardID":"","TimeStamp":5,"From":1}})");
}

TEST_CASE("Elegoo start print: a CANVAS with every filament as sliced sends an empty slot_map", "[ElegooCanvas]")
{
    const nlohmann::json d = start_data(Info{{"canvasAttached", "1"}, {"slotMap", ""}});
    REQUIRE(d.contains("slot_map"));
    CHECK(d.at("slot_map").is_array());
    CHECK(d.at("slot_map").empty());
    // ...and the switch to an identity map stays off until the owner's test print says otherwise.
    CHECK_FALSE(ElegooStartPrint::AS_SLICED_IDENTITY_MAP);
}

TEST_CASE("Elegoo start print: the mapped filaments go out as t / canvas_id / tray_id", "[ElegooCanvas]")
{
    const nlohmann::json d = start_data(Info{{"canvasAttached", "1"}, {"slotMap", "0:0:2,3:1:0"}});
    const auto&          m = d.at("slot_map");
    REQUIRE(m.size() == 2);
    CHECK(m[0].at("t") == 0);
    CHECK(m[0].at("canvas_id") == 0);
    CHECK(m[0].at("tray_id") == 2);
    CHECK(m[1].at("t") == 3);
    CHECK(m[1].at("canvas_id") == 1);
    CHECK(m[1].at("tray_id") == 0);
    // The other options are where they were.
    CHECK(d.at("Tlp_Switch") == 0);
    CHECK(d.at("Filename") == "/local/cube.gcode");
}

TEST_CASE("Elegoo start print: the slotMap text round-trips and junk is dropped", "[ElegooCanvas]")
{
    const std::vector<SlotMapEntry> e = { { 0, 0, 1 }, { 2, 0, 3 } };
    CHECK(ElegooStartPrint::format_slot_map(e) == "0:0:1,2:0:3");
    CHECK(ElegooStartPrint::parse_slot_map("0:0:1,2:0:3") == e);
    CHECK(ElegooStartPrint::parse_slot_map("").empty());
    const auto junk = ElegooStartPrint::parse_slot_map("x:0:1,1:0,1:0:2,-1:0:0,3:a:1,4:0:1z");
    REQUIRE(junk.size() == 1);
    CHECK(junk[0] == SlotMapEntry{ 1, 0, 2 });
}

TEST_CASE("A CANVAS read becomes the send dialog's slots", "[ElegooCanvas]")
{
    const PrintHostDevices::Status st = PrintHostDevices::status_from_canvas(mock_canvas());
    CHECK(st.probed);
    CHECK(st.online);
    CHECK(st.canvas);
    CHECK(st.slots_have_filaments);
    REQUIRE(st.slots.size() == 4);
    CHECK(st.slots[0].index == 0);
    CHECK(st.slots[0].unit == 0);
    CHECK(st.slots[0].tray == 0);
    CHECK(st.slots[0].loaded);
    CHECK(st.slots[0].color == "#FF0000");
    CHECK(st.slots[0].type == "PLA");
    CHECK(st.slots[0].name == "ELEGOO PLA");
    CHECK(st.slots[0].min_temp == 190);
    CHECK(st.slots[0].max_temp == 230);
    CHECK(st.slots[0].label() == "Tray 1 PLA");
    CHECK(! st.slots[2].loaded);
    CHECK(st.slots[2].label() == "Tray 3 (empty)");
    CHECK(st.slots[3].name == "PLA");

    SECTION("no CANVAS attached, or no answer: nothing to map, and the dialog says why")
    {
        CanvasQueryResult none;
        none.reached   = true;
        none.no_canvas = true;
        const auto a   = PrintHostDevices::status_from_canvas(none);
        CHECK_FALSE(a.canvas);
        CHECK(a.slots.empty());
        CHECK(a.note.find("no CANVAS") != std::string::npos);

        CanvasQueryResult off;
        off.error    = "connect: timed out";
        const auto b = PrintHostDevices::status_from_canvas(off);
        CHECK_FALSE(b.online);
        CHECK_FALSE(b.canvas);
        CHECK(b.note.find("did not answer") != std::string::npos);
    }
}

TEST_CASE("The dialog's choices become slot_map entries with the raw ids", "[ElegooCanvas]")
{
    const auto slots_0 = PrintHostDevices::status_from_canvas(mock_canvas(0)).slots;
    const auto slots_1 = PrintHostDevices::status_from_canvas(mock_canvas(1)).slots;
    const std::vector<UsedFilament> plate = { used(0, "PLA", "#FE0000"), used(2, "PETG", "#000000", "Generic PETG", 240) };

    SECTION("mapped filaments carry the printer's own tray_id, whatever base it counts from")
    {
        CHECK(ElegooCanvasMapping::slot_map_for(plate, { 0, 1 }, slots_0, false) ==
              std::vector<SlotMapEntry>{ { 0, 0, 0 }, { 2, 0, 1 } });
        CHECK(ElegooCanvasMapping::slot_map_for(plate, { 0, 1 }, slots_1, false) ==
              std::vector<SlotMapEntry>{ { 0, 0, 1 }, { 2, 0, 2 } });
    }
    SECTION("As sliced sends nothing for that filament")
    {
        CHECK(ElegooCanvasMapping::slot_map_for(plate, { -1, 1 }, slots_0, false) == std::vector<SlotMapEntry>{ { 2, 0, 1 } });
        CHECK(ElegooCanvasMapping::slot_map_for(plate, { -1, -1 }, slots_0, false).empty());
    }
    SECTION("the identity switch maps As sliced filament t onto tray position t")
    {
        CHECK(ElegooCanvasMapping::slot_map_for(plate, { -1, -1 }, slots_1, true) ==
              std::vector<SlotMapEntry>{ { 0, 0, 1 }, { 2, 0, 3 } });
    }
    SECTION("a slot that is not a CANVAS tray is never named")
    {
        std::vector<Slot> moonraker(2);
        moonraker[0].index = 0;
        moonraker[1].index = 1;
        CHECK(ElegooCanvasMapping::slot_map_for(plate, { 0, 1 }, moonraker, true).empty());
    }
}

TEST_CASE("CANVAS mapping warnings never block, and say what is odd", "[ElegooCanvas]")
{
    const auto slots = PrintHostDevices::status_from_canvas(mock_canvas()).slots;
    using K          = ElegooCanvasMapping::WarningKind;
    auto has = [](const std::vector<ElegooCanvasMapping::Warning>& w, K k) {
        for (const auto& x : w)
            if (x.kind == k)
                return true;
        return false;
    };

    SECTION("a clean mapping")
    {
        const auto w = ElegooCanvasMapping::warnings_for({ used(0, "PLA", "#FF0000"), used(1, "PETG", "#000000", "P", 240) }, { 0, 1 }, slots);
        CHECK(w.empty());
    }
    SECTION("two filaments on one tray")
    {
        const auto w = ElegooCanvasMapping::warnings_for({ used(0, "PLA", "#FF0000"), used(1, "PLA", "#FF1010") }, { 0, 0 }, slots);
        CHECK(has(w, K::SharedTray));
    }
    SECTION("a nozzle temperature outside the tray's range")
    {
        const auto w = ElegooCanvasMapping::warnings_for({ used(0, "PETG", "#000000", "P", 200) }, { 1 }, slots);
        REQUIRE(has(w, K::TemperatureOutOfRange));
        CHECK(w[0].filament == 0);
        CHECK(w[0].slot == 1);
    }
    SECTION("more filaments than loaded trays")
    {
        const std::vector<UsedFilament> four = { used(0, "PLA", "#FF0000"), used(1, "PLA", "#00FF00"), used(2, "PLA", "#0000FF"),
                                                 used(3, "PLA", "#FFFFFF") };
        CHECK(has(ElegooCanvasMapping::warnings_for(four, { -1, -1, -1, -1 }, slots), K::MoreFilamentsThanTrays));
    }
    SECTION("an empty tray")
    {
        CHECK(has(ElegooCanvasMapping::warnings_for({ used(0, "PLA", "#FF0000") }, { 2 }, slots), K::EmptyTray));
    }
}

TEST_CASE("A mapped tray that has gone stops the send before the upload", "[ElegooCanvas]")
{
    const auto                      slots   = PrintHostDevices::status_from_canvas(mock_canvas()).slots;
    const std::vector<SlotMapEntry> entries = { { 0, 0, 0 }, { 1, 0, 3 } };
    CHECK(ElegooCanvasMapping::validate(entries, slots).empty());

    auto emptied      = slots;
    emptied[3].loaded = false;
    CHECK(ElegooCanvasMapping::validate(entries, emptied).find("now empty") != std::string::npos);

    auto unplugged = slots;
    unplugged.pop_back();
    CHECK(ElegooCanvasMapping::validate(entries, unplugged).find("no longer reported") != std::string::npos);
}

TEST_CASE("Remembered CANVAS mappings follow the spool, not the tray number", "[ElegooCanvas]")
{
    TempStore store("remember");
    const auto slots = PrintHostDevices::status_from_canvas(mock_canvas()).slots;
    const UsedFilament red = used(0, "PLA", "#ff0000", "Elegoo PLA @ECC");

    // First send: the red PLA went to the white tray on purpose.
    std::map<std::string, ElegooCanvasMapping::Remembered> memory;
    ElegooCanvasMapping::remember(memory, { red }, { 3 }, slots);
    ElegooCanvasMapping::save(memory);
    REQUIRE(boost::filesystem::exists(store.path));

    SECTION("the file holds the tray's content under the filament's key")
    {
        const auto back = ElegooCanvasMapping::load();
        REQUIRE(back.size() == 1);
        const auto it = back.find(ElegooCanvasMapping::key_for(red));
        REQUIRE(it != back.end());
        CHECK(it->first == "PLA|Elegoo PLA @ECC|#FF0000");
        CHECK(it->second.type == "PLA");
        CHECK(it->second.name == "PLA");
        CHECK(it->second.colour == "#FFFFFF");
    }
    SECTION("the next send preselects it")
    {
        CHECK(ElegooCanvasMapping::recall(ElegooCanvasMapping::load(), red, slots) == 3);
    }
    SECTION("the white spool moved to tray 1: the mapping moves with it")
    {
        auto moved = slots;
        std::swap(moved[1], moved[3]);
        CHECK(ElegooCanvasMapping::recall(ElegooCanvasMapping::load(), red, moved) == 1);
    }
    SECTION("the spool is gone: no preselection, the colour match decides")
    {
        auto gone      = slots;
        gone[3].loaded = false;
        CHECK(ElegooCanvasMapping::recall(ElegooCanvasMapping::load(), red, gone) == -1);
    }
    SECTION("another filament has no memory")
    {
        CHECK(ElegooCanvasMapping::recall(ElegooCanvasMapping::load(), used(1, "PETG", "#000000"), slots) == -1);
    }
    SECTION("As sliced is not remembered, and an unreadable file is an empty memory")
    {
        std::map<std::string, ElegooCanvasMapping::Remembered> m2;
        ElegooCanvasMapping::remember(m2, { red }, { -1 }, slots);
        CHECK(m2.empty());
        {
            boost::nowide::ofstream out(store.path.string(), std::ios::trunc);
            out << "{ not json";
        }
        CHECK(ElegooCanvasMapping::load().empty());
    }
}
