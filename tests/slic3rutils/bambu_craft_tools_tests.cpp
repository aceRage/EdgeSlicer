#include <catch2/catch.hpp>

// The wx / Windows headers first, as every GUI source has them (GUI_App.hpp): a libslic3r header
// read before them leaves std::byte and the SDK's byte ambiguous (see bambu_reprint_tests.cpp).
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/BambuCraftTools.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/DualNozzleState.hpp"
#include "libslic3r/BambuDualNozzleSync.hpp"
#include "libslic3r/NozzleOverride.hpp"
#include "libslic3r/ToolSlots.hpp"

#include <algorithm>

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::BambuCraftTools;
using nlohmann::json;

// Craft kit tools (cutter, pen, laser, fan) in a Bambu printer's nozzle list.
//
// No printer with a craft tool has been seen yet. Every payload below is made up in the shape
// Bambu Studio's parser reads (v02.09.01.70 DevNozzleSystem.cpp s_parse_nozzle_type: the first
// character of "type" says hotend / cutter / laser / fan, a cutter's second character 'E' is the
// pen; DevExtensionTool.cpp for "ext_tool"), around the nozzle-only rack reports the other tests
// use. They are derived from that source, not captured from a printer.

namespace {

// A new-protocol print report: the four flag strings (empty) and a device block.
json np_print(const json& nozzle, const json& extruder)
{
    json p;
    p["cfg"]              = "";
    p["fun"]              = "";
    p["aux"]              = "";
    p["stat"]             = "";
    p["device"]           = json::object();
    p["device"]["nozzle"] = nozzle;
    if (!extruder.is_null()) p["device"]["extruder"] = extruder;
    return p;
}

// One device.nozzle.info entry. diameter < 0: the key is left out.
json tool(int id, const std::string& type, double diameter = 0.4, int addr = -1)
{
    json n = { { "id", id }, { "type", type }, { "stat", 0 }, { "wear", 0 } };
    if (diameter >= 0.) n["diameter"] = diameter;
    if (addr >= 0) n["addr_id"] = addr;
    return n;
}

// device.nozzle with every listed id present in "exist".
json nozzle_block(const json& info)
{
    int exist = 0;
    for (const json& n : info) exist |= 1 << n["id"].get<int>();
    return { { "exist", exist }, { "state", 0 }, { "info", info } };
}

// One device.extruder.info entry: extruder `id` with the tool `hnow` on it, nothing loaded.
json extruder(int id, int hnow)
{
    return { { "id", id }, { "hnow", hnow }, { "hpre", hnow }, { "htar", hnow }, { "info", 0 }, { "temp", 0 },
             { "snow", 65535 }, { "spre", 65535 }, { "star", 65535 }, { "stat", 0 } };
}

json extruder_block(const json& info, int count) { return { { "state", count }, { "info", info } }; }

// An H2C (two extruders, rack on the main / right one): extruder nozzles 0 and 1, rack positions 0x10.. .
json h2c_report(const std::string& rack2_type, const std::string& mounted_type = "HS01")
{
    json info = json::array({ tool(0, mounted_type), tool(1, "HS01"), tool(0x10, "HS01", 0.6), tool(0x11, "HH01", 0.4),
                              tool(0x12, rack2_type, -1) });
    return np_print(nozzle_block(info), extruder_block(json::array({ extruder(0, 0), extruder(1, 1) }), 2));
}

// A printer object that has read `print` (MachineObject is neither copied nor moved).
struct Parsed
{
    MachineObject obj { nullptr, "fake", "FAKECRAFT0001", "127.0.0.1" };
    Parsed(const json& print, const char* model)
    {
        obj.printer_type = model;
        obj.parse_new_info(print);
    }
};

const Nozzle* find_nozzle(const MachineObject& obj, int id)
{
    for (const Nozzle& n : obj.m_nozzle_data.nozzles)
        if (n.id == id) return &n;
    return nullptr;
}

DualNozzleSync::PrinterState reported(const MachineObject& obj)
{
    DualNozzleSync::PrinterState s;
    s.physical_extruder_map = { 1, 0 };
    DualNozzle::add_reported_nozzles(s, &obj);
    return s;
}

} // namespace

