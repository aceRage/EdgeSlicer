// Per-nozzle process P4 (tests/design_per_nozzle_process.md "P4: Generic N-tool"): the per-tool slot model with a
// diameter dimension, rack nozzle diameters (H2C) per filament and per layer, per-nozzle filament values, the
// physical nozzle declaration and the G-code's tool numbers.

#include <catch2/catch.hpp>

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/MultiNozzleUtils.hpp"
#include "libslic3r/NozzleProcess.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ToolSlots.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>

#include <string>
#include <vector>

using namespace Slic3r;

namespace {

const int STD = int(fvtStandard);
const int HF  = int(fvtHighFlow);

// A four-tool toolchanger (a U1 or a K3-like machine) with the given nozzle diameters and one filament per tool.
DynamicPrintConfig toolchanger(const std::vector<double> &diameters)
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_num_filaments(unsigned(diameters.size()));
    cfg.option<ConfigOptionFloats>("nozzle_diameter", true)->values             = diameters;
    cfg.option<ConfigOptionBool>("single_extruder_multi_material", true)->value = false;
    cfg.option<ConfigOptionStrings>("process_flow_support", true)->values       = { FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW };
    cfg.option<ConfigOptionEnumsGeneric>("filament_volume_type", true)->values  = std::vector<int>(diameters.size(), STD);
    cfg.option<ConfigOptionFloats>("outer_wall_speed", true)->values            = { 200, 500 };
    return cfg;
}

// One process for every tool, with values per nozzle size and flow type: 0.4 Standard 101, 0.4 High Flow 102,
// 0.6 Standard 61, 0.6 High Flow 62.
void add_size_slots(DynamicPrintConfig &cfg)
{
    set_process_tool_slots(cfg, { 0, 0, 0, 0 },
                           { "Direct Drive Standard", "Direct Drive High Flow", "Direct Drive Standard", "Direct Drive High Flow" },
                           { 0.4, 0.4, 0.6, 0.6 });
    cfg.option<ConfigOptionFloats>("outer_wall_speed", true)->values = { 101, 102, 61, 62 };
}

double outer_wall_speed_of(const DynamicPrintConfig &cfg, unsigned int filament_idx)
{
    const auto *opt = cfg.option<ConfigOptionFloats>("outer_wall_speed");
    return get_value_at(cfg, *opt, ConfigFlowDomain::Process, filament_idx);
}

} // namespace

// ---- 1. The slot model -----------------------------------------------------------------------------------------

TEST_CASE("A slot fits by tool and diameter, the more specific one wins", "[ToolSlots][Resolver]")
{
    const std::vector<ToolSlot> slots = {
        { 0, STD, 0. },  // 0: any tool, any size, Standard
        { 0, STD, 0.6 }, // 1: any tool, 0.6, Standard
        { 0, HF, 0.6 },  // 2: any tool, 0.6, High Flow
        { 3, STD, 0.6 }, // 3: T3, 0.6, Standard
        { 2, -1, 0.2 },  // 4: T2, 0.2, no flow mode (E3D High Flow)
    };
    CHECK(match_tool_slot(slots, { 0, STD, 0.4 }) == 0);
    CHECK(match_tool_slot(slots, { 0, STD, 0.6 }) == 1);
    CHECK(match_tool_slot(slots, { 0, HF, 0.6 }) == 2);
    CHECK(match_tool_slot(slots, { 2, STD, 0.6 }) == 3); // T3 is tool index 2
    CHECK(match_tool_slot(slots, { 2, HF, 0.6 }) == 3);  // its own tool beats the flow mode
    CHECK(match_tool_slot(slots, { 1, HF, 0.2 }) == 4);
    // A 0.4 High Flow filament: only the any-size Standard slot fits; it is taken rather than nothing.
    CHECK(match_tool_slot(slots, { 0, HF, 0.4 }) == 0);
    CHECK(match_tool_slot({ { 2, STD, 0.4 } }, { 0, STD, 0.4 }) == -1);
    CHECK(same_nozzle_diameter(0.4, 0.4000001));
    CHECK_FALSE(same_nozzle_diameter(0.4, 0.6));
}

