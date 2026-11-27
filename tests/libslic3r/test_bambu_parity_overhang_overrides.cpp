// Bambu Studio 02.09.01.70 parity, PR 3 "Filament overhang overrides" (owner decision D6, 2026-11-25):
// override_process_overhang_speed, filament_enable_overhang_speed, filament_overhang_{1_4,2_4,3_4,4_4,totally}_speed
// and filament_bridge_speed (BS PrintConfig.cpp:101-108, 7548-7581; Tab.cpp:4663-4673; GCode.cpp:6603-6640,
// 7176-7196, 7726-7777). Until this PR the keys were dropped on load (PrintConfig.cpp handle_legacy, "not in
// print_config_def").
#include <catch2/catch.hpp>

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r.h"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

const std::vector<std::string> &override_keys()
{
    static const std::vector<std::string> keys{ "override_process_overhang_speed", "filament_enable_overhang_speed",
                                                "filament_overhang_1_4_speed",     "filament_overhang_2_4_speed",
                                                "filament_overhang_3_4_speed",     "filament_overhang_4_4_speed",
                                                "filament_overhang_totally_speed", "filament_bridge_speed" };
    return keys;
}

DynamicPrintConfig load_json_text(const std::string &name, const std::string &body)
{
    const fs::path path = fs::temp_directory_path() / fs::unique_path("bbparity3_%%%%%%%%_" + name);
    {
        boost::nowide::ofstream out(path.string());
        out << body;
    }
    DynamicPrintConfig                 cfg;
    ConfigSubstitutionContext          ctxt{ ForwardCompatibilitySubstitutionRule::Enable };
    std::map<std::string, std::string> key_values;
    std::string                        reason;
    const int                          rc = cfg.load_from_json(path.string(), ctxt, false, key_values, reason);
    fs::remove(path);
    REQUIRE(rc == 0);
    CHECK(ctxt.substitutions.empty());
    return cfg;
}

DynamicPrintConfig load_profile(const std::string &relative)
{
    DynamicPrintConfig                 cfg;
    ConfigSubstitutionContext          ctxt{ ForwardCompatibilitySubstitutionRule::Enable };
    std::map<std::string, std::string> key_values;
    std::string                        reason;
    const std::string path = std::string(SLIC3R_TEST_RESOURCES_DIR) + "/profiles/BBL/" + relative;
    REQUIRE(cfg.load_from_json(path, ctxt, false, key_values, reason) == 0);
    return cfg;
}

// A Bambu filament preset fragment as the H2C PET-CF one: three variant slots, the override on in slot 0 only.
const char *h2c_petcf_like = R"({
    "type": "filament",
    "name": "PET-CF test",
    "filament_extruder_variant": [ "Direct Drive Standard", "Direct Drive High Flow", "Direct Drive E3D High Flow" ],
    "override_process_overhang_speed": [ "1", "0", "nil" ],
    "filament_enable_overhang_speed": [ "1", "1", "1" ],
    "filament_overhang_1_4_speed": [ "0", "0", "0" ],
    "filament_overhang_2_4_speed": [ "35", "50", "50" ],
    "filament_overhang_3_4_speed": [ "35", "30", "30" ],
    "filament_overhang_4_4_speed": [ "10", "10", "10" ],
    "filament_overhang_totally_speed": [ "10", "10", "nil" ],
    "filament_bridge_speed": [ "25", "25", "25" ]
})";

// One filament declaring Standard + High Flow (two packed slots); filament_volume_type picks the slot.
DynamicPrintConfig two_slot_filament(FilamentVolumeType volume)
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.option<ConfigOptionInts>("filament_flow_step_size", true)->values      = { 2 };
    cfg.option<ConfigOptionStrings>("filament_flow_support", true)->values     = { FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW };
    cfg.option<ConfigOptionEnumsGeneric>("filament_volume_type", true)->values = { int(volume) };
    cfg.set_deserialize_strict({ { "override_process_overhang_speed", "0,1" },
                                 { "filament_enable_overhang_speed", "1,0" },
                                 { "filament_overhang_1_4_speed", "0,11" },
                                 { "filament_overhang_2_4_speed", "0,12" },
                                 { "filament_overhang_3_4_speed", "0,13" },
                                 { "filament_overhang_4_4_speed", "0,14" },
                                 { "filament_overhang_totally_speed", "0,15" },
                                 { "filament_bridge_speed", "0,16" } });
    return cfg;
}

} // namespace