TEST_CASE("A nozzle-list type names the tool by its first letter", "[BambuCraftTools]")
{
    // Hotends, as every report so far has them.
    CHECK(tool_kind_of("HS01") == ToolKind::Nozzle);
    CHECK(tool_kind_of("HH01") == ToolKind::Nozzle);
    CHECK(tool_kind_of("HT01") == ToolKind::Nozzle);
    CHECK(tool_kind_of("HS00-0.4") == ToolKind::Nozzle);
    CHECK(tool_kind_of("hardened_steel") == ToolKind::Nozzle);
    CHECK(tool_kind_of("stainless_steel") == ToolKind::Nozzle); // not 's' = unknown: the long names come first
    // Cutting tools: 'C', the second letter the kind; 'E' is the pen.
    CHECK(tool_kind_of("CA00") == ToolKind::Cutter);
    CHECK(tool_kind_of("CP00") == ToolKind::Cutter);
    CHECK(tool_kind_of("CU00") == ToolKind::Cutter);
    CHECK(tool_kind_of("C000") == ToolKind::Cutter);
    CHECK(tool_kind_of("CZ00") == ToolKind::Cutter); // a cutter kind not known yet is still a cutter
    CHECK(tool_kind_of("CE00") == ToolKind::Pen);
    CHECK(tool_kind_of("L000") == ToolKind::Laser);
    CHECK(tool_kind_of("F000") == ToolKind::Fan);
    // Lower case reads the same.
    CHECK(tool_kind_of("ce00") == ToolKind::Pen);
    CHECK(tool_kind_of("ca00") == ToolKind::Cutter);
    CHECK(tool_kind_of("hs01") == ToolKind::Nozzle);
    // Short, empty and N/A: a nozzle, as EdgeSlicer always read them.
    CHECK(tool_kind_of("") == ToolKind::Nozzle);
    CHECK(tool_kind_of("N/A") == ToolKind::Nozzle);
    CHECK(tool_kind_of("C") == ToolKind::Nozzle);
    CHECK(tool_kind_of("CE0") == ToolKind::Nozzle);
    // Another first letter: unknown, and still treated as a nozzle.
    CHECK(tool_kind_of("XS01") == ToolKind::Unknown);
    CHECK(prints(ToolKind::Unknown));
    CHECK(prints(ToolKind::Nozzle));
    for (ToolKind k : { ToolKind::Cutter, ToolKind::Pen, ToolKind::Laser, ToolKind::Fan }) {
        CHECK_FALSE(prints(k));
        CHECK(is_craft_tool(k));
    }
    CHECK(std::string(tool_kind_key(ToolKind::Pen)) == "pen");
    CHECK(std::string(tool_kind_name(ToolKind::Laser)) == "Laser");
}

TEST_CASE("A nozzle-only rack reads exactly as before", "[BambuCraftTools]")
{
    const Parsed obj_p(h2c_report("HS01"), "O1C2");
    const MachineObject& obj = obj_p.obj;
    REQUIRE(obj.m_nozzle_data.nozzles.size() == 5);
    for (const Nozzle& n : obj.m_nozzle_data.nozzles) {
        CHECK(n.tool_kind == ToolKind::Nozzle);
        CHECK(n.prints());
        CHECK(n.diameter > 0.f);
    }
    // A real nozzle that does not say its size is still taken as 0.4 mm.
    REQUIRE(find_nozzle(obj, 0x12));
    CHECK(find_nozzle(obj, 0x12)->diameter == Approx(0.4f));
    CHECK(find_nozzle(obj, 0x12)->nozzle_type == NozzleType::ntHardenedSteel);
    CHECK_FALSE(obj.reports_craft_tools());
    REQUIRE(obj.m_extder_data.extders.size() == 2);
    CHECK(obj.m_extder_data.extders[0].current_tool == ToolKind::Nozzle);
    CHECK(obj.m_extder_data.extders[0].current_nozzle_diameter == Approx(0.4f));
    CHECK(obj.m_extder_data.extders[0].current_nozzle_type == NozzleType::ntHardenedSteel);
    // Every entry goes to the nozzle sync and the nozzle mapping.
    CHECK(reported(obj).nozzles.size() == 5);
    // The send check passes whatever the job uses.
    CHECK(obj.craft_tool_send_check({ 0, 1 }).ok());
    CHECK(obj.craft_tool_send_check({}).ok());
}

