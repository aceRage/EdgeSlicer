// Per-nozzle process P8 (NozzleProcess.hpp "P8", owner design 2026-11-01): every tool has a whole process profile of its
// own. Which keys are plate-wide and which are the tool's, the stored per-tool changes, the automatic choice of a process
// for a tool's nozzle size, the region settings stamped per role filament, the Process tab's tool view in the preset
// bundle, the validation messages that name the tool, and an older project's per-nozzle width table.
// The G-code side is in tests/fff_print/test_nozzle_tool_process_gcode.cpp.

#include <catch2/catch.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/NozzleOverride.hpp"
#include "libslic3r/NozzleProcess.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {

bool contains(const std::vector<std::string> &keys, const std::string &key) { return std::find(keys.begin(), keys.end(), key) != keys.end(); }

// The shipped profiles of one vendor, loaded once (a scratch data dir, never the user's).
PresetBundle &vendor_bundle(const std::string &vendor)
{
    static std::map<std::string, std::unique_ptr<PresetBundle>> bundles;
    static std::unique_ptr<PresetBundle>                        library;
    auto it = bundles.find(vendor);
    if (it != bundles.end())
        return *it->second;
    const std::string saved_data_dir = data_dir();
    const boost::filesystem::path scratch = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("nozzle_tool_process_%%%%-%%%%");
    boost::filesystem::create_directories(scratch);
    set_data_dir(scratch.string());
    const std::string profiles = (boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
    if (!library) {
        library = std::make_unique<PresetBundle>();
        library->load_vendor_configs_from_json(profiles, PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem,
                                               ForwardCompatibilitySubstitutionRule::EnableSilent);
    }
    auto bundle = std::make_unique<PresetBundle>();
    bundle->load_vendor_configs_from_json(profiles, vendor, PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent,
                                          library.get());
    set_data_dir(saved_data_dir);
    return *(bundles[vendor] = std::move(bundle));
}

// The U1 0.4 printer and its 0.20 process, no per-tool choices, the given project sizes.
PresetBundle &u1_bundle(const std::vector<double> &sizes)
{
    PresetBundle &b = vendor_bundle("Snapmaker");
    b.end_nozzle_tool_view();
    REQUIRE(b.printers.select_preset_by_name("Snapmaker U1 (0.4 nozzle)", true));
    REQUIRE(b.prints.select_preset_by_name("0.20mm Standard @Snapmaker U1 (0.4 nozzle)", true));
    b.update_compatible(PresetSelectCompatibleType::Never);
    if (b.filament_presets.size() != 4)
        b.set_num_filaments(4);
    b.project_config.option<ConfigOptionStrings>("nozzle_process_presets", true)->values.clear();
    b.project_config.option<ConfigOptionStrings>("nozzle_process_edits", true)->values.clear();
    b.project_config.option<ConfigOptionInts>("filament_nozzle_assignment", true)->values.clear();
    b.set_nozzle_diameter_override(sizes);
    return b;
}

using Kind = PresetBundle::NozzleToolProcessKind;

} // namespace

// ---- 1. Plate-wide and per-tool keys ---------------------------------------------------------------------------

TEST_CASE("Plate-wide keys and the keys a tool's process supplies do not overlap", "[NozzleToolProcess][Keys]")
{
    for (const char *key : { "layer_height", "initial_layer_print_height", "print_sequence", "enable_prime_tower", "enable_support",
                             "support_type", "brim_type", "brim_width", "skirt_loops", "seam_position", "wall_filament",
                             "sparse_infill_filament", "slow_down_layers", "spiral_mode" }) {
        INFO(key);
        CHECK(is_plate_process_option(key));
        CHECK_FALSE(is_per_tool_process_option(key));
    }
    for (const char *key : { "outer_wall_speed", "sparse_infill_speed", "default_acceleration", "outer_wall_line_width", "line_width",
                             "initial_layer_line_width", "wall_loops", "sparse_infill_density", "sparse_infill_pattern", "top_shell_layers",
                             "bottom_shell_layers", "ironing_type", "top_solid_infill_flow_ratio", "fuzzy_skin", "infill_wall_overlap" }) {
        INFO(key);
        CHECK(is_per_tool_process_option(key));
        CHECK_FALSE(is_plate_process_option(key));
    }
    // A profile's own description is neither.
    for (const char *key : { "inherits", "compatible_printers", "print_extruder_id", "process_extruder_values" }) {
        INFO(key);
        CHECK_FALSE(is_plate_process_option(key));
        CHECK_FALSE(is_per_tool_process_option(key));
    }
    const std::vector<std::string> plate = plate_process_option_keys();
    CHECK(contains(plate, "layer_height"));
    for (const std::string &key : plate)
        CHECK_FALSE(is_per_tool_process_option(key));
    // The region keys never include the role routing or the internal per-filament arrays.
    for (const std::string &key : nozzle_region_option_keys()) {
        INFO(key);
        CHECK(key.find("_filament") == std::string::npos);
        CHECK(key.rfind("nozzle_", 0) != 0);
    }
}

