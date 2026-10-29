// Per-nozzle process P1 (tests/design_per_nozzle_process.md, owner decisions 2026-10-29): the
// per-extruder process slot resolver, the per-nozzle process fill, the PresetBundle selection on real
// U1 / H2D profiles, and the project round trip.

#include <catch2/catch.hpp>

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/NozzleProcess.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {

const std::vector<int>         H2D_IDS      = { 1, 1, 1, 2, 2, 2, 2 };
const std::vector<std::string> H2D_VARIANTS = { "Direct Drive Standard", "Direct Drive High Flow", "Direct Drive E3D High Flow",
                                                "Direct Drive Standard", "Direct Drive High Flow", "Direct Drive TPU High Flow",
                                                "Direct Drive E3D High Flow" };

// Two H2D filaments over the 7-slot process layout of the BBL dual processes.
DynamicPrintConfig h2d_like(const std::vector<int> &filament_map, const std::vector<int> &volume_types, bool flag)
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_num_filaments(2);
    cfg.option<ConfigOptionFloats>("nozzle_diameter", true)->values                 = { 0.4, 0.4 };
    cfg.option<ConfigOptionBool>("single_extruder_multi_material", true)->value     = false;
    cfg.option<ConfigOptionStrings>("extruder_variant_list", true)->values          = { "Direct Drive Standard,Direct Drive High Flow",
                                                                                        "Direct Drive Standard,Direct Drive High Flow" };
    cfg.option<ConfigOptionInts>("print_extruder_id", true)->values                 = H2D_IDS;
    cfg.option<ConfigOptionStrings>("print_extruder_variant", true)->values         = H2D_VARIANTS;
    cfg.option<ConfigOptionStrings>("process_flow_support", true)->values           = { FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW };
    cfg.option<ConfigOptionFloats>("outer_wall_speed", true)->values                = { 200, 500, 510, 201, 501, 502, 503 };
    cfg.option<ConfigOptionInts>("filament_map", true)->values                      = filament_map;
    cfg.option<ConfigOptionEnumsGeneric>("filament_volume_type", true)->values      = volume_types;
    cfg.option<ConfigOptionBool>("process_extruder_values", true)->value            = flag;
    return cfg;
}

const int STD = int(fvtStandard);
const int HF  = int(fvtHighFlow);

} // namespace

TEST_CASE("Process slots stay the flow-mode columns until the extruder slots differ", "[NozzleProcess][Resolver]")
{
    const DynamicPrintConfig cfg = h2d_like({ 1, 2 }, { STD, STD }, false);
    CHECK(process_config_idx(cfg, 7, 0) == 0);
    CHECK(process_config_idx(cfg, 7, 1) == 0);
    CHECK(get_value_at(cfg, *cfg.option<ConfigOptionFloats>("outer_wall_speed"), ConfigFlowDomain::Process, 1) == 200.);
    const ResolvedFilamentFlow cache = ResolvedFilamentFlow::resolve(cfg);
    CHECK(cache.process_layout_size == 0);
    CHECK(cache.process_config_idx_for(cfg, 7, 1) == 0);
}

TEST_CASE("A filament reads its own extruder's slot when the process carries per-extruder values", "[NozzleProcess][Resolver]")
{
    SECTION("left Standard, right Standard")
    {
        const DynamicPrintConfig cfg = h2d_like({ 1, 2 }, { STD, STD }, true);
        CHECK(process_config_idx(cfg, 7, 0) == 0);
        CHECK(process_config_idx(cfg, 7, 1) == 3);
        CHECK(get_value_at(cfg, *cfg.option<ConfigOptionFloats>("outer_wall_speed"), ConfigFlowDomain::Process, 1) == 201.);
        // A flow-mode vector (a user preset that saved two values, a per-object override) keeps its column.
        CHECK(process_config_idx(cfg, 2, 1) == 0);
        const ResolvedFilamentFlow cache = ResolvedFilamentFlow::resolve(cfg);
        CHECK(cache.process_layout_size == 7);
        CHECK(cache.process_config_idx_for(cfg, 7, 0) == 0);
        CHECK(cache.process_config_idx_for(cfg, 7, 1) == 3);
        CHECK(cache.process_config_idx_for(cfg, 2, 1) == 0);
    }
    SECTION("High Flow filaments")
    {
        const DynamicPrintConfig cfg = h2d_like({ 1, 2 }, { HF, HF }, true);
        CHECK(process_config_idx(cfg, 7, 0) == 1);
        CHECK(process_config_idx(cfg, 7, 1) == 4);
    }
    SECTION("filament_map swaps the sides")
    {
        const DynamicPrintConfig cfg = h2d_like({ 2, 1 }, { STD, HF }, true);
        CHECK(process_config_idx(cfg, 7, 0) == 3);
        CHECK(process_config_idx(cfg, 7, 1) == 1);
    }
    SECTION("both filaments on the right nozzle")
    {
        const DynamicPrintConfig cfg = h2d_like({ 2, 2 }, { STD, STD }, true);
        CHECK(process_config_idx(cfg, 7, 0) == 3);
        CHECK(process_config_idx(cfg, 7, 1) == 3);
    }
}

