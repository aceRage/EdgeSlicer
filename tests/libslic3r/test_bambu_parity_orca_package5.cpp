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
#include <limits>
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

TEST_CASE("Adaptive volumetric speed keys are filament settings stored per flow variant", "[OrcaPackage5][AdaptiveVolumetricSpeed]")
{
    const ConfigOptionDef *adaptive = print_config_def.get("filament_adaptive_volumetric_speed");
    REQUIRE(adaptive != nullptr);
    CHECK(adaptive->type == coBools);
    CHECK(adaptive->nullable);
    CHECK(adaptive->mode == comAdvanced);
    CHECK(adaptive->default_value->serialize() == "0");

    const ConfigOptionDef *coeffs = print_config_def.get("volumetric_speed_coefficients");
    REQUIRE(coeffs != nullptr);
    CHECK(coeffs->type == coStrings);

    for (const char *key : { "filament_adaptive_volumetric_speed", "volumetric_speed_coefficients" }) {
        INFO(key);
        CHECK(in_list(Preset::filament_options(), key));
        CHECK(is_filament_flow_variant_option(key));
        CHECK_FALSE(in_list(Preset::print_options(), key));
        CHECK_FALSE(in_list(Preset::printer_options(), key));
    }
}

TEST_CASE("Volumetric speed coefficients parse to a fit or to no fit", "[OrcaPackage5][AdaptiveVolumetricSpeed]")
{
    SECTION("six numbers are a fit: a*h^2 + b*w^2 + c*h*w + d*h + e*w + f") {
        const VolumetricSpeedFit fit = parse_volumetric_speed_coefficients("12.4717 -2.8601 9.7830 2.9503 3.9235 -1.8644");
        REQUIRE(fit.valid);
        const double h = 0.2, w = 0.42;
        const double expected = 12.4717 * h * h - 2.8601 * w * w + 9.7830 * h * w + 2.9503 * h + 3.9235 * w - 1.8644;
        CHECK(fit.max_volumetric_speed(h, w) == Approx(expected));
        CHECK(fit.max_volumetric_speed(h, w) > 0.);
    }
    SECTION("a constant fit does not depend on the line") {
        const VolumetricSpeedFit fit = parse_volumetric_speed_coefficients("0 0 0 0 0 7.5");
        REQUIRE(fit.valid);
        CHECK(fit.max_volumetric_speed(0.1, 0.4) == Approx(7.5));
        CHECK(fit.max_volumetric_speed(0.3, 0.8) == Approx(7.5));
    }
    SECTION("no fit: empty, all zero, wrong count, not numbers") {
        for (const char *bad : { "", "   ", "0 0 0 0 0 0", "0 0 0 0 0", "1 2 3 4 5 6 7", "a b c d e f", "1 2 3 4 5 6x", "1 2 3 4 5 nan" }) {
            INFO("coefficients '" << bad << "'");
            const VolumetricSpeedFit fit = parse_volumetric_speed_coefficients(bad);
            CHECK_FALSE(fit.valid);
            CHECK(fit.max_volumetric_speed(0.2, 0.42) == std::numeric_limits<double>::max());
        }
    }
}

TEST_CASE("Adaptive volumetric speed keys round trip through a config", "[OrcaPackage5][AdaptiveVolumetricSpeed][Config]")
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    CHECK(cfg.opt_serialize("filament_adaptive_volumetric_speed") == "0");

    cfg.set_deserialize_strict("filament_adaptive_volumetric_speed", "1,nil,0");
    const auto *adaptive = cfg.option<ConfigOptionBoolsNullable>("filament_adaptive_volumetric_speed");
    REQUIRE(adaptive != nullptr);
    REQUIRE(adaptive->values.size() == 3);
    CHECK(adaptive->get_at(0));
    CHECK(adaptive->is_nil(1));
    CHECK_FALSE(adaptive->get_at(2));
    CHECK(cfg.opt_serialize("filament_adaptive_volumetric_speed") == "1,nil,0");

    cfg.option<ConfigOptionStrings>("volumetric_speed_coefficients", true)->values = { "1 2 3 4 5 6", "0 0 0 0 0 0" };
    DynamicPrintConfig copy;
    copy.apply_only(cfg, { "filament_adaptive_volumetric_speed", "volumetric_speed_coefficients" });
    CHECK(copy.opt_serialize("filament_adaptive_volumetric_speed") == "1,nil,0");
    CHECK(copy.option<ConfigOptionStrings>("volumetric_speed_coefficients")->values == std::vector<std::string>{ "1 2 3 4 5 6", "0 0 0 0 0 0" });
}

