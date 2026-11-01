// Per-nozzle process P7 (NozzleOverride.hpp): every tool's nozzle size set independently for the project, without a
// printer preset per combination. The stored form, the per-tool setter, the size choices, the app config text, the
// sizes a printer reports, Print::apply's resolution (nozzle_diameter, layer limits, P6 widths, P4 size slots and
// per-nozzle filament values), the preset bundle's project key and memory per printer model, and the 3MF round trip.
// The G-code side is in tests/fff_print/test_nozzle_override_gcode.cpp.

#include <catch2/catch.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/BambuDualNozzleSync.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/NozzleOverride.hpp"
#include "libslic3r/NozzleProcess.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ToolSlots.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>

#include <cmath>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {

bool same_sizes(double a, double b) { return std::abs(a - b) < 1e-6; }

bool same_sizes(const std::vector<double> &a, const std::vector<double> &b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!same_sizes(a[i], b[i]))
            return false;
    return true;
}

// A U1-like four-tool printer preset "Snapmaker U1 (0.4 nozzle)": four 0.4 nozzles, a process with mm widths for 0.4.
DynamicPrintConfig u1_04()
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_num_filaments(4);
    cfg.set_deserialize_strict({
        { "nozzle_diameter", "0.4,0.4,0.4,0.4" },
        { "max_layer_height", "0.28,0.28,0.28,0.28" },
        { "min_layer_height", "0.08,0.08,0.08,0.08" },
        { "single_extruder_multi_material", "0" },
        { "printer_variant", "0.4" },
        { "line_width", "0.42" },
        { "outer_wall_line_width", "0.42" },
        { "inner_wall_line_width", "0.45" },
        { "sparse_infill_line_width", "0.45" },
        { "internal_solid_infill_line_width", "0.42" },
        { "top_surface_line_width", "0.42" },
        { "initial_layer_line_width", "0.5" },
        { "support_line_width", "0.42" },
    });
    return cfg;
}

const std::vector<FloatOrPercent> &array(const DynamicPrintConfig &cfg, const char *key)
{
    return cfg.option<ConfigOptionFloatsOrPercents>(key)->values;
}

} // namespace

// ---- 1. The pure helpers -----------------------------------------------------------------------------------------

TEST_CASE("The stored override drops sizes equal to the preset's, and is { 0 } when nothing is overridden", "[NozzleOverride]")
{
    const std::vector<double> preset{ 0.4, 0.4, 0.4, 0.4 };
    CHECK(nozzle_diameter_override_stored({}, preset) == std::vector<double>{ 0. });
    CHECK(nozzle_diameter_override_stored({ 0.4, 0.4, 0., 0.4 }, preset) == std::vector<double>{ 0. });
    CHECK(same_sizes(nozzle_diameter_override_stored({ 0.2, 0.4, 0.6, 0.8 }, preset), { 0.2, 0.4, 0.6, 0.8 }));
    // Entries past the printer's extruders are dropped.
    CHECK(same_sizes(nozzle_diameter_override_stored({ 0.2, 0., 0., 0., 0.6 }, preset), { 0.2, 0., 0., 0. }));
    // Negative / nonsense entries are "the preset's".
    CHECK(same_sizes(nozzle_diameter_override_stored({ -1., 0.6 }, preset), { 0., 0.6 }));
}

