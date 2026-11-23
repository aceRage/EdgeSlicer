#include <catch2/catch.hpp>

#include "libslic3r/PresetDiffSession.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>

using namespace Slic3r;
using Side = PresetDiffSession::Side;

static bool has_key(const std::vector<std::string> &keys, const std::string &key)
{
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

static DynamicPrintConfig process_config(double layer_height, int wall_loops, double infill)
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("layer_height", new ConfigOptionFloat(layer_height));
    cfg.set_key_value("wall_loops", new ConfigOptionInt(wall_loops));
    cfg.set_key_value("sparse_infill_density", new ConfigOptionPercent(infill));
    return cfg;
}

static DynamicPrintConfig printer_config(std::vector<double> nozzles)
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("nozzle_diameter", new ConfigOptionFloats(nozzles));
    cfg.set_key_value("extruder_colour", new ConfigOptionStrings(std::vector<std::string>(nozzles.size(), "#FFFFFF")));
    cfg.set_key_value("printer_notes", new ConfigOptionString("a"));
    return cfg;
}

TEST_CASE("Compare presets: diff rows and copy both ways", "[PresetDiffSession]")
{
    PresetDiffSession s;
    s.reset(process_config(0.2, 2, 15), process_config(0.16, 3, 15), false);

    REQUIRE(s.diff_keys() == std::vector<std::string>{"layer_height", "wall_loops"});
    CHECK_FALSE(s.is_modified());

    SECTION("copy to the right makes the row equal and marks the right side modified")
    {
        CHECK(s.copy({"layer_height"}, Side::Right) == 1);
        CHECK_FALSE(s.differs("layer_height"));
        CHECK(s.differs("wall_loops"));
        CHECK_THAT(s.config(Side::Right).opt_float("layer_height"), Catch::Matchers::WithinAbs(0.2, 1e-9));
        CHECK(s.is_modified(Side::Right));
        CHECK_FALSE(s.is_modified(Side::Left));
        CHECK(s.changed_keys(Side::Right) == std::vector<std::string>{"layer_height"});
        CHECK(s.changed_keys(Side::Left).empty());
        // A row stays in the initial list after it became equal.
        CHECK(has_key(s.initial_diff_keys(), "layer_height"));
    }
    SECTION("copy to the left")
    {
        CHECK(s.copy({"wall_loops"}, Side::Left) == 1);
        CHECK(s.config(Side::Left).opt_int("wall_loops") == 3);
        CHECK(s.is_modified(Side::Left));
        CHECK_FALSE(s.is_modified(Side::Right));
    }
    SECTION("multi-row copy is one undo step; equal rows are skipped")
    {
        CHECK(s.copy({"layer_height", "wall_loops", "sparse_infill_density"}, Side::Right) == 2);
        CHECK(s.diff_keys().empty());
        REQUIRE(s.undo());
        CHECK(s.diff_keys().size() == 2);
        CHECK_FALSE(s.is_modified());
        CHECK_FALSE(s.can_undo());
        REQUIRE(s.redo());
        CHECK(s.diff_keys().empty());
        CHECK(s.is_modified(Side::Right));
    }
    SECTION("a new copy drops the redo tail")
    {
        s.copy({"layer_height"}, Side::Right);
        s.undo();
        CHECK(s.can_redo());
        s.copy({"wall_loops"}, Side::Left);
        CHECK_FALSE(s.can_redo());
    }
    SECTION("copying back restores the original and clears the modified flag")
    {
        s.copy({"layer_height"}, Side::Right);
        s.copy({"layer_height"}, Side::Right); // equal now: nothing to do
        CHECK(s.can_undo());
        s.undo();
        CHECK_FALSE(s.is_modified());
    }
    SECTION("mark_saved makes the current config the new original")
    {
        s.copy({"layer_height"}, Side::Right);
        s.mark_saved(Side::Right);
        CHECK_FALSE(s.is_modified(Side::Right));
        s.undo();
        CHECK(s.is_modified(Side::Right));
    }
}