TEST_CASE("The resolver follows the H2C / H2D Pro and X2D slot layouts", "[NozzleProcess][Resolver]")
{
    SECTION("five slots, extruder 2 with TPU High Flow")
    {
        DynamicPrintConfig cfg = h2d_like({ 1, 2 }, { STD, HF }, true);
        cfg.option<ConfigOptionInts>("print_extruder_id")->values = { 1, 1, 2, 2, 2 };
        cfg.option<ConfigOptionStrings>("print_extruder_variant")->values = { "Direct Drive Standard", "Direct Drive High Flow",
                                                                              "Direct Drive Standard", "Direct Drive High Flow",
                                                                              "Direct Drive TPU High Flow" };
        CHECK(process_config_idx(cfg, 5, 0) == 0);
        CHECK(process_config_idx(cfg, 5, 1) == 3);
    }
    SECTION("four slots")
    {
        DynamicPrintConfig cfg = h2d_like({ 1, 2 }, { HF, STD }, true);
        cfg.option<ConfigOptionInts>("print_extruder_id")->values = { 1, 1, 2, 2 };
        cfg.option<ConfigOptionStrings>("print_extruder_variant")->values = { "Direct Drive Standard", "Direct Drive High Flow",
                                                                              "Direct Drive Standard", "Direct Drive High Flow" };
        CHECK(process_config_idx(cfg, 4, 0) == 1);
        CHECK(process_config_idx(cfg, 4, 1) == 2);
    }
    SECTION("the X2D's Bowden second extruder")
    {
        DynamicPrintConfig cfg = h2d_like({ 1, 2 }, { STD, HF }, true);
        cfg.option<ConfigOptionInts>("print_extruder_id")->values = { 1, 1, 1, 2, 2, 2 };
        cfg.option<ConfigOptionStrings>("print_extruder_variant")->values = { "Direct Drive Standard", "Direct Drive High Flow",
                                                                              "Direct Drive E3D High Flow", "Bowden Standard",
                                                                              "Bowden High Flow", "Bowden E3D High Flow" };
        CHECK(process_config_idx(cfg, 6, 1) == 4);
    }
    SECTION("a single extruder with three variants (H2S) is no per-extruder layout")
    {
        DynamicPrintConfig cfg = h2d_like({ 1, 1 }, { STD, STD }, true);
        cfg.option<ConfigOptionInts>("print_extruder_id")->values = { 1, 1, 1 };
        cfg.option<ConfigOptionStrings>("print_extruder_variant")->values = { "Direct Drive Standard", "Direct Drive High Flow",
                                                                              "Direct Drive E3D High Flow" };
        CHECK(process_extruder_variant_slots(cfg).empty());
        CHECK(process_config_idx(cfg, 3, 1) == 0);
    }
}

TEST_CASE("A toolchanger's filament prints on its own tool whatever filament_map says", "[NozzleProcess][Resolver]")
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_num_filaments(4);
    cfg.option<ConfigOptionFloats>("nozzle_diameter", true)->values = { 0.4, 0.4, 0.6, 0.6 };
    cfg.option<ConfigOptionInts>("filament_map", true)->values      = { 1, 1, 1, 1 };
    for (unsigned int f = 0; f < 4; ++f)
        CHECK(process_extruder_for_filament(cfg, f, 4) == f);
}

