// Per-nozzle process P9 (NozzleProcess.hpp / ToolSlots.hpp "P9", owner 2026-11-01): the H2C's rack slots as tools of their
// own. Slot ids and their storage, the selector's entries, filament -> slot pins and what a filament resolves to, per-filament
// process sources in the slicing config, the rack nozzle change between slots, the send-time check against the printer's
// reported rack, "Sync from printer", and the messages that name a slot. The preset-bundle part loads the shipped BBL H2C
// profiles.

#include <catch2/catch.hpp>

#include "libslic3r/BambuDualNozzleSync.hpp"
#include "libslic3r/GCode.hpp"
#include "libslic3r/MultiNozzleUtils.hpp"
#include "libslic3r/NozzleOverride.hpp"
#include "libslic3r/NozzleProcess.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ToolSlots.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {

bool has(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }

// An H2C-like printer: left 0.4, right 0.4 with the given rack (one text per extruder), three filaments, filament 1 on the left,
// 2 and 3 on the right.
DynamicPrintConfig h2c(const std::string &right_rack)
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_num_filaments(3);
    cfg.option<ConfigOptionFloats>("nozzle_diameter", true)->values             = { 0.4, 0.4 };
    cfg.option<ConfigOptionBool>("single_extruder_multi_material", true)->value = false;
    cfg.option<ConfigOptionInts>("filament_map", true)->values                  = { 1, 2, 2 };
    // The H2C's rack: one nozzle on the left, up to six on the right.
    cfg.set_key_value("extruder_max_nozzle_count", new ConfigOptionIntsNullable({ 1, 6 }));
    if (!right_rack.empty())
        cfg.option<ConfigOptionStrings>("extruder_rack_nozzle_diameter", true)->values = { "", right_rack };
    return cfg;
}

double outer_wall_speed_of(const DynamicPrintConfig &cfg, unsigned int filament_idx)
{
    const auto *opt = cfg.option<ConfigOptionFloats>("outer_wall_speed");
    return get_value_at(cfg, *opt, ConfigFlowDomain::Process, filament_idx);
}

MultiNozzleUtils::NozzleInfo nozzle(int id, int extruder, const char *diameter)
{
    MultiNozzleUtils::NozzleInfo n;
    n.diameter    = diameter;
    n.volume_type = nvtStandard;
    n.extruder_id = extruder;
    n.group_id    = id;
    return n;
}

} // namespace

// ---- 1. Ids and storage -------------------------------------------------------------------------------------------

TEST_CASE("Rack size tool ids are T<extruder>:<size> and older readers ignore them", "[NozzleRackSlots][Ids]")
{
    CHECK(nozzle_size_group_id(1, 0.6) == "T2:0.6");
    CHECK(nozzle_size_group_id(0, 0.25) == "T1:0.25");
    size_t e = 9;
    double d = 9.;
    REQUIRE(parse_nozzle_size_group_id("T2:0.6", e, d));
    CHECK(e == 1);
    CHECK(d == Approx(0.6));
    CHECK_FALSE(parse_nozzle_size_group_id("T2", e, d));
    CHECK_FALSE(parse_nozzle_size_group_id("T2:", e, d));
    CHECK_FALSE(parse_nozzle_size_group_id("T2:x", e, d));
    CHECK_FALSE(parse_nozzle_size_group_id("T2:0", e, d));
    CHECK_FALSE(parse_nozzle_size_group_id("Left:0.4", e, d));
    // P8's reader of extruder ids rejects them: a build without P9 ignores them.
    CHECK(nozzle_tool_id_extruder("T2:0.6") == -1);
    CHECK(nozzle_tool_id_extruder("T2") == 1);
}

TEST_CASE("A rack size's changes round-trip in nozzle_process_edits next to the extruders'", "[NozzleRackSlots][Ids][Edits]")
{
    NozzleProcessEdits edits;
    edits["T2:0.6"].set_key_value("wall_loops", new ConfigOptionInt(5));
    edits["T1"].set_key_value("outer_wall_speed", new ConfigOptionFloats({ 77. }));
    const std::vector<std::string> stored = serialize_nozzle_process_edits(edits);
    REQUIRE(stored.size() == 2);
    const NozzleProcessEdits back = parse_nozzle_process_edits(stored);
    REQUIRE(back.count("T2:0.6") == 1);
    REQUIRE(back.count("T1") == 1);
    CHECK(back.at("T2:0.6").opt_int("wall_loops") == 5);
}

TEST_CASE("Rack size process choices are stored by tool id; bad entries are dropped", "[NozzleRackSlots][Ids]")
{
    std::map<std::string, std::string> names{ { "T2:0.6", "0.30mm Standard @BBL H2C 0.6 nozzle" }, { "T2:0.2", "0.10mm Standard @BBL H2C 0.2 nozzle" } };
    DynamicPrintConfig cfg;
    std::vector<std::string> stored = serialize_nozzle_slot_process_names(names);
    stored.push_back("T2\nnot a size tool id");
    stored.push_back("T2:0.8\n");
    stored.push_back("garbage");
    cfg.set_key_value("nozzle_slot_process_presets", new ConfigOptionStrings(stored));
    const std::map<std::string, std::string> back = nozzle_slot_process_names(cfg);
    CHECK(back == names);
}