TEST_CASE("A process with diameter slots gives each filament the values of its nozzle size", "[ToolSlots][Resolver]")
{
    DynamicPrintConfig cfg = toolchanger({ 0.4, 0.4, 0.6, 0.6 });
    // Without the diameter dimension the old flow-mode rule reads column 0 for everybody.
    for (unsigned int f = 0; f < 4; ++f)
        CHECK(outer_wall_speed_of(cfg, f) == 200);
    add_size_slots(cfg);
    REQUIRE(process_has_diameter_slots(cfg));
    REQUIRE(tool_slot_model_active(cfg));
    CHECK(outer_wall_speed_of(cfg, 0) == 101);
    CHECK(outer_wall_speed_of(cfg, 1) == 101);
    CHECK(outer_wall_speed_of(cfg, 2) == 61);
    CHECK(outer_wall_speed_of(cfg, 3) == 61);
    // A High Flow filament reads its size's High Flow slot.
    cfg.option<ConfigOptionEnumsGeneric>("filament_volume_type")->values = { STD, HF, STD, HF };
    CHECK(outer_wall_speed_of(cfg, 1) == 102);
    CHECK(outer_wall_speed_of(cfg, 3) == 62);
    // A filament put on another tool (filament_nozzle_assignment) reads that tool's size.
    cfg.option<ConfigOptionInts>("filament_nozzle_assignment", true)->values = { 4 };
    CHECK(outer_wall_speed_of(cfg, 0) == 61);
    // The per-export cache agrees with the direct read.
    const ResolvedFilamentFlow cache = ResolvedFilamentFlow::resolve(cfg);
    const auto                *opt   = cfg.option<ConfigOptionFloats>("outer_wall_speed");
    for (unsigned int f = 0; f < 4; ++f) {
        INFO("filament " << f);
        CHECK(cache.process_config_idx_for(cfg, opt->values.size(), f) == process_config_idx(cfg, opt->values.size(), f));
    }
    CHECK(cache.process_config_idx_for(cfg, opt->values.size(), 0) == 2);
}

TEST_CASE("A slot layout is read per tool only on a printer that opts in", "[ToolSlots][Resolver]")
{
    DynamicPrintConfig cfg = toolchanger({ 0.4, 0.4 });
    cfg.set_num_filaments(2);
    cfg.option<ConfigOptionFloats>("nozzle_diameter")->values = { 0.4, 0.4 };
    cfg.option<ConfigOptionInts>("print_extruder_id", true)->values = { 1, 1, 2, 2 };
    cfg.option<ConfigOptionStrings>("print_extruder_variant", true)->values = { "Direct Drive Standard", "Direct Drive High Flow",
                                                                               "Direct Drive Standard", "Direct Drive High Flow" };
    cfg.option<ConfigOptionFloats>("outer_wall_speed")->values = { 100, 150, 80, 120 };
    // A stock-like layout without process_extruder_values: every filament reads the flow-mode column (D14).
    CHECK_FALSE(tool_slot_model_active(cfg));
    CHECK(outer_wall_speed_of(cfg, 1) == 100);
    // The printer opts into the slot model: T2's filament reads T2's slots.
    cfg.option<ConfigOptionBool>("tool_slot_model", true)->value = true;
    CHECK(tool_slot_model_active(cfg));
    CHECK(outer_wall_speed_of(cfg, 0) == 100);
    CHECK(outer_wall_speed_of(cfg, 1) == 80);
    cfg.option<ConfigOptionEnumsGeneric>("filament_volume_type")->values = { HF, HF };
    CHECK(outer_wall_speed_of(cfg, 0) == 150);
    CHECK(outer_wall_speed_of(cfg, 1) == 120);
}