TEST_CASE("A cutter in a rack position is no nozzle", "[BambuCraftTools]")
{
    const Parsed obj_p(h2c_report("CA00"), "O1C2");
    const MachineObject& obj = obj_p.obj;
    const Nozzle*       c   = find_nozzle(obj, 0x12);
    REQUIRE(c);
    CHECK(c->tool_kind == ToolKind::Cutter);
    CHECK(c->type_code == "CA00");
    CHECK_FALSE(c->prints());
    CHECK(c->diameter == Approx(0.f)); // not the invented 0.4
    CHECK(c->nozzle_type == NozzleType::ntUndefine);
    CHECK(obj.reports_craft_tools());

    // Not synced, not offered to the nozzle mapping: the four nozzles only.
    const DualNozzleSync::PrinterState s = reported(obj);
    CHECK(s.nozzles.size() == 4);
    for (const auto& n : s.nozzles) CHECK(n.pos != 0x12);
    // The main extruder's rack: its own 0.4, then 0.6 and 0.4 HF - no cutter slot of 0.4.
    const std::vector<std::vector<RackNozzle>> rack = reported_rack_nozzles(s, 2);
    REQUIRE(rack.size() == 2);
    CHECK(rack[1].size() == 3);

    // Nozzles are there for both extruders: the job may go.
    CHECK(obj.craft_tool_send_check({ 0, 1 }).ok());
}

TEST_CASE("A pen, a laser and a fan read as themselves", "[BambuCraftTools]")
{
    CHECK(find_nozzle(Parsed(h2c_report("CE00"), "O1C2").obj, 0x12)->tool_kind == ToolKind::Pen);
    CHECK(find_nozzle(Parsed(h2c_report("L000"), "O1C2").obj, 0x12)->tool_kind == ToolKind::Laser);
    CHECK(find_nozzle(Parsed(h2c_report("F000"), "O1C2").obj, 0x12)->tool_kind == ToolKind::Fan);
    CHECK(find_nozzle(Parsed(h2c_report("ce00"), "O1C2").obj, 0x12)->tool_kind == ToolKind::Pen);
    for (const char* t : { "CE00", "L000", "F000" }) {
        const Parsed obj_p(h2c_report(t), "O1C2");
        const MachineObject& obj = obj_p.obj;
        CHECK(find_nozzle(obj, 0x12)->diameter == Approx(0.f));
        CHECK(reported(obj).nozzles.size() == 4);
    }
}

TEST_CASE("Short, empty and odd types never throw and stay nozzles", "[BambuCraftTools]")
{
    for (const char* t : { "", "C", "N/A", "XS01" }) {
        const Parsed obj_p(h2c_report(t), "O1C2");
        const MachineObject& obj = obj_p.obj;
        REQUIRE(find_nozzle(obj, 0x12));
        CHECK(find_nozzle(obj, 0x12)->prints());
        CHECK(find_nozzle(obj, 0x12)->diameter == Approx(0.4f)); // no diameter key: 0.4, as before
        CHECK_FALSE(obj.reports_craft_tools());
    }
    // A type that is not a string, an entry without an id, info / exist of the wrong JSON type.
    json print = h2c_report("HS01");
    json& info = print["device"]["nozzle"]["info"];
    info[2]["type"] = 7;
    info.push_back({ { "type", "CA00" } }); // no id: skipped
    MachineObject obj { nullptr, "fake", "FAKECRAFT0002", "127.0.0.1" };
    REQUIRE_NOTHROW(obj.parse_new_info(print));
    CHECK(obj.m_nozzle_data.nozzles.size() == 5);
    CHECK(find_nozzle(obj, 0x10)->prints());

    json bad = h2c_report("HS01");
    bad["device"]["nozzle"]["exist"] = "zz";
    bad["device"]["nozzle"]["info"]  = "not a list";
    MachineObject obj2 { nullptr, "fake", "FAKECRAFT0003", "127.0.0.1" };
    REQUIRE_NOTHROW(obj2.parse_new_info(bad));
    CHECK(obj2.m_nozzle_data.nozzles.empty());
    CHECK(obj2.m_extder_data.extders.size() == 2); // the rest of the device block is still read
}