// ---- 2. The selector's entries ----------------------------------------------------------------------------------

TEST_CASE("The selector groups an extruder's rack by nozzle size", "[NozzleRackSlots][Entries]")
{
    SECTION("no rack text: the machine's default rack (six of the right nozzle's size), so one entry per extruder, as in P8")
    {
        const std::vector<NozzleToolEntry> entries = nozzle_tool_entries(h2c(""));
        REQUIRE(entries.size() == 2);
        CHECK(entries[1].slot == 0);
        CHECK(nozzle_slot_tools(h2c("")).empty());
        CHECK(rack_slot_count(h2c(""), 1) == 6);
        CHECK(rack_slot_count(h2c(""), 0) == 0);
        DynamicPrintConfig no_rack = h2c("");
        no_rack.set_key_value("extruder_max_nozzle_count", new ConfigOptionIntsNullable({ 1, 1 }));
        CHECK(rack_slot_count(no_rack, 1) == 0);
    }
    SECTION("a rack of one size is the extruder itself: Right 0.4, nothing more")
    {
        CHECK(nozzle_slot_tools(h2c("0.4")).empty());
        CHECK(nozzle_slot_tools(h2c("0.4, 0.4, 0.4, 0.4, 0.4, 0.4")).empty());
        CHECK(nozzle_tool_entries(h2c("0.4, 0.4, 0.4")).size() == 2);
    }
    SECTION("Left, Right (its mounted size), then each other size of the rack once, in rack order")
    {
        const DynamicPrintConfig           cfg     = h2c("0.4, 0.4, 0.2, 0.6:high_flow, 0.2");
        const std::vector<NozzleToolEntry> entries = nozzle_tool_entries(cfg);
        REQUIRE(entries.size() == 4);
        CHECK(entries[0].extruder == 0);
        CHECK(entries[0].slot == 0);
        CHECK(entries[1].extruder == 1);
        CHECK(entries[1].slot == 0);
        CHECK(entries[1].diameter == Approx(0.4));
        CHECK(entries[2].slot == 1);
        CHECK(entries[2].diameter == Approx(0.2));
        CHECK(nozzle_tool_entry_id(entries[3]) == "T2:0.6");
        CHECK(has(nozzle_tool_entry_name(cfg, entries[3]), "0.6"));
        CHECK(nozzle_slot_tools(cfg).size() == 2);
        CHECK(rack_summary(extruder_rack_nozzles(cfg)[1]) == "2 x 0.4, 2 x 0.2, 1 x 0.6");
    }
    SECTION("the project's rack sizes win over the printer preset's")
    {
        DynamicPrintConfig cfg = h2c("0.4, 0.4");
        cfg.option<ConfigOptionStrings>("extruder_rack_nozzle_override", true)->values = { "", "0.4, 0.6, 0.8" };
        const std::vector<NozzleToolEntry> slots = nozzle_slot_tools(cfg);
        REQUIRE(slots.size() == 2);
        CHECK(slots[1].diameter == Approx(0.8));
        // An empty project text keeps the preset's rack.
        cfg.option<ConfigOptionStrings>("extruder_rack_nozzle_override")->values = { "", "" };
        CHECK(nozzle_slot_tools(cfg).empty());
    }
}

TEST_CASE("A U1 has no rack: Plate + T1..T4, even with a rack text left over from an H2C project", "[NozzleRackSlots][Entries][U1]")
{
    // Owner hand test 2026-11-02: the U1 selector read "T1 0.2 | T2 slot 1 0.4 | ...".
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_num_filaments(4);
    cfg.option<ConfigOptionFloats>("nozzle_diameter", true)->values             = { 0.2, 0.4, 0.6, 0.8 };
    cfg.option<ConfigOptionBool>("single_extruder_multi_material", true)->value = false;
    cfg.option<ConfigOptionStrings>("extruder_rack_nozzle_override", true)->values = { "", "0.4,0.4,0.4,0.4,0.4" };
    const std::vector<NozzleToolEntry> entries = nozzle_tool_entries(cfg);
    REQUIRE(entries.size() == 4);
    for (size_t e = 0; e < 4; ++e) {
        CHECK(entries[e].extruder == e);
        CHECK(entries[e].slot == 0);
        CHECK(nozzle_tool_entry_id(entries[e]) == "T" + std::to_string(e + 1));
    }
    CHECK(nozzle_slot_tools(cfg).empty());
    CHECK(rack_slot_count(cfg, 1) == 0);
    CHECK(nozzle_filament_group_id(cfg, 2).empty());
}

// ---- 3. Filament -> slot -----------------------------------------------------------------------------------------