TEST_CASE("A diameter layout of the wrong length or all zero leaves the old rule", "[ToolSlots][Resolver]")
{
    DynamicPrintConfig cfg = toolchanger({ 0.4, 0.4, 0.6, 0.6 });
    add_size_slots(cfg);
    cfg.option<ConfigOptionFloats>("print_extruder_nozzle_diameter")->values = { 0., 0., 0., 0. };
    CHECK_FALSE(process_has_diameter_slots(cfg));
    CHECK(outer_wall_speed_of(cfg, 2) == 101); // column 0 (flow mode Standard) of the 4-value vector
    cfg.option<ConfigOptionFloats>("print_extruder_nozzle_diameter")->values = { 0.4, 0.6 };
    CHECK_FALSE(process_has_diameter_slots(cfg));
}

// ---- 3. Rack nozzle diameters (H2C) ----------------------------------------------------------------------------

namespace {

// An H2C-like machine: left extruder one 0.4 nozzle, right extruder a rack of 0.4, 0.4, 0.2 and 0.6 nozzles;
// three filaments, filament 1 left, filaments 2 and 3 right.
DynamicPrintConfig h2c_rack()
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_num_filaments(3);
    cfg.option<ConfigOptionFloats>("nozzle_diameter", true)->values                 = { 0.4, 0.4 };
    cfg.option<ConfigOptionBool>("single_extruder_multi_material", true)->value     = false;
    cfg.option<ConfigOptionInts>("filament_map", true)->values                      = { 1, 2, 2 };
    cfg.option<ConfigOptionStrings>("extruder_rack_nozzle_diameter", true)->values  = { "", "0.4, 0.4, 0.2, 0.6:high_flow" };
    cfg.option<ConfigOptionFloats>("filament_rack_nozzle_diameter", true)->values   = { 0., 0.2, 0. };
    return cfg;
}

std::vector<MultiNozzleUtils::NozzleInfo> rack_nozzle_list()
{
    // Left 0.4 (id 0); right 0.2 (id 1), 0.4 x2 (ids 2, 3), 0.6 High Flow (id 4): build_nozzle_list's order.
    std::vector<MultiNozzleUtils::NozzleInfo> list;
    auto add = [&list](const char *d, NozzleVolumeType type, int extruder) {
        MultiNozzleUtils::NozzleInfo n;
        n.diameter    = d;
        n.volume_type = type;
        n.extruder_id = extruder;
        n.group_id    = int(list.size());
        list.push_back(n);
    };
    add("0.4", nvtStandard, 0);
    add("0.2", nvtStandard, 1);
    add("0.4", nvtStandard, 1);
    add("0.4", nvtStandard, 1);
    add("0.6", nvtHighFlow, 1);
    return list;
}

} // namespace

TEST_CASE("Rack nozzle diameters parse with an optional flow type", "[ToolSlots][Rack]")
{
    const std::vector<RackNozzle> rack = parse_rack_nozzles(" 0.4,0.2:high_flow, bad, 0.6:tpu_high_flow,");
    REQUIRE(rack.size() == 3);
    CHECK(rack[0].diameter == 0.4);
    CHECK_FALSE(rack[0].volume_given);
    CHECK(rack[1].diameter == 0.2);
    CHECK(rack[1].volume_type == nvtHighFlow);
    CHECK(rack[2].volume_type == nvtTPUHighFlow);
    CHECK(parse_rack_nozzles("").empty());
}