TEST_CASE("The resolved flow caps the volumetric speed only for filaments that turned the fit on", "[OrcaPackage5][AdaptiveVolumetricSpeed]")
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_deserialize_strict("filament_diameter", "1.75,1.75,1.75");
    cfg.option<ConfigOptionFloats>("filament_max_volumetric_speed", true)->values = { 10., 10., 10. };
    cfg.option<ConfigOptionFloats>("filament_flow_ratio", true)->values           = { 1., 1., 1. };
    cfg.option<ConfigOptionBools>("enable_pressure_advance", true)->values        = { 0, 0, 0 };
    cfg.option<ConfigOptionStrings>("volumetric_speed_coefficients", true)->values = { "0 0 0 0 0 4", "0 0 0 0 0 40", "0 0 0 0 0 4" };

    SECTION("disabled everywhere: nothing is resolved and the limit is unchanged") {
        cfg.set_deserialize_strict("filament_adaptive_volumetric_speed", "0,0,0");
        const ResolvedFilamentFlow flow = ResolvedFilamentFlow::resolve(cfg);
        CHECK(flow.adaptive_fit.empty());
        for (unsigned id = 0; id < 3; ++id)
            CHECK(flow.adaptive_max_volumetric_speed(id, 10., 0.2, 0.42) == 10.);
    }
    SECTION("enabled on filament 0 and 1, nil on 2") {
        cfg.set_deserialize_strict("filament_adaptive_volumetric_speed", "1,1,nil");
        const ResolvedFilamentFlow flow = ResolvedFilamentFlow::resolve(cfg);
        REQUIRE(flow.adaptive_fit.size() == 3);
        CHECK(flow.adaptive_max_volumetric_speed(0, 10., 0.2, 0.42) == Approx(4.));  // fit below the limit
        CHECK(flow.adaptive_max_volumetric_speed(1, 10., 0.2, 0.42) == Approx(10.)); // limit below the fit
        CHECK(flow.adaptive_max_volumetric_speed(2, 10., 0.2, 0.42) == 10.);         // nil reads as off
        CHECK(flow.adaptive_max_volumetric_speed(7, 10., 0.2, 0.42) == 10.);         // outside the resolved range
        // A filament without a limit of its own (0) takes the fit.
        CHECK(flow.adaptive_max_volumetric_speed(0, 0., 0.2, 0.42) == Approx(4.));
    }
    SECTION("enabled but no usable fit, or a fit that goes negative: the limit is unchanged") {
        cfg.set_deserialize_strict("filament_adaptive_volumetric_speed", "1,1,1");
        cfg.option<ConfigOptionStrings>("volumetric_speed_coefficients", true)->values = { "", "0 0 0 0 0 0", "0 0 0 0 0 -2" };
        const ResolvedFilamentFlow flow = ResolvedFilamentFlow::resolve(cfg);
        for (unsigned id = 0; id < 3; ++id)
            CHECK(flow.adaptive_max_volumetric_speed(id, 10., 0.2, 0.42) == 10.);
    }
}

TEST_CASE("Bambu filament profiles carry the adaptive keys with the fit off", "[OrcaPackage5][AdaptiveVolumetricSpeed][Profiles]")
{
    const DynamicPrintConfig cfg = load_profile("filament/Bambu TPU 85A @BBL H2D.json");
    const auto *adaptive = cfg.option<ConfigOptionBoolsNullable>("filament_adaptive_volumetric_speed");
    REQUIRE(adaptive != nullptr);
    REQUIRE_FALSE(adaptive->values.empty());
    for (size_t i = 0; i < adaptive->values.size(); ++i)
        CHECK_FALSE(adaptive->get_at(i));
    const auto *coeffs = cfg.option<ConfigOptionStrings>("volumetric_speed_coefficients");
    REQUIRE(coeffs != nullptr);
    // Even a stored fit is not applied while the flag is off (see test_adaptive_volumetric_speed.cpp).
    CHECK_FALSE(parse_volumetric_speed_coefficients(coeffs->get_at(0)).valid);
}