TEST_CASE("Tool ids are generic and round-trip", "[NozzleToolProcess][Keys]")
{
    CHECK(nozzle_tool_id(0) == "T1");
    CHECK(nozzle_tool_id(3) == "T4");
    CHECK(nozzle_tool_id_extruder("T1") == 0);
    CHECK(nozzle_tool_id_extruder("T12") == 11);
    CHECK(nozzle_tool_id_extruder("R3") == -1); // a later rack slot id is not an extruder
    CHECK(nozzle_tool_id_extruder("T") == -1);
    CHECK(nozzle_tool_id_extruder("T0") == -1);
}

// ---- 2. The tools' stored changes ------------------------------------------------------------------------------

TEST_CASE("Per-tool changes serialize per tool id and keep only per-tool keys", "[NozzleToolProcess][Edits]")
{
    NozzleProcessEdits edits;
    edits["T2"].set_key_value("wall_loops", new ConfigOptionInt(5));
    edits["T2"].set_key_value("outer_wall_speed", new ConfigOptionFloats({ 33, 44 }));
    edits["T4"].set_key_value("sparse_infill_density", new ConfigOptionPercent(35));
    const std::vector<std::string> stored = serialize_nozzle_process_edits(edits);
    REQUIRE(stored.size() == 2);
    CHECK(stored[0].rfind("T2\n", 0) == 0);

    const NozzleProcessEdits back = parse_nozzle_process_edits(stored);
    REQUIRE(back.size() == 2);
    CHECK(back.at("T2").opt_int("wall_loops") == 5);
    CHECK(back.at("T2").option<ConfigOptionFloats>("outer_wall_speed")->values == std::vector<double>{ 33, 44 });
    CHECK(back.at("T4").option<ConfigOptionPercent>("sparse_infill_density")->value == 35);

    // Plate-wide keys, unknown keys and garbage lines are dropped; an id with nothing left is dropped.
    const NozzleProcessEdits filtered = parse_nozzle_process_edits(
        { "T1\nlayer_height = 0.3\nnot_a_key = 1\ngarbage\nwall_loops = 3", "T3\nlayer_height = 0.1", "" });
    REQUIRE(filtered.size() == 1);
    CHECK(filtered.at("T1").keys() == std::vector<std::string>{ "wall_loops" });
}

TEST_CASE("A tool's effective process: its profile, its changes, the plate's plate-wide keys", "[NozzleToolProcess][Edits]")
{
    DynamicPrintConfig plate = DynamicPrintConfig::full_print_config();
    plate.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    plate.set_key_value("wall_loops", new ConfigOptionInt(2));
    DynamicPrintConfig base = plate;
    base.set_key_value("layer_height", new ConfigOptionFloat(0.3));
    base.set_key_value("wall_loops", new ConfigOptionInt(3));
    DynamicPrintConfig edits;
    edits.set_key_value("sparse_infill_density", new ConfigOptionPercent(40));
    edits.set_key_value("layer_height", new ConfigOptionFloat(0.4)); // never applied: plate-wide

    const DynamicPrintConfig tool = nozzle_tool_process(plate, base, &edits);
    CHECK(tool.opt_float("layer_height") == Approx(0.2));
    CHECK(tool.opt_int("wall_loops") == 3);
    CHECK(tool.option<ConfigOptionPercent>("sparse_infill_density")->value == 40);

    const DynamicPrintConfig diff = nozzle_process_diff(tool, plate);
    CHECK(diff.has("wall_loops"));
    CHECK(diff.has("sparse_infill_density"));
    CHECK_FALSE(diff.has("layer_height"));
    CHECK(nozzle_process_diff(plate, plate).empty());
}

