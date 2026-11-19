// Per-nozzle process P5 (tests/design_per_nozzle_process.md): the editing side of the per-nozzle data. The Filament
// tab's nozzle-size view and table over filament_nozzle_variants, the rack nozzle field and the filament's rack pin,
// a process split by nozzle size, and the P4 / P2 follow-ups (the flush matrix in tool space, skin / skeleton and
// first-layer widths per nozzle).

#include <catch2/catch.hpp>

#include "libslic3r/NozzleProcess.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ToolSlots.hpp"

#include <string>
#include <vector>

using namespace Slic3r;

namespace {

const int STD = int(fvtStandard);
const int HF  = int(fvtHighFlow);

// A filament preset (its own config) with Standard and High Flow values.
DynamicPrintConfig filament_preset(const std::string &variants)
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.option<ConfigOptionStrings>("filament_flow_support", true)->values    = { FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW };
    cfg.option<ConfigOptionInts>("nozzle_temperature", true)->values          = { 220, 230 };
    cfg.option<ConfigOptionFloats>("fan_max_speed", true)->values             = { 80, 90 };
    cfg.option<ConfigOptionInts>("hot_plate_temp", true)->values              = { 60 };
    cfg.option<ConfigOptionFloats>("filament_max_volumetric_speed", true)->values = { 12, 20 };
    cfg.option<ConfigOptionStrings>("filament_nozzle_variants", true)->values = { variants };
    return cfg;
}

std::vector<std::string> values_of(const DynamicPrintConfig &cfg, const char *key)
{
    return static_cast<const ConfigOptionVectorBase *>(cfg.option(key))->vserialize();
}

// A four-tool 0.4 + 0.6 toolchanger with one process for every tool.
DynamicPrintConfig u1_mixed()
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_num_filaments(4);
    cfg.option<ConfigOptionFloats>("nozzle_diameter", true)->values             = { 0.4, 0.4, 0.6, 0.6 };
    cfg.option<ConfigOptionBool>("single_extruder_multi_material", true)->value = false;
    cfg.option<ConfigOptionStrings>("process_flow_support", true)->values       = { FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW };
    cfg.option<ConfigOptionEnumsGeneric>("filament_volume_type", true)->values  = { STD, STD, STD, HF };
    cfg.option<ConfigOptionFloats>("outer_wall_speed", true)->values            = { 200, 300 };
    return cfg;
}

double outer_wall_speed_of(const DynamicPrintConfig &cfg, unsigned int filament_idx)
{
    return get_value_at(cfg, *cfg.option<ConfigOptionFloats>("outer_wall_speed"), ConfigFlowDomain::Process, filament_idx);
}

} // namespace

// ---- 1. The text the editor writes ------------------------------------------------------------------------------

TEST_CASE("Per-nozzle filament values written by the editor read back the same", "[NozzleEditing][FilamentValues]")
{
    CHECK(format_nozzle_diameter_word(0.6) == "0.6");
    CHECK(format_nozzle_diameter_word(0.25) == "0.25");
    CHECK(format_filament_nozzle_selector(2, 0.6, HF) == "T2 0.6 high_flow");
    CHECK(format_filament_nozzle_selector(0, 0.4, -1) == "0.4");
    CHECK(format_filament_nozzle_selector(0, 0., STD) == "standard");

    const std::string text = "0.6: nozzle_temperature=230, filament_max_volumetric_speed=30 | T2 high_flow: fan_max_speed=50";
    const std::vector<FilamentNozzleVariant> variants = parse_filament_nozzle_variants(text);
    REQUIRE(variants.size() == 2);
    const std::string again = format_filament_nozzle_variants(variants);
    CHECK(again == "0.6: nozzle_temperature=230, filament_max_volumetric_speed=30 | T2 high_flow: fan_max_speed=50");
    const std::vector<FilamentNozzleVariant> reparsed = parse_filament_nozzle_variants(again);
    REQUIRE(reparsed.size() == 2);
    CHECK(reparsed[1].tool == 2);
    CHECK(reparsed[1].flow_mode == HF);
    CHECK(reparsed[1].values.front().second == "50");
    CHECK(filament_nozzle_variant_diameters(text) == std::vector<double>{ 0.6 });
}