TEST_CASE("Each tool's size is set on its own", "[NozzleOverride]")
{
    const std::vector<double> preset{ 0.4, 0.4, 0.4, 0.4 };
    std::vector<double>       ov{ 0. };
    ov = set_tool_nozzle_diameter(ov, preset, 0, 0.2);
    ov = set_tool_nozzle_diameter(ov, preset, 2, 0.6);
    ov = set_tool_nozzle_diameter(ov, preset, 3, 0.8);
    CHECK(same_sizes(nozzle_diameters_with_override(preset, ov), { 0.2, 0.4, 0.6, 0.8 }));
    // Back to the preset's size for T1 keeps the others.
    ov = set_tool_nozzle_diameter(ov, preset, 0, 0.);
    CHECK(same_sizes(nozzle_diameters_with_override(preset, ov), { 0.4, 0.4, 0.6, 0.8 }));
    // Every tool back: unused again.
    ov = set_tool_nozzle_diameter(ov, preset, 2, 0.4);
    ov = set_tool_nozzle_diameter(ov, preset, 3, 0.);
    CHECK(ov == std::vector<double>{ 0. });
    // A tool the printer does not have changes nothing.
    CHECK(set_tool_nozzle_diameter({ 0. }, preset, 7, 0.6) == std::vector<double>{ 0. });
}

TEST_CASE("The size picker offers the standard sizes, the family's variants and the current sizes", "[NozzleOverride]")
{
    CHECK(same_sizes(nozzle_size_choices({}, {}), { 0.2, 0.4, 0.6, 0.8 }));
    CHECK(same_sizes(printer_variant_sizes("0.4+0.6"), { 0.4, 0.6 }));
    CHECK(printer_variant_sizes("").empty());
    CHECK(same_sizes(nozzle_size_choices({ 0.4, 0.25 }, { "0.4", "0.4+0.6", "1.0" }), { 0.2, 0.25, 0.4, 0.6, 0.8, 1.0 }));
}

TEST_CASE("The app config text round-trips and refuses garbage", "[NozzleOverride]")
{
    CHECK(serialize_nozzle_diameter_override({ 0. }).empty());
    CHECK(serialize_nozzle_diameter_override({ 0.2, 0., 0.6, 0.8 }) == "0.2,0,0.6,0.8");
    CHECK(same_sizes(parse_nozzle_diameter_override("0.2,0,0.6,0.8"), { 0.2, 0., 0.6, 0.8 }));
    CHECK(same_sizes(parse_nozzle_diameter_override(" 0.25 , 0.4"), { 0.25, 0.4 }));
    CHECK(parse_nozzle_diameter_override("").empty());
    CHECK(parse_nozzle_diameter_override("0,0").empty());
    CHECK(parse_nozzle_diameter_override("0.4,abc").empty());
    CHECK(parse_nozzle_diameter_override("0.4,,0.6").empty());
    CHECK(parse_nozzle_diameter_override("9").empty());
}

TEST_CASE("Sync from printer and the mismatch check before sending", "[NozzleOverride][Sync]")
{
    const std::vector<double> preset{ 0.4, 0.4, 0.4, 0.4 };
    // The printer reports T1..T4; T2 is unknown (0) and keeps the current size (overridden to 0.6 before).
    const std::vector<double> ov = nozzle_diameter_override_from_reported(preset, { 0., 0.6 }, { 0.2, 0., 0.6, 0.8 });
    CHECK(same_sizes(nozzle_diameters_with_override(preset, ov), { 0.2, 0.6, 0.6, 0.8 }));
    // A report equal to the preset clears the override.
    CHECK(nozzle_diameter_override_from_reported(preset, { 0.2, 0.6 }, { 0.4, 0.4, 0.4, 0.4 }) == std::vector<double>{ 0. });

    const std::vector<double> sliced{ 0.2, 0.4, 0.6, 0.8 };
    CHECK(nozzle_size_mismatches(sliced, { 0.2, 0.4, 0.6, 0.8 }).empty());
    const auto all = nozzle_size_mismatches(sliced, { 0.2, 0.4, 0.4, 0. });
    REQUIRE(all.size() == 1); // T4 unknown: no mismatch
    CHECK(all.front().tool == 2);
    CHECK(describe_nozzle_size_mismatches(all) == "T3 0.6 mm (printer: 0.4 mm)");
    // Only the tools the plate uses count.
    CHECK(nozzle_size_mismatches(sliced, { 0.4, 0.4, 0.4, 0.4 }, { 1 }).empty());
    CHECK(nozzle_size_mismatches(sliced, { 0.4, 0.4, 0.4, 0.4 }, { 0, 3, 3 }).size() == 2);
}