TEST_CASE("Only Standard / High Flow slots that differ from extruder 1 mark per-extruder values", "[NozzleProcess][Resolver]")
{
    DynamicPrintConfig cfg = h2d_like({ 1, 2 }, { STD, STD }, false);
    cfg.option<ConfigOptionFloats>("outer_wall_speed")->values = { 200, 500, 510, 200, 500, 777, 888 };
    CHECK_FALSE(process_extruder_slots_differ(cfg)); // only the TPU / E3D slots differ: never read
    cfg.option<ConfigOptionFloats>("outer_wall_speed")->values = { 200, 500, 510, 150, 500, 500, 500 };
    CHECK(process_extruder_slots_differ(cfg));
    // A flow-mode vector says nothing about extruders.
    cfg.option<ConfigOptionFloats>("outer_wall_speed")->values = { 200, 500 };
    CHECK_FALSE(process_extruder_slots_differ(cfg));
    // slow_down_layers is read once for the print: not a per-nozzle key.
    cfg.option<ConfigOptionInts>("slow_down_layers", true)->values = { 1, 1, 1, 5, 5, 5, 5 };
    CHECK_FALSE(process_extruder_slots_differ(cfg));
    CHECK_FALSE(is_per_nozzle_process_option("slow_down_layers"));
    CHECK(is_per_nozzle_process_option("outer_wall_speed"));
    CHECK(per_nozzle_process_options().size() + 4 == process_flow_variant_options().size());
}

TEST_CASE("Variant names map to flow modes, with TPU / E3D High Flow per D6", "[NozzleProcess][Resolver]")
{
    CHECK(process_variant_flow_mode("Direct Drive Standard") == STD);
    CHECK(process_variant_flow_mode("Bowden High Flow") == HF);
    CHECK(process_variant_flow_mode("Direct Drive E3D High Flow") == -1);
    CHECK(process_variant_fill_mode("Direct Drive TPU High Flow") == fvtStandard);
    CHECK(process_variant_fill_mode("Direct Drive E3D High Flow") == fvtHighFlow);
    CHECK(process_variant_fill_mode("Direct Drive TPU285") == fvtStandard);
}

namespace {

// A U1 0.4+0.6: four tools, a Standard / High Flow process with no extruder layout.
DynamicPrintConfig u1_mixed()
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_num_filaments(4);
    cfg.option<ConfigOptionFloats>("nozzle_diameter", true)->values             = { 0.4, 0.4, 0.6, 0.6 };
    cfg.option<ConfigOptionBool>("single_extruder_multi_material", true)->value = false;
    cfg.option<ConfigOptionStrings>("process_flow_support", true)->values       = { FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW };
    cfg.option<ConfigOptionFloats>("outer_wall_speed", true)->values            = { 200, 300 };
    cfg.option<ConfigOptionFloats>("travel_speed", true)->values                = { 500, 500 };
    cfg.option<ConfigOptionInts>("slow_down_layers", true)->values              = { 3, 3 };
    return cfg;
}

DynamicPrintConfig u1_06_process()
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("process_flow_support", new ConfigOptionStrings({ FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW }));
    cfg.set_key_value("outer_wall_speed", new ConfigOptionFloats({ 120, 180 }));
    cfg.set_key_value("travel_speed", new ConfigOptionFloats({ 400, 450 }));
    cfg.set_key_value("slow_down_layers", new ConfigOptionInts({ 9, 9 }));
    return cfg;
}

} // namespace