TEST_CASE("The filament overhang overrides are defined as in Bambu Studio", "[BambuParity][OverhangOverrides]")
{
    const std::map<std::string, std::pair<ConfigOptionType, std::string>> expected{
        { "override_process_overhang_speed", { coBools, "0" } },
        { "filament_enable_overhang_speed", { coBools, "1" } },
        { "filament_overhang_1_4_speed", { coFloats, "0" } },
        { "filament_overhang_2_4_speed", { coFloats, "0" } },
        { "filament_overhang_3_4_speed", { coFloats, "0" } },
        { "filament_overhang_4_4_speed", { coFloats, "0" } },
        { "filament_overhang_totally_speed", { coFloats, "10" } },
        { "filament_bridge_speed", { coFloats, "25" } },
    };
    const auto &filament_keys = Preset::filament_options();
    for (const auto &[key, type_default] : expected) {
        INFO(key);
        const ConfigOptionDef *def = print_config_def.get(key);
        REQUIRE(def != nullptr);
        CHECK(def->type == type_default.first);
        CHECK(def->nullable);
        CHECK(def->mode == comAdvanced);
        CHECK(def->default_value->serialize() == type_default.second);
        // Stored per flow variant, as Bambu's filament_options_with_variant, and part of the filament preset.
        CHECK(is_filament_flow_variant_option(key));
        CHECK(std::find(filament_keys.begin(), filament_keys.end(), key) != filament_keys.end());
    }
    // D6: the process "totally" bucket is not revived.
    CHECK_FALSE(print_config_def.has("overhang_totally_speed"));
    // In the static print config the G-code reads.
    PrintConfig print_config;
    CHECK(print_config.has("override_process_overhang_speed"));
    CHECK(print_config.has("filament_bridge_speed"));
}

TEST_CASE("Filament overhang overrides load from a preset or project and serialize back", "[BambuParity][OverhangOverrides][Config]")
{
    SECTION("handle_legacy keeps the keys under their own names") {
        for (const std::string &key : override_keys()) {
            std::string k = key, v = "1";
            PrintConfigDef::handle_legacy(k, v);
            CHECK(k == key);
        }
    }
    SECTION("a Bambu filament preset with three variant slots and nil entries") {
        const DynamicPrintConfig cfg = load_json_text("petcf.json", h2c_petcf_like);
        for (const std::string &key : override_keys()) {
            INFO(key);
            CHECK(cfg.has(key));
        }
        const auto *on = cfg.option<ConfigOptionBoolsNullable>("override_process_overhang_speed");
        REQUIRE(on != nullptr);
        REQUIRE(on->values.size() == 3);
        CHECK(on->values[0] == 1);
        CHECK(on->values[1] == 0);
        CHECK(on->is_nil(2));
        CHECK(cfg.opt_serialize("override_process_overhang_speed") == "1,0,nil");
        CHECK(cfg.opt_serialize("filament_overhang_2_4_speed") == "35,50,50");
        CHECK(cfg.opt_serialize("filament_overhang_totally_speed") == "10,10,nil");

        // Serialize and read back (the ini / CLI path) gives the same options.
        DynamicPrintConfig again;
        for (const std::string &key : override_keys())
            again.set_deserialize_strict(key, cfg.opt_serialize(key));
        for (const std::string &key : override_keys()) {
            INFO(key);
            CHECK(*again.option(key) == *cfg.option(key));
        }
    }
}