TEST_CASE("A filament pinned to a rack nozzle size slices for that size", "[ToolSlots][Rack]")
{
    DynamicPrintConfig cfg = h2c_rack();
    REQUIRE(has_mixed_rack_diameters(cfg));
    CHECK(nozzle_diameter_for_filament(static_cast<const ConfigBase &>(cfg), 1) == 0.4);
    CHECK(nozzle_diameter_for_filament(static_cast<const ConfigBase &>(cfg), 2) == 0.2);
    CHECK(nozzle_diameter_for_filament(static_cast<const ConfigBase &>(cfg), 3) == 0.4);
    PrintConfig print_config;
    print_config.apply(cfg, true);
    CHECK(nozzle_diameter_for_filament(print_config, 2) == 0.2);
    CHECK(nozzle_diameter_for_filament(print_config, 3) == 0.4);
    // A pin no rack nozzle has: the extruder's own size, and validation names it.
    cfg.option<ConfigOptionFloats>("filament_rack_nozzle_diameter")->values = { 0., 0.8, 0. };
    print_config.apply(cfg, true);
    CHECK(nozzle_diameter_for_filament(print_config, 2) == 0.4);
    CHECK_FALSE(validate_rack_nozzle_diameters(print_config, { 0, 1, 2 }).empty());
    // Without rack diameters nothing changes, pins or not.
    cfg.option<ConfigOptionStrings>("extruder_rack_nozzle_diameter")->values = {};
    print_config.apply(cfg, true);
    CHECK(nozzle_diameter_for_filament(print_config, 2) == 0.4);
    CHECK(validate_rack_nozzle_diameters(print_config, { 0, 1, 2 }).empty());
    CHECK_FALSE(has_mixed_rack_diameters(cfg));
}

TEST_CASE("The extruder's own nozzle size must be in its rack", "[ToolSlots][Rack]")
{
    DynamicPrintConfig cfg = h2c_rack();
    cfg.option<ConfigOptionStrings>("extruder_rack_nozzle_diameter")->values = { "", "0.2,0.6" };
    PrintConfig print_config;
    print_config.apply(cfg, true);
    CHECK_FALSE(validate_rack_nozzle_diameters(print_config, { 0 }).empty());
}

TEST_CASE("Rack nozzle groups carry each kind of nozzle", "[ToolSlots][Rack]")
{
    const DynamicPrintConfig                                              cfg = h2c_rack();
    std::vector<std::pair<std::string, std::pair<NozzleVolumeType, int>>> groups;
    CHECK_FALSE(rack_nozzle_groups(cfg, 0, groups));
    REQUIRE(rack_nozzle_groups(cfg, 1, groups));
    REQUIRE(groups.size() == 3);
    CHECK(groups[0].first == "0.4");
    CHECK(groups[0].second.second == 2);
    CHECK(groups[1].first == "0.2");
    CHECK(groups[2].first == "0.6");
    CHECK(groups[2].second.first == nvtHighFlow);
}

TEST_CASE("The nozzle grouping is made to print each filament with its rack nozzle size on every layer", "[ToolSlots][Rack][Layers]")
{
    PrintConfig cfg;
    cfg.apply(h2c_rack(), true);
    const auto                      list = rack_nozzle_list();
    const std::vector<unsigned int> used = { 0, 1, 2 };

    // An engine result that ignores sizes: filament 2 (pinned 0.2) on a 0.4, filament 3 (0.4) on the 0.2.
    std::vector<int> map = { 0, 2, 1 };
    {
        const auto wrong = MultiNozzleUtils::LayeredNozzleGroupResult::create(map, list, used);
        REQUIRE(wrong);
        CHECK_FALSE(check_rack_nozzle_diameters(cfg, *wrong).empty());
    }
    CHECK(pin_rack_nozzle_diameters(cfg, list, used, map) == 2);
    CHECK(map == std::vector<int>{ 0, 1, 2 });
    const auto fixed = MultiNozzleUtils::LayeredNozzleGroupResult::create(map, list, used);
    REQUIRE(fixed);
    CHECK(check_rack_nozzle_diameters(cfg, *fixed).empty());
    CHECK(nozzle_diameter_for_filament_on_layer(cfg, &*fixed, 2, 0) == 0.2);

    // A per-layer grouping (a filament switcher): filament 2 on the 0.2 on layer 1, on the 0.6 on layer 2. Flow
    // sliced it for 0.2 everywhere, so layer 2 is refused, and the per-layer lookup tells the layers apart.
    const std::vector<std::vector<int>>          layer_maps = { { 0, 1, 2 }, { 0, 4, 2 } };
    const std::vector<std::vector<unsigned int>> sequences  = { { 0, 1, 2 }, { 0, 1, 2 } };
    const auto layered = MultiNozzleUtils::LayeredNozzleGroupResult::create(layer_maps, list, used, sequences);
    REQUIRE(layered);
    CHECK(nozzle_diameter_for_filament_on_layer(cfg, &*layered, 2, 0) == 0.2);
    CHECK(nozzle_diameter_for_filament_on_layer(cfg, &*layered, 2, 1) == 0.6);
    const std::string message = check_rack_nozzle_diameters(cfg, *layered);
    CHECK(message.find("layer 2") != std::string::npos);
    CHECK(message.find("Filament 2") != std::string::npos);
    // No grouping at hand: the static answer.
    CHECK(nozzle_diameter_for_filament_on_layer(cfg, nullptr, 2, 5) == 0.2);
}