TEST_CASE("A U1 0.4+0.6 takes the 0.6 process's speeds on its 0.6 tools", "[NozzleProcess][Fill]")
{
    DynamicPrintConfig       cfg = u1_mixed();
    const DynamicPrintConfig p06 = u1_06_process();
    const std::vector<const DynamicPrintConfig *> sources = { nullptr, nullptr, &p06, &p06 };
    REQUIRE(fill_nozzle_process_slots(cfg, sources));

    CHECK(cfg.option<ConfigOptionInts>("print_extruder_id")->values == std::vector<int>{ 1, 1, 2, 2, 3, 3, 4, 4 });
    CHECK(cfg.option<ConfigOptionStrings>("print_extruder_variant")->values.front() == "Direct Drive Standard");
    CHECK(cfg.option<ConfigOptionStrings>("print_extruder_variant")->values[1] == "Direct Drive High Flow");
    CHECK(cfg.option<ConfigOptionFloats>("outer_wall_speed")->values == std::vector<double>{ 200, 300, 200, 300, 120, 180, 120, 180 });
    CHECK(cfg.option<ConfigOptionFloats>("travel_speed")->values == std::vector<double>{ 500, 500, 500, 500, 400, 450, 400, 450 });
    // Read once for the whole print: stays the plate's.
    CHECK(cfg.option<ConfigOptionInts>("slow_down_layers")->values == std::vector<int>{ 3, 3 });
    CHECK(nozzle_process_value(cfg, "outer_wall_speed", 2, false) == "120");
    CHECK(nozzle_process_value(cfg, "outer_wall_speed", 1, true) == "300");
    CHECK(process_extruder_slots_differ(cfg));
    CHECK(cfg.opt_bool("process_extruder_values"));

    // What a filament on T3 reads once Print::apply sets the flag.
    cfg.option<ConfigOptionBool>("process_extruder_values", true)->value = true;
    cfg.option<ConfigOptionEnumsGeneric>("filament_volume_type", true)->values = { STD, STD, HF, STD };
    const auto &outer = *cfg.option<ConfigOptionFloats>("outer_wall_speed");
    CHECK(get_value_at(cfg, outer, ConfigFlowDomain::Process, 0) == 200.);
    CHECK(get_value_at(cfg, outer, ConfigFlowDomain::Process, 2) == 180.);
    CHECK(get_value_at(cfg, outer, ConfigFlowDomain::Process, 3) == 120.);

    SECTION("filling again changes nothing")
    {
        DynamicPrintConfig again = cfg;
        REQUIRE(fill_nozzle_process_slots(again, sources));
        CHECK(again.diff(cfg).empty());
    }
}

TEST_CASE("A per-nozzle process fills only that nozzle's slots of an H2D process", "[NozzleProcess][Fill]")
{
    DynamicPrintConfig cfg = h2d_like({ 1, 2 }, { STD, STD }, false);
    DynamicPrintConfig right;
    right.set_key_value("print_extruder_id", new ConfigOptionInts(H2D_IDS));
    right.set_key_value("print_extruder_variant", new ConfigOptionStrings(H2D_VARIANTS));
    right.set_key_value("process_flow_support", new ConfigOptionStrings({ FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW }));
    right.set_key_value("outer_wall_speed", new ConfigOptionFloats({ 90, 91, 92, 150, 151, 152, 153 }));
    REQUIRE(fill_nozzle_process_slots(cfg, { nullptr, &right }));
    // Left untouched; right Standard / High Flow from the right process's own right slots; TPU High Flow
    // from its Standard slot, E3D High Flow from its High Flow slot (D6).
    CHECK(cfg.option<ConfigOptionFloats>("outer_wall_speed")->values == std::vector<double>{ 200, 500, 510, 150, 151, 150, 151 });
    CHECK(cfg.option<ConfigOptionInts>("print_extruder_id")->values == H2D_IDS);

    SECTION("a process with only flow-mode columns")
    {
        DynamicPrintConfig plate = h2d_like({ 1, 2 }, { STD, STD }, false);
        DynamicPrintConfig flat;
        flat.set_key_value("process_flow_support", new ConfigOptionStrings({ FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW }));
        flat.set_key_value("outer_wall_speed", new ConfigOptionFloats({ 60, 70 }));
        REQUIRE(fill_nozzle_process_slots(plate, { nullptr, &flat }));
        CHECK(plate.option<ConfigOptionFloats>("outer_wall_speed")->values == std::vector<double>{ 200, 500, 510, 60, 70, 60, 70 });
    }
    SECTION("a plate vector a user preset cut to two values is spread over the layout first")
    {
        DynamicPrintConfig plate = h2d_like({ 1, 2 }, { STD, STD }, false);
        plate.option<ConfigOptionFloats>("outer_wall_speed")->values = { 33, 44 };
        DynamicPrintConfig flat;
        flat.set_key_value("outer_wall_speed", new ConfigOptionFloats({ 60 }));
        REQUIRE(fill_nozzle_process_slots(plate, { nullptr, &flat }));
        CHECK(plate.option<ConfigOptionFloats>("outer_wall_speed")->values == std::vector<double>{ 33, 44, 44, 60, 60, 60, 60 });
    }
}