TEST_CASE("A filament's overhang override is read from its flow variant slot", "[BambuParity][OverhangOverrides]")
{
    SECTION("Standard slot: override off") {
        const FilamentOverhangOverride ovr = filament_overhang_override(two_slot_filament(fvtStandard), 0);
        CHECK_FALSE(ovr.on);
    }
    SECTION("High Flow slot: override on with its own values") {
        const FilamentOverhangOverride ovr = filament_overhang_override(two_slot_filament(fvtHighFlow), 0);
        CHECK(ovr.on);
        CHECK_FALSE(ovr.enable);
        CHECK(ovr.speed[0] == Approx(11.));
        CHECK(ovr.speed[1] == Approx(12.));
        CHECK(ovr.speed[2] == Approx(13.));
        CHECK(ovr.speed[3] == Approx(14.));
        CHECK(ovr.totally == Approx(15.));
        CHECK(ovr.bridge == Approx(16.));
    }
    SECTION("no keys, nil and an id past the vector read as the defaults") {
        CHECK_FALSE(filament_overhang_override(DynamicPrintConfig{}, 0).on);
        DynamicPrintConfig cfg;
        cfg.set_deserialize_strict({ { "override_process_overhang_speed", "nil,1" }, { "filament_overhang_totally_speed", "nil,nil" } });
        CHECK_FALSE(filament_overhang_override(cfg, 0).on);
        const FilamentOverhangOverride second = filament_overhang_override(cfg, 1);
        CHECK(second.on);
        CHECK(second.enable);                    // default
        CHECK(second.totally == Approx(10.));    // nil -> default
        CHECK(second.bridge == Approx(25.));     // missing -> default
        CHECK_FALSE(filament_overhang_override(cfg, 5).on); // past the vector -> slot 0 (ConfigOptionVector::get_at)
    }
    SECTION("any_filament_overhang_override_enabled needs the override and its slowdown in one slot") {
        CHECK_FALSE(any_filament_overhang_override_enabled(DynamicPrintConfig{}));
        CHECK_FALSE(any_filament_overhang_override_enabled(DynamicPrintConfig::full_print_config()));
        DynamicPrintConfig cfg;
        cfg.set_deserialize_strict({ { "override_process_overhang_speed", "1,0" }, { "filament_enable_overhang_speed", "0,1" } });
        CHECK_FALSE(any_filament_overhang_override_enabled(cfg));
        cfg.set_deserialize_strict("filament_enable_overhang_speed", "1,1");
        CHECK(any_filament_overhang_override_enabled(cfg));
    }
}

TEST_CASE("Overhang speeds with and without a filament override (D6)", "[BambuParity][OverhangOverrides]")
{
    const FilamentOverhangOverride off{};
    FilamentOverhangOverride       on;
    on.on       = true;
    on.enable   = false;
    on.speed[0] = 0.;
    on.speed[1] = 35.;
    on.speed[2] = 30.;
    on.speed[3] = 10.;
    on.totally  = 8.;
    on.bridge   = 20.;

    SECTION("override off: every value is the process one") {
        CHECK(overhang_slowdown_enabled(off, true));
        CHECK_FALSE(overhang_slowdown_enabled(off, false));
        CHECK(overhang_bridge_speed(off, 50.) == Approx(50.));
        for (int b = 0; b < 4; ++b)
            CHECK(overhang_bucket_speed(off, b, 40. + b) == Approx(40. + b));
        // Orca #17144 rules of the fully unsupported slot.
        CHECK(overhang_fully_unsupported_speed(off, false, false, 12., 50.) == Approx(50.)); // bridge speed
        CHECK(overhang_fully_unsupported_speed(off, true, false, 12., 50.) == Approx(50.));
        CHECK(overhang_fully_unsupported_speed(off, false, true, 12., 50.) == Approx(12.));  // curled: 4/4
        CHECK(overhang_fully_unsupported_speed(off, true, true, 12., 50.) == Approx(50.));   // bridge infill
    }
    SECTION("override on: the filament switch, buckets and bridge speed") {
        CHECK_FALSE(overhang_slowdown_enabled(on, true));
        on.enable = true;
        CHECK(overhang_slowdown_enabled(on, false));
        CHECK(overhang_bridge_speed(on, 50.) == Approx(20.));
        CHECK(overhang_bucket_speed(on, 0, 40.) == Approx(0.)); // 0 = the wall speed, not the process bucket
        CHECK(overhang_bucket_speed(on, 1, 40.) == Approx(35.));
        CHECK(overhang_bucket_speed(on, 3, 40.) == Approx(10.));
        // Walls take the "100%" speed in the fully unsupported slot, curled or not; bridge infill the bridge speed.
        CHECK(overhang_fully_unsupported_speed(on, false, false, 10., 20.) == Approx(8.));
        CHECK(overhang_fully_unsupported_speed(on, false, true, 10., 20.) == Approx(8.));
        CHECK(overhang_fully_unsupported_speed(on, true, false, 10., 20.) == Approx(20.));
        CHECK(overhang_fully_unsupported_speed(on, true, true, 10., 20.) == Approx(20.));
    }
    SECTION("override on with 0 speeds falls back as the process does") {
        on.bridge  = 0.;
        on.totally = 0.;
        CHECK(overhang_bridge_speed(on, 50.) == Approx(50.));
        CHECK(overhang_fully_unsupported_speed(on, false, false, 10., 50.) == Approx(50.));
        CHECK(overhang_fully_unsupported_speed(on, false, true, 10., 50.) == Approx(10.));
    }
}