TEST_CASE("A Bambu report gives the mounted size per extruder and, with a rack, every size it has", "[NozzleOverride][Sync]")
{
    DualNozzleSync::PrinterState state;
    state.has_report            = true;
    state.physical_extruder_map = { 1, 0 }; // H2D / H2C: logical 0 (left) is physical 1
    auto nozzle = [](int pos, const char *d, bool normal = true) {
        DualNozzleSync::PrinterNozzle n;
        n.pos      = pos;
        n.diameter = d;
        n.normal   = normal;
        return n;
    };
    state.nozzles = { nozzle(0, "0.6"), nozzle(1, "0.4"), nozzle(0x10, "0.2"), nozzle(0x11, "0.8", false) };
    CHECK(same_sizes(reported_extruder_nozzle_sizes(state, 2), { 0.4, 0.6 }));
    const auto available = reported_available_nozzle_sizes(state, 2);
    CHECK(same_sizes(available[0], { 0.4 }));
    CHECK(same_sizes(available[1], { 0.6, 0.2 })); // the rack nozzles belong to physical 0 = logical 1 (right); the 0.8 is abnormal
    // Rack printer: a size on the rack is no mismatch, one the extruder has nowhere is.
    CHECK(rack_nozzle_size_mismatches({ 0.4, 0.2 }, available).empty());
    const auto missing = rack_nozzle_size_mismatches({ 0.4, 0.8 }, available);
    REQUIRE(missing.size() == 1);
    CHECK(missing.front().tool == 1);
    // A tool the plate does not use is not checked.
    CHECK(rack_nozzle_size_mismatches({ 0.4, 0.8 }, available, { 0 }).empty());
}

// ---- 2. Print::apply's resolution --------------------------------------------------------------------------------

TEST_CASE("Without an override nothing is resolved", "[NozzleOverride][Identity]")
{
    DynamicPrintConfig cfg = u1_04();
    REQUIRE_FALSE(has_nozzle_diameter_override(cfg));
    const DynamicPrintConfig before = cfg;
    CHECK_FALSE(apply_nozzle_diameter_override(cfg));
    CHECK_FALSE(resolve_nozzle_line_widths(cfg));
    CHECK(cfg == before);
    // An override equal to the preset changes no size and no width either.
    cfg.set_deserialize_strict({ { "nozzle_diameter_override", "0.4,0.4,0.4,0.4" } });
    CHECK_FALSE(apply_nozzle_diameter_override(cfg));
    CHECK_FALSE(resolve_nozzle_line_widths(cfg));
}

