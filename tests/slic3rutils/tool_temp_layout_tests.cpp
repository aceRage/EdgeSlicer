#include <catch2/catch.hpp>

#include "slic3r/GUI/ToolTempLayout.hpp"

using namespace Slic3r::GUI;

namespace {

RackNozzleSample rack_nozzle(int pos, int temp, int target, float diameter = 0.4f, bool high_flow = false)
{
    RackNozzleSample s;
    s.report_id = 0x10 + pos;
    s.addr_id   = pos;
    s.has_temp  = true;
    s.temp      = temp;
    s.target    = target;
    s.diameter  = diameter;
    s.high_flow = high_flow;
    return s;
}

// The mockup's H3C rack (case 4): slot 2 heating, 3 mounted, 5 at target, 7 empty, 8 a cutter,
// 9 an unknown tool. Slots are 1-based in the UI, positions 0-based here.
std::vector<RackNozzleSample> mockup_rack()
{
    std::vector<RackNozzleSample> v;
    v.push_back(rack_nozzle(0, 32, 0));
    v.push_back(rack_nozzle(1, 148, 220, 0.4f, true));
    v.push_back(rack_nozzle(2, 0, 0));
    v.push_back(rack_nozzle(3, 96, 0, 0.2f));
    v.push_back(rack_nozzle(4, 220, 220, 0.6f, true));
    v.push_back(rack_nozzle(5, 28, 0, 0.8f));
    RackNozzleSample empty = rack_nozzle(6, 0, 0);
    empty.exist            = false;
    empty.has_temp         = false;
    v.push_back(empty);
    RackNozzleSample cutter = rack_nozzle(7, 0, 0);
    cutter.kind             = ToolKind::Cutter;
    cutter.has_temp         = false;
    v.push_back(cutter);
    RackNozzleSample unknown = rack_nozzle(8, 27, 0);
    unknown.kind             = ToolKind::Unknown;
    v.push_back(unknown);
    // the extruder's nozzle, taken from rack position 2 (slot 3)
    RackNozzleSample extruder;
    extruder.report_id = 0;
    extruder.addr_id   = 2;
    extruder.diameter  = 0.4f;
    v.push_back(extruder);
    return v;
}

const ToolTempEntry &slot_at(const RackEntries &r, int pos)
{
    for (const ToolTempEntry &e : r.slots)
        if (e.slot == pos) return e;
    FAIL("no rack slot at position " << pos);
    return r.slots.front();
}

} // namespace

TEST_CASE("Tool count picks the column layout", "[ToolTempLayout]")
{
    SECTION("0 and 1 tool: today's single row, no strip")
    {
        for (int n : {0, 1}) {
            const ToolTempLayoutPlan p = tool_temp_layout(n, 0);
            CHECK(p.column == ToolColumn::Single);
            CHECK(p.uses_fixed_rows());
            CHECK_FALSE(p.has_strip());
        }
    }
    SECTION("2 tools: today's two rows, no strip")
    {
        const ToolTempLayoutPlan p = tool_temp_layout(2, 0);
        CHECK(p.column == ToolColumn::Dual);
        CHECK(p.uses_fixed_rows());
        CHECK_FALSE(p.has_strip());
    }
    SECTION("3 and 4 tools: compact rows in the column")
    {
        for (int n : {3, 4}) {
            const ToolTempLayoutPlan p = tool_temp_layout(n, 0);
            CHECK(p.column == ToolColumn::Compact);
            CHECK_FALSE(p.uses_fixed_rows());
            CHECK_FALSE(p.has_strip());
        }
    }
    SECTION("5 to 8 tools: two-column grid, no strip")
    {
        for (int n : {5, 8}) {
            const ToolTempLayoutPlan p = tool_temp_layout(n, 0);
            CHECK(p.column == ToolColumn::Grid);
            CHECK_FALSE(p.has_strip());
        }
    }
    SECTION("9 tools: the strip, one row of nine, nothing in the column")
    {
        const ToolTempLayoutPlan p = tool_temp_layout(9, 0);
        CHECK(p.column == ToolColumn::InStrip);
        REQUIRE(p.strip.size() == 1);
        CHECK(p.strip[0].what == ToolStripGroup::What::Tools);
        CHECK(p.strip[0].count == 9);
        CHECK(p.strip[0].cols == 9);
        CHECK(p.strip[0].rows == 1);
    }
    SECTION("16 tools: the strip in two rows of eight")
    {
        const ToolTempLayoutPlan p = tool_temp_layout(16, 0);
        CHECK(p.column == ToolColumn::InStrip);
        REQUIRE(p.strip.size() == 1);
        CHECK(p.strip[0].cols == 8);
        CHECK(p.strip[0].rows == 2);
    }
}