TEST_CASE("A nozzle without its own process keeps printing what it printed before", "[NozzleProcess][Fill]")
{
    // The X2D's system processes hold different Bowden slots that nothing ever read; an older user preset
    // edited only the first column. Neither may start printing differently because the other nozzle got a
    // process of its own.
    DynamicPrintConfig cfg = h2d_like({ 1, 2 }, { STD, STD }, false);
    cfg.option<ConfigOptionInts>("print_extruder_id")->values = { 1, 1, 1, 2, 2, 2 };
    cfg.option<ConfigOptionStrings>("print_extruder_variant")->values = { "Direct Drive Standard", "Direct Drive High Flow",
                                                                          "Direct Drive E3D High Flow", "Bowden Standard",
                                                                          "Bowden High Flow", "Bowden E3D High Flow" };
    cfg.option<ConfigOptionFloats>("outer_wall_speed")->values = { 60, 61, 62, 50, 51, 52 };
    DynamicPrintConfig left;
    left.set_key_value("outer_wall_speed", new ConfigOptionFloats({ 40 }));
    REQUIRE(fill_nozzle_process_slots(cfg, { &left, nullptr }));
    CHECK(cfg.option<ConfigOptionFloats>("outer_wall_speed")->values == std::vector<double>{ 40, 40, 40, 60, 61, 61 });
    CHECK(cfg.opt_bool("process_extruder_values"));

    SECTION("unless the process was split per nozzle")
    {
        DynamicPrintConfig split = h2d_like({ 1, 2 }, { STD, STD }, true);
        split.option<ConfigOptionFloats>("outer_wall_speed")->values = { 200, 500, 510, 150, 450, 502, 503 };
        DynamicPrintConfig left7;
        left7.set_key_value("outer_wall_speed", new ConfigOptionFloats({ 40 }));
        REQUIRE(fill_nozzle_process_slots(split, { &left7, nullptr }));
        CHECK(split.option<ConfigOptionFloats>("outer_wall_speed")->values == std::vector<double>{ 40, 40, 40, 150, 450, 502, 503 });
    }
}

TEST_CASE("No per-nozzle process, or one nozzle, leaves the config alone", "[NozzleProcess][Fill]")
{
    DynamicPrintConfig       cfg    = u1_mixed();
    const DynamicPrintConfig before = cfg;
    CHECK_FALSE(fill_nozzle_process_slots(cfg, {}));
    CHECK_FALSE(fill_nozzle_process_slots(cfg, { nullptr, nullptr, nullptr, nullptr }));
    CHECK(cfg.diff(before).empty());

    DynamicPrintConfig single = DynamicPrintConfig::full_print_config();
    const DynamicPrintConfig p06 = u1_06_process();
    CHECK_FALSE(fill_nozzle_process_slots(single, { &p06 }));

    CHECK_FALSE(has_nozzle_process_presets(cfg));
    cfg.option<ConfigOptionStrings>("nozzle_process_presets", true)->values = { "", "" };
    CHECK_FALSE(has_nozzle_process_presets(cfg));
    cfg.option<ConfigOptionStrings>("nozzle_process_presets", true)->values = { "", "", "P" };
    CHECK(nozzle_process_preset_names(cfg) == std::vector<std::string>{ "P" });
}

// ---- Projects -----------------------------------------------------------------------------------