// ---- 3. The automatic choice -----------------------------------------------------------------------------------

TEST_CASE("The picked process: system first, nearest layer height, the plate's quality word, Standard", "[NozzleToolProcess][AutoPick]")
{
    CHECK(pick_nozzle_process({}, 0.2, "0.20mm Standard @X").empty());
    CHECK(pick_nozzle_process({ { "0.24mm Standard @X (0.6 nozzle)", 0.24, true }, { "0.18mm Standard @X (0.6 nozzle)", 0.18, true },
                                { "0.30mm Standard @X (0.6 nozzle)", 0.30, true } },
                              0.2, "0.20mm Standard @X (0.4 nozzle)") == "0.18mm Standard @X (0.6 nozzle)");
    CHECK(pick_nozzle_process({ { "0.40mm Standard @X (0.8 nozzle)", 0.40, true }, { "0.40mm Strength @X (0.8 nozzle)", 0.40, true } }, 0.4,
                              "0.20mm Strength @X") == "0.40mm Strength @X (0.8 nozzle)");
    CHECK(pick_nozzle_process({ { "0.40mm Strength @X (0.8 nozzle)", 0.40, true }, { "0.40mm Standard @X (0.8 nozzle)", 0.40, true } }, 0.4,
                              "0.20mm Fine @X") == "0.40mm Standard @X (0.8 nozzle)");
    CHECK(pick_nozzle_process({ { "my 0.18", 0.20, false }, { "0.24mm Standard @X", 0.24, true } }, 0.2, "") == "0.24mm Standard @X");
}

TEST_CASE("U1 0.2 / 0.4 / 0.6 / 0.8: tools the plate's 0.4 process does not fit get their size's system process", "[NozzleToolProcess][AutoPick][PresetBundle]")
{
    PresetBundle &b = u1_bundle({ 0.2, 0.4, 0.6, 0.8 });
    CHECK(b.nozzle_tool_process_info(1).kind == Kind::FollowsPlate);
    const PresetBundle::NozzleToolProcess t1 = b.nozzle_tool_process_info(0);
    const PresetBundle::NozzleToolProcess t3 = b.nozzle_tool_process_info(2);
    const PresetBundle::NozzleToolProcess t4 = b.nozzle_tool_process_info(3);
    CHECK(t1.kind == Kind::AutoPicked);
    CHECK(t1.name == "0.12mm Standard @Snapmaker U1 (0.2 nozzle)");
    CHECK(t3.kind == Kind::AutoPicked);
    CHECK(t3.name == "0.18mm Standard @Snapmaker U1 (0.6 nozzle)");
    CHECK(t4.kind == Kind::AutoPicked);
    CHECK(t4.name == "0.24mm Standard @Snapmaker U1 (0.8 nozzle)");

    const DynamicPrintConfig full = b.full_config();
    CHECK(full.option<ConfigOptionInts>("nozzle_process_missing")->values.empty());
    // The 0.6 tool prints the 0.6 process's own walls; the plate's layer height stays.
    const Preset *p06 = b.prints.find_preset("0.18mm Standard @Snapmaker U1 (0.6 nozzle)", false);
    REQUIRE(p06 != nullptr);
    CHECK(full.opt_float("layer_height") == Approx(0.2));
    CHECK(nozzle_process_value(full, "outer_wall_speed", 2, false) ==
          static_cast<const ConfigOptionVectorBase *>(p06->config.option("outer_wall_speed"))->vserialize()[0]);
    const auto *outer = full.option<ConfigOptionFloatsOrPercents>("nozzle_outer_wall_line_width");
    REQUIRE(outer != nullptr);
    REQUIRE(outer->values.size() >= 3);
    CHECK(outer->values[2].value == Approx(p06->config.option<ConfigOptionFloatOrPercent>("outer_wall_line_width")->value));

    // A size no U1 process is made for: that tool has no process, slicing will be refused naming it.
    b.set_nozzle_diameter_override({ 0.3, 0.4, 0.4, 0.4 });
    CHECK(b.nozzle_tool_process_info(0).kind == Kind::Missing);
    CHECK(b.full_config().option<ConfigOptionInts>("nozzle_process_missing")->values == std::vector<int>{ 1 });

    // A process chosen for the tool wins over the automatic choice.
    b.set_nozzle_diameter_override({ 0.4, 0.4, 0.6, 0.4 });
    REQUIRE(b.set_nozzle_process(2, "0.30mm Standard @Snapmaker U1 (0.6 nozzle)"));
    CHECK(b.nozzle_tool_process_info(2).kind == Kind::Chosen);
    CHECK(b.nozzle_tool_process_info(2).name == "0.30mm Standard @Snapmaker U1 (0.6 nozzle)");
    b.set_nozzle_process(2, "");
    b.set_nozzle_diameter_override({ 0. });
}