TEST_CASE("One value per nozzle is set, replaced and removed without touching the others", "[NozzleEditing][FilamentValues]")
{
    std::string text = set_filament_nozzle_value("", 0, 0.6, -1, "nozzle_temperature", "230");
    CHECK(text == "0.6: nozzle_temperature=230");
    text = set_filament_nozzle_value(text, 0, 0.6, -1, "filament_max_volumetric_speed", "30");
    CHECK(text == "0.6: nozzle_temperature=230, filament_max_volumetric_speed=30");
    text = set_filament_nozzle_value(text, 0, 0.2, HF, "fan_max_speed", "40");
    CHECK(text == "0.6: nozzle_temperature=230, filament_max_volumetric_speed=30 | 0.2 high_flow: fan_max_speed=40");
    // The same selector written another way ("0.60") is the same entry.
    text = set_filament_nozzle_value(text, 0, 0.60, -1, "nozzle_temperature", "235");
    CHECK(text == "0.6: nozzle_temperature=235, filament_max_volumetric_speed=30 | 0.2 high_flow: fan_max_speed=40");

    std::string value;
    CHECK(find_filament_nozzle_value(text, 0, 0.6, -1, "nozzle_temperature", &value));
    CHECK(value == "235");
    CHECK_FALSE(find_filament_nozzle_value(text, 0, 0.6, HF, "nozzle_temperature"));
    CHECK_FALSE(find_filament_nozzle_value(text, 2, 0.6, -1, "nozzle_temperature"));

    text = set_filament_nozzle_value(text, 0, 0.6, -1, "nozzle_temperature", "");
    CHECK(text == "0.6: filament_max_volumetric_speed=30 | 0.2 high_flow: fan_max_speed=40");
    // An entry with no value left goes.
    text = set_filament_nozzle_value(text, 0, 0.6, -1, "filament_max_volumetric_speed", "");
    CHECK(text == "0.2 high_flow: fan_max_speed=40");
    // Removing what is not there changes nothing.
    CHECK(set_filament_nozzle_value(text, 3, 0.4, -1, "fan_max_speed", "") == text);
    // A tool's value.
    CHECK(set_filament_nozzle_value("", 3, 0., -1, "nozzle_temperature", "240") == "T3: nozzle_temperature=240");
}

TEST_CASE("Only the nozzle's own filament values vary per nozzle size", "[NozzleEditing][FilamentValues]")
{
    CHECK(is_filament_nozzle_size_key("nozzle_temperature"));
    CHECK(is_filament_nozzle_size_key("nozzle_temperature_initial_layer"));
    CHECK(is_filament_nozzle_size_key("filament_max_volumetric_speed"));
    CHECK(is_filament_nozzle_size_key("fan_min_speed"));
    CHECK(is_filament_nozzle_size_key("pressure_advance"));
    CHECK(is_filament_nozzle_size_key("enable_pressure_advance"));
    CHECK(is_filament_nozzle_size_key("filament_retraction_length"));
    CHECK(is_filament_nozzle_size_key("filament_flow_ratio"));
    // The spool's, the bed's, identity and text values do not.
    CHECK_FALSE(is_filament_nozzle_size_key("filament_cost"));
    CHECK_FALSE(is_filament_nozzle_size_key("filament_density"));
    CHECK_FALSE(is_filament_nozzle_size_key("hot_plate_temp"));
    CHECK_FALSE(is_filament_nozzle_size_key("hot_plate_temp_initial_layer"));
    CHECK_FALSE(is_filament_nozzle_size_key("filament_type"));
    CHECK_FALSE(is_filament_nozzle_size_key("filament_diameter"));
    CHECK_FALSE(is_filament_nozzle_size_key("filament_start_gcode"));
    CHECK_FALSE(is_filament_nozzle_size_key("filament_nozzle_variants"));
    CHECK_FALSE(is_filament_nozzle_size_key("outer_wall_speed")); // a process value
}