namespace {

boost::filesystem::path nozzle_process_tmp_dir()
{
    const boost::filesystem::path dir = boost::filesystem::temp_directory_path() / ("snorca_tests_nozzle_process_" + std::to_string(get_current_pid()));
    boost::filesystem::create_directories(dir);
    Slic3r::set_temporary_dir(dir.string());
    return dir;
}

bool round_trip_project(DynamicPrintConfig store_config, DynamicPrintConfig &dst_config)
{
    Model        src_model;
    ModelObject *object = src_model.add_object();
    object->name        = "cube";
    object->add_volume(make_cube(10., 10., 10.))->name = "cube";
    object->add_instance();
    object->ensure_on_bed();

    const std::string file = (nozzle_process_tmp_dir() / "nozzle_process_project.3mf").string();
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

TEST_CASE("A project with per-nozzle processes saves and reopens with its selection and values", "[NozzleProcess][3mf]")
{
    DynamicPrintConfig cfg = u1_mixed();
    const DynamicPrintConfig p06 = u1_06_process();
    cfg.option<ConfigOptionStrings>("nozzle_process_presets", true)->values = { "", "", "0.30mm Standard @Snapmaker U1 (0.6 nozzle)",
                                                                                 "0.30mm Standard @Snapmaker U1 (0.6 nozzle)" };
    REQUIRE(fill_nozzle_process_slots(cfg, { nullptr, nullptr, &p06, &p06 }));

    DynamicPrintConfig loaded;
    REQUIRE(round_trip_project(cfg, loaded));
    for (const char *key : { "nozzle_process_presets", "print_extruder_id", "print_extruder_variant", "outer_wall_speed", "travel_speed" }) {
        INFO("option " << key);
        REQUIRE(loaded.has(key));
        CHECK(*loaded.option(key) == *cfg.option(key));
    }
    // The CLI fills a loaded project again by name; with the same process that changes nothing.
    DynamicPrintConfig again = loaded;
    REQUIRE(fill_nozzle_process_slots(again, { nullptr, nullptr, &p06, &p06 }));
    CHECK(*again.option("outer_wall_speed") == *loaded.option("outer_wall_speed"));
}

TEST_CASE("A project saved before per-nozzle processes reopens unchanged", "[NozzleProcess][3mf]")
{
    DynamicPrintConfig cfg = u1_mixed();
    cfg.erase("nozzle_process_presets");
    cfg.erase("process_extruder_values");
    DynamicPrintConfig loaded;
    REQUIRE(round_trip_project(cfg, loaded));
    CHECK_FALSE(has_nozzle_process_presets(loaded));
    CHECK(*loaded.option("outer_wall_speed") == *cfg.option("outer_wall_speed"));
    // Nothing chosen: the fill leaves it alone, so it slices as before.
    DynamicPrintConfig filled = loaded;
    CHECK_FALSE(fill_nozzle_process_slots(filled, { nullptr, nullptr, nullptr, nullptr }));
    CHECK(filled.diff(loaded).empty());
}

TEST_CASE("A config with a key this build does not know loads without it", "[NozzleProcess][3mf]")
{
    // What an older EdgeSlicer meets in a newer project: unknown keys are dropped, the rest loads.
    const std::string file = (nozzle_process_tmp_dir() / "future_keys.json").string();
    {
        boost::nowide::ofstream os(file);
        os << "{\n  \"name\": \"future\",\n  \"from\": \"project\",\n  \"version\": \"9.9.9.9\",\n"
              "  \"nozzle_process_presets_from_the_future\": [\"\", \"x\"],\n"
              "  \"outer_wall_speed\": [\"77\", \"88\"]\n}\n";
    }
    DynamicPrintConfig                 cfg;
    ConfigSubstitutionContext          ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
    std::map<std::string, std::string> key_values;
    std::string                        reason;
    CHECK(cfg.load_from_json(file, ctxt, false, key_values, reason) == 0);
    CHECK_FALSE(cfg.has("nozzle_process_presets_from_the_future"));
    CHECK(cfg.option<ConfigOptionFloats>("outer_wall_speed")->values == std::vector<double>{ 77, 88 });
    boost::filesystem::remove(file);
}

// ---- PresetBundle on the shipped profiles -------------------------------------------------------

namespace {

PresetBundle &vendor_bundle(const std::string &vendor)
{
    static std::map<std::string, std::unique_ptr<PresetBundle>> bundles;
    static std::unique_ptr<PresetBundle>                        library;
    auto it = bundles.find(vendor);
    if (it != bundles.end())
        return *it->second;
    const std::string saved_data_dir = data_dir();
    const boost::filesystem::path scratch = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("nozzle_process_%%%%-%%%%");
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

bool contains(const std::vector<std::string> &names, const std::string &name)
{
    return std::find(names.begin(), names.end(), name) != names.end();
}

} // namespace

TEST_CASE("A U1 0.4+0.6 offers 0.6 processes for its 0.6 tools and fills them at full_config", "[NozzleProcess][PresetBundle]")
{
    PresetBundle &b = vendor_bundle("Snapmaker");
    REQUIRE(b.printers.select_preset_by_name("Snapmaker U1 (0.4+0.6 nozzle)", true));
    REQUIRE(b.prints.select_preset_by_name("0.20mm Standard @Snapmaker U1 (0.4+0.6 nozzle)", true));
    b.update_compatible(PresetSelectCompatibleType::Never);
    b.project_config.option<ConfigOptionStrings>("nozzle_process_presets", true)->values.clear();

    const std::string p06 = "0.30mm Standard @Snapmaker U1 (0.6 nozzle)";
    const std::string p04 = "0.20mm Standard @Snapmaker U1 (0.4 nozzle)";
    CHECK(contains(b.nozzle_process_candidates(2), p06));
    CHECK_FALSE(contains(b.nozzle_process_candidates(2), p04));
    CHECK(contains(b.nozzle_process_candidates(0), p04));
    CHECK_FALSE(contains(b.nozzle_process_candidates(0), p06));
    // The machine's own 0.4+0.6 process claims no single nozzle size: offered on every tool.
    CHECK(contains(b.nozzle_process_candidates(3), "0.20mm Standard @Snapmaker U1 (0.4+0.6 nozzle)"));

    const DynamicPrintConfig plain = b.full_config();
    CHECK(plain.option<ConfigOptionInts>("print_extruder_id")->values.size() == 1);

    REQUIRE(b.set_nozzle_process(2, p06));
    REQUIRE(b.set_nozzle_process(3, p06));
    CHECK(b.nozzle_process_selection() == std::vector<std::string>{ "", "", p06, p06 });
    const DynamicPrintConfig filled = b.full_config();
    const Preset            *p06_preset = b.prints.find_preset(p06, false);
    REQUIRE(p06_preset != nullptr);
    CHECK(filled.option<ConfigOptionInts>("print_extruder_id")->values.size() == 8);
    for (const char *key : { "outer_wall_speed", "inner_wall_speed", "sparse_infill_speed", "travel_speed", "default_acceleration" }) {
        INFO("option " << key);
        CHECK(nozzle_process_value(filled, key, 2, false) == static_cast<const ConfigOptionVectorBase *>(p06_preset->config.option(key))->vserialize()[0]);
        CHECK(nozzle_process_value(filled, key, 0, false) == static_cast<const ConfigOptionVectorBase *>(plain.option(key))->vserialize()[0]);
    }
    // Geometry stays the plate's.
    CHECK(filled.opt_float("layer_height") == plain.opt_float("layer_height"));
    CHECK(*filled.option("outer_wall_line_width") == *plain.option("outer_wall_line_width"));

    // Back to the plate's process on every tool: the stored list empties and full_config is as before.
    REQUIRE(b.set_nozzle_process(2, ""));
    REQUIRE(b.set_nozzle_process(3, ""));
    CHECK(b.project_config.option<ConfigOptionStrings>("nozzle_process_presets")->values.empty());
    CHECK(b.full_config().diff(plain).empty());
}

TEST_CASE("No shipped dual-extruder process asks for per-extruder values", "[NozzleProcess][PresetBundle]")
{
    // Stock jobs stay byte-identical: the slicer reads extruder slots only for a process that asks.
    PresetBundle &b = vendor_bundle("BBL");
    size_t        layouts = 0;
    for (const Preset &preset : b.prints.get_presets()) {
        if (!preset.is_system || process_extruder_variant_slots(preset.config).empty())
            continue;
        ++layouts;
        INFO("process " << preset.name);
        const auto *flag = preset.config.option<ConfigOptionBool>("process_extruder_values");
        CHECK((flag == nullptr || !flag->value));
    }
    CHECK(layouts >= 30); // H2D, H2D Pro, H2C and X2D processes
}

TEST_CASE("An H2D offers processes per side and drops a choice that no longer fits", "[NozzleProcess][PresetBundle]")
{
    PresetBundle &b = vendor_bundle("BBL");
    REQUIRE(b.printers.select_preset_by_name("Bambu Lab H2D 0.4 nozzle", true));
    REQUIRE(b.prints.select_preset_by_name("0.20mm Standard @BBL H2D", true));
    b.update_compatible(PresetSelectCompatibleType::Never);
    b.project_config.option<ConfigOptionStrings>("nozzle_process_presets", true)->values.clear();

    CHECK(contains(b.nozzle_process_candidates(1), "0.16mm Standard @BBL H2D"));
    CHECK_FALSE(contains(b.nozzle_process_candidates(1), "0.30mm Standard @BBL H2D 0.6 nozzle"));
    REQUIRE(b.set_nozzle_process(1, "0.16mm Standard @BBL H2D"));
    CHECK_FALSE(b.prune_nozzle_processes());

    // A right nozzle of 0.6: the H2D 0.6 processes fit it, the 0.4 choice is dropped.
    b.printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter")->values = { 0.4, 0.6 };
    CHECK(contains(b.nozzle_process_candidates(1), "0.30mm Standard @BBL H2D 0.6 nozzle"));
    CHECK(b.prune_nozzle_processes());
    CHECK(b.nozzle_process_selection() == std::vector<std::string>{ "", "" });
    b.printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter")->values = { 0.4, 0.4 };
}