TEST_CASE("Every tool on the plate's size: nothing is picked and full_config is unchanged", "[NozzleToolProcess][Identity][PresetBundle]")
{
    PresetBundle &b = u1_bundle({ 0. });
    for (size_t e = 0; e < 4; ++e)
        CHECK(b.nozzle_tool_process_info(e).kind == Kind::FollowsPlate);
    const DynamicPrintConfig full = b.full_config();
    CHECK(full.option<ConfigOptionStrings>("nozzle_region_values")->values.empty());
    CHECK(full.option<ConfigOptionFloatsOrPercents>("nozzle_outer_wall_line_width")->values.empty());
    CHECK_FALSE(full.opt_bool("process_extruder_values"));
    CHECK(full.option<ConfigOptionInts>("print_extruder_id")->values.size() == 1);

    // The U1 0.4+0.6 machine's own process is made for no single size: it fits both sizes, nothing is picked.
    REQUIRE(b.printers.select_preset_by_name("Snapmaker U1 (0.4+0.6 nozzle)", true));
    REQUIRE(b.prints.select_preset_by_name("0.20mm Standard @Snapmaker U1 (0.4+0.6 nozzle)", true));
    for (size_t e = 0; e < 4; ++e)
        CHECK(b.nozzle_tool_process_info(e).kind == Kind::FollowsPlate);
}

// ---- 4. Region settings per role filament ----------------------------------------------------------------------

TEST_CASE("Region settings of a tool's process reach the regions its filaments print", "[NozzleToolProcess][Region]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_filaments(2);
    config.set_deserialize_strict({ { "nozzle_diameter", "0.4,0.4" }, { "single_extruder_multi_material", "0" }, { "wall_loops", "2" },
                                    { "sparse_infill_density", "15%" }, { "top_shell_layers", "4" } });
    DynamicPrintConfig second;
    second.set_key_value("wall_loops", new ConfigOptionInt(4));
    second.set_key_value("sparse_infill_density", new ConfigOptionPercent(40));
    second.set_key_value("top_shell_layers", new ConfigOptionInt(7));
    REQUIRE(fill_nozzle_region_values(config, { nullptr, &second }));
    const auto &entries = config.option<ConfigOptionStrings>("nozzle_region_values")->values;
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].find("wall_loops = 2") != std::string::npos);
    CHECK(entries[1].find("wall_loops = 4") != std::string::npos);

    PrintRegionConfig region;
    region.apply(config, true);
    // Everything on filament 2: the second tool's settings.
    region.wall_filament.value = region.sparse_infill_filament.value = region.solid_infill_filament.value = 2;
    PrintRegionConfig on_two = region;
    stamp_nozzle_region_values(on_two);
    CHECK(on_two.wall_loops.value == 4);
    CHECK(on_two.sparse_infill_density.value == 40);
    CHECK(on_two.top_shell_layers.value == 7);
    // Walls on filament 1, infill on filament 2: each role from its own tool.
    PrintRegionConfig split = region;
    split.wall_filament.value = 1;
    stamp_nozzle_region_values(split);
    CHECK(split.wall_loops.value == 2);
    CHECK(split.sparse_infill_density.value == 40);
    CHECK(split.top_shell_layers.value == 7);

    // No tool differs from the plate: nothing to stamp.
    DynamicPrintConfig same_config = config;
    DynamicPrintConfig same;
    same.set_key_value("wall_loops", new ConfigOptionInt(2));
    CHECK_FALSE(fill_nozzle_region_values(same_config, { nullptr, &same }));
    CHECK(same_config.option<ConfigOptionStrings>("nozzle_region_values")->values.empty());
}

