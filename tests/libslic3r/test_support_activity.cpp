#include <catch2/catch.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SupportActivity.hpp"

#include <set>
#include <utility>
#include <vector>

using namespace Slic3r;

TEST_CASE("Support activity is off when support, raft, enforcers and stabilizers are off", "[SupportActivity]")
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("enable_support", new ConfigOptionBool(false));
    cfg.set_key_value("raft_layers", new ConfigOptionInt(0));
    cfg.set_key_value("enforce_support_layers", new ConfigOptionInt(0));
    cfg.set_key_value("stabilizer_supports", new ConfigOptionEnum<StabilizerMode>(smOff));

    const SupportActivity a = support_activity_from_config(cfg);
    CHECK_FALSE(a.enable_support);
    CHECK(a.raft_layers == 0);
    CHECK(a.enforce_layers == 0);
    CHECK_FALSE(a.stabilizers);
    CHECK_FALSE(a.uses_support_material());
    CHECK_FALSE(a.uses_support_filament());
    CHECK_FALSE(a.uses_support_interface_filament());
    CHECK_FALSE(a.active());
}

TEST_CASE("Support activity follows missing keys as off", "[SupportActivity]")
{
    const SupportActivity a = support_activity_from_config(DynamicPrintConfig());
    CHECK_FALSE(a.active());
    CHECK_FALSE(a.uses_support_filament());
    CHECK_FALSE(a.uses_support_interface_filament());
}

TEST_CASE("Enable supports consumes support and interface filaments", "[SupportActivity]")
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("enable_support", new ConfigOptionBool(true));

    const SupportActivity a = support_activity_from_config(cfg);
    CHECK(a.uses_support_material());
    CHECK(a.uses_support_filament());
    CHECK(a.uses_support_interface_filament());
    CHECK(a.active());
}

TEST_CASE("Raft layers consume support filaments with supports off", "[SupportActivity]")
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("enable_support", new ConfigOptionBool(false));
    cfg.set_key_value("raft_layers", new ConfigOptionInt(2));

    const SupportActivity a = support_activity_from_config(cfg);
    CHECK(a.uses_support_material());
    CHECK(a.uses_support_filament());
    CHECK(a.uses_support_interface_filament());
    CHECK(a.active());
}

TEST_CASE("Enforce support layers consume support filaments with supports off", "[SupportActivity]")
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("enable_support", new ConfigOptionBool(false));
    cfg.set_key_value("enforce_support_layers", new ConfigOptionInt(3));

    const SupportActivity a = support_activity_from_config(cfg);
    CHECK(a.uses_support_material());
    CHECK(a.uses_support_filament());
    CHECK(a.uses_support_interface_filament());
    CHECK(a.active());
}

TEST_CASE("Stabilizers consume the support filament but not the interface filament", "[SupportActivity]")
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("enable_support", new ConfigOptionBool(false));
    cfg.set_key_value("raft_layers", new ConfigOptionInt(0));
    cfg.set_key_value("enforce_support_layers", new ConfigOptionInt(0));
    cfg.set_key_value("stabilizer_supports", new ConfigOptionEnum<StabilizerMode>(smAuto));

    const SupportActivity a = support_activity_from_config(cfg);
    CHECK_FALSE(a.uses_support_material());
    CHECK(a.uses_support_filament());
    CHECK_FALSE(a.uses_support_interface_filament());
    CHECK(a.active());
}

TEST_CASE("Manual stabilizers also count as support-filament use", "[SupportActivity]")
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("stabilizer_supports", new ConfigOptionEnum<StabilizerMode>(smManual));

    const SupportActivity a = support_activity_from_config(cfg);
    CHECK(a.stabilizers);
    CHECK(a.uses_support_filament());
    CHECK_FALSE(a.uses_support_interface_filament());
}