TEST_CASE("U1 T1-T4 at 0.2 / 0.4 / 0.6 / 0.8 on a 0.4 printer preset", "[NozzleOverride][Resolve]")
{
    DynamicPrintConfig cfg = u1_04();
    cfg.set_deserialize_strict({ { "nozzle_diameter_override", "0.2,0.4,0.6,0.8" } });
    REQUIRE(apply_nozzle_diameter_override(cfg));
    CHECK(same_sizes(cfg.option<ConfigOptionFloats>("nozzle_diameter")->values, { 0.2, 0.4, 0.6, 0.8 }));
    // The layer limits follow each size (the preset's 0.28 is for its 0.4).
    CHECK(same_sizes(cfg.option<ConfigOptionFloats>("max_layer_height")->values, { 0.14, 0.28, 0.42, 0.56 }));
    CHECK(same_sizes(cfg.option<ConfigOptionFloats>("min_layer_height")->values, { 0.08, 0.08, 0.08, 0.08 }));
    // Applying again changes nothing more (Print::apply runs on every apply).
    const DynamicPrintConfig once = cfg;
    CHECK_FALSE(apply_nozzle_diameter_override(cfg));
    CHECK(cfg == once);
    // Each filament reads its tool's size, and the smallest nozzle is the 0.2.
    PrintConfig pc;
    pc.apply(cfg, true);
    for (unsigned f = 1; f <= 4; ++f)
        CHECK(same_sizes(nozzle_diameter_for_filament(pc, f), 0.2 * f));
    CHECK(same_sizes(min_physical_nozzle_diameter(pc), 0.2));

    // P6: mm widths made for the 0.4 scale to each tool's size.
    REQUIRE(resolve_nozzle_line_widths(cfg));
    const auto &outer = array(cfg, "nozzle_outer_wall_line_width");
    const auto &inner = array(cfg, "nozzle_inner_wall_line_width");
    const auto &first = array(cfg, "nozzle_initial_layer_line_width");
    REQUIRE(outer.size() == 4);
    const double expect_outer[] = { 0.21, 0.42, 0.63, 0.84 };
    const double expect_inner[] = { 0.225, 0.45, 0.675, 0.9 };
    const double expect_first[] = { 0.25, 0.5, 0.75, 1.0 };
    for (size_t f = 0; f < 4; ++f) {
        INFO("filament " << f + 1);
        CHECK(same_sizes(outer[f].value, expect_outer[f]));
        CHECK(same_sizes(inner[f].value, expect_inner[f]));
        CHECK(same_sizes(first[f].value, expect_first[f]));
    }
}

TEST_CASE("One size overridden on a single-nozzle printer scales its widths", "[NozzleOverride][Resolve]")
{
    // An X1C-like printer preset for 0.4 with a 0.6 nozzle fitted in this project.
    DynamicPrintConfig cfg = u1_04();
    cfg.set_num_filaments(1);
    cfg.set_deserialize_strict({ { "nozzle_diameter", "0.4" }, { "max_layer_height", "0.28" }, { "min_layer_height", "0.08" },
                                 { "nozzle_diameter_override", "0.6" } });
    REQUIRE(apply_nozzle_diameter_override(cfg));
    REQUIRE(resolve_nozzle_line_widths(cfg));
    CHECK(same_sizes(array(cfg, "nozzle_outer_wall_line_width").front().value, 0.63));
    CHECK(same_sizes(array(cfg, "nozzle_sparse_infill_line_width").front().value, 0.675));
    // The same printer preset edited to 0.6 (no override) keeps P6's one-size rule: nothing is scaled.
    DynamicPrintConfig edited = u1_04();
    edited.set_num_filaments(1);
    edited.set_deserialize_strict({ { "nozzle_diameter", "0.6" } });
    CHECK_FALSE(resolve_nozzle_line_widths(edited));
}

TEST_CASE("Process values by size and filament values by size follow the overridden sizes", "[NozzleOverride][Resolve]")
{
    DynamicPrintConfig cfg = u1_04();
    cfg.option<ConfigOptionStrings>("process_flow_support", true)->values      = { FLOW_MODE_STANDARD };
    cfg.option<ConfigOptionEnumsGeneric>("filament_volume_type", true)->values = std::vector<int>(4, int(fvtStandard));
    // P4/P5: one process, values per nozzle size (outer wall speed 20 / 40 / 60 / 80).
    set_process_tool_slots(cfg, { 0, 0, 0, 0 }, std::vector<std::string>(4, "Direct Drive Standard"), { 0.2, 0.4, 0.6, 0.8 });
    cfg.option<ConfigOptionFloats>("outer_wall_speed", true)->values = { 20, 40, 60, 80 };
    // P4: one filament preset in all four slots with values for the 0.2 and the 0.8.
    const std::string values = "0.2: nozzle_temperature=200 | 0.8: nozzle_temperature=240";
    cfg.option<ConfigOptionInts>("nozzle_temperature", true)->values           = { 220, 220, 220, 220 };
    cfg.option<ConfigOptionStrings>("filament_nozzle_variants", true)->values = { values, values, values, values };

    const auto *speed = cfg.option<ConfigOptionFloats>("outer_wall_speed");
    // Before the override every tool is a 0.4.
    for (unsigned f = 0; f < 4; ++f)
        CHECK(get_value_at(cfg, *speed, ConfigFlowDomain::Process, f) == 40);

    cfg.set_deserialize_strict({ { "nozzle_diameter_override", "0.2,0.4,0.6,0.8" } });
    REQUIRE(apply_nozzle_diameter_override(cfg));
    speed = cfg.option<ConfigOptionFloats>("outer_wall_speed");
    CHECK(get_value_at(cfg, *speed, ConfigFlowDomain::Process, 0) == 20);
    CHECK(get_value_at(cfg, *speed, ConfigFlowDomain::Process, 1) == 40);
    CHECK(get_value_at(cfg, *speed, ConfigFlowDomain::Process, 2) == 60);
    CHECK(get_value_at(cfg, *speed, ConfigFlowDomain::Process, 3) == 80);
    resolve_filament_nozzle_variants(cfg);
    CHECK(cfg.option<ConfigOptionInts>("nozzle_temperature")->values == std::vector<int>{ 200, 220, 220, 240 });
}

