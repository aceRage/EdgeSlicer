// GUI::change_opt_value() is where every settings-tab edit lands in the preset config
// (ConfigOptionsGroup::on_change_OG). The per-flow-mode "mm/s or %" options became
// coFloatsOrPercents vectors with the High Flow work, and the function had no case for that type:
// edits to the overhang speeds, the internal bridge speed and the rest were dropped silently while
// the field went on showing the typed value, so saving the preset and switching away and back
// "lost" them (owner, 2026-10-30, Process > Speed > Overhang speed).
#include <catch2/catch.hpp>

#include <boost/any.hpp>

#include <string>

#include "slic3r/GUI/GUI.hpp"

#include "libslic3r/NozzleProcess.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

namespace {

DynamicPrintConfig two_mode_process()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("process_flow_support", new ConfigOptionStrings{ "standard", "high_flow" });
    config.set_key_value("overhang_1_4_speed", new ConfigOptionFloatsOrPercents{ FloatOrPercent{ 60, false }, FloatOrPercent{ 60, false } });
    config.set_key_value("internal_bridge_speed", new ConfigOptionFloatsOrPercents{ FloatOrPercent{ 150, true }, FloatOrPercent{ 150, true } });
    config.set_key_value("bridge_speed", new ConfigOptionFloats{ 50., 50. });
    return config;
}

const FloatOrPercent &slot(const DynamicPrintConfig &config, const std::string &key, size_t idx)
{
    return config.option<ConfigOptionFloatsOrPercents>(key)->values.at(idx);
}

} // namespace

TEST_CASE("An edit of a mm/s-or-% flow-mode option reaches the config", "[GUI][ChangeOptValue][FlowVariant]")
{
    DynamicPrintConfig config = two_mode_process();

    SECTION("A plain value in the Standard slot leaves High flow alone") {
        GUI::change_opt_value(config, "overhang_1_4_speed", boost::any(std::string("45")), 0);
        CHECK(slot(config, "overhang_1_4_speed", 0) == FloatOrPercent{ 45, false });
        CHECK(slot(config, "overhang_1_4_speed", 1) == FloatOrPercent{ 60, false });
    }

    SECTION("A percentage in the High flow slot leaves Standard alone") {
        GUI::change_opt_value(config, "internal_bridge_speed", boost::any(std::string("120%")), 1);
        CHECK(slot(config, "internal_bridge_speed", 0) == FloatOrPercent{ 150, true });
        CHECK(slot(config, "internal_bridge_speed", 1) == FloatOrPercent{ 120, true });
    }

    SECTION("A slot past the end grows the vector, as the other vector types do") {
        GUI::change_opt_value(config, "overhang_1_4_speed", boost::any(std::string("33")), 2);
        REQUIRE(config.option<ConfigOptionFloatsOrPercents>("overhang_1_4_speed")->values.size() == 3);
        CHECK(slot(config, "overhang_1_4_speed", 2) == FloatOrPercent{ 33, false });
        CHECK(slot(config, "overhang_1_4_speed", 0) == FloatOrPercent{ 60, false });
    }

    SECTION("A key the config does not carry yet is created first") {
        config.erase("small_perimeter_speed");
        GUI::change_opt_value(config, "small_perimeter_speed", boost::any(std::string("40%")), 0);
        REQUIRE(config.has("small_perimeter_speed"));
        CHECK(slot(config, "small_perimeter_speed", 0) == FloatOrPercent{ 40, true });
    }
}

// The rest of the owner's section went through types that always worked; kept here so the whole
// Overhang speed group is covered by one test.
TEST_CASE("The rest of the Overhang speed section reaches the config", "[GUI][ChangeOptValue][FlowVariant]")
{
    DynamicPrintConfig config = two_mode_process();
    GUI::change_opt_value(config, "overhang_shadow_speed_ratio", boost::any(55.), 0);
    GUI::change_opt_value(config, "overhang_shadow_distance", boost::any(150.), 0);
    GUI::change_opt_value(config, "bridge_speed", boost::any(30.), 0);
    CHECK(config.option<ConfigOptionPercent>("overhang_shadow_speed_ratio")->value == Approx(55.));
    CHECK(config.option<ConfigOptionPercent>("overhang_shadow_distance")->value == Approx(150.));
    CHECK(config.option<ConfigOptionFloats>("bridge_speed")->values == std::vector<double>{ 30., 50. });
}