TEST_CASE("Per-nozzle filament values are checked key by key", "[NozzleEditing][FilamentValues]")
{
    CHECK(validate_filament_nozzle_value("nozzle_temperature", "230").empty());
    CHECK(validate_filament_nozzle_value("pressure_advance", "0.04").empty());
    CHECK(validate_filament_nozzle_value("enable_pressure_advance", "1").empty());
    CHECK(validate_filament_nozzle_value("filament_retraction_length", "nil").empty()); // a nullable override switched off
    CHECK_FALSE(validate_filament_nozzle_value("nozzle_temperature", "").empty());
    CHECK_FALSE(validate_filament_nozzle_value("nozzle_temperature", "hot").empty());
    CHECK_FALSE(validate_filament_nozzle_value("nozzle_temperature", "2,3").empty());
    CHECK_FALSE(validate_filament_nozzle_value("nozzle_temperature", "-5").empty()); // below the option's minimum
    CHECK_FALSE(validate_filament_nozzle_value("enable_pressure_advance", "maybe").empty());
    CHECK_FALSE(validate_filament_nozzle_value("filament_type", "PLA").empty());
    CHECK_FALSE(validate_filament_nozzle_value("outer_wall_speed", "60").empty());

    CHECK(validate_filament_nozzle_variants("").empty());
    CHECK(validate_filament_nozzle_variants("0.6: nozzle_temperature=230 | T2: fan_max_speed=50").empty());
    CHECK(validate_filament_nozzle_variants("0.6: nozzle_temperature=hot").size() == 1);
    CHECK(validate_filament_nozzle_variants("big: nozzle_temperature=230").size() == 1);
    CHECK(validate_filament_nozzle_variants("0.6 nozzle_temperature=230").size() == 1);
}

// ---- 2. The Filament tab's nozzle-size view ---------------------------------------------------------------------

TEST_CASE("A filament preset seen on one nozzle size shows that size's values", "[NozzleEditing][SizeView]")
{
    const std::string text = "0.6: nozzle_temperature=240, filament_max_volumetric_speed=30 | 0.6 high_flow: nozzle_temperature=250"
                             " | high_flow: fan_max_speed=50 | T2: nozzle_temperature=260 | 0.4: hot_plate_temp=70";

    SECTION("All shows the preset's own values")
    {
        DynamicPrintConfig view = filament_preset(text);
        CHECK(apply_filament_nozzle_size_view(view, 0., STD) == 0);
        CHECK(values_of(view, "nozzle_temperature") == std::vector<std::string>{ "220", "230" });
    }
    SECTION("0.6 Standard view: the size entry in both flow slots, the more specific High Flow entry in its own")
    {
        DynamicPrintConfig view = filament_preset(text);
        CHECK(apply_filament_nozzle_size_view(view, 0.6, STD) > 0);
        CHECK(values_of(view, "nozzle_temperature") == std::vector<std::string>{ "240", "250" });
        CHECK(values_of(view, "filament_max_volumetric_speed") == std::vector<std::string>{ "30", "30" });
        // A flow-type entry for every size.
        CHECK(values_of(view, "fan_max_speed") == std::vector<std::string>{ "80", "50" });
    }
    SECTION("0.4 view: only the entries for every size; a tool's value is not a size's")
    {
        DynamicPrintConfig view = filament_preset(text);
        apply_filament_nozzle_size_view(view, 0.4, STD);
        CHECK(values_of(view, "nozzle_temperature") == std::vector<std::string>{ "220", "230" });
        CHECK(values_of(view, "fan_max_speed") == std::vector<std::string>{ "80", "50" });
        // A value the slicer would refuse (a bed temperature is not a filament-per-nozzle key it can use) still
        // shows: is_filament_nozzle_variant_key() takes it, so the view does too.
        CHECK(values_of(view, "hot_plate_temp") == std::vector<std::string>{ "70" });
    }
}