TEST_CASE("Object support activity falls back to the global scope", "[SupportActivity]")
{
    SupportActivity global;
    global.enable_support = true;
    global.raft_layers    = 1;
    global.enforce_layers = 2;
    global.stabilizers    = true;

    ModelConfig object;
    const SupportActivity a = effective_support_activity(object, global);
    CHECK(a.enable_support);
    CHECK(a.raft_layers == 1);
    CHECK(a.enforce_layers == 2);
    CHECK(a.stabilizers);
    CHECK(a.active());
}

TEST_CASE("A layer range can enable support on its own", "[SupportActivity]")
{
    SupportActivity object;
    object.enable_support = false;

    ModelConfig range;
    range.set_key_value("enable_support", new ConfigOptionBool(true));

    const SupportActivity a = effective_support_activity(range, object);
    CHECK(a.enable_support);
    CHECK(a.uses_support_filament());
    CHECK(a.uses_support_interface_filament());
}

TEST_CASE("A layer range can turn support off without dropping a raft from the object", "[SupportActivity]")
{
    SupportActivity object;
    object.enable_support = true;
    object.raft_layers    = 2;

    ModelConfig range;
    range.set_key_value("enable_support", new ConfigOptionBool(false));

    const SupportActivity a = effective_support_activity(range, object);
    CHECK_FALSE(a.enable_support);
    CHECK(a.raft_layers == 2);
    CHECK(a.uses_support_material());
    CHECK(a.active());
}

TEST_CASE("A leftover support filament override is inert when the scope is inactive", "[SupportActivity]")
{
    SupportActivity global;
    ModelConfig     object;
    object.set_key_value("support_filament", new ConfigOptionInt(2));
    object.set_key_value("support_interface_filament", new ConfigOptionInt(3));

    const SupportActivity a = effective_support_activity(object, global);
    CHECK_FALSE(a.active());
    CHECK_FALSE(a.uses_support_filament());
}

TEST_CASE("Object support_filament override is collected when a height range turns support on", "[SupportActivity]")
{
    SupportActivity global;
    ModelConfig     object;
    object.set_key_value("enable_support", new ConfigOptionBool(false));
    object.set_key_value("support_filament", new ConfigOptionInt(2));
    const SupportActivity object_activity = effective_support_activity(object, global);
    CHECK_FALSE(object_activity.active());

    SECTION("no range: leftover override is not collected") {
        std::vector<std::pair<int, ModelConfig>> ranges;
        CHECK_FALSE(object_or_range_support_active(object_activity, ranges));
        std::set<int> slots;
        collect_support_filament_slots_from_model_config(object, 4,
            object_or_range_support_active(object_activity, ranges), slots);
        CHECK(slots.empty());
    }

    SECTION("a range turns support on: the object's override slot is collected") {
        ModelConfig range;
        range.set_key_value("enable_support", new ConfigOptionBool(true));
        std::vector<std::pair<int, ModelConfig>> ranges;
        ranges.emplace_back(0, range);
        CHECK(object_or_range_support_active(object_activity, ranges));
        std::set<int> slots;
        collect_support_filament_slots_from_model_config(object, 4,
            object_or_range_support_active(object_activity, ranges), slots);
        REQUIRE(slots.size() == 1);
        CHECK(slots.count(1) == 1); // 1-based slot 2
    }
}

TEST_CASE("A layer range can disable all support activity inherited from the object", "[SupportActivity]")
{
    SupportActivity object;
    object.enable_support = true;
    object.raft_layers    = 2;
    object.enforce_layers = 1;
    object.stabilizers    = true;

    ModelConfig range;
    range.set_key_value("enable_support", new ConfigOptionBool(false));
    range.set_key_value("raft_layers", new ConfigOptionInt(0));
    range.set_key_value("enforce_support_layers", new ConfigOptionInt(0));
    range.set_key_value("stabilizer_supports", new ConfigOptionEnum<StabilizerMode>(smOff));

    const SupportActivity a = effective_support_activity(range, object);
    CHECK_FALSE(a.enable_support);
    CHECK(a.raft_layers == 0);
    CHECK(a.enforce_layers == 0);
    CHECK_FALSE(a.stabilizers);
    CHECK_FALSE(a.active());
}
