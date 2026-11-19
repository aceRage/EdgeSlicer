// Snapmaker #1050 / Edge: Preset::normalize used one count (nozzle count when not SEMM)
// for both extruder and filament vectors. A project with 2 nozzles and 4 filaments then
// grew short filament vectors to 2 and ignored filament_flow_step_size of length 4.
// Filament vectors stay sized from filament_diameter; extruder vectors from nozzle_diameter.
// Edge's BambuFlowSupport::derive and flow-variant packing are unchanged.

#include <catch2/catch.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <string>
#include <vector>

using namespace Slic3r;

namespace {

std::vector<double> floats(const DynamicPrintConfig &config, const char *key)
{
    const auto *opt = config.option<ConfigOptionFloats>(key);
    REQUIRE(opt != nullptr);
    return opt->values;
}

std::vector<std::string> strings(const DynamicPrintConfig &config, const char *key)
{
    const auto *opt = config.option<ConfigOptionStrings>(key);
    REQUIRE(opt != nullptr);
    return opt->values;
}

} // namespace

TEST_CASE("Preset::normalize sizes filament vectors from filament count when nozzles differ",
          "[Preset][Normalize]")
{
    // full_print_config seeds every extruder_option_keys() entry so set_num_extruders
    // does not hit its Debug asserts (PrintConfig.cpp). Overlay the 2-nozzle / 4-filament shape.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.option<ConfigOptionBool>("single_extruder_multi_material", true)->value = false;
    config.option<ConfigOptionFloats>("nozzle_diameter", true)->values             = {0.4, 0.6};
    config.option<ConfigOptionFloats>("filament_diameter", true)->values           = {1.75, 1.75, 1.75, 1.75};
    config.option<ConfigOptionFloats>("filament_density", true)->values            = {1.24, 1.25, 1.26, 1.27};
    config.option<ConfigOptionFloats>("filament_cost", true)->values               = {20.};
    config.option<ConfigOptionStrings>("filament_settings_id", true)->values       = {"a", "b"};
    config.option<ConfigOptionFloats>("retraction_length", true)->values           = {0.8};
    config.option<ConfigOptionFloats>("filament_max_volumetric_speed", true)->values = {22.};
    config.option<ConfigOptionInts>("filament_flow_step_size", true)->values       = {2, 1, 2, 1};

    Preset::normalize(config);

    CHECK(floats(config, "filament_diameter").size() == 4);
    CHECK(floats(config, "filament_density") == std::vector<double>({1.24, 1.25, 1.26, 1.27}));
    CHECK(floats(config, "filament_cost").size() == 4);
    CHECK(strings(config, "filament_settings_id").size() == 4);
    CHECK(floats(config, "nozzle_diameter").size() == 2);
    CHECK(floats(config, "retraction_length").size() == 2);
    // High-flow segments stay packed by filament: 2+1+2+1 = 6, not the nozzle count.
    CHECK(floats(config, "filament_max_volumetric_speed").size() == 6);
}

// Upstream's example: four extruders, two filaments, step sizes [2, 1]. The three-value
// high-flow array must stay three, not grow to the nozzle count.
TEST_CASE("Preset::normalize does not grow filament flow vectors to the nozzle count",
          "[Preset][Normalize]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.option<ConfigOptionBool>("single_extruder_multi_material", true)->value = false;
    config.option<ConfigOptionFloats>("nozzle_diameter", true)->values               = {0.4, 0.4, 0.4, 0.4};
    config.option<ConfigOptionFloats>("filament_diameter", true)->values             = {1.75, 1.75};
    config.option<ConfigOptionInts>("filament_flow_step_size", true)->values         = {2, 1};
    config.option<ConfigOptionFloats>("filament_max_volumetric_speed", true)->values = {25., 45., 22.};
    config.option<ConfigOptionFloats>("filament_density", true)->values              = {1.24, 1.25};

    Preset::normalize(config);

    CHECK(floats(config, "filament_density") == std::vector<double>({1.24, 1.25}));
    CHECK(floats(config, "filament_max_volumetric_speed") == std::vector<double>({25., 45., 22.}));
    CHECK(floats(config, "nozzle_diameter").size() == 4);
}

TEST_CASE("Preset::normalize sizes a standalone filament from filament_flow_support",
          "[Preset][Normalize]")
{
    DynamicPrintConfig config;
    config.set_key_value("filament_diameter", new ConfigOptionFloats(std::vector<double>{1.75}));
    config.set_key_value("filament_flow_support",
                         new ConfigOptionStrings(std::vector<std::string>{FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW}));
    config.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats(std::vector<double>{22.}));

    Preset::normalize(config);

    CHECK(floats(config, "filament_max_volumetric_speed") == std::vector<double>({22., 22.}));
}

TEST_CASE("Preset::normalize still runs BambuFlowSupport::derive before flow-variant sizing",
          "[Preset][Normalize][BambuFlowSupport]")
{
    DynamicPrintConfig config;
    config.set_key_value("filament_diameter", new ConfigOptionFloats(std::vector<double>{1.75}));
    config.set_key_value("filament_flow_support", new ConfigOptionStrings(std::vector<std::string>{FLOW_MODE_STANDARD}));
    config.set_key_value("filament_extruder_variant",
                         new ConfigOptionStrings(std::vector<std::string>{"Direct Drive Standard", "Direct Drive High Flow"}));
    config.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats(std::vector<double>{25., 40.}));
    config.set_key_value("filament_flow_ratio", new ConfigOptionFloats(std::vector<double>{0.98}));

    Preset::normalize(config);

    CHECK(strings(config, "filament_flow_support") ==
          std::vector<std::string>({FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW}));
    CHECK(floats(config, "filament_flow_ratio") == std::vector<double>({0.98, 0.98}));
    CHECK(floats(config, "filament_max_volumetric_speed") == std::vector<double>({25., 40.}));
}