TEST_CASE("Editing a value in the size view and back gives the text the slicer reads", "[NozzleEditing][SizeView]")
{
    // What the Tab does on an edit at 0.6 Standard: keep the value unless it equals what the size shows without it.
    DynamicPrintConfig preset = filament_preset("");
    std::string        text   = set_filament_nozzle_value("", 0, 0.6, STD, "nozzle_temperature", "235");
    preset.option<ConfigOptionStrings>("filament_nozzle_variants")->values = { text };
    DynamicPrintConfig view = preset;
    apply_filament_nozzle_size_view(view, 0.6, STD);
    CHECK(values_of(view, "nozzle_temperature") == std::vector<std::string>{ "235", "230" });

    // The slicer, for a filament of this preset on a 0.6 nozzle, Standard flow: 235. On a 0.4: 220.
    DynamicPrintConfig full = u1_mixed();
    full.option<ConfigOptionStrings>("filament_nozzle_variants", true)->values = { text, text, text, text };
    full.option<ConfigOptionInts>("nozzle_temperature", true)->values          = { 220, 220, 220, 220 };
    resolve_filament_nozzle_variants(full);
    CHECK(full.option<ConfigOptionInts>("nozzle_temperature")->values == std::vector<int>{ 220, 220, 235, 220 }); // filament 4 is High Flow
}

// ---- 3. Rack nozzle sizes ----------------------------------------------------------------------------------------

TEST_CASE("The rack nozzle field says what is wrong with its text", "[NozzleEditing][Rack]")
{
    CHECK(validate_rack_nozzle_text("", 0.4).empty());
    CHECK(validate_rack_nozzle_text("0.4, 0.4, 0.2, 0.6:high_flow", 0.4).empty());
    CHECK(validate_rack_nozzle_text("0.4,0.2", 0.).empty()); // diameter unknown: not checked
    CHECK_FALSE(validate_rack_nozzle_text("0.4, abc", 0.4).empty());
    CHECK_FALSE(validate_rack_nozzle_text("0.4, 0", 0.4).empty());
    CHECK_FALSE(validate_rack_nozzle_text("0.4, 4", 0.4).empty()); // not a nozzle size in mm
    CHECK_FALSE(validate_rack_nozzle_text("0.4, 0.2:fast", 0.4).empty());
    CHECK(validate_rack_nozzle_text("0.2, 0.6", 0.4).find("not in the rack") != std::string::npos);
}

TEST_CASE("A filament offers the sizes of its extruder's rack and pins one", "[NozzleEditing][Rack]")
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_num_filaments(2);
    cfg.option<ConfigOptionFloats>("nozzle_diameter", true)->values             = { 0.4, 0.4 };
    cfg.option<ConfigOptionBool>("single_extruder_multi_material", true)->value = false;
    cfg.option<ConfigOptionStrings>("extruder_rack_nozzle_diameter", true)->values = { "0.4,0.4", "0.4,0.2,0.6,0.2" };
    CHECK(filament_rack_pin_choices(cfg, 1).empty()); // one size in the first rack: nothing to choose
    CHECK(filament_rack_pin_choices(cfg, 2) == std::vector<double>{ 0.2, 0.4, 0.6 });

    std::vector<double> pins = set_filament_rack_pin({ 0. }, 1, 2, 0.2);
    CHECK(pins == std::vector<double>{ 0., 0.2 });
    cfg.option<ConfigOptionFloats>("filament_rack_nozzle_diameter", true)->values = pins;
    CHECK(filament_rack_pin(cfg, 2) == 0.2);
    CHECK(nozzle_diameter_for_filament(cfg, 2) == 0.2);
    // Unpinning the last pin stores the unused value again.
    CHECK(set_filament_rack_pin(pins, 1, 2, 0.) == std::vector<double>{ 0. });
}

// ---- 4. A process split by nozzle size --------------------------------------------------------------------------