// Every option of this type, so one added later cannot fall through the same way.
TEST_CASE("Every coFloatsOrPercents option can be edited", "[GUI][ChangeOptValue][FlowVariant]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    size_t             checked = 0;
    for (const auto &[key, def] : print_config_def.options) {
        if (def.type != coFloatsOrPercents || !config.has(key))
            continue;
        INFO("option " << key);
        GUI::change_opt_value(config, key, boost::any(std::string("77%")), 0);
        const auto *vec = dynamic_cast<const ConfigOptionVector<FloatOrPercent> *>(config.option(key));
        REQUIRE(vec != nullptr);
        REQUIRE(!vec->values.empty());
        CHECK(vec->values.front() == FloatOrPercent{ 77, true });
        ++checked;
    }
    // overhang_1_4..4_4_speed, internal_bridge_speed, small_perimeter_speed, ... (11 on 2026-10-30).
    CHECK(checked >= 10);
}

// Per-nozzle process P1/P2: on a process split per nozzle ("Edit nozzles separately"), the Speed page writes
// the shown nozzle's slot through change_opt_value(). Before #795 an overhang speed edited for Right was
// dropped like every other mm/s-or-% edit; now it lands in Right's slot only and the slicer reads it there.
TEST_CASE("A per-nozzle edit of an overhang speed sticks to that nozzle", "[GUI][ChangeOptValue][NozzleProcess]")
{
    // An H2D-like two-extruder process: Bambu's 7-slot layout, filament 1 on Left, filament 2 on Right.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("nozzle_diameter", true)->values             = { 0.4, 0.4 };
    config.option<ConfigOptionBool>("single_extruder_multi_material", true)->value = false;
    config.option<ConfigOptionStrings>("extruder_variant_list", true)->values      = { "Direct Drive Standard,Direct Drive High Flow",
                                                                                     "Direct Drive Standard,Direct Drive High Flow" };
    config.option<ConfigOptionInts>("print_extruder_id", true)->values             = { 1, 1, 1, 2, 2, 2, 2 };
    config.option<ConfigOptionStrings>("print_extruder_variant", true)->values     = { "Direct Drive Standard", "Direct Drive High Flow",
                                                                                     "Direct Drive E3D High Flow", "Direct Drive Standard",
                                                                                     "Direct Drive High Flow", "Direct Drive TPU High Flow",
                                                                                     "Direct Drive E3D High Flow" };
    config.option<ConfigOptionStrings>("process_flow_support", true)->values       = { "standard", "high_flow" };
    config.option<ConfigOptionInts>("filament_map", true)->values                  = { 1, 2 };
    config.set_key_value("overhang_1_4_speed", new ConfigOptionFloatsOrPercents{ FloatOrPercent{ 60, false }, FloatOrPercent{ 70, false } });

    REQUIRE(split_process_per_nozzle(config, 2));
    const int right = process_nozzle_slot(config, "overhang_1_4_speed", 1, fvtStandard);
    const int left  = process_nozzle_slot(config, "overhang_1_4_speed", 0, fvtStandard);
    REQUIRE(right >= 0);
    REQUIRE(left >= 0);
    REQUIRE(right != left);

    // What the Speed page does for a field bound to Right's Standard slot.
    GUI::change_opt_value(config, "overhang_1_4_speed", boost::any(std::string("25")), right);
    CHECK(slot(config, "overhang_1_4_speed", size_t(right)) == FloatOrPercent{ 25, false });
    CHECK(slot(config, "overhang_1_4_speed", size_t(left)) == FloatOrPercent{ 60, false });
    // The slicer reads it for a filament on Right only.
    const auto &opt = *config.option<ConfigOptionFloatsOrPercents>("overhang_1_4_speed");
    CHECK(opt.get_at(process_config_idx(config, opt.size(), 1)) == FloatOrPercent{ 25, false });
    CHECK(opt.get_at(process_config_idx(config, opt.size(), 0)) == FloatOrPercent{ 60, false });
}