TEST_CASE("A filament pinned to a rack slot prints with that slot's size and process", "[NozzleRackSlots][Pins]")
{
    DynamicPrintConfig cfg = h2c("0.4, 0.4, 0.2, 0.6");
    // Unpinned: the first slot of the size it prints with.
    CHECK(filament_rack_slot(cfg, 1) == 0); // the left extruder shows no slots
    CHECK(filament_rack_slot(cfg, 2) == 1);
    CHECK(filament_rack_slot(cfg, 3) == 1);
    // A size pin (P4) picks the first slot of that size.
    cfg.option<ConfigOptionFloats>("filament_rack_nozzle_diameter", true)->values = { 0., 0., 0.6 };
    CHECK(filament_rack_slot(cfg, 3) == 4);
    // A slot pin: that slot, and its size.
    cfg.option<ConfigOptionFloats>("filament_rack_nozzle_diameter")->values = { 0. };
    cfg.option<ConfigOptionInts>("filament_rack_nozzle_slot", true)->values = { 0, 2, 3 };
    CHECK(filament_rack_slot_pin(cfg, 2) == 2);
    CHECK(filament_rack_slot(cfg, 2) == 2);
    CHECK(filament_rack_slot(cfg, 3) == 3);
    CHECK(nozzle_diameter_for_filament(static_cast<const ConfigBase &>(cfg), 3) == Approx(0.2));
    PrintConfig print_config;
    print_config.apply(cfg, true);
    CHECK(nozzle_diameter_for_filament(print_config, 3) == Approx(0.2));
    CHECK(validate_rack_nozzle_diameters(print_config, { 0, 1, 2 }).empty());
    // A slot past the end of the rack is refused, naming the slot.
    cfg.option<ConfigOptionInts>("filament_rack_nozzle_slot")->values = { 0, 0, 7 };
    print_config.apply(cfg, true);
    const std::string error = validate_rack_nozzle_diameters(print_config, { 0, 1, 2 });
    CHECK(has(error, "Filament 3"));
    CHECK(has(error, "slot 7"));
    // Without a rack, slot pins do nothing.
    DynamicPrintConfig plain = h2c("");
    plain.set_key_value("extruder_max_nozzle_count", new ConfigOptionIntsNullable({ 1, 1 }));
    plain.option<ConfigOptionInts>("filament_rack_nozzle_slot", true)->values = { 0, 2, 3 };
    CHECK(filament_rack_slot(plain, 2) == 0);
    CHECK(nozzle_diameter_for_filament(static_cast<const ConfigBase &>(plain), 3) == Approx(0.4));
}

TEST_CASE("Slot pins are stored as { 0 } when nothing is pinned", "[NozzleRackSlots][Pins]")
{
    CHECK(set_filament_rack_slot_pin({ 0 }, 1, 3, 2) == std::vector<int>{ 0, 2, 0 });
    CHECK(set_filament_rack_slot_pin({ 0, 2, 0 }, 1, 3, 0) == std::vector<int>{ 0 });
    CHECK(set_filament_rack_slot_pin({}, 0, 2, -4) == std::vector<int>{ 0 });
}

// ---- 4. Per-filament processes in the slicing config ---------------------------------------------------------------

TEST_CASE("A rack slot's own process reaches only the filaments on that slot", "[NozzleRackSlots][Fill]")
{
    DynamicPrintConfig cfg = h2c("0.4, 0.4, 0.6");
    cfg.option<ConfigOptionFloats>("outer_wall_speed", true)->values = { 200. };
    cfg.set_key_value("wall_loops", new ConfigOptionInt(2));
    DynamicPrintConfig slot_process = cfg;
    slot_process.option<ConfigOptionFloats>("outer_wall_speed")->values = { 77. };
    slot_process.set_key_value("wall_loops", new ConfigOptionInt(5));
    slot_process.set_key_value("outer_wall_line_width", new ConfigOptionFloatOrPercent(0.66, false));

    // Filament 3 on the 0.6 (slot 3), filament 2 on the mounted 0.4 (the plate's).
    const std::vector<const DynamicPrintConfig *> sources{ nullptr, nullptr };
    const std::vector<const DynamicPrintConfig *> filament_sources{ nullptr, nullptr, &slot_process };
    REQUIRE(fill_nozzle_process_slots(cfg, sources, &filament_sources));

    CHECK(outer_wall_speed_of(cfg, 0) == Approx(200.));
    CHECK(outer_wall_speed_of(cfg, 1) == Approx(200.));
    CHECK(outer_wall_speed_of(cfg, 2) == Approx(77.));
    const auto *own = cfg.option<ConfigOptionInts>("nozzle_filament_process_slot");
    REQUIRE(own != nullptr);
    REQUIRE(own->values.size() == 3);
    CHECK(own->values[0] == -1);
    CHECK(own->values[1] == -1);
    CHECK(own->values[2] >= 0);
    // The layout keeps running 1, .., 2, ..: the added slots follow extruder 2's.
    const std::vector<int> ids = cfg.option<ConfigOptionInts>("print_extruder_id")->values;
    CHECK(std::is_sorted(ids.begin(), ids.end()));
    CHECK(ids[size_t(own->values[2])] == 2);
    // Region settings and widths per filament.
    const std::vector<std::string> &regions = cfg.option<ConfigOptionStrings>("nozzle_region_values")->values;
    REQUIRE(regions.size() == 3);
    CHECK(has(regions[2], "wall_loops = 5"));
    CHECK(has(regions[1], "wall_loops = 2"));
    const auto *widths = cfg.option<ConfigOptionFloatsOrPercents>("nozzle_outer_wall_line_width");
    REQUIRE(widths != nullptr);
    REQUIRE(widths->values.size() == 3);
    CHECK(widths->values[2].value == Approx(0.66));
}

