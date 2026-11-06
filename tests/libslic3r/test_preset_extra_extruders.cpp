// A user printer with more extruders than its parent (the U1 with the add-on unit: a 4-extruder system
// printer, a user printer with 5 or more). Upstream OrcaSlicer #16536 fixed its loader, which filled only
// the parent's extruder slots from a saved user preset. Edge's loader differs (a user preset saves every
// option that differs from its parent in full and loads by applying it over the parent), so these pin that
// every extruder's values survive save and reload, from a file and from the cloud map. Catch2 v2.
#include <catch2/catch.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/filesystem.hpp>

#include <map>
#include <string>

namespace fs = boost::filesystem;
using namespace Slic3r;

namespace {

struct ScopedTemporaryDir
{
    fs::path root;
    ScopedTemporaryDir()
    {
        root = fs::temp_directory_path() / "snorca_tests" / fs::unique_path("extra_extruders_%%%%%%%%");
        fs::create_directories(root);
    }
    ~ScopedTemporaryDir()
    {
        boost::system::error_code ec;
        fs::remove_all(root, ec);
    }
};

struct TestPrinters : public PresetCollection
{
    TestPrinters()
        : PresetCollection(Preset::TYPE_PRINTER, Preset::printer_options(), static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults()),
                           "Default Printer")
    {}
};

// A four-extruder system printer, as the U1.
void add_parent(PresetCollection &printers)
{
    DynamicPrintConfig config = printers.default_preset().config;
    config.set_key_value("single_extruder_multi_material", new ConfigOptionBool(false));
    config.set_deserialize_strict("nozzle_diameter", "0.4,0.4,0.4,0.4");
    config.set_num_extruders(4);
    config.set_deserialize_strict("retraction_length", "0.8,0.8,0.8,0.8");
    config.set_deserialize_strict("retract_length_toolchange", "10,10,10,10");
    printers.load_preset("", "Parent Printer", config, false);
    Preset *parent     = printers.find_preset("Parent Printer", false, true);
    parent->is_system  = true;
    parent->is_visible = true;
}

const Preset &parent_of(const PresetCollection &printers)
{
    const Preset *parent = printers.find_preset("Parent Printer", false, true);
    REQUIRE(parent != nullptr);
    return *parent;
}

// The parent grown to five extruders, retraction edited on extruder 5 only, as the settings tab leaves it.
DynamicPrintConfig five_extruder_child(const Preset &parent)
{
    DynamicPrintConfig config = parent.config;
    config.set_deserialize_strict("nozzle_diameter", "0.4,0.4,0.4,0.4,0.4");
    config.set_num_extruders(5);
    config.set_deserialize_strict("retraction_length", "0.8,0.8,0.8,0.8,1.5");
    config.set_deserialize_strict("retract_length_toolchange", "1,2,3,4,5");
    config.set_key_value("inherits", new ConfigOptionString("Parent Printer"));
    return config;
}

void check_child(const PresetCollection &printers)
{
    const Preset *child = printers.find_preset("User Printer", false, true);
    REQUIRE(child != nullptr);
    CHECK(child->config.option<ConfigOptionFloats>("nozzle_diameter")->values.size() == 5);
    CHECK(child->config.opt_serialize("retraction_length") == "0.8,0.8,0.8,0.8,1.5");
    CHECK(child->config.opt_serialize("retract_length_toolchange") == "1,2,3,4,5");
}

} // namespace

TEST_CASE("A user printer with more extruders than its parent keeps every extruder's values", "[Preset][Bundle][Orca16536]")
{
    ScopedTemporaryDir temp_dir;
    TestPrinters       printers;
    add_parent(printers);
    REQUIRE(printers.default_preset().config.option<ConfigOptionFloats>("retraction_length") != nullptr);

    SECTION("saved and reloaded from its file")
    {
        Preset child(Preset::TYPE_PRINTER, "User Printer");
        child.file    = (temp_dir.root / PRESET_PRINTER_NAME / "User Printer.json").string();
        child.config  = five_extruder_child(parent_of(printers));
        child.version = Semver(1, 0, 0);
        DynamicPrintConfig parent_config = parent_of(printers).config;
        REQUIRE(child.save(&parent_config));

        TestPrinters reloaded;
        add_parent(reloaded);
        PresetsConfigSubstitutions substitutions;
        reloaded.load_presets(temp_dir.root.string(), PRESET_PRINTER_NAME, substitutions, ForwardCompatibilitySubstitutionRule::EnableSilent);
        check_child(reloaded);
    }
    SECTION("from the cloud")
    {
        const std::map<std::string, std::string> values = {
            {BBL_JSON_KEY_VERSION, "1.0.0"},
            {BBL_JSON_KEY_SETTING_ID, "child-id"},
            {BBL_JSON_KEY_USER_ID, "user"},
            {BBL_JSON_KEY_BASE_ID, ""},
            {BBL_JSON_KEY_INHERITS, "Parent Printer"},
            {"printer_settings_id", "User Printer"},
            {"nozzle_diameter", "0.4,0.4,0.4,0.4,0.4"},
            {"retraction_length", "0.8,0.8,0.8,0.8,1.5"},
            {"retract_length_toolchange", "1,2,3,4,5"},
        };
        PresetsConfigSubstitutions substitutions;
        REQUIRE(printers.load_user_preset("User Printer", values, substitutions, ForwardCompatibilitySubstitutionRule::EnableSilent));
        check_child(printers);
    }
}