// ---- 5. The Process tab's tool view ----------------------------------------------------------------------------

TEST_CASE("Showing a tool selects its profile; plate-wide edits go to the Plate, the tool's own to the project", "[NozzleToolProcess][View][PresetBundle]")
{
    PresetBundle &b = u1_bundle({ 0.4, 0.4, 0.6, 0.4 });
    const std::string plate_name = b.prints.get_selected_preset_name();
    const DynamicPrintConfig before = b.full_config();

    REQUIRE(b.begin_nozzle_tool_view(2));
    CHECK(b.nozzle_tool_view() == 2);
    CHECK(b.prints.get_selected_preset_name() == "0.18mm Standard @Snapmaker U1 (0.6 nozzle)");
    CHECK(b.plate_print_preset_name() == plate_name);
    // The tool's profile shows the Plate's layer height, and that is not a change to the profile.
    CHECK(b.prints.get_edited_preset().config.opt_float("layer_height") == Approx(0.2));
    CHECK_FALSE(b.prints.current_is_dirty());
    // Slicing still uses the plate's process as the plate.
    CHECK(b.full_config().opt_string("print_settings_id") == plate_name);
    CHECK(b.full_config().diff(before).empty());

    // A change to the tool's own setting: the tool's profile is modified, the change is kept per tool.
    b.prints.get_edited_preset().config.set_key_value("wall_loops", new ConfigOptionInt(6));
    b.nozzle_tool_view_sync();
    CHECK(b.prints.current_is_dirty());
    REQUIRE(b.nozzle_process_edits_map().count("T3") == 1);
    CHECK(b.nozzle_process_edits_map().at("T3").opt_int("wall_loops") == 6);
    // A plate-wide setting edited on the tool is the Plate's.
    b.prints.get_edited_preset().config.set_key_value("layer_height", new ConfigOptionFloat(0.16));
    b.nozzle_tool_view_plate_key_changed("layer_height");
    b.nozzle_tool_view_sync();
    CHECK(b.plate_print_preset().config.opt_float("layer_height") == Approx(0.16));

    b.end_nozzle_tool_view();
    CHECK(b.nozzle_tool_view() == -1);
    CHECK(b.prints.get_selected_preset_name() == plate_name);
    CHECK(b.prints.get_edited_preset().config.opt_float("layer_height") == Approx(0.16));
    CHECK(b.prints.current_is_dirty()); // the plate's layer height changed
    CHECK(b.prints.dirty_ignore().empty());
    // The tool's change slices: its filament's regions get 6 walls.
    const DynamicPrintConfig full = b.full_config();
    const auto &regions = full.option<ConfigOptionStrings>("nozzle_region_values")->values;
    REQUIRE(regions.size() >= 3);
    CHECK(regions[2].find("wall_loops = 6") != std::string::npos);
    CHECK(regions[0].find("wall_loops = 6") == std::string::npos);
    CHECK(b.nozzle_tool_process_info(2).edited);

    // Picking another profile for the tool drops the changes made to the old one.
    REQUIRE(b.set_nozzle_process(2, "0.30mm Standard @Snapmaker U1 (0.6 nozzle)"));
    CHECK(b.nozzle_process_edits_map().count("T3") == 0);
    b.set_nozzle_process(2, "");
    b.prints.get_edited_preset().config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    b.set_nozzle_diameter_override({ 0. });
}