TEST_CASE("Per-filament sources equal to their extruders' fill exactly what P8 fills", "[NozzleRackSlots][Fill][Identity]")
{
    DynamicPrintConfig base = h2c("");
    base.option<ConfigOptionFloats>("outer_wall_speed", true)->values = { 200. };
    DynamicPrintConfig right = base;
    right.option<ConfigOptionFloats>("outer_wall_speed")->values = { 90. };
    const std::vector<const DynamicPrintConfig *> sources{ nullptr, &right };

    DynamicPrintConfig p8 = base;
    REQUIRE(fill_nozzle_process_slots(p8, sources));
    DynamicPrintConfig p9 = base;
    const std::vector<const DynamicPrintConfig *> filament_sources{ nullptr, &right, &right };
    REQUIRE(fill_nozzle_process_slots(p9, sources, &filament_sources));
    CHECK(p8.diff(p9).empty());
    CHECK(p9.option<ConfigOptionInts>("nozzle_filament_process_slot")->values.empty());
    CHECK(outer_wall_speed_of(p9, 2) == Approx(90.));
}

// ---- 5. The rack nozzle change between slots ----------------------------------------------------------------------

TEST_CASE("Filaments pinned to different rack slots print on different nozzles, with a rack nozzle change between them", "[NozzleRackSlots][GCode]")
{
    // A rack of three 0.4 nozzles: one size, so only the slot pins tell the filaments apart.
    DynamicPrintConfig cfg = h2c("0.4, 0.4, 0.4");
    cfg.option<ConfigOptionInts>("filament_rack_nozzle_slot", true)->values = { 0, 1, 3 };
    cfg.option<ConfigOptionInts>("physical_extruder_map", true)->values     = { 1, 0 };
    PrintConfig print_config;
    print_config.apply(cfg, true);

    // build_nozzle_list's order: the left nozzle, then the right's three 0.4 nozzles.
    const std::vector<MultiNozzleUtils::NozzleInfo> list{ nozzle(0, 0, "0.4"), nozzle(1, 1, "0.4"), nozzle(2, 1, "0.4"), nozzle(3, 1, "0.4") };
    CHECK(rack_slot_nozzle_index(print_config, list, 1, 1) == 1);
    CHECK(rack_slot_nozzle_index(print_config, list, 1, 3) == 3);
    CHECK(rack_slot_nozzle_index(print_config, list, 1, 4) == -1);
    CHECK(rack_slot_nozzle_index(print_config, list, 0, 1) == -1);

    // The engine put filaments 2 and 3 on the same right nozzle (it saves a nozzle change); the pins move them apart.
    const std::vector<unsigned int> used{ 0, 1, 2 };
    std::vector<int>                map{ 0, 1, 1 };
    CHECK(pin_rack_nozzle_diameters(print_config, list, used, map) == 1);
    CHECK(map == std::vector<int>{ 0, 1, 3 });
    const auto result = MultiNozzleUtils::LayeredNozzleGroupResult::create(map, list, used);
    REQUIRE(result);
    const auto n2 = result->get_nozzle_for_filament(1);
    const auto n3 = result->get_nozzle_for_filament(2);
    REQUIRE(n2);
    REQUIRE(n3);
    CHECK(n2->extruder_id == n3->extruder_id);
    CHECK(n2->group_id != n3->group_id);
    // The block GCode::nozzle_change_gcode() writes for that change: same extruder, so the M632 / M633 rack interlock, and the
    // pre-cool names the right extruder's PHYSICAL id (physical_extruder_map [1, 0]).
    const std::string block = format_nozzle_change_block(1, 2, n2->group_id, n3->group_id, n2->extruder_id != n3->extruder_id,
                                                         result->is_support_dynamic_nozzle_map(), 180,
                                                         print_config.physical_extruder_map.get_at(n2->extruder_id));
    CHECK(has(block, "M632 S2 M N"));
    CHECK(has(block, "M104 T0 S180 N0"));
    CHECK(has(block, "M633"));

    // Pinned to one slot: one nozzle, no nozzle change between them.
    cfg.option<ConfigOptionInts>("filament_rack_nozzle_slot")->values = { 0, 2, 2 };
    print_config.apply(cfg, true);
    std::vector<int> same{ 0, 1, 3 };
    pin_rack_nozzle_diameters(print_config, list, used, same);
    CHECK(same == std::vector<int>{ 0, 2, 2 });

    // No pins on a rack of one size: the engine's map is left alone (P4 / P8 behaviour).
    cfg.option<ConfigOptionInts>("filament_rack_nozzle_slot")->values = { 0 };
    print_config.apply(cfg, true);
    std::vector<int> untouched{ 0, 1, 1 };
    CHECK(pin_rack_nozzle_diameters(print_config, list, used, untouched) == 0);
    CHECK(untouched == std::vector<int>{ 0, 1, 1 });
}