TEST_CASE("A process splits by nozzle size, prints each size its own speed and links back", "[NozzleEditing][ProcessSizes]")
{
    DynamicPrintConfig cfg = u1_mixed();
    CHECK(printer_nozzle_sizes(cfg) == std::vector<double>{ 0.4, 0.6 });
    REQUIRE(split_process_per_diameter(cfg, { 0.6, 0.4, 0.4 }));
    CHECK(cfg.option<ConfigOptionInts>("print_extruder_id")->values == std::vector<int>{ 0, 0, 0, 0 });
    CHECK(cfg.option<ConfigOptionFloats>("print_extruder_nozzle_diameter")->values == std::vector<double>{ 0.4, 0.4, 0.6, 0.6 });
    CHECK(cfg.option<ConfigOptionFloats>("outer_wall_speed")->values == std::vector<double>{ 200, 300, 200, 300 });
    CHECK(process_slot_diameters(cfg) == std::vector<double>{ 0.4, 0.6 });
    // Nothing changes yet: every filament prints what it printed before.
    CHECK(outer_wall_speed_of(cfg, 0) == 200);
    CHECK(outer_wall_speed_of(cfg, 3) == 300);

    // The Speed page's fields of the 0.6 size.
    const int std06 = process_diameter_slot(cfg, "outer_wall_speed", 0.6, fvtStandard);
    const int hf06  = process_diameter_slot(cfg, "outer_wall_speed", 0.6, fvtHighFlow);
    CHECK(std06 == 2);
    CHECK(hf06 == 3);
    CHECK(process_diameter_slot(cfg, "outer_wall_speed", 0.8, fvtStandard) == -1);
    CHECK(process_diameter_slot(cfg, "layer_height", 0.6, fvtStandard) == -1); // not a per-nozzle key
    cfg.option<ConfigOptionFloats>("outer_wall_speed")->values[size_t(std06)] = 60;
    cfg.option<ConfigOptionFloats>("outer_wall_speed")->values[size_t(hf06)]  = 70;
    CHECK(outer_wall_speed_of(cfg, 0) == 200); // T1 0.4
    CHECK(outer_wall_speed_of(cfg, 2) == 60);  // T3 0.6 Standard
    CHECK(outer_wall_speed_of(cfg, 3) == 70);  // T4 0.6 High Flow

    // Splitting twice, or per nozzle while split by size, is refused.
    CHECK_FALSE(split_process_per_diameter(cfg, { 0.4, 0.6 }));

    // Link from 0.6: one value per flow mode again, the 0.6 values.
    DynamicPrintConfig reference = u1_mixed();
    REQUIRE(link_process_diameters(cfg, 0.6, &reference));
    CHECK_FALSE(process_has_diameter_slots(cfg));
    CHECK(cfg.option<ConfigOptionFloats>("outer_wall_speed")->values == std::vector<double>{ 60, 70 });
    CHECK(outer_wall_speed_of(cfg, 0) == 60);
    CHECK_FALSE(link_process_diameters(cfg, 0.6, &reference));
}

TEST_CASE("A process split per nozzle is not split by size", "[NozzleEditing][ProcessSizes]")
{
    DynamicPrintConfig cfg = u1_mixed();
    REQUIRE(split_process_per_nozzle(cfg, 4));
    CHECK_FALSE(split_process_per_diameter(cfg, { 0.4, 0.6 }));
    DynamicPrintConfig one_size = u1_mixed();
    CHECK_FALSE(split_process_per_diameter(one_size, { 0.4 }));
}

TEST_CASE("Split by size and linked back unchanged, a process is the one it was", "[NozzleEditing][ProcessSizes]")
{
    const DynamicPrintConfig before = u1_mixed();
    DynamicPrintConfig       cfg    = before;
    REQUIRE(split_process_per_diameter(cfg, { 0.4, 0.6 }));
    REQUIRE(link_process_diameters(cfg, 0.4, &before));
    for (const std::string &key : per_nozzle_process_options()) {
        const ConfigOption *a = before.option(key);
        const ConfigOption *b = cfg.option(key);
        INFO(key);
        REQUIRE(a != nullptr);
        REQUIRE(b != nullptr);
        CHECK(a->serialize() == b->serialize());
    }
    CHECK_FALSE(process_has_diameter_slots(cfg));
}

// ---- 5. P4 / P2 follow-ups ---------------------------------------------------------------------------------------