TEST_CASE("A tool following the plate edits a copy of the plate's process", "[NozzleToolProcess][View][PresetBundle]")
{
    PresetBundle &b = u1_bundle({ 0. });
    const DynamicPrintConfig before = b.full_config();
    REQUIRE(b.begin_nozzle_tool_view(1));
    CHECK(b.prints.get_selected_preset_name() == b.plate_print_preset_name());
    CHECK_FALSE(b.prints.current_is_dirty());
    b.end_nozzle_tool_view();
    // Shown and left unchanged: nothing stored, the job is the plain one.
    CHECK(b.nozzle_process_edits_map().empty());
    CHECK(b.full_config().diff(before).empty());

    REQUIRE(b.begin_nozzle_tool_view(1));
    b.prints.get_edited_preset().config.set_key_value("sparse_infill_density", new ConfigOptionPercent(55));
    b.end_nozzle_tool_view();
    CHECK(b.nozzle_tool_process_info(1).kind == Kind::FollowsPlate);
    CHECK(b.nozzle_tool_process_info(1).edited);
    CHECK_FALSE(b.prints.current_is_dirty()); // the plate's process itself is unchanged
    const DynamicPrintConfig full    = b.full_config();
    const auto              &regions = full.option<ConfigOptionStrings>("nozzle_region_values")->values;
    REQUIRE(regions.size() >= 2);
    CHECK(regions[1].find("sparse_infill_density = 55%") != std::string::npos);
    b.project_config.option<ConfigOptionStrings>("nozzle_process_edits", true)->values.clear();
}

TEST_CASE("Saving a tool's profile keeps its own plate-wide values and makes it the tool's choice", "[NozzleToolProcess][View][PresetBundle]")
{
    PresetBundle &b = u1_bundle({ 0.4, 0.4, 0.6, 0.4 });
    REQUIRE(b.begin_nozzle_tool_view(2));
    const double own_layer_height = b.prints.get_selected_preset().config.opt_float("layer_height");
    b.nozzle_tool_view_before_save();
    CHECK(b.prints.get_edited_preset().config.opt_float("layer_height") == Approx(own_layer_height));
    b.nozzle_tool_view_after_save();
    CHECK(b.prints.get_edited_preset().config.opt_float("layer_height") == Approx(0.2));
    // As if saved under another name: selecting another preset in the view makes it the tool's choice.
    REQUIRE(b.prints.select_preset_by_name("0.30mm Standard @Snapmaker U1 (0.6 nozzle)", true));
    b.nozzle_tool_view_sync();
    CHECK(b.nozzle_process_selection()[2] == "0.30mm Standard @Snapmaker U1 (0.6 nozzle)");
    CHECK(b.prints.get_edited_preset().config.opt_float("layer_height") == Approx(0.2));
    b.end_nozzle_tool_view();
    b.set_nozzle_process(2, "");
    b.set_nozzle_diameter_override({ 0. });
}

// ---- 6. Validation that names the tool --------------------------------------------------------------------------

namespace {

DynamicPrintConfig four_tool_config(const std::vector<double> &nozzles)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(unsigned(nozzles.size()));
    config.set_num_filaments(unsigned(nozzles.size()));
    config.option<ConfigOptionFloats>("nozzle_diameter")->values = nozzles;
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value = false;
    config.option<ConfigOptionBool>("enable_prime_tower")->value = false;
    config.option<ConfigOptionFloats>("min_layer_height")->values = std::vector<double>(nozzles.size(), 0.04);
    config.option<ConfigOptionFloats>("max_layer_height")->values = std::vector<double>(nozzles.size(), 0.6);
    config.option<ConfigOptionFloat>("layer_height")->value = 0.15;
    config.option<ConfigOptionFloat>("initial_layer_print_height")->value = 0.25;
    for (const char *key : { "line_width", "inner_wall_line_width", "outer_wall_line_width", "sparse_infill_line_width",
                             "internal_solid_infill_line_width", "top_surface_line_width", "support_line_width", "initial_layer_line_width" }) {
        auto *opt    = config.option<ConfigOptionFloatOrPercent>(key);
        opt->value   = 100.;
        opt->percent = true;
    }
    return config;
}

std::string validate_message(const DynamicPrintConfig &config, int filament)
{
    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "cube";
    object->add_volume(make_cube(20., 20., 20.));
    object->add_instance();
    object->ensure_on_bed();
    for (const char *key : { "extruder", "wall_filament", "sparse_infill_filament", "solid_infill_filament" })
        object->config.set_key_value(key, new ConfigOptionInt(filament));
    Print print;
    print.set_status_silent();
    print.apply(model, config);
    print.apply(model, config);
    return print.validate().string;
}

} // namespace