TEST_CASE("BBL filament profiles carry Bambu's overhang overrides", "[BambuParity][OverhangOverrides][Profiles]")
{
    // Bambu Studio 02.09.01.70 resources/profiles/BBL/filament: the override is on in the Standard slot of the
    // H2C / H2D PET-CF presets and in the one slot of the H3C TPU and the H2D 0.4 TPU 85A presets.
    for (const char *name : { "Bambu PET-CF @BBL H2C.json", "Bambu PET-CF @BBL H2D.json", "Generic TPU(2.85mm) @BBL H3C.json",
                              "Bambu TPU 85A @BBL H2D 0.4 nozzle.json", "Bambu PET-CF @BBL X1C.json", "Bambu PET-CF @BBL A1.json" }) {
        INFO(name);
        const DynamicPrintConfig cfg = load_profile(std::string("filament/") + name);
        REQUIRE(cfg.has("override_process_overhang_speed"));
        CHECK(cfg.option<ConfigOptionBoolsNullable>("override_process_overhang_speed")->values.front() == 1);
    }
    // X1C / A1 (this PR, from the tag): 35 mm/s at 2/4 and 3/4 like the other PET-CF presets.
    const DynamicPrintConfig x1c = load_profile("filament/Bambu PET-CF @BBL X1C.json");
    CHECK(x1c.opt_serialize("filament_overhang_2_4_speed") == "35");
    CHECK(x1c.opt_serialize("filament_overhang_3_4_speed") == "35");
    // The H2D TPU 85A (0.6 / 0.8 nozzle) has it on only in the TPU High Flow slot (slot 2), which slices with the
    // Standard column here (BambuFlowSupport): no change for those nozzles.
    const DynamicPrintConfig tpu = load_profile("filament/Bambu TPU 85A @BBL H2D.json");
    CHECK(tpu.opt_serialize("override_process_overhang_speed") == "0,0,1,0");
}

TEST_CASE("A project 3MF keeps the filament overhang overrides", "[BambuParity][OverhangOverrides][3mf]")
{
    Model        src_model;
    ModelObject *src_object = src_model.add_object();
    src_object->name        = "cube";
    src_object->add_volume(make_cube(10., 10., 10.))->name = "cube";
    src_object->add_instance();
    src_object->ensure_on_bed();
    DynamicPrintConfig store_config = DynamicPrintConfig::full_print_config();
    store_config.set_deserialize_strict({ { "override_process_overhang_speed", "1" },
                                          { "filament_enable_overhang_speed", "1" },
                                          { "filament_overhang_1_4_speed", "0" },
                                          { "filament_overhang_2_4_speed", "35" },
                                          { "filament_overhang_3_4_speed", "30" },
                                          { "filament_overhang_4_4_speed", "12" },
                                          { "filament_overhang_totally_speed", "9" },
                                          { "filament_bridge_speed", "21" } });

    const fs::path tmp_root = fs::temp_directory_path() / ("snorca_tests_bbparity3_" + std::to_string(get_current_pid()));
    fs::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());
    const std::string test_file = (tmp_root / "overhang_overrides.3mf").string();

    StoreParams store_params;
    store_params.path     = test_file.c_str();
    store_params.model    = &src_model;
    store_params.config   = &store_config;
    store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    const bool stored     = store_bbs_3mf(store_params);
    REQUIRE(stored);

    Model                     dst_model;
    DynamicPrintConfig        dst_config;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
    PlateDataPtrs             plate_data;
    std::vector<Preset *>     project_presets;
    bool                      is_bbl_3mf = false;
    Semver                    file_version;
    const bool loaded = load_bbs_3mf(test_file.c_str(), &dst_config, &ctxt, &dst_model, &plate_data, &project_presets, &is_bbl_3mf,
                                     &file_version, nullptr,
                                     LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances |
                                         LoadStrategy::Silence);
    release_PlateData_list(plate_data);
    fs::remove(test_file);
    REQUIRE(loaded);

    for (const std::string &key : override_keys()) {
        INFO(key);
        REQUIRE(dst_config.has(key));
        CHECK(dst_config.opt_serialize(key) == store_config.opt_serialize(key));
    }
    const FilamentOverhangOverride ovr = filament_overhang_override(dst_config, 0);
    CHECK(ovr.on);
    CHECK(ovr.speed[3] == Approx(12.));
    CHECK(ovr.totally == Approx(9.));
    CHECK(ovr.bridge == Approx(21.));
}
