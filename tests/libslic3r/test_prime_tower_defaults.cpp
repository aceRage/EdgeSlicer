#include <catch2/catch.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrimeTowerDefaults.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace Slic3r;
namespace PTD = Slic3r::PrimeTowerDefaults;

// "Use Bambu defaults" on the process Prime tower group (design_prime_tower_menu_rework.md section 5).

namespace {

bool contains(const std::vector<std::string>& v, const std::string& key)
{
    return std::find(v.begin(), v.end(), key) != v.end();
}

// A process key that names the prime / wipe tower.
bool is_tower_key(const std::string& key)
{
    return key.find("tower") != std::string::npos || boost::starts_with(key, "prime_") || key == "preload_all_filaments" ||
           key == "single_extruder_multi_material_priming";
}

} // namespace

TEST_CASE("Bambu tower defaults: the key lists hold process keys only", "[PrimeTowerDefaults]")
{
    const std::vector<std::string>& keys     = PTD::bambu_default_tower_keys();
    const std::vector<std::string>& excluded = PTD::excluded_tower_keys();
    const std::vector<std::string>& process  = Preset::print_options();
    const std::vector<std::string>& filament = Preset::filament_options();
    const std::vector<std::string>& printer  = Preset::printer_options();

    REQUIRE(keys.size() == 45);
    for (const std::string& key : keys) {
        INFO(key);
        CHECK(print_config_def.get(key) != nullptr);
        CHECK(contains(process, key));
        CHECK_FALSE(contains(filament, key));
        CHECK_FALSE(contains(printer, key));
        CHECK_FALSE(boost::starts_with(key, "filament_"));
        CHECK_FALSE(contains(excluded, key));
        CHECK(std::count(keys.begin(), keys.end(), key) == 1);
    }
    for (const std::string& key : excluded) {
        INFO(key);
        CHECK(print_config_def.get(key) != nullptr);
        CHECK(contains(process, key));
    }
    // The button never switches a tower on or off.
    CHECK_FALSE(contains(keys, "enable_prime_tower"));
    CHECK(contains(excluded, "enable_prime_tower"));
}

TEST_CASE("Bambu tower defaults: every tower process key is listed or excluded", "[PrimeTowerDefaults]")
{
    // A port that adds a tower key fails here until someone decides whether the button sets it.
    for (const std::string& key : Preset::print_options()) {
        if (!is_tower_key(key))
            continue;
        INFO(key);
        CHECK((contains(PTD::bambu_default_tower_keys(), key) || contains(PTD::excluded_tower_keys(), key)));
    }
}

TEST_CASE("Bambu tower defaults: offered only on Bambu printers with the tower on", "[PrimeTowerDefaults]")
{
    CHECK(PTD::offered(true, true));
    CHECK_FALSE(PTD::offered(true, false));
    CHECK_FALSE(PTD::offered(false, true));
    CHECK_FALSE(PTD::offered(false, false));
}

TEST_CASE("Bambu tower defaults: values come from the system preset plus the rib wall", "[PrimeTowerDefaults]")
{
    DynamicPrintConfig system = DynamicPrintConfig::full_print_config();
    system.set_deserialize_strict({
        { "prime_tower_width", "41" },
        { "wipe_tower_max_purge_speed", "70" },
        { "enable_tower_interface_features", "1" },
        { "wipe_tower_wall_type", "rectangle" },
    });
    const DynamicPrintConfig defaults = PTD::bambu_tower_defaults(system);

    // Only the listed keys, all of them.
    CHECK(defaults.keys().size() == PTD::bambu_default_tower_keys().size());
    for (const std::string& key : defaults.keys()) {
        INFO(key);
        CHECK(contains(PTD::bambu_default_tower_keys(), key));
    }
    CHECK_FALSE(defaults.has("enable_prime_tower"));
    CHECK_FALSE(defaults.has("filament_tower_interface_print_temp"));
    CHECK_FALSE(defaults.has("prime_tower_brim_chamfer"));

    CHECK(defaults.opt_float("prime_tower_width") == 41.);
    CHECK(defaults.opt_float("wipe_tower_max_purge_speed") == 70.);
    CHECK(defaults.opt_bool("enable_tower_interface_features"));
    // D7: Bambu Studio's prime_tower_rib_wall = true.
    CHECK(defaults.opt_enum<WipeTowerWallType>("wipe_tower_wall_type") == wtwRib);
}

TEST_CASE("Bambu tower defaults: a key the system preset lacks takes the code default", "[PrimeTowerDefaults]")
{
    DynamicPrintConfig system; // a preset that sets nothing
    const DynamicPrintConfig defaults = PTD::bambu_tower_defaults(system);
    CHECK(defaults.keys().size() == PTD::bambu_default_tower_keys().size());
    for (const std::string& key : PTD::bambu_default_tower_keys()) {
        if (key == "wipe_tower_wall_type")
            continue;
        INFO(key);
        REQUIRE(defaults.option(key) != nullptr);
        CHECK(*defaults.option(key) == *print_config_def.get(key)->default_value);
    }
    CHECK(defaults.opt_enum<WipeTowerWallType>("wipe_tower_wall_type") == wtwRib);
}

