#include <catch2/catch.hpp>

#include "slic3r/GUI/FlowVariantEdit.hpp"

#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;

static DynamicPrintConfig make_two_mode_filament_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("filament_flow_support", new ConfigOptionStrings{FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW});
    config.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats{12.0, 24.0});
    config.set_key_value("nozzle_temperature", new ConfigOptionInts{210, 230});
    config.set_key_value("filament_flow_ratio", new ConfigOptionFloats{0.95, 0.88});
    config.set_key_value("enable_pressure_advance", new ConfigOptionBools{true, false});
    return config;
}

static DynamicPrintConfig make_two_mode_process_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("process_flow_support", new ConfigOptionStrings{FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW});
    config.set_key_value("outer_wall_speed", new ConfigOptionFloats{60.0, 120.0});
    config.set_key_value("inner_wall_speed", new ConfigOptionFloats{80.0, 160.0});
    return config;
}

TEST_CASE("flow_variant_slots_differ detects Standard vs High flow diverge", "[FlowVariantEdit]")
{
    DynamicPrintConfig filament = make_two_mode_filament_config();
    REQUIRE(flow_variant_slots_differ(filament, ConfigFlowDomain::Filament, FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW));
    // Same-mode and missing High-flow support are no-ops (Both-enter / Copy confirm stay silent).
    REQUIRE_FALSE(flow_variant_slots_differ(filament, ConfigFlowDomain::Filament, FLOW_MODE_STANDARD, FLOW_MODE_STANDARD));
    REQUIRE_FALSE(flow_variant_slots_differ(filament, ConfigFlowDomain::Process, FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW));

    DynamicPrintConfig process = make_two_mode_process_config();
    REQUIRE(flow_variant_slots_differ(process, ConfigFlowDomain::Process, FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW));

    DynamicPrintConfig single = DynamicPrintConfig::full_print_config();
    single.set_key_value("filament_flow_support", new ConfigOptionStrings{FLOW_MODE_STANDARD});
    single.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats{12.0});
    REQUIRE_FALSE(flow_variant_slots_differ(single, ConfigFlowDomain::Filament, FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW));

    REQUIRE(copy_flow_variant_slot(filament, ConfigFlowDomain::Filament, FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW));
    REQUIRE_FALSE(flow_variant_slots_differ(filament, ConfigFlowDomain::Filament, FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW));
}

TEST_CASE("copy_flow_variant_slot copies domain keys Standard to High flow", "[FlowVariantEdit]")
{
    DynamicPrintConfig config = make_two_mode_filament_config();
    config.set_key_value("filament_flow_step_size", new ConfigOptionInts{2, 2});
    REQUIRE(flow_variant_slots_differ(config, ConfigFlowDomain::Filament, FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW));

    REQUIRE(copy_flow_variant_slot(config, ConfigFlowDomain::Filament, FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW));
    REQUIRE_FALSE(flow_variant_slots_differ(config, ConfigFlowDomain::Filament, FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW));

    REQUIRE(config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == std::vector<double>{12.0, 12.0});
    REQUIRE(config.option<ConfigOptionInts>("nozzle_temperature")->values == std::vector<int>{210, 210});
    REQUIRE(config.option<ConfigOptionFloats>("filament_flow_ratio")->values == std::vector<double>{0.95, 0.95});
    REQUIRE(config.option<ConfigOptionBools>("enable_pressure_advance")->values[0] != 0);
    REQUIRE(config.option<ConfigOptionBools>("enable_pressure_advance")->values[1] != 0);
    REQUIRE(config.option<ConfigOptionInts>("filament_flow_step_size")->values == std::vector<int>{2, 2});
}

TEST_CASE("copy_flow_variant_slot copies process keys and leaves filament slots alone", "[FlowVariantEdit]")
{
    DynamicPrintConfig config = make_two_mode_process_config();
    config.set_key_value("filament_flow_support", new ConfigOptionStrings{FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW});
    config.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats{12.0, 24.0});

    REQUIRE(copy_flow_variant_slot(config, ConfigFlowDomain::Process, FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW));
    REQUIRE(config.option<ConfigOptionFloats>("outer_wall_speed")->values == std::vector<double>{60.0, 60.0});
    REQUIRE(config.option<ConfigOptionFloats>("inner_wall_speed")->values == std::vector<double>{80.0, 80.0});
    REQUIRE(config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == std::vector<double>{12.0, 24.0});
}

TEST_CASE("copy_flow_variant_slot is a no-op without a High-flow support entry", "[FlowVariantEdit]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("filament_flow_support", new ConfigOptionStrings{FLOW_MODE_STANDARD});
    config.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats{12.0});

    REQUIRE_FALSE(copy_flow_variant_slot(config, ConfigFlowDomain::Filament, FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW));
    REQUIRE(config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == std::vector<double>{12.0});
}

TEST_CASE("replicate_flow_variant_value dual-writes one key across mode indices", "[FlowVariantEdit]")
{
    DynamicPrintConfig config = make_two_mode_filament_config();
    config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values[0] = 18.0;

    REQUIRE(replicate_flow_variant_value(config, "filament_max_volumetric_speed", 0,
                                         {FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW}));
    REQUIRE(config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == std::vector<double>{18.0, 18.0});
    REQUIRE(config.option<ConfigOptionInts>("nozzle_temperature")->values == std::vector<int>{210, 230});
}

TEST_CASE("replicate_flow_variant_value leaves indices past modes.size() intact", "[FlowVariantEdit]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("filament_flow_support", new ConfigOptionStrings{FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW});
    // Preset-sized Standard/HF pair plus a trailing composed-layout leftover.
    config.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats{12.0, 24.0, 36.0, 48.0});

    REQUIRE(replicate_flow_variant_value(config, "filament_max_volumetric_speed", 0,
                                         {FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW}));
    REQUIRE(config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values ==
            std::vector<double>{12.0, 12.0, 36.0, 48.0});
}

TEST_CASE("replicate_flow_variant_value is a no-op for a single mode", "[FlowVariantEdit]")
{
    DynamicPrintConfig config = make_two_mode_filament_config();
    REQUIRE_FALSE(replicate_flow_variant_value(config, "filament_max_volumetric_speed", 0, {FLOW_MODE_STANDARD}));
    REQUIRE(config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values == std::vector<double>{12.0, 24.0});
}

TEST_CASE("copy_flow_variant_slot visits every filament_flow_variant_options key", "[FlowVariantEdit]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("filament_flow_support", new ConfigOptionStrings{FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW});

    for (const std::string &key : filament_flow_variant_options()) {
        auto *opt = config.option(key);
        if (opt == nullptr)
            continue;
        auto *vec = dynamic_cast<ConfigOptionVectorBase *>(opt);
        REQUIRE(vec != nullptr);
        if (vec->size() < 2)
            vec->resize(2);
    }

    copy_flow_variant_slot(config, ConfigFlowDomain::Filament, FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW);
    REQUIRE_FALSE(flow_variant_slots_differ(config, ConfigFlowDomain::Filament, FLOW_MODE_STANDARD, FLOW_MODE_HIGH_FLOW));

    for (const std::string &key : filament_flow_variant_options()) {
        const auto *opt = config.option(key);
        if (opt == nullptr)
            continue;
        const auto *vec = dynamic_cast<const ConfigOptionVectorBase *>(opt);
        REQUIRE(vec != nullptr);
        const auto values = vec->vserialize();
        REQUIRE_FALSE(values.empty());
        const std::string high_flow = values.size() > 1 ? values[1] : values.front();
        REQUIRE(values.front() == high_flow);
    }
}