TEST_CASE("A cutter on the extruder leaves its nozzle unknown", "[BambuCraftTools]")
{
    // The main extruder holds the cutter (hnow 0, type CA00); the rack still has nozzles for it.
    const Parsed obj_p(h2c_report("HS01", "CA00"), "O1C2");
    const MachineObject& obj = obj_p.obj;
    const Extder&       e   = obj.m_extder_data.extders[0];
    CHECK(e.current_tool == ToolKind::Cutter);
    CHECK(e.current_nozzle_type == NozzleType::ntUndefine);
    CHECK(e.current_nozzle_diameter == Approx(0.f)); // unknown, as with no nozzle mounted
    CHECK(obj.nozzle_diameter(0) == Approx(0.f));
    CHECK(obj.m_extder_data.extders[1].current_tool == ToolKind::Nozzle);
    // The printer swaps a rack nozzle in: allowed.
    CHECK(obj.craft_tool_send_check({ 0, 1 }).ok());
}

TEST_CASE("The send check blocks a job that needs a nozzle where there is a tool", "[BambuCraftTools]")
{
    SECTION("an H2D whose left extruder holds a pen")
    {
        json info = json::array({ tool(0, "HS01"), tool(1, "CE00", -1) });
        const Parsed obj_p(np_print(nozzle_block(info), extruder_block(json::array({ extruder(0, 0), extruder(1, 1) }), 2)), "O1D");
        const MachineObject& obj = obj_p.obj;
        // A job on the right extruder only: fine.
        CHECK(obj.craft_tool_send_check({ 0 }).ok());
        // One that prints on the left: refused, naming the pen.
        const SendCheck c = obj.craft_tool_send_check({ 0, 1 });
        CHECK(c.block == SendBlock::ExtruderNoHotend);
        CHECK(c.physical_extruder == 1);
        CHECK(c.mounted == ToolKind::Pen);
        CHECK(send_block_text(c, true).find("the left extruder holds a pen, not a nozzle") != std::string::npos);
        // Extruders not known (a file from the printer's storage): only the whole-printer check, which passes.
        CHECK(obj.craft_tool_send_check({}).ok());
    }
    SECTION("an H3C whose rack holds only a cutter and a laser")
    {
        json info = json::array({ tool(0x10, "CA00", -1, 0), tool(0x11, "L000", -1, 1) });
        const Parsed obj_p(np_print(nozzle_block(info), extruder_block(json::array({ extruder(0, 0x10) }), 1)), "N10");
        const MachineObject& obj = obj_p.obj;
        const SendCheck     c   = obj.craft_tool_send_check({ 0 });
        CHECK(c.block == SendBlock::NoHotend);
        CHECK(obj.craft_tool_send_check({}).block == SendBlock::NoHotend);
        CHECK(send_block_text(c, false).find("no nozzle to print with") != std::string::npos);
        CHECK(reported(obj).nozzles.empty());
        CHECK(obj.m_extder_data.extders[0].current_tool == ToolKind::Cutter);
    }
    SECTION("a nozzle that is reported but not there does not count")
    {
        json print = h2c_report("CA00", "CA00");
        // Only the left extruder's nozzle (id 1) is present; the rack's nozzles have gone.
        print["device"]["nozzle"]["exist"] = (1 << 0) | (1 << 1) | (1 << 0x12);
        const Parsed obj_p(print, "O1C2");
        const MachineObject& obj = obj_p.obj;
        const SendCheck     c   = obj.craft_tool_send_check({ 0 });
        CHECK(c.block == SendBlock::ExtruderNoHotend);
        CHECK(c.physical_extruder == 0);
        CHECK(c.mounted == ToolKind::Cutter);
        CHECK(send_block_text(c, true).find("the right extruder holds a cutter") != std::string::npos);
        CHECK(obj.craft_tool_send_check({ 1 }).ok());
    }
    SECTION("the pure check: nozzle-only lists always pass")
    {
        const std::vector<Tool> none_there { { 0, ToolKind::Nozzle, false }, { 0x10, ToolKind::Nozzle, false } };
        CHECK(check_send(none_there, { { 0, 0 } }, { 0 }).ok()); // as before: not this check's business
        CHECK(check_send({}, {}, { 0 }).ok());
        CHECK(send_block_text(SendCheck(), true).empty());
    }
}