// ---- 4. Per-nozzle filament values -----------------------------------------------------------------------------

TEST_CASE("Per-nozzle filament values parse selectors and pairs", "[ToolSlots][FilamentValues]")
{
    std::vector<std::string> errors;
    const auto               variants = parse_filament_nozzle_variants(
        "0.6: nozzle_temperature=230, filament_max_volumetric_speed = 30 | 0.6 High Flow: filament_max_volumetric_speed=40 |"
        " T2: nozzle_temperature=215 | high_flow: filament_flow_ratio=0.95 | nonsense | 0.4 bogus: nozzle_temperature=1 | 0.2: novalue",
        &errors);
    REQUIRE(variants.size() == 4);
    CHECK(variants[0].diameter == 0.6);
    CHECK(variants[0].flow_mode == -1);
    REQUIRE(variants[0].values.size() == 2);
    CHECK(variants[0].values[1].first == "filament_max_volumetric_speed");
    CHECK(variants[0].values[1].second == "30");
    CHECK(variants[1].flow_mode == HF);
    CHECK(variants[1].diameter == 0.6);
    CHECK(variants[2].tool == 2);
    CHECK(variants[3].flow_mode == HF);
    CHECK(variants[3].diameter == 0.);
    CHECK(errors.size() == 3);
    CHECK(is_filament_nozzle_variant_key("nozzle_temperature"));
    CHECK(is_filament_nozzle_variant_key("filament_max_volumetric_speed"));
    CHECK_FALSE(is_filament_nozzle_variant_key("filament_type"));
    CHECK_FALSE(is_filament_nozzle_variant_key("outer_wall_speed"));
}

TEST_CASE("One filament preset on a 0.4 and a 0.6 nozzle takes each nozzle's values", "[ToolSlots][FilamentValues]")
{
    DynamicPrintConfig cfg = toolchanger({ 0.4, 0.6 });
    cfg.option<ConfigOptionInts>("nozzle_temperature", true)->values               = { 210, 210 };
    cfg.option<ConfigOptionFloats>("filament_max_volumetric_speed", true)->values  = { 15, 15 };
    const std::string same_preset = "0.6: nozzle_temperature=230, filament_max_volumetric_speed=30 | 0.2: nozzle_temperature=200";
    cfg.option<ConfigOptionStrings>("filament_nozzle_variants", true)->values = { same_preset, same_preset };
    std::vector<std::string> errors;
    CHECK(resolve_filament_nozzle_variants(cfg, &errors) == 2);
    CHECK(errors.empty());
    CHECK(cfg.option<ConfigOptionInts>("nozzle_temperature")->values == std::vector<int>{ 210, 230 });
    CHECK(cfg.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == std::vector<double>{ 15, 30 });
    // Resolving again changes nothing more (Print::apply runs it on every apply).
    const DynamicPrintConfig once = cfg;
    resolve_filament_nozzle_variants(cfg);
    CHECK(cfg == once);
    // The filament moved to the 0.6 tool takes the 0.6 values.
    DynamicPrintConfig moved = toolchanger({ 0.4, 0.6 });
    moved.option<ConfigOptionInts>("nozzle_temperature", true)->values              = { 210, 210 };
    moved.option<ConfigOptionStrings>("filament_nozzle_variants", true)->values     = { same_preset, "" };
    moved.option<ConfigOptionInts>("filament_nozzle_assignment", true)->values      = { 2 };
    resolve_filament_nozzle_variants(moved);
    CHECK(moved.option<ConfigOptionInts>("nozzle_temperature")->values == std::vector<int>{ 230, 210 });
}

