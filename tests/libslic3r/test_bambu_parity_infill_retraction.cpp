// Bambu Studio 02.09.01.70 parity, PR 2 "Auto infill retraction" (owner decision D3, 2026-11-25):
//  - filament_metal_stickiness (BS PrintConfig.cpp:3310-3327, Tab.cpp:4804);
//  - reduce_infill_retraction_mode Disabled / Auto / Enabled (BS PrintConfig.cpp:5272-5289, GCode.cpp:8466-8482),
//    which replaces our reduce_infill_retraction bool: an old 1 loads as Enabled, an old 0 as Disabled.
#include <catch2/catch.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r.h"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <map>
#include <string>

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

std::string legacy(std::string key, std::string value)
{
    PrintConfigDef::handle_legacy(key, value);
    return key + "=" + value;
}

ReduceInfillRetractionMode mode_of(const DynamicPrintConfig &cfg)
{
    return cfg.opt_enum<ReduceInfillRetractionMode>("reduce_infill_retraction_mode");
}

// Writes `body` to a scratch file and loads it the way a preset .json or a 3MF project_settings.config is loaded.
DynamicPrintConfig load_json_text(const std::string &name, const std::string &body)
{
    const fs::path path = fs::temp_directory_path() / fs::unique_path("bbparity2_%%%%%%%%_" + name);
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

} // namespace

TEST_CASE("Reduce infill retraction skips by mode and metal stickiness", "[BambuParity][InfillRetraction]")
{
    for (int s : { int(fmsNone), int(fmsLow), int(fmsMedium), int(fmsHigh) }) {
        INFO("stickiness " << s);
        CHECK(reduce_infill_retraction_applies(rirEnabled, s));
        CHECK_FALSE(reduce_infill_retraction_applies(rirDisabled, s));
    }
    // Auto: None (untested) behaves like Low.
    CHECK(reduce_infill_retraction_applies(rirAuto, fmsNone));
    CHECK(reduce_infill_retraction_applies(rirAuto, fmsLow));
    CHECK_FALSE(reduce_infill_retraction_applies(rirAuto, fmsMedium));
    CHECK_FALSE(reduce_infill_retraction_applies(rirAuto, fmsHigh));
}

TEST_CASE("Metal stickiness and the three-way mode are defined as in Bambu Studio", "[BambuParity][InfillRetraction]")
{
    const ConfigOptionDef *stick = print_config_def.get("filament_metal_stickiness");
    REQUIRE(stick != nullptr);
    CHECK(stick->type == coEnums);
    CHECK(stick->enum_values == std::vector<std::string>{ "None", "Low", "Medium", "High" });
    CHECK(stick->default_value->serialize() == "None");

    const ConfigOptionDef *mode = print_config_def.get("reduce_infill_retraction_mode");
    REQUIRE(mode != nullptr);
    CHECK(mode->type == coEnum);
    CHECK(mode->enum_values == std::vector<std::string>{ "Disabled", "Auto", "Enabled" });
    // Ours stays off by default (the old bool's false); Bambu's default is Auto.
    CHECK(mode->default_value->serialize() == "Disabled");
    CHECK_FALSE(print_config_def.has("reduce_infill_retraction"));

    // A filament setting: one value per filament, and in the filament preset's key list.
    const auto &filament_keys = Preset::filament_options();
    CHECK(std::find(filament_keys.begin(), filament_keys.end(), "filament_metal_stickiness") != filament_keys.end());
    const auto &print_keys = Preset::print_options();
    CHECK(std::find(print_keys.begin(), print_keys.end(), "reduce_infill_retraction_mode") != print_keys.end());
}

