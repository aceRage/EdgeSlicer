// Bambu Studio 02.09.01.70 parity, PR 5 "Orca small package":
//  - support_cooling_filter + cooling_filter_enabled (Orca #14685), the printer's cooling filter accessory;
//  - filament_adaptive_volumetric_speed + volumetric_speed_coefficients (Orca #12688 / Bambu Studio), the
//    flow cap fitted from layer height and line width.
#include <catch2/catch.hpp>

#include "libslic3r/PlaceholderParser.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r.h"

#include <algorithm>
#include <map>
#include <string>

using namespace Slic3r;

namespace {

bool in_list(const std::vector<std::string> &keys, const std::string &key)
{
    return std::find(keys.begin(), keys.end(), key) != keys.end();
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

TEST_CASE("Cooling filter keys are printer settings, off by default", "[OrcaPackage5][CoolingFilter]")
{
    for (const char *key : { "support_cooling_filter", "cooling_filter_enabled" }) {
        INFO(key);
        const ConfigOptionDef *def = print_config_def.get(key);
        REQUIRE(def != nullptr);
        CHECK(def->type == coBool);
        CHECK(def->default_value->serialize() == "0");
        CHECK(in_list(Preset::printer_options(), key));
        CHECK_FALSE(in_list(Preset::filament_options(), key));
        CHECK_FALSE(in_list(Preset::print_options(), key));
    }
}

TEST_CASE("Cooling filter keys round trip through a config", "[OrcaPackage5][CoolingFilter][Config]")
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    CHECK_FALSE(cfg.opt_bool("support_cooling_filter"));
    CHECK_FALSE(cfg.opt_bool("cooling_filter_enabled"));

    cfg.set_deserialize_strict("support_cooling_filter", "1");
    cfg.set_deserialize_strict("cooling_filter_enabled", "1");
    CHECK(cfg.opt_bool("support_cooling_filter"));
    CHECK(cfg.opt_bool("cooling_filter_enabled"));
    CHECK(cfg.opt_serialize("cooling_filter_enabled") == "1");

    DynamicPrintConfig copy;
    copy.apply_only(cfg, { "support_cooling_filter", "cooling_filter_enabled" });
    CHECK(copy.opt_bool("support_cooling_filter"));
    CHECK(copy.opt_bool("cooling_filter_enabled"));
}

TEST_CASE("Machine G-code sees the real cooling_filter_enabled value", "[OrcaPackage5][CoolingFilter][Placeholder]")
{
    // The H2D start G-code reads {if cooling_filter_enabled}; it used to see a hard-coded false.
    const std::string tmpl = "{if cooling_filter_enabled}M145.2 P0 F0{else}M145.2 P0 F1{endif}";
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    {
        PlaceholderParser parser;
        parser.apply_config(cfg);
        CHECK(parser.process(tmpl) == "M145.2 P0 F1");
    }
    cfg.set_deserialize_strict("cooling_filter_enabled", "1");
    {
        PlaceholderParser parser;
        parser.apply_config(cfg);
        CHECK(parser.process(tmpl) == "M145.2 P0 F0");
    }
}

TEST_CASE("Bambu machine profiles declare support_cooling_filter for the H2 series", "[OrcaPackage5][CoolingFilter][Profiles]")
{
    // These are the printers Bambu Studio 02.09.01.70 shows the toggle for; the profiles already carry the flag.
    for (const char *name : { "H2C", "H2D", "H2D Pro", "H2S" }) {
        INFO(name);
        const DynamicPrintConfig cfg = load_profile(std::string("machine/Bambu Lab ") + name + " 0.4 nozzle.json");
        const auto *flag = cfg.option<ConfigOptionBool>("support_cooling_filter");
        REQUIRE(flag != nullptr);
        CHECK(flag->value);
    }
}