TEST_CASE("Per-nozzle filament values reach packed flow-variant slots", "[ToolSlots][FilamentValues]")
{
    // Filament 1 carries Standard + High Flow values (step 2), filament 2 one value; filament 1 is on the 0.6.
    DynamicPrintConfig cfg = toolchanger({ 0.6, 0.4 });
    cfg.option<ConfigOptionInts>("filament_flow_step_size", true)->values         = { 2, 1 };
    cfg.option<ConfigOptionStrings>("filament_flow_support", true)->values        = { FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW, FLOW_MODE_STANDARD };
    cfg.option<ConfigOptionFloats>("filament_max_volumetric_speed", true)->values = { 20, 35, 15 };
    cfg.option<ConfigOptionStrings>("filament_nozzle_variants", true)->values     = { "0.6 high_flow: filament_max_volumetric_speed=45 | "
                                                                                      "0.6: filament_flow_ratio=0.97",
                                                                                      "0.6: filament_max_volumetric_speed=99" };
    cfg.option<ConfigOptionFloats>("filament_flow_ratio", true)->values           = { 1, 1, 1 };
    resolve_filament_nozzle_variants(cfg);
    // Only filament 1's High Flow slot of the volumetric speed changes; both its flow ratio slots do; filament 2
    // is on a 0.4 and keeps everything.
    CHECK(cfg.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == std::vector<double>{ 20, 45, 15 });
    CHECK(cfg.option<ConfigOptionFloats>("filament_flow_ratio")->values == std::vector<double>{ 0.97, 0.97, 1 });
}

TEST_CASE("Per-nozzle filament values report keys and values they cannot set", "[ToolSlots][FilamentValues]")
{
    DynamicPrintConfig cfg = toolchanger({ 0.4, 0.6 });
    cfg.option<ConfigOptionStrings>("filament_nozzle_variants", true)->values = { "", "0.6: outer_wall_speed=10, nozzle_temperature=hot" };
    std::vector<std::string> errors;
    CHECK(resolve_filament_nozzle_variants(cfg, &errors) == 0);
    CHECK(errors.size() == 2);
    // No variants at all: nothing to do.
    DynamicPrintConfig plain = toolchanger({ 0.4, 0.6 });
    const DynamicPrintConfig before = plain;
    CHECK(resolve_filament_nozzle_variants(plain) == 0);
    CHECK(plain == before);
}

// ---- 5. physical_nozzle_properties -----------------------------------------------------------------------------

TEST_CASE("The physical nozzles are declared for mixed or mapping printers only", "[ToolSlots][Header]")
{
    DynamicPrintConfig uniform = toolchanger({ 0.4, 0.4, 0.4, 0.4 });
    CHECK_FALSE(emit_physical_nozzle_properties(uniform));
    CHECK(physical_nozzle_properties(uniform) == "0.4-standard,0.4-standard,0.4-standard,0.4-standard");
    uniform.option<ConfigOptionBool>("support_filament_nozzle_mapping", true)->value = true;
    CHECK(emit_physical_nozzle_properties(uniform));

    DynamicPrintConfig mixed = toolchanger({ 0.4, 0.4, 0.6, 0.6 });
    mixed.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = { int(nvtStandard), int(nvtHighFlow), int(nvtStandard),
                                                                                  int(nvtHighFlow) };
    CHECK(emit_physical_nozzle_properties(mixed));
    CHECK(physical_nozzle_properties(mixed) == "0.4-standard,0.4-high_flow,0.6-standard,0.6-high_flow");

    const DynamicPrintConfig rack = h2c_rack();
    CHECK(emit_physical_nozzle_properties(rack));
    CHECK(physical_nozzle_properties(rack) == "0.4-standard,0.4-standard,0.4-standard,0.2-standard,0.6-high_flow");
}