TEST_CASE("An H3C rack position holding a cutter feeds no slot", "[BambuCraftTools]")
{
    // Five positional rack entries, position 3 the cutter; nine spool holders 255..247 (a rack printer).
    json info = json::array();
    for (int addr = 0; addr < 5; ++addr) info.push_back(tool(0x10 + addr, addr == 3 ? "CA00" : "HS01", addr == 3 ? -1 : 0.4, addr));
    const json print = np_print(nozzle_block(info), extruder_block(json::array({ extruder(0, 0x10) }), 1));
    MachineObject obj { nullptr, "fake", "FAKECRAFT0004", "127.0.0.1" };
    obj.printer_type = "N10";
    obj.parse_new_info(print);
    for (int id = 255; id >= 247; --id) obj.vir_slots.push_back(AmsTray(std::to_string(id)));
    REQUIRE(obj.is_rack_printer());

    const std::vector<std::pair<int, int>> bound = obj.rack_bound_slots();
    const std::vector<std::pair<int, int>> want { { 255, 0 }, { 254, 0 }, { 253, 0 }, { 251, 0 } }; // no 252 (position 3)
    CHECK(bound == want);

    // Synced as a positional rack: position 3 is a gap, not the next nozzle moved up.
    DualNozzleSync::PrinterState s;
    s.physical_extruder_map = { 0 }; // the H3C profile's
    DualNozzle::add_reported_nozzles(s, &obj);
    CHECK(s.nozzles.size() == 4);
    const std::vector<std::vector<RackNozzle>> layout = reported_rack_nozzles(s, 1);
    REQUIRE(layout.size() == 1);
    REQUIRE(layout[0].size() == 5);
    CHECK(layout[0][3].diameter == Approx(0.));
    CHECK(layout[0][4].diameter == Approx(0.4));
}

TEST_CASE("The extension tool beside the toolhead is read and shown", "[BambuCraftTools]")
{
    CHECK(ext_tool_kind_of("CP00") == ExtToolKind::Cutter);
    CHECK(ext_tool_kind_of("LB00") == ExtToolKind::Laser);
    CHECK(ext_tool_kind_of("F000") == ExtToolKind::Fan);
    CHECK(ext_tool_kind_of("") == ExtToolKind::None);
    CHECK(ext_tool_kind_of("ZZ99") == ExtToolKind::Unknown);
    CHECK(ext_tool_display_kind(ExtToolKind::Laser) == ToolKind::Laser);

    ExtTool t = parse_ext_tool(json::parse(R"({"type":"LB00","mount_3d":1,"calib":2})"));
    CHECK(t.reported);
    CHECK(t.kind == ExtToolKind::Laser);
    CHECK(t.mounted());
    CHECK(t.calib == 2);
    // A field missing or of the wrong type keeps what was there.
    t = parse_ext_tool(json::parse(R"({"mount_3d":"x"})"), t);
    CHECK(t.kind == ExtToolKind::Laser);
    CHECK(t.mount == 1);
    t = parse_ext_tool(json::parse(R"({"mount_3d":0})"), t);
    CHECK_FALSE(t.mounted());
    CHECK_NOTHROW(parse_ext_tool(json("odd")));
    CHECK_FALSE(parse_ext_tool(json()).reported);

    // Through the report.
    json print = h2c_report("HS01");
    print["device"]["ext_tool"] = json::parse(R"({"type":"CP00","mount_3d":1,"calib":0})");
    const Parsed obj_p(print, "O1D");
    const MachineObject& obj = obj_p.obj;
    CHECK(obj.m_ext_tool.kind == ExtToolKind::Cutter);
    CHECK(obj.m_ext_tool.mounted());
    CHECK_FALSE(obj.reports_craft_tools()); // an extension tool is not in the nozzle list
}