TEST_CASE("The old reduce_infill_retraction bool loads as the three-way mode", "[BambuParity][InfillRetraction][Config]")
{
    SECTION("handle_legacy: 1 -> Enabled, 0 -> Disabled, key-only pass renames") {
        CHECK(legacy("reduce_infill_retraction", "1") == "reduce_infill_retraction_mode=Enabled");
        CHECK(legacy("reduce_infill_retraction", "true") == "reduce_infill_retraction_mode=Enabled");
        CHECK(legacy("reduce_infill_retraction", "0") == "reduce_infill_retraction_mode=Disabled");
        CHECK(legacy("reduce_infill_retraction", "false") == "reduce_infill_retraction_mode=Disabled");
        CHECK(legacy("reduce_infill_retraction", "") == "reduce_infill_retraction_mode=");
        // The new key and its values pass through.
        CHECK(legacy("reduce_infill_retraction_mode", "Auto") == "reduce_infill_retraction_mode=Auto");
        CHECK(legacy("filament_metal_stickiness", "High") == "filament_metal_stickiness=High");
    }
    SECTION("set_deserialize (CLI --key=value, per-key loads)") {
        DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
        cfg.set_deserialize_strict("reduce_infill_retraction", "1");
        CHECK(mode_of(cfg) == rirEnabled);
        cfg.set_deserialize_strict("reduce_infill_retraction", "0");
        CHECK(mode_of(cfg) == rirDisabled);
        cfg.set_deserialize_strict("reduce_infill_retraction_mode", "Auto");
        CHECK(mode_of(cfg) == rirAuto);
        CHECK_FALSE(cfg.has("reduce_infill_retraction"));
    }
    SECTION("an old .ini config (CLI --load)") {
        DynamicPrintConfig cfg;
        cfg.load_from_ini_string("reduce_infill_retraction = 1\n", ForwardCompatibilitySubstitutionRule::Disable);
        CHECK(mode_of(cfg) == rirEnabled);
        DynamicPrintConfig off;
        off.load_from_ini_string("reduce_infill_retraction = 0\n", ForwardCompatibilitySubstitutionRule::Disable);
        CHECK(mode_of(off) == rirDisabled);
    }
    SECTION("an old process preset or 3MF project_settings.config (JSON)") {
        CHECK(mode_of(load_json_text("on.json", R"({ "type": "process", "reduce_infill_retraction": "1" })")) == rirEnabled);
        CHECK(mode_of(load_json_text("off.json", R"({ "type": "process", "reduce_infill_retraction": "0" })")) == rirDisabled);
        CHECK(mode_of(load_json_text("new.json", R"({ "type": "process", "reduce_infill_retraction_mode": "Auto" })")) == rirAuto);
    }
    SECTION("a file with both spellings keeps the new key") {
        CHECK(mode_of(load_json_text("both1.json",
                                     R"({ "type": "process", "reduce_infill_retraction": "1", "reduce_infill_retraction_mode": "Auto" })")) ==
              rirAuto);
        CHECK(mode_of(load_json_text("both2.json",
                                     R"({ "type": "process", "reduce_infill_retraction_mode": "Disabled", "reduce_infill_retraction": "1" })")) ==
              rirDisabled);
    }
    SECTION("per-filament stickiness from a JSON array") {
        const DynamicPrintConfig cfg = load_json_text("fil.json", R"({ "type": "filament", "filament_metal_stickiness": ["High"] })");
        REQUIRE(cfg.has("filament_metal_stickiness"));
        CHECK(cfg.option<ConfigOptionEnumsGeneric>("filament_metal_stickiness")->get_at(0) == int(fmsHigh));
    }
}

TEST_CASE("Our BBL profiles use Auto with High stickiness on the Bambu bases", "[BambuParity][InfillRetraction][Profiles]")
{
    CHECK(mode_of(load_profile("process/fdm_process_common.json")) == rirAuto);
    CHECK(mode_of(load_profile("process/fdm_process_common_h3c.json")) == rirAuto);
    // Bambu Studio 02.09.01.70 resources/profiles/BBL/filament/fdm_filament_{pa,pctg,pet,ppa,tpu}.json: High.
    for (const char *base : { "fdm_filament_pa.json", "fdm_filament_pctg.json", "fdm_filament_pet.json", "fdm_filament_ppa.json",
                              "fdm_filament_tpu.json" }) {
        INFO(base);
        const DynamicPrintConfig cfg = load_profile(std::string("filament/") + base);
        REQUIRE(cfg.has("filament_metal_stickiness"));
        CHECK(cfg.option<ConfigOptionEnumsGeneric>("filament_metal_stickiness")->get_at(0) == int(fmsHigh));
    }
    const DynamicPrintConfig common = load_profile("filament/fdm_filament_common.json");
    REQUIRE(common.has("filament_metal_stickiness"));
    CHECK(common.option<ConfigOptionEnumsGeneric>("filament_metal_stickiness")->get_at(0) == int(fmsNone));
}