TEST_CASE("Too high a layer for a tool's nozzle names the tool, its size and the setting", "[NozzleToolProcess][Validate]")
{
    const DynamicPrintConfig config = four_tool_config({ 0.2, 0.4, 0.6, 0.8 });
    // The first layer (0.25) on T1's 0.2 nozzle.
    CHECK(validate_message(config, 1) ==
          "T1 has a 0.2 mm nozzle; first layer height 0.25 mm is too high (max 0.2 mm). Lower it on Plate, or change T1's nozzle.");
    // The 0.4 tool is fine with it.
    CHECK(validate_message(config, 2).find("T2 has") == std::string::npos);
    // The layer height on its own.
    DynamicPrintConfig layers = config;
    layers.option<ConfigOptionFloat>("initial_layer_print_height")->value = 0.15;
    layers.option<ConfigOptionFloat>("layer_height")->value               = 0.3;
    CHECK(validate_message(layers, 1).find("T1 has a 0.2 mm nozzle; layer height 0.3 mm is too high (max 0.2 mm)") != std::string::npos);
    // One nozzle: no tool to name.
    DynamicPrintConfig single = four_tool_config({ 0.2 });
    CHECK(validate_message(single, 1) ==
          "The nozzle is 0.2 mm; first layer height 0.25 mm is too high (max 0.2 mm). Lower the first layer height, or use a larger nozzle.");
}

TEST_CASE("A tool without a process for its size is refused, naming it", "[NozzleToolProcess][Validate]")
{
    DynamicPrintConfig config = four_tool_config({ 0.4, 0.4, 0.3, 0.4 });
    config.option<ConfigOptionFloat>("initial_layer_print_height")->value = 0.2;
    config.option<ConfigOptionInts>("nozzle_process_missing", true)->values = { 3 };
    const std::string message = validate_message(config, 3);
    CHECK(message.find("T3 has a 0.3 mm nozzle") != std::string::npos);
    CHECK(message.find("Choose a process for T3") != std::string::npos);
    // A job that does not print with T3 is not refused for it.
    CHECK(validate_message(config, 1).find("T3") == std::string::npos);
}

TEST_CASE("A width error on a mixed printer names the tool and jumps to its process", "[NozzleToolProcess][Validate]")
{
    DynamicPrintConfig config = four_tool_config({ 0.4, 0.2 });
    config.option<ConfigOptionFloat>("initial_layer_print_height")->value = 0.15;
    auto *outer    = config.option<ConfigOptionFloatOrPercent>("outer_wall_line_width");
    outer->value   = 0.12;
    outer->percent = false;
    Model        model;
    ModelObject *object = model.add_object();
    object->add_volume(make_cube(20., 20., 20.));
    object->add_instance();
    object->ensure_on_bed();
    for (const char *key : { "wall_filament", "sparse_infill_filament", "solid_infill_filament" })
        object->config.set_key_value(key, new ConfigOptionInt(2));
    Print print;
    print.set_status_silent();
    print.apply(model, config);
    print.apply(model, config);
    const StringObjectException err = print.validate();
    INFO(err.string);
    CHECK(err.string.find("T2 has a 0.2 mm nozzle") != std::string::npos);
    CHECK(err.opt_key == "outer_wall_line_width@T2");
}

// ---- 7. An older project's per-nozzle width table -------------------------------------------------------------

TEST_CASE("An older project's per-nozzle width table is still read", "[NozzleToolProcess][Legacy]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_filaments(2);
    config.set_deserialize_strict({ { "nozzle_diameter", "0.4,0.6" }, { "single_extruder_multi_material", "0" }, { "printer_variant", "0.4" },
                                    { "outer_wall_line_width", "0.42" }, { "process_nozzle_line_widths", "0.6: outer_wall_line_width=0.7" } });
    REQUIRE(resolve_nozzle_line_widths(config));
    const auto &outer = config.option<ConfigOptionFloatsOrPercents>("nozzle_outer_wall_line_width")->values;
    REQUIRE(outer.size() == 2);
    CHECK(outer[0].value == Approx(0.42));
    CHECK(outer[1].value == Approx(0.7));
}