TEST_CASE("The purge matrix in tool space follows the filaments to their tools", "[NozzleEditing][OutputTools]")
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_num_filaments(2);
    // From filament 1 to 2: 100; from 2 to 1: 200.
    cfg.option<ConfigOptionFloats>("flush_volumes_matrix", true)->values = { 0, 100, 200, 0 };
    DynamicConfig out;
    filament_values_to_tool_space(out, cfg, { 1, 0 }); // filament 1 on T1 (0-based 1), filament 2 on T0
    const auto *matrix = out.option<ConfigOptionFloats>("flush_volumes_matrix");
    REQUIRE(matrix != nullptr);
    // From tool 0 (filament 2) to tool 1 (filament 1) is 200, the other way 100.
    CHECK(matrix->values == std::vector<double>{ 0, 200, 100, 0 });

    // Two blocks (two extruders), each permuted.
    cfg.option<ConfigOptionFloats>("flush_volumes_matrix", true)->values = { 0, 100, 200, 0, 0, 300, 400, 0 };
    DynamicConfig out2;
    filament_values_to_tool_space(out2, cfg, { 1, 0 });
    CHECK(out2.option<ConfigOptionFloats>("flush_volumes_matrix")->values == std::vector<double>{ 0, 200, 100, 0, 0, 400, 300, 0 });
}

TEST_CASE("Skin, skeleton and first-layer widths come from the process of the filament's nozzle", "[NozzleEditing][Widths]")
{
    DynamicPrintConfig cfg = u1_mixed();
    cfg.set_deserialize_strict({ { "skin_infill_line_width", "0.42" }, { "skeleton_infill_line_width", "0.42" },
                                 { "initial_layer_line_width", "0.5" }, { "line_width", "0.42" } });
    DynamicPrintConfig p06;
    p06.set_key_value("process_flow_support", new ConfigOptionStrings({ FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW }));
    p06.set_key_value("outer_wall_speed", new ConfigOptionFloats({ 120, 180 }));
    p06.set_key_value("skin_infill_line_width", new ConfigOptionFloatOrPercent(0.62, false));
    p06.set_key_value("skeleton_infill_line_width", new ConfigOptionFloatOrPercent(110., true));
    p06.set_key_value("initial_layer_line_width", new ConfigOptionFloatOrPercent(0., false)); // 0: the role widths

    SECTION("no per-nozzle process: empty arrays")
    {
        DynamicPrintConfig plain = cfg;
        CHECK_FALSE(fill_nozzle_line_widths(plain, { nullptr, nullptr, nullptr, nullptr }));
        CHECK(plain.option<ConfigOptionFloatsOrPercents>("nozzle_skin_infill_line_width")->values.empty());
        CHECK(plain.option<ConfigOptionFloatsOrPercents>("nozzle_initial_layer_line_width")->values.empty());
    }
    SECTION("T3 / T4 on the 0.6 process")
    {
        REQUIRE(fill_nozzle_process_slots(cfg, { nullptr, nullptr, &p06, &p06 }));
        const auto &skin = cfg.option<ConfigOptionFloatsOrPercents>("nozzle_skin_infill_line_width")->values;
        REQUIRE(skin.size() == 4);
        CHECK(skin[0].value == 0.42);
        CHECK(skin[2].value == 0.62);
        const auto &first = cfg.option<ConfigOptionFloatsOrPercents>("nozzle_initial_layer_line_width")->values;
        REQUIRE(first.size() == 4);
        CHECK(first[0].value == 0.5); // the plate's
        CHECK(first[3].value == 0.);  // the 0.6 process's 0 keeps its meaning: the role widths
        // The plate's own first-layer width is not touched.
        CHECK(cfg.option<ConfigOptionFloatOrPercent>("initial_layer_line_width")->value == 0.5);

        PrintRegionConfig region;
        region.apply(cfg, true);
        region.wall_filament.value          = 1;
        region.sparse_infill_filament.value = 3;
        region.solid_infill_filament.value  = 1;
        stamp_nozzle_line_widths(region);
        CHECK(region.skin_infill_line_width.value == 0.62);
        CHECK(region.skeleton_infill_line_width.value == 110.);
        CHECK(region.skeleton_infill_line_width.percent);
        region.sparse_infill_filament.value = 2;
        stamp_nozzle_line_widths(region);
        CHECK(region.skin_infill_line_width.value == 0.42);
    }
}