// ---- 6. Send-time check and "Sync from printer" -------------------------------------------------------------------

TEST_CASE("Sending refuses rack slots the printer's rack cannot serve, naming the slots", "[NozzleRackSlots][Send]")
{
    DynamicPrintConfig cfg = h2c("0.4, 0.4, 0.6");
    cfg.option<ConfigOptionInts>("filament_rack_nozzle_slot", true)->values = { 0, 1, 3 };
    const std::vector<RackSlotUse> uses = rack_slots_in_use(cfg, { 0, 1, 2 });
    REQUIRE(uses.size() == 2);
    CHECK(uses[0].slot == 1);
    CHECK(uses[1].slot == 3);
    CHECK(uses[1].diameter == Approx(0.6));

    // The printer's right extruder has 0.4 nozzles only.
    std::vector<std::string> problems = rack_slot_send_problems(cfg, uses, { { 0.4 }, { 0.4, 0.4, 0.2 } });
    REQUIRE(problems.size() == 1);
    CHECK(has(problems[0], "slot 3"));
    CHECK(has(problems[0], "0.6 mm"));
    CHECK(has(problems[0], "0.4, 0.4, 0.2"));
    // It has the 0.6: fine.
    CHECK(rack_slot_send_problems(cfg, uses, { { 0.4 }, { 0.4, 0.6 } }).empty());
    // Nothing reported for the extruder: never a problem.
    CHECK(rack_slot_send_problems(cfg, uses, { { 0.4 }, {} }).empty());

    // Two slots of one size pinned need two nozzles of it.
    cfg.option<ConfigOptionInts>("filament_rack_nozzle_slot")->values = { 0, 1, 2 };
    problems = rack_slot_send_problems(cfg, rack_slots_in_use(cfg, { 0, 1, 2 }), { { 0.4 }, { 0.4, 0.6 } });
    REQUIRE(problems.size() == 1);
    CHECK(has(problems[0], "slot 1"));
    CHECK(has(problems[0], "slot 2"));
    CHECK(has(problems[0], "2 separate 0.4 mm"));
    // Unpinned filaments of one size share a nozzle.
    cfg.option<ConfigOptionInts>("filament_rack_nozzle_slot")->values = { 0 };
    CHECK(rack_slot_send_problems(cfg, rack_slots_in_use(cfg, { 0, 1, 2 }), { { 0.4 }, { 0.4, 0.6 } }).empty());
}

TEST_CASE("Sync from printer: the mounted nozzle then the rack positions become the project's rack slots", "[NozzleRackSlots][Sync]")
{
    DualNozzleSync::PrinterState state;
    state.physical_extruder_map = { 1, 0 }; // logical right (1) is physical 0
    auto add = [&state](int pos, const char *d, bool normal) {
        DualNozzleSync::PrinterNozzle n;
        n.pos      = pos;
        n.diameter = d;
        n.normal   = normal;
        state.nozzles.push_back(n);
    };
    add(0, "0.4", true);         // mounted on the right
    add(1, "0.4", true);         // mounted on the left
    add(0x12, "0.2", true);      // rack position 2
    add(0x10, "0.6", true);      // rack position 0
    add(0x11, "0.8", false);     // rack position 1: not usable
    const std::vector<std::vector<double>> layout = reported_rack_layout(state, 2);
    REQUIRE(layout.size() == 2);
    CHECK(layout[0].empty());
    CHECK(layout[1] == std::vector<double>{ 0.4, 0.6, 0.2 });
    const std::vector<std::string> texts = rack_override_from_reported(layout, { "", "" });
    CHECK(texts == std::vector<std::string>{ "", "0.4,0.6,0.2" });
    // A printer without rack nozzles changes nothing.
    CHECK(rack_override_from_reported({ {}, {} }, { "", "" }).empty());
}

// ---- 7. Messages that name the slot -------------------------------------------------------------------------------