TEST_CASE("Bambu tower defaults: only differing listed keys change", "[PrimeTowerDefaults]")
{
    DynamicPrintConfig system = DynamicPrintConfig::full_print_config();
    system.set_deserialize_strict({ { "wipe_tower_wall_type", "rib" } });
    const DynamicPrintConfig defaults = PTD::bambu_tower_defaults(system);

    SECTION("already at the defaults: nothing to do")
    {
        DynamicPrintConfig current = system;
        CHECK(PTD::changed_keys(current, defaults).empty());
        CHECK(PTD::changes_to_apply(current, defaults).empty());
    }

    SECTION("edited tower values come back, nothing else is touched")
    {
        DynamicPrintConfig current = system;
        current.set_deserialize_strict({
            { "prime_tower_width", "80" },
            { "wipe_tower_max_purge_speed", "50" },
            { "wipe_tower_wall_type", "rectangle" },
            // Not the button's business: must never be reported or applied.
            { "enable_prime_tower", "0" },
            { "prime_tower_brim_chamfer", "0" },
            { "wipe_tower_filament", "2" },
            { "layer_height", "0.12" },
        });
        const std::vector<std::string> changed = PTD::changed_keys(current, defaults);
        // In the list's (the page's) order.
        CHECK(changed == std::vector<std::string>{ "prime_tower_width", "wipe_tower_max_purge_speed", "wipe_tower_wall_type" });

        const DynamicPrintConfig delta = PTD::changes_to_apply(current, defaults);
        CHECK(delta.keys().size() == 3);
        CHECK_FALSE(delta.has("enable_prime_tower"));
        CHECK_FALSE(delta.has("prime_tower_brim_chamfer"));
        CHECK_FALSE(delta.has("wipe_tower_filament"));
        CHECK_FALSE(delta.has("layer_height"));

        // What Tab::load_config() does with the delta, then ask again: nothing left.
        current.apply(delta);
        CHECK(PTD::changed_keys(current, defaults).empty());
        CHECK(current.opt_float("prime_tower_width") == system.opt_float("prime_tower_width"));
        CHECK_FALSE(current.opt_bool("enable_prime_tower"));
        CHECK(current.opt_int("wipe_tower_filament") == 2);
        CHECK(current.opt_float("layer_height") == 0.12);
    }

    SECTION("a filament config is never changed")
    {
        // A config holding only filament keys: none is a tower process key, so nothing is reported or applied.
        DynamicPrintConfig filament;
        filament.set_key_value("filament_tower_interface_print_temp", new ConfigOptionInts{ 250 });
        filament.set_key_value("filament_prime_volume", new ConfigOptionFloats{ 99. });
        CHECK(PTD::changed_keys(filament, defaults).empty());
        CHECK(PTD::changes_to_apply(filament, defaults).empty());
    }
}

namespace {

// The BBL vendor bundle, loaded once (the same way as test_bbl_rib_tower.cpp).
PresetBundle& bbl_bundle_for_defaults()
{
    static PresetBundle* bundle = [] {
        const std::string saved_data_dir = data_dir();
        const boost::filesystem::path scratch = boost::filesystem::temp_directory_path() /
                                                boost::filesystem::unique_path("bbl_tower_defaults_%%%%-%%%%");
        boost::filesystem::create_directories(scratch);
        set_data_dir(scratch.string());
        const std::string profiles = (boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
        static PresetBundle library;
        library.load_vendor_configs_from_json(profiles, PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem,
                                              ForwardCompatibilitySubstitutionRule::EnableSilent);
        auto* b = new PresetBundle();
        b->load_vendor_configs_from_json(profiles, "BBL", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent,
                                         &library);
        set_data_dir(saved_data_dir);
        return b;
    }();
    return *bundle;
}

} // namespace

TEST_CASE("Bambu tower defaults: a shipped Bambu process is already at the defaults but for the rib wall",
          "[PrimeTowerDefaults][slow]")
{
    PresetBundle& b = bbl_bundle_for_defaults();
    for (const char* name : { "0.20mm Standard @BBL X1C", "0.20mm Standard @BBL H2D", "0.20mm Standard @BBL H2C",
                              "0.20mm Standard @BBL H3C" }) {
        INFO(name);
        const Preset* preset = b.prints.find_preset(name, false);
        REQUIRE(preset != nullptr);
        CHECK(preset->is_system);
        const DynamicPrintConfig& cfg = preset->config;
        const std::vector<std::string> changed = PTD::changed_keys(cfg, PTD::bambu_tower_defaults(cfg));
        if (cfg.opt_enum<WipeTowerWallType>("wipe_tower_wall_type") == wtwRib)
            CHECK(changed.empty());
        else
            CHECK(changed == std::vector<std::string>{ "wipe_tower_wall_type" });
    }
}
