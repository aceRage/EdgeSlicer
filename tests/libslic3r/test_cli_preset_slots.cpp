#include <catch2/catch.hpp>

#include "libslic3r/CliPresetSlots.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <string>
#include <vector>

using namespace Slic3r;

// Regression for the CLI crash of 2026-10-29: a project saved with 2 filaments, sliced with
// 4 --filament-presets, kept filament_count at 2, so the 4th preset wrote
// different_settings_to_system[4] / inherits_group[4] past the end of 4-entry vectors
// (heap corruption, intermittent segfault). With 3 presets it overwrote the printer slot.

TEST_CASE("CLI filament slot count grows to the presets passed", "[CLI][Config]")
{
    CHECK(cli_filament_slot_count(2, 4) == 4);
    CHECK(cli_filament_slot_count(4, 2) == 4);
    CHECK(cli_filament_slot_count(0, 3) == 3);
    CHECK(cli_filament_slot_count(2, 0) == 2);
    CHECK(cli_filament_slot_count(0, 0) == 0);
}

TEST_CASE("CLI preset slots keep the printer last when they grow", "[CLI][Config]")
{
    SECTION("2-filament project sliced with 4 presets")
    {
        // process, filament 1, filament 2, printer
        std::vector<std::string> slots{"process", "fil1", "fil2", "printer"};
        const int filament_count = cli_filament_slot_count(2, 4);
        resize_cli_preset_slots(slots, filament_count);
        REQUIRE(slots.size() == 6);
        CHECK(slots[0] == "process");
        CHECK(slots[1] == "fil1");
        CHECK(slots[2] == "fil2");
        CHECK(slots[3].empty());
        CHECK(slots[4].empty());
        CHECK(slots[5] == "printer");
        // Every filament index the CLI loop writes (1..filament_count) is in range,
        // and none of them is the printer slot.
        for (int filament_index = 1; filament_index <= filament_count; ++filament_index) {
            CAPTURE(filament_index);
            CHECK(size_t(filament_index) < slots.size() - 1);
        }
    }
    SECTION("empty vector from a bare 3MF")
    {
        std::vector<std::string> slots;
        resize_cli_preset_slots(slots, 4);
        REQUIRE(slots.size() == 6);
        for (const std::string &s : slots)
            CHECK(s.empty());
    }
    SECTION("same size is left alone")
    {
        std::vector<std::string> slots{"process", "fil1", "fil2", "printer"};
        resize_cli_preset_slots(slots, 2);
        CHECK(slots == std::vector<std::string>{"process", "fil1", "fil2", "printer"});
    }
    SECTION("shrinking keeps the old plain resize")
    {
        std::vector<std::string> slots{"process", "fil1", "fil2", "fil3", "fil4", "printer"};
        resize_cli_preset_slots(slots, 2);
        CHECK(slots == std::vector<std::string>{"process", "fil1", "fil2", "fil3"});
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Filament presets given by name (--filament-presets) or file (--load-filaments) written into a project's config.
// The CLI took entry 0 of every list of a preset, so a filament set to high flow printed at its standard values, and
// a project saved by the application, which keeps each filament's flow variants side by side, kept its old values
// past the filaments (nozzle_temperature = 215,255,265,260,265,280 for three filaments) and read some of them.

namespace {

DynamicPrintConfig config_of(std::initializer_list<std::pair<std::string, std::string>> values)
{
    DynamicPrintConfig config;
    for (const auto &[key, value] : values)
        config.set_deserialize_strict(key, value);
    return config;
}

// A filament preset with a standard and a high-flow value of its temperature and flow ratio.
DynamicPrintConfig two_flow_preset(const std::string &temperatures, const std::string &ratios)
{
    return config_of({{"filament_flow_support", "standard;high_flow"},
                      {"nozzle_temperature", temperatures},
                      {"filament_flow_ratio", ratios},
                      {"filament_type", "PLA"}});
}

// What the CLI's filament loop does with the presets, by filament index (0-based).
void apply_presets(DynamicPrintConfig &config, const std::vector<std::pair<size_t, DynamicPrintConfig>> &presets, size_t filament_count)
{
    cli_flatten_filament_flow_variants(config, filament_count);
    for (const auto &[filament_idx, preset] : presets)
        for (const std::string &key : preset.keys())
            if (!preset.option(key)->is_scalar())
                cli_set_filament_preset_value(config, key, preset, filament_idx, filament_count);
}

// The temperature filament f prints at, as the slicer reads it.
int temperature_of(const DynamicPrintConfig &config, size_t f)
{
    return config.option<ConfigOptionInts>("nozzle_temperature")->get_at(get_config_idx(config, ConfigFlowDomain::Filament, unsigned(f)));
}

} // namespace

TEST_CASE("A filament set to high flow reads its high-flow values from a preset given to the CLI", "[CLI][Config][Regression]")
{
    // A project with two filaments, the second at high flow, its lists one per filament.
    DynamicPrintConfig config = config_of({{"filament_volume_type", "standard,high_flow"},
                                           {"filament_flow_support", "standard;standard"},
                                           {"nozzle_temperature", "200,200"},
                                           {"filament_flow_ratio", "1,1"}});
    apply_presets(config, {{0, two_flow_preset("215,255", "0.98,0.95")}, {1, two_flow_preset("220,260", "0.97,0.94")}}, 2);

    CHECK(temperature_of(config, 0) == 215);
    CHECK(temperature_of(config, 1) == 260);
    const auto *ratio = config.option<ConfigOptionFloats>("filament_flow_ratio");
    CHECK_THAT(ratio->get_at(get_config_idx(config, ConfigFlowDomain::Filament, 1)), Catch::Matchers::WithinRel(0.94, 1e-9));
    CHECK(config.option<ConfigOptionInts>("nozzle_temperature")->values == std::vector<int>{215, 260});
}

TEST_CASE("A project's flow variants side by side come out one per filament", "[CLI][Config][Regression]")
{
    // Saved by the application: three filaments, two flow variants each, the second filament at high flow.
    const auto project = [] {
        return config_of({{"filament_volume_type", "standard,high_flow,standard"},
                          {"filament_flow_support", "standard;high_flow;standard;high_flow;standard;high_flow"},
                          {"filament_flow_step_size", "2,2,2"},
                          {"nozzle_temperature", "200,210,220,230,240,250"},
                          {"filament_flow_ratio", "1,1,1,1,1,1"},
                          {"filament_type", "PLA;PLA;PLA"}});
    };
    REQUIRE(temperature_of(project(), 0) == 200);
    REQUIRE(temperature_of(project(), 1) == 230);
    REQUIRE(temperature_of(project(), 2) == 240);

    SECTION("every filament given a preset")
    {
        DynamicPrintConfig config = project();
        apply_presets(config,
                      {{0, two_flow_preset("215,255", "1,1")}, {1, two_flow_preset("265,260", "1,1")}, {2, two_flow_preset("265,280", "1,1")}},
                      3);
        CHECK(config.option<ConfigOptionInts>("nozzle_temperature")->values == std::vector<int>{215, 260, 265});
        CHECK(config.option<ConfigOptionStrings>("filament_flow_support")->values ==
              std::vector<std::string>{"standard", "high_flow", "standard"});
        CHECK(config.option<ConfigOptionInts>("filament_flow_step_size")->values == std::vector<int>{1, 1, 1});
        CHECK(config.option<ConfigOptionFloats>("filament_flow_ratio")->values.size() == 3);
        CHECK(temperature_of(config, 0) == 215);
        CHECK(temperature_of(config, 1) == 260);
        CHECK(temperature_of(config, 2) == 265);
    }
    SECTION("only the second filament given a preset")
    {
        DynamicPrintConfig config = project();
        apply_presets(config, {{1, two_flow_preset("265,260", "1,1")}}, 3);
        // The others read what they read before.
        CHECK(config.option<ConfigOptionInts>("nozzle_temperature")->values == std::vector<int>{200, 260, 240});
        CHECK(temperature_of(config, 0) == 200);
        CHECK(temperature_of(config, 1) == 260);
        CHECK(temperature_of(config, 2) == 240);
    }
}

TEST_CASE("A preset given to the CLI falls back to its first value as the application does", "[CLI][Config][Regression]")
{
    SECTION("a preset with no high-flow value, for a filament at high flow")
    {
        DynamicPrintConfig config = config_of({{"filament_volume_type", "high_flow"}, {"nozzle_temperature", "200"}});
        apply_presets(config, {{0, config_of({{"filament_flow_support", "standard"}, {"nozzle_temperature", "215"}})}}, 1);
        CHECK(temperature_of(config, 0) == 215);
    }
    SECTION("no high-flow retraction override of its own over a standard one")
    {
        DynamicPrintConfig config = config_of({{"filament_volume_type", "high_flow"}, {"filament_retraction_length", "nil"}});
        apply_presets(config, {{0, config_of({{"filament_flow_support", "standard;high_flow"}, {"filament_retraction_length", "0.8,nil"}})}}, 1);
        const auto *opt = config.option<ConfigOptionFloatsNullable>("filament_retraction_length");
        REQUIRE(opt != nullptr);
        REQUIRE(!opt->is_nil(0));
        CHECK_THAT(opt->get_at(0), Catch::Matchers::WithinRel(0.8, 1e-9));
    }
}