TEST_CASE("Compare presets: per-extruder element rows", "[PresetDiffSession]")
{
    PresetDiffSession s;
    s.reset(printer_config({0.4, 0.4}), printer_config({0.4, 0.6}), true);

    REQUIRE(has_key(s.diff_keys(), "nozzle_diameter#1"));
    CHECK_FALSE(has_key(s.diff_keys(), "nozzle_diameter#0"));
    CHECK(s.differs("nozzle_diameter")); // plain key: any element differs
    CHECK_FALSE(has_key(s.diff_keys(), PresetDiffSession::ExtrudersCountKey));

    CHECK(s.copy({"nozzle_diameter#1"}, Side::Left) == 1);
    const auto *left = s.config(Side::Left).option<ConfigOptionFloats>("nozzle_diameter");
    REQUIRE(left->values.size() == 2);
    CHECK_THAT(left->values[0], Catch::Matchers::WithinAbs(0.4, 1e-9));
    CHECK_THAT(left->values[1], Catch::Matchers::WithinAbs(0.6, 1e-9));
    CHECK(s.diff_keys().empty());
}

TEST_CASE("Compare presets: different extruder counts", "[PresetDiffSession]")
{
    PresetDiffSession s;
    s.reset(printer_config({0.4}), printer_config({0.4, 0.6}), true);

    REQUIRE(has_key(s.diff_keys(), PresetDiffSession::ExtrudersCountKey));
    REQUIRE(has_key(s.diff_keys(), "nozzle_diameter#1")); // found walking the right config
    // The synthetic row never copies; an element the target lacks cannot be copied.
    CHECK_FALSE(s.can_copy(PresetDiffSession::ExtrudersCountKey, Side::Left));
    CHECK_FALSE(s.can_copy("nozzle_diameter#1", Side::Left));
    // The longer side can take the shorter side's (first) value.
    CHECK(s.can_copy("nozzle_diameter#1", Side::Right));
    CHECK(s.copy({"nozzle_diameter#1", PresetDiffSession::ExtrudersCountKey}, Side::Right) == 1);
    CHECK_THAT(s.config(Side::Right).option<ConfigOptionFloats>("nozzle_diameter")->values[1], Catch::Matchers::WithinAbs(0.4, 1e-9));
    CHECK(s.config(Side::Right).option<ConfigOptionFloats>("nozzle_diameter")->values.size() == 2);
}

TEST_CASE("Compare presets: a key only one side has", "[PresetDiffSession]")
{
    DynamicPrintConfig left  = process_config(0.2, 2, 15);
    DynamicPrintConfig right = process_config(0.2, 2, 15);
    left.set_key_value("compatible_printers", new ConfigOptionStrings({"A"}));

    PresetDiffSession s;
    s.reset(left, right, false);
    REQUIRE(s.differs("compatible_printers"));

    // Copying the absence removes the key; undo puts it back.
    CHECK(s.copy({"compatible_printers"}, Side::Left) == 1);
    CHECK_FALSE(s.config(Side::Left).has("compatible_printers"));
    CHECK(s.diff_keys().empty());
    CHECK(s.changed_keys(Side::Left) == std::vector<std::string>{"compatible_printers"});
    s.undo();
    CHECK(s.config(Side::Left).has("compatible_printers"));
    CHECK_FALSE(s.is_modified());
}

TEST_CASE("Compare presets: row key helpers", "[PresetDiffSession]")
{
    CHECK(PresetDiffSession::pure_key("nozzle_diameter#3") == "nozzle_diameter");
    CHECK(PresetDiffSession::pure_key("layer_height") == "layer_height");
    CHECK(PresetDiffSession::key_index("nozzle_diameter#3") == 3);
    CHECK(PresetDiffSession::key_index("layer_height") == -1);
}