TEST_CASE("H2D left and right sizes set independently", "[NozzleOverride][Resolve]")
{
    DynamicPrintConfig cfg = u1_04();
    cfg.set_num_filaments(2);
    cfg.set_deserialize_strict({ { "nozzle_diameter", "0.4,0.4" }, { "max_layer_height", "0.28,0.28" }, { "min_layer_height", "0.08,0.08" },
                                 { "nozzle_diameter_override", "0,0.6" } });
    REQUIRE(apply_nozzle_diameter_override(cfg));
    CHECK(same_sizes(cfg.option<ConfigOptionFloats>("nozzle_diameter")->values, { 0.4, 0.6 }));
    REQUIRE(resolve_nozzle_line_widths(cfg));
    CHECK(same_sizes(array(cfg, "nozzle_outer_wall_line_width")[0].value, 0.42));
    CHECK(same_sizes(array(cfg, "nozzle_outer_wall_line_width")[1].value, 0.63));
}

// ---- 3. The preset bundle: project key, per-model memory -----------------------------------------------------

TEST_CASE("The project's nozzle sizes leave the printer preset unchanged", "[NozzleOverride][PresetBundle]")
{
    PresetBundle bundle;
    DynamicPrintConfig &printer = bundle.printers.get_edited_preset().config;
    printer.set_key_value("nozzle_diameter", new ConfigOptionFloats{ 0.4, 0.4, 0.4, 0.4 });
    printer.set_key_value("single_extruder_multi_material", new ConfigOptionBool(false));
    printer.set_key_value("printer_model", new ConfigOptionString("Snapmaker U1"));

    CHECK_FALSE(bundle.has_nozzle_diameter_override());
    CHECK(same_sizes(bundle.effective_nozzle_diameters(), { 0.4, 0.4, 0.4, 0.4 }));
    CHECK(bundle.set_tool_nozzle_diameter(0, 0.2));
    CHECK(bundle.set_tool_nozzle_diameter(2, 0.6));
    CHECK(bundle.set_tool_nozzle_diameter(3, 0.8));
    CHECK_FALSE(bundle.set_tool_nozzle_diameter(3, 0.8)); // no change
    CHECK(bundle.has_nozzle_diameter_override());
    CHECK(same_sizes(bundle.effective_nozzle_diameters(), { 0.2, 0.4, 0.6, 0.8 }));
    CHECK(same_sizes(bundle.preset_nozzle_diameters(), { 0.4, 0.4, 0.4, 0.4 })); // the preset is untouched
    // The Tabs list sizes from the printer config with the project's sizes.
    CHECK(same_sizes(printer_nozzle_sizes(bundle.printer_config_with_project_sizes()), { 0.2, 0.4, 0.6, 0.8 }));
    // The slice gets them as full_config() builds it (printer, then the project keys) + Print::apply.
    DynamicPrintConfig full = printer;
    full.apply(bundle.project_config);
    CHECK(same_sizes(full.option<ConfigOptionFloats>("nozzle_diameter")->values, { 0.4, 0.4, 0.4, 0.4 }));
    CHECK(same_sizes(effective_nozzle_diameters(full), { 0.2, 0.4, 0.6, 0.8 }));

    // Remembered per printer model; another model brings its own (none), coming back restores them.
    AppConfig app;
    bundle.remember_nozzle_diameter_override(app);
    CHECK(app.get("nozzle_diameter_override", "Snapmaker U1") == "0.2,0.4,0.6,0.8");
    bundle.update_nozzle_diameter_override_for_printer(app); // the model it was set for: kept
    CHECK(same_sizes(bundle.effective_nozzle_diameters(), { 0.2, 0.4, 0.6, 0.8 }));
    printer.set_key_value("printer_model", new ConfigOptionString("Other printer"));
    bundle.update_nozzle_diameter_override_for_printer(app);
    CHECK_FALSE(bundle.has_nozzle_diameter_override());
    printer.set_key_value("printer_model", new ConfigOptionString("Snapmaker U1"));
    bundle.update_nozzle_diameter_override_for_printer(app);
    CHECK(same_sizes(bundle.effective_nozzle_diameters(), { 0.2, 0.4, 0.6, 0.8 }));

    // Every tool back to the preset's size: the override is unused again.
    for (size_t t = 0; t < 4; ++t)
        bundle.set_tool_nozzle_diameter(t, 0.);
    CHECK_FALSE(bundle.has_nozzle_diameter_override());
    CHECK(bundle.project_config.option<ConfigOptionFloats>(NOZZLE_DIAMETER_OVERRIDE_KEY)->values == std::vector<double>{ 0. });
}