// ---- 6. K3 Tier C: data only -----------------------------------------------------------------------------------

TEST_CASE("A K3-like four-nozzle machine of four sizes is described by data alone", "[ToolSlots][K3]")
{
    // Four direct-feed nozzles, 0.2 / 0.4 / 0.6 / 0.8, the printer maps filaments itself.
    DynamicPrintConfig k3 = toolchanger({ 0.2, 0.4, 0.6, 0.8 });
    k3.option<ConfigOptionBool>("support_filament_nozzle_mapping", true)->value = true;
    set_process_tool_slots(k3, { 0, 0, 0, 0 }, { "Direct Drive Standard", "Direct Drive Standard", "Direct Drive Standard", "Direct Drive Standard" },
                           { 0.2, 0.4, 0.6, 0.8 });
    k3.option<ConfigOptionFloats>("outer_wall_speed", true)->values = { 40, 100, 80, 60 };
    CHECK(outer_wall_speed_of(k3, 0) == 40);
    CHECK(outer_wall_speed_of(k3, 1) == 100);
    CHECK(outer_wall_speed_of(k3, 2) == 80);
    CHECK(outer_wall_speed_of(k3, 3) == 60);
    CHECK(physical_nozzle_properties(k3) == "0.2-standard,0.4-standard,0.6-standard,0.8-standard");
    // Layer height guard (D9): the smallest nozzle that prints.
    k3.option<ConfigOptionFloats>("min_layer_height", true)->values = { 0.04, 0.08, 0.12, 0.16 };
    k3.option<ConfigOptionFloats>("max_layer_height", true)->values = { 0.14, 0.28, 0.42, 0.56 };
    double lo = 0., hi = 0.;
    layer_height_limits_for_nozzles(k3, { 1, 3 }, lo, hi);
    CHECK(lo == 0.08);
    CHECK(hi == 0.28);
}

// ---- 7. The G-code's tool numbers ------------------------------------------------------------------------------

TEST_CASE("Assigned filaments name their assigned tools in the G-code when the tools are distinct", "[ToolSlots][OutputTools]")
{
    PrintConfig cfg;
    cfg.apply(toolchanger({ 0.4, 0.4, 0.6, 0.6 }), true);
    // Nothing assigned: the G-code names each filament's own tool (no map).
    CHECK(filament_output_tools(cfg, { 0, 1 }).empty());

    // Filament 1 on T4, filaments 1 and 2 used: T3 for filament 1, filament 4 (unused) takes the free T1.
    cfg.filament_nozzle_assignment.values = { 4 };
    bool shared = true;
    const std::vector<int> tools = filament_output_tools(cfg, { 0, 1 }, &shared);
    CHECK_FALSE(shared);
    CHECK(tools == std::vector<int>{ 3, 1, 2, 0 });
    CHECK(output_tool_filaments(tools) == std::vector<int>{ 3, 1, 2, 0 });

    // Filament 1 and filament 4 both on T4: they share a tool, so no map, and the caller is told.
    filament_output_tools(cfg, { 0, 3 }, &shared);
    CHECK(shared);
    CHECK(filament_output_tools(cfg, { 0, 3 }).empty());

    // An assignment to the filament's own tool is the identity: no map.
    cfg.filament_nozzle_assignment.values = { 1, 2 };
    CHECK(filament_output_tools(cfg, { 0, 1 }).empty());

    // A swap.
    cfg.filament_nozzle_assignment.values = { 2, 1 };
    CHECK(filament_output_tools(cfg, { 0, 1 }) == std::vector<int>{ 1, 0, 2, 3 });
}