TEST_CASE("A rack adds a strip group and never changes the column", "[ToolTempLayout]")
{
    SECTION("H3C: one extruder row plus a nine-slot rack strip")
    {
        const ToolTempLayoutPlan p = tool_temp_layout(1, 9);
        CHECK(p.column == ToolColumn::Single);
        CHECK(p.uses_fixed_rows());
        REQUIRE(p.strip.size() == 1);
        CHECK(p.strip[0].what == ToolStripGroup::What::Rack);
        CHECK(p.strip[0].cols == 9);
        CHECK(p.strip[0].rows == 1);
    }
    SECTION("two extruders and a rack keep the two rows")
    {
        const ToolTempLayoutPlan p = tool_temp_layout(2, 6);
        CHECK(p.column == ToolColumn::Dual);
        REQUIRE(p.strip.size() == 1);
        CHECK(p.strip[0].cols == 6);
    }
    SECTION("9+ tools and a rack: tools group first, then the rack")
    {
        const ToolTempLayoutPlan p = tool_temp_layout(10, 9);
        REQUIRE(p.strip.size() == 2);
        CHECK(p.strip[0].what == ToolStripGroup::What::Tools);
        CHECK(p.strip[0].cols == 5);
        CHECK(p.strip[0].rows == 2);
        CHECK(p.strip[1].what == ToolStripGroup::What::Rack);
    }
}

TEST_CASE("Strip columns stay within nine cells", "[ToolTempLayout]")
{
    CHECK(tool_strip_columns(0) == 0);
    CHECK(tool_strip_columns(1) == 1);
    CHECK(tool_strip_columns(9) == 9);
    CHECK(tool_strip_columns(10) == 5);
    CHECK(tool_strip_columns(16) == 8);
    CHECK(tool_strip_columns(18) == 9);
    CHECK(tool_strip_columns(30) == 9);
}

TEST_CASE("Extruder rows become settable tool entries in display order", "[ToolTempLayout]")
{
    SECTION("two nozzles keep L above R")
    {
        const auto rows    = nozzle_temp_rows({{0, 220, 220}, {1, 185, 220}}, 2, 0);
        const auto entries = tool_entries_from_rows(rows);
        REQUIRE(entries.size() == 2);
        CHECK(entries[0].badge == "L");
        CHECK(entries[0].id == "nozzle1");
        CHECK(entries[1].badge == "R");
        CHECK(entries[1].active);
        CHECK(entries[0].settable);
        CHECK(entries[1].settable);
    }
    SECTION("four tools are listed 1 to 4, top down, with the active one marked")
    {
        const auto rows = nozzle_temp_rows({{0, 220, 220}, {1, 168, 220}, {2, 45, 0}, {3, 31, 0}}, 4, 0);
        const auto entries = tool_entries_from_rows(rows);
        REQUIRE(entries.size() == 4);
        for (int i = 0; i < 4; ++i) {
            CHECK(entries[i].extruder_id == i);
            CHECK(entries[i].badge == std::to_string(i + 1));
            CHECK(entries[i].slot == -1);
        }
        CHECK(entries[0].active);
        CHECK(entries[1].heating);
        CHECK_FALSE(entries[2].heating);
    }
}