// ---- 4. 3MF ----------------------------------------------------------------------------------------------------

namespace {

bool override_round_trip(DynamicPrintConfig store_config, DynamicPrintConfig &dst_config)
{
    const boost::filesystem::path dir = boost::filesystem::temp_directory_path() / ("snorca_tests_nozzle_override_" + std::to_string(get_current_pid()));
    boost::filesystem::create_directories(dir);
    Slic3r::set_temporary_dir(dir.string());

    Model        src_model;
    ModelObject *object = src_model.add_object();
    object->name        = "cube";
    object->add_volume(make_cube(10., 10., 10.))->name = "cube";
    object->add_instance();
    object->ensure_on_bed();

    const std::string file = (dir / "nozzle_override_project.3mf").string();
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

TEST_CASE("The project's nozzle sizes save and reopen with the 3MF; the preset's sizes stay the preset's", "[NozzleOverride][3mf]")
{
    DynamicPrintConfig cfg = u1_04();
    cfg.set_deserialize_strict({ { "nozzle_diameter_override", "0.2,0.4,0.6,0.8" } });
    DynamicPrintConfig loaded;
    REQUIRE(override_round_trip(cfg, loaded));
    REQUIRE(loaded.has(NOZZLE_DIAMETER_OVERRIDE_KEY));
    CHECK(same_sizes(nozzle_diameter_override_values(loaded), { 0.2, 0.4, 0.6, 0.8 }));
    CHECK(same_sizes(loaded.option<ConfigOptionFloats>("nozzle_diameter")->values, { 0.4, 0.4, 0.4, 0.4 }));
    CHECK(same_sizes(effective_nozzle_diameters(loaded), { 0.2, 0.4, 0.6, 0.8 }));

    // A project without the key reopens as "the preset's sizes".
    DynamicPrintConfig plain = u1_04();
    DynamicPrintConfig plain_loaded;
    REQUIRE(override_round_trip(plain, plain_loaded));
    CHECK_FALSE(has_nozzle_diameter_override(plain_loaded));
}