TEST_CASE("Bambu grouping printers and AMS-shared nozzles never renumber tools", "[ToolSlots][OutputTools]")
{
    DynamicPrintConfig h2d = toolchanger({ 0.4, 0.4 });
    h2d.option<ConfigOptionStrings>("extruder_variant_list", true)->values = { "Direct Drive Standard,Direct Drive High Flow",
                                                                               "Direct Drive Standard,Direct Drive High Flow" };
    h2d.option<ConfigOptionInts>("filament_nozzle_assignment", true)->values = { 2, 1 };
    PrintConfig cfg;
    cfg.apply(h2d, true);
    CHECK(filament_output_tools(cfg, { 0, 1 }).empty());

    // More filaments than tools (an AMS on a one-nozzle extruder): no map.
    DynamicPrintConfig ams = toolchanger({ 0.4, 0.4 });
    ams.set_num_filaments(3);
    ams.option<ConfigOptionInts>("filament_nozzle_assignment", true)->values = { 2 };
    cfg.apply(ams, true);
    CHECK(filament_output_tools(cfg, { 0 }).empty());
}

// ---- Projects --------------------------------------------------------------------------------------------------

namespace {

bool tool_slots_round_trip(DynamicPrintConfig store_config, DynamicPrintConfig &dst_config)
{
    const boost::filesystem::path dir = boost::filesystem::temp_directory_path() / ("snorca_tests_tool_slots_" + std::to_string(get_current_pid()));
    boost::filesystem::create_directories(dir);
    Slic3r::set_temporary_dir(dir.string());

    Model        src_model;
    ModelObject *object = src_model.add_object();
    object->name        = "cube";
    object->add_volume(make_cube(10., 10., 10.))->name = "cube";
    object->add_instance();
    object->ensure_on_bed();

    const std::string file = (dir / "tool_slots_project.3mf").string();
    StoreParams       store_params;
    store_params.path     = file.c_str();
    store_params.model    = &src_model;
    store_params.config   = &store_config;
    store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    if (!store_bbs_3mf(store_params))
        return false;

    Model                     dst_model;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
    PlateDataPtrs             plate_data;
    std::vector<Preset *>     project_presets;
    bool                      is_bbl_3mf = false;
    Semver                    file_version;
    const bool loaded = load_bbs_3mf(file.c_str(), &dst_config, &ctxt, &dst_model, &plate_data, &project_presets, &is_bbl_3mf,
                                     &file_version, nullptr,
                                     LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances |
                                         LoadStrategy::Silence);
    release_PlateData_list(plate_data);
    boost::filesystem::remove(file);
    return loaded;
}

} // namespace

TEST_CASE("Slot diameters, rack sizes and per-nozzle filament values save and reopen with the project", "[ToolSlots][3mf]")
{
    DynamicPrintConfig cfg = h2c_rack();
    set_process_tool_slots(cfg, { 0, 0 }, { "Direct Drive Standard", "Direct Drive Standard" }, { 0.4, 0.2 });
    cfg.option<ConfigOptionFloats>("outer_wall_speed", true)->values = { 200, 50 };
    cfg.option<ConfigOptionStrings>("filament_nozzle_variants", true)->values = { "", "0.2: nozzle_temperature=205", "" };
    DynamicPrintConfig loaded;
    REQUIRE(tool_slots_round_trip(cfg, loaded));
    for (const char *key : { "print_extruder_nozzle_diameter", "extruder_rack_nozzle_diameter", "filament_rack_nozzle_diameter",
                             "filament_nozzle_variants", "outer_wall_speed" }) {
        INFO("option " << key);
        REQUIRE(loaded.has(key));
        CHECK(*loaded.option(key) == *cfg.option(key));
    }
}