TEST_CASE("Validation messages name the rack slot", "[NozzleRackSlots][Validate]")
{
    DynamicPrintConfig cfg = h2c("0.4, 0.4, 0.2");
    cfg.option<ConfigOptionInts>("filament_rack_nozzle_slot", true)->values = { 0, 0, 3 };
    CHECK(nozzle_filament_tool_id(cfg, 3) == "T2:0.2");
    CHECK(nozzle_filament_group_id(cfg, 3) == "T2:0.2");
    CHECK(nozzle_filament_tool_id(cfg, 2) == "T2"); // on the mounted size: the extruder's own process
    CHECK(nozzle_filament_tool_id(cfg, 1) == "T1");
    CHECK(has(nozzle_filament_tool_name(cfg, 3), "0.2"));
    const std::string layer = nozzle_layer_height_error(cfg, 3, false, 0.28, 0.2);
    CHECK(has(layer, "T2 0.2"));
    CHECK(has(layer, "0.2 mm"));
    const std::string missing = nozzle_size_group_missing_error(cfg, 1, 0.2);
    CHECK(has(missing, "T2 0.2"));
    CHECK(has(missing, "0.2 mm"));
}

// ---- 8. The preset bundle (shipped BBL H2C profiles) --------------------------------------------------------------

namespace {

PresetBundle &bbl_bundle()
{
    static std::unique_ptr<PresetBundle> bundle;
    static std::unique_ptr<PresetBundle> library;
    if (bundle)
        return *bundle;
    const std::string             saved_data_dir = data_dir();
    const boost::filesystem::path scratch = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("nozzle_rack_slots_%%%%-%%%%");
    boost::filesystem::create_directories(scratch);
    set_data_dir(scratch.string());
    const std::string profiles = (boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
    library = std::make_unique<PresetBundle>();
    library->load_vendor_configs_from_json(profiles, PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem,
                                           ForwardCompatibilitySubstitutionRule::EnableSilent);
    bundle = std::make_unique<PresetBundle>();
    bundle->load_vendor_configs_from_json(profiles, "BBL", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent,
                                          library.get());
    set_data_dir(saved_data_dir);
    return *bundle;
}

PresetBundle &h2c_bundle(const std::vector<std::string> &rack_override)
{
    PresetBundle &b = bbl_bundle();
    b.end_nozzle_tool_view();
    REQUIRE(b.printers.select_preset_by_name("Bambu Lab H2C 0.4 nozzle", true));
    REQUIRE(b.prints.select_preset_by_name("0.20mm Standard @BBL H2C", true));
    b.update_compatible(PresetSelectCompatibleType::Never);
    if (b.filament_presets.size() != 3)
        b.set_num_filaments(3);
    b.project_config.option<ConfigOptionInts>("filament_map", true)->values = { 1, 2, 2 };
    b.project_config.option<ConfigOptionStrings>("nozzle_process_presets", true)->values.clear();
    b.project_config.option<ConfigOptionStrings>("nozzle_process_edits", true)->values.clear();
    b.project_config.option<ConfigOptionStrings>("nozzle_slot_process_presets", true)->values.clear();
    b.project_config.option<ConfigOptionInts>("filament_rack_nozzle_slot", true)->values = { 0 };
    b.set_nozzle_diameter_override({ 0. });
    b.set_rack_nozzle_override(rack_override);
    return b;
}

} // namespace

TEST_CASE("H2C: rack sizes are tools of the preset bundle with their own picked process", "[NozzleRackSlots][PresetBundle]")
{
    SECTION("without a rack the tools are the two extruders, and nothing P9 reaches the slicing config")
    {
        PresetBundle &b = h2c_bundle({});
        CHECK(b.nozzle_tool_count() == 2);
        CHECK(b.nozzle_selector_tools() == std::vector<size_t>{ 0, 1 });
        const DynamicPrintConfig full = b.full_config();
        CHECK(full.option<ConfigOptionInts>("nozzle_filament_process_slot")->values.empty());
        CHECK(full.option<ConfigOptionStrings>("nozzle_slot_process_missing")->values.empty());
    }
    SECTION("Left, Right 0.4, Right 0.6: the 0.6 gets the H2C 0.6 process, and the filament pinned to its slot prints with it")
    {
        PresetBundle &b = h2c_bundle({ "", "0.4,0.4,0.6" });
        CHECK(b.nozzle_tool_count() == 3);
        CHECK(b.nozzle_selector_tools() == std::vector<size_t>{ 0, 1, 2 });
        const PresetBundle::NozzleToolRef slot3 = b.nozzle_tool_ref(2);
        REQUIRE(slot3.valid);
        CHECK(slot3.id == "T2:0.6");
        CHECK(slot3.diameter == Approx(0.6));
        CHECK(b.nozzle_tool_index("T2:0.6") == 2);
        CHECK(b.nozzle_tool_index("T2:0.8") == -1);
        CHECK(has(b.nozzle_tool_name(2), "0.6"));
        // Right (0.4) follows the plate; Right 0.6 gets the 0.6 system process of the plate's kind.
        CHECK(b.nozzle_tool_process_info(1).kind == PresetBundle::NozzleToolProcessKind::FollowsPlate);
        const PresetBundle::NozzleToolProcess info = b.nozzle_tool_process_info(2);
        CHECK(info.kind == PresetBundle::NozzleToolProcessKind::AutoPicked);
        CHECK(has(info.name, "0.6 nozzle"));

        REQUIRE(b.set_filament_rack_slot(2, 3));
        const DynamicPrintConfig full = b.full_config();
        CHECK(filament_rack_slot(full, 3) == 3);
        CHECK(nozzle_diameter_for_filament(full, 3) == Approx(0.6));
        const auto *own = full.option<ConfigOptionInts>("nozzle_filament_process_slot");
        REQUIRE(own->values.size() == 3);
        CHECK(own->values[0] == -1);
        CHECK(own->values[1] == -1);
        CHECK(own->values[2] >= 0);
        const Preset *picked = b.prints.find_preset(info.name, false);
        REQUIRE(picked != nullptr);
        // Filament 3 reads one of the picked process's own values (its layout's slot for the right extruder).
        const std::vector<double> &own_speeds = picked->config.option<ConfigOptionFloats>("outer_wall_speed")->values;
        const double               got        = outer_wall_speed_of(full, 2);
        CHECK(std::any_of(own_speeds.begin(), own_speeds.end(), [got](double v) { return std::abs(v - got) < 1e-6; }));
        b.set_filament_rack_slot(2, 0);
    }
    SECTION("a rack size's choice and changes are kept by its id, and go when the size goes")
    {
        PresetBundle &b = h2c_bundle({ "", "0.4,0.4,0.6" });
        REQUIRE(b.set_nozzle_process(2, "0.24mm Balanced Strength @BBL H2C 0.6 nozzle"));
        CHECK(b.nozzle_tool_selection(2) == "0.24mm Balanced Strength @BBL H2C 0.6 nozzle");
        CHECK(nozzle_slot_process_names(b.project_config).count("T2:0.6") == 1);
        // A 0.4 process does not fit the 0.6 slot.
        const Preset *p04 = b.prints.find_preset("0.20mm Standard @BBL H2C", false);
        REQUIRE(p04 != nullptr);
        CHECK_FALSE(b.nozzle_process_fits(*p04, 2));
        // The tool view: the size tool's profile is shown, an edit is stored under T2:0.6.
        REQUIRE(b.begin_nozzle_tool_view(2));
        CHECK(b.prints.get_selected_preset_name() == "0.24mm Balanced Strength @BBL H2C 0.6 nozzle");
        b.prints.get_edited_preset().config.set_key_value("wall_loops", new ConfigOptionInt(7));
        b.nozzle_tool_view_sync();
        b.end_nozzle_tool_view();
        CHECK(b.nozzle_process_edits_map().count("T2:0.6") == 1);
        CHECK(b.prints.get_selected_preset_name() == "0.20mm Standard @BBL H2C");
        // "Use one process": every choice and change goes.
        {
            const std::vector<std::string> keep  = b.project_config.option<ConfigOptionStrings>("nozzle_slot_process_presets")->values;
            const std::vector<std::string> edits = b.project_config.option<ConfigOptionStrings>("nozzle_process_edits")->values;
            CHECK(b.clear_nozzle_processes());
            CHECK(nozzle_slot_process_names(b.project_config).empty());
            CHECK(b.nozzle_process_edits_map().empty());
            CHECK_FALSE(b.clear_nozzle_processes());
            b.project_config.option<ConfigOptionStrings>("nozzle_slot_process_presets")->values = keep;
            b.project_config.option<ConfigOptionStrings>("nozzle_process_edits")->values        = edits;
        }
        // The rack loses its 0.6: that size's choice and changes are dropped.
        b.set_rack_nozzle_override({ "", "0.4,0.4" });
        CHECK(nozzle_slot_process_names(b.project_config).empty());
        CHECK(b.nozzle_process_edits_map().count("T2:0.6") == 0);
        b.set_rack_nozzle_override({});
    }
    SECTION("what the Extruder pages and the sidebar read for the stock H2C (owner crash report on Extruder 2)")
    {
        PresetBundle &b = h2c_bundle({});
        // The stock preset declares no rack; its rack count is a nullable vector.
        const DynamicPrintConfig sized  = b.printer_config_with_project_sizes();
        const auto              *counts = dynamic_cast<const ConfigOptionIntsNullable *>(sized.option("extruder_max_nozzle_count"));
        REQUIRE(counts != nullptr);
        REQUIRE(counts->values.size() == 2);
        CHECK(counts->values[1] > 1);
        const auto racks = extruder_rack_nozzles(sized);
        REQUIRE(racks.size() == 2);
        // Owner hand test 2026-11-03: with no rack text the right extruder shows its full rack, six of its nozzle size.
        CHECK(racks[0].empty());
        REQUIRE(racks[1].size() == 6);
        for (const RackNozzle &n : racks[1])
            CHECK(n.diameter == Approx(0.4));
        for (size_t e = 0; e < 2; ++e) {
            CHECK(b.rack_nozzle_override(e).empty());
            CHECK(validate_rack_nozzle_text(b.rack_nozzle_override(e), b.effective_nozzle_diameters()[e]).empty());
        }
        // Out of range and odd texts are taken without harm.
        CHECK_FALSE(b.set_rack_nozzle_override(5, "0.4,0.6"));
        CHECK(b.set_rack_nozzle_override(1, " 0.4, x, 0.6 "));
        CHECK(b.nozzle_tool_count() == 3);
        CHECK(b.nozzle_selector_tools() == std::vector<size_t>{ 0, 1, 2 });
        CHECK(b.set_rack_nozzle_override(1, ""));
        CHECK(b.nozzle_tool_count() == 2);
    }
}

TEST_CASE("Every per-extruder value the Extruder N pages read exists for N = 1, 2 on the stock H2D and H2C", "[NozzleRackSlots][PresetBundle][Pages]")
{
    // Owner crash report 2026-11-02 (H2C Printer settings > Extruder 2). What the Extruder N page and its P4..P9 rows read, for
    // each extruder of the stock two-extruder Bambu presets, without a value missing or a read past a vector's end.
    PresetBundle &b = bbl_bundle();
    for (const char *printer : { "Bambu Lab H2C 0.4 nozzle", "Bambu Lab H2D 0.4 nozzle" }) {
        INFO(printer);
        b.end_nozzle_tool_view();
        REQUIRE(b.printers.select_preset_by_name(printer, true));
        b.update_compatible(PresetSelectCompatibleType::Never);
        const DynamicPrintConfig &cfg   = b.printers.get_edited_preset().config;
        const DynamicPrintConfig  sized = b.printer_config_with_project_sizes();
        const size_t              n     = cfg.option<ConfigOptionFloats>("nozzle_diameter")->values.size();
        REQUIRE(n == 2);
        for (size_t e = 0; e < n; ++e) {
            INFO("extruder " << e + 1);
            for (const std::string &key : print_config_def.extruder_option_keys()) {
                const auto *opt = dynamic_cast<const ConfigOptionVectorBase *>(cfg.option(key));
                if (opt == nullptr)
                    continue;
                INFO(key);
                CHECK(opt->size() > 0);
                // What a page field reads for its index (ConfigOptionVector::get_at falls back to the first value).
                const std::vector<std::string> values = opt->vserialize();
                CHECK_FALSE(values.empty());
            }
            // P4..P9 reads of the Extruder page and the sidebar for this extruder.
            CHECK_NOTHROW(rack_slot_count(sized, e));
            CHECK_NOTHROW(extruder_has_nozzle_rack(sized, e));
            CHECK_NOTHROW(validate_rack_nozzle_text(b.rack_nozzle_override(e), b.effective_nozzle_diameters()[e]));
            CHECK(e < b.effective_nozzle_diameters().size());
            CHECK(e < b.preset_nozzle_diameters().size());
        }
        CHECK(extruder_has_nozzle_rack(sized, 1) == (std::string(printer).find("H2C") != std::string::npos));
        CHECK_FALSE(extruder_has_nozzle_rack(sized, 0));
        CHECK(nozzle_tool_entries(sized).size() == 2);
    }
}

TEST_CASE("H2C/H2D side pins are per filament: pinning one never changes another", "[NozzleRackSlots][Sides]")
{
    // Owner hand test 2026-11-02: setting filament 3 to Left showed L on all four filaments, Auto cleared all four.
    std::vector<int> pins = { 0 };
    pins = set_filament_side_pin(pins, 2, 4, 1); // filament 3 -> Left
    CHECK(pins == std::vector<int>{ 0, 0, 1, 0 });
    pins = set_filament_side_pin(pins, 0, 4, 2); // filament 1 -> Right
    CHECK(pins == std::vector<int>{ 2, 0, 1, 0 });
    pins = set_filament_side_pin(pins, 2, 4, 0); // filament 3 -> Auto: filament 1 keeps Right
    CHECK(pins == std::vector<int>{ 2, 0, 0, 0 });
    pins = set_filament_side_pin(pins, 0, 4, 0);
    CHECK(pins == std::vector<int>{ 0 }); // nothing pinned: the unused value
    CHECK(set_filament_side_pin({ 0 }, 1, 2, 7) == std::vector<int>{ 0 });

    // The plate's manual map: pinned filaments on their side, the others where the grouping put them.
    const std::vector<int> auto_map = { 1, 2, 2, 1 };
    CHECK(manual_filament_map_from_pins({ 0 }, auto_map, 4).empty());
    CHECK(manual_filament_map_from_pins({ 0, 0, 1, 0 }, auto_map, 4) == std::vector<int>{ 1, 2, 1, 1 });
    CHECK(manual_filament_map_from_pins({ 2, 0, 0, 0 }, auto_map, 4) == std::vector<int>{ 2, 2, 2, 1 });
    // No grouping known: unpinned filaments on the left.
    CHECK(manual_filament_map_from_pins({ 0, 2 }, {}, 3) == std::vector<int>{ 1, 2, 1 });
}