TEST_CASE("Rack slots from the nozzle report", "[ToolTempLayout][Rack]")
{
    SECTION("no rack entry reports a temperature: no strip at all")
    {
        std::vector<RackNozzleSample> v = mockup_rack();
        for (RackNozzleSample &s : v) s.has_temp = false;
        const RackEntries r = rack_entries(v);
        CHECK(r.slots.empty());
        CHECK(r.mounted_slot == -1);
    }
    SECTION("printer without a rack: no strip")
    {
        RackNozzleSample extruder;
        extruder.report_id = 0;
        CHECK(rack_entries({extruder}).slots.empty());
    }

    const RackEntries r = rack_entries(mockup_rack());
    REQUIRE(r.slots.size() == 9);
    CHECK(r.mounted_slot == 2);
    for (int i = 0; i < 9; ++i) {
        CHECK(r.slots[i].slot == i);
        CHECK(r.slots[i].badge == std::to_string(i + 1));
        CHECK(r.slots[i].id == "rack" + std::to_string(i));
        CHECK_FALSE(r.slots[i].settable);
    }

    SECTION("a waiting nozzle carries its reading and size")
    {
        const ToolTempEntry &e = slot_at(r, 1);
        CHECK(e.has_temp);
        CHECK(e.temp == 148);
        CHECK(e.target == 220);
        CHECK(e.heating);
        CHECK(e.size_text == "0.4 HF");
        CHECK(slot_at(r, 3).size_text == "0.2");
    }
    SECTION("the mounted slot shows no temperature of its own")
    {
        const ToolTempEntry &e = slot_at(r, 2);
        CHECK(e.mounted);
        CHECK(e.present);
        CHECK_FALSE(e.has_temp);
        CHECK_FALSE(e.heating);
    }
    SECTION("an empty slot has nothing")
    {
        const ToolTempEntry &e = slot_at(r, 6);
        CHECK_FALSE(e.present);
        CHECK_FALSE(e.has_temp);
        CHECK(e.size_text.empty());
    }
    SECTION("a cutter never has a temperature")
    {
        const ToolTempEntry &e = slot_at(r, 7);
        CHECK(e.kind == ToolKind::Cutter);
        CHECK_FALSE(e.has_temp);
    }
    SECTION("an unknown tool keeps its reading")
    {
        const ToolTempEntry &e = slot_at(r, 8);
        CHECK(e.kind == ToolKind::Unknown);
        CHECK(e.has_temp);
        CHECK(e.temp == 27);
    }
    SECTION("warm count: slots 2, 4 and 5 (a target, or 50 degrees and up); not the mounted one, not the unknown tool")
    {
        CHECK(rack_warm_count(r.slots) == 3);
    }
}

TEST_CASE("A mounted position missing from the rack list is added as mounted", "[ToolTempLayout][Rack]")
{
    std::vector<RackNozzleSample> v;
    v.push_back(rack_nozzle(0, 30, 0));
    v.push_back(rack_nozzle(1, 31, 0));
    RackNozzleSample extruder;
    extruder.report_id = 0;
    extruder.addr_id   = 4;
    extruder.has_temp  = true;
    extruder.temp      = 220;
    extruder.target    = 220;
    extruder.diameter  = 0.6f;
    v.push_back(extruder);

    const RackEntries r = rack_entries(v);
    REQUIRE(r.slots.size() == 3);
    CHECK(r.mounted_slot == 4);
    const ToolTempEntry &m = slot_at(r, 4);
    CHECK(m.mounted);
    CHECK_FALSE(m.has_temp);
    CHECK(m.size_text == "0.6");
    CHECK(r.slots.back().slot == 4);
}

TEST_CASE("Duplicate rack positions are shown once", "[ToolTempLayout][Rack]")
{
    std::vector<RackNozzleSample> v{rack_nozzle(0, 30, 0), rack_nozzle(0, 99, 200), rack_nozzle(1, 31, 0)};
    const RackEntries r = rack_entries(v);
    REQUIRE(r.slots.size() == 2);
    CHECK(slot_at(r, 0).temp == 30);
}

TEST_CASE("What a strip cell shows", "[ToolTempLayout][Rack]")
{
    const RackEntries r = rack_entries(mockup_rack());

    SECTION("waiting, heating nozzle: reading, read-only target, heater icon, size")
    {
        const ToolCellView v = tool_cell_view(slot_at(r, 1), false);
        CHECK(v.word == ToolCellWord::None);
        CHECK(v.show_temp);
        CHECK(v.show_target);
        CHECK_FALSE(v.settable);
        CHECK(v.heating);
        CHECK(v.show_heater_icon);
        CHECK(v.show_size);
    }
    SECTION("cold nozzle without a target: reading only")
    {
        const ToolCellView v = tool_cell_view(slot_at(r, 0), false);
        CHECK(v.show_temp);
        CHECK_FALSE(v.show_target);
        CHECK_FALSE(v.show_heater_icon);
    }
    SECTION("mounted: 'On tool', green badge and border, no temperature")
    {
        const ToolCellView v = tool_cell_view(slot_at(r, 2), true);
        CHECK(v.word == ToolCellWord::OnTool);
        CHECK(v.highlight_badge);
        CHECK(v.accent_border);
        CHECK_FALSE(v.show_temp);
        CHECK_FALSE(v.settable);
    }
    SECTION("empty: 'Empty', dashed border, dim badge")
    {
        const ToolCellView v = tool_cell_view(slot_at(r, 6), true);
        CHECK(v.word == ToolCellWord::Empty);
        CHECK(v.dashed_border);
        CHECK(v.dim_badge);
        CHECK_FALSE(v.show_temp);
        CHECK_FALSE(v.settable);
    }
    SECTION("non-heating kinds: a word, never a temperature, never settable")
    {
        for (ToolKind k : {ToolKind::Cutter, ToolKind::Pen, ToolKind::Laser, ToolKind::Fan}) {
            ToolTempEntry e = slot_at(r, 7);
            e.kind          = k;
            e.has_temp      = true; // even if a reading slipped through
            e.temp          = 40;
            e.settable      = true;
            const ToolCellView v = tool_cell_view(e, true);
            CHECK(v.word == tool_kind_word(k));
            CHECK(v.word != ToolCellWord::None);
            CHECK_FALSE(v.show_temp);
            CHECK_FALSE(v.settable);
        }
    }
    SECTION("unknown kind: reading, 'Unknown', read-only even with the switch on")
    {
        const ToolCellView v = tool_cell_view(slot_at(r, 8), true);
        CHECK(v.word == ToolCellWord::Unknown);
        CHECK(v.show_temp);
        CHECK_FALSE(v.settable);
    }
    SECTION("the rack off switch: no rack slot is settable while it is off")
    {
        for (const ToolTempEntry &e : r.slots) CHECK_FALSE(tool_cell_view(e, false).settable);
    }
    SECTION("with the switch on only waiting nozzles get a field")
    {
        CHECK(tool_cell_view(slot_at(r, 0), true).settable);
        CHECK(tool_cell_view(slot_at(r, 1), true).settable);
        CHECK_FALSE(tool_cell_view(slot_at(r, 2), true).settable);
        CHECK_FALSE(tool_cell_view(slot_at(r, 6), true).settable);
        CHECK_FALSE(tool_cell_view(slot_at(r, 7), true).settable);
        CHECK_FALSE(tool_cell_view(slot_at(r, 8), true).settable);
    }
    SECTION("a tool on a carriage follows its own settable flag, not the rack switch")
    {
        ToolTempEntry e;
        e.id       = "nozzle9";
        e.badge    = "10";
        e.settable = true;
        e.temp     = 24;
        CHECK(tool_cell_view(e, false).settable);
        e.settable = false;
        CHECK_FALSE(tool_cell_view(e, true).settable);
    }
}

TEST_CASE("Tools header counts tools with a target", "[ToolTempLayout]")
{
    std::vector<ToolTempEntry> tools(16);
    tools[0].target = 215;
    tools[2].target = 240;
    CHECK(tools_with_target_count(tools) == 2);
}
