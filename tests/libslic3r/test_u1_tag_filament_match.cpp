// A Snapmaker U1 toolhead whose spool carries a third-party RFID tag (or a vendor set on the printer):
// print_task_config reports vendor / type / sub-type, SSWCP names it "<vendor> <type> <sub>", and the
// slicer has to find a preset for it in the sidebar's "Machine Filament" section and on Sync.
//
// The 2.4.7.0 report: an "iBoss" spool. The user's own preset for it was never matched (the exact-name
// rule only looked at system presets, the Snapmaker-name rule only at "<name> @U1 ..." names), so Sync
// fell back to "Generic PLA" and the slot vanished from the dropdown. These tests pin the rules
// (FilamentSyncMatch::match_preset) and the user preset a third-party spool now gets
// (PresetCollection::create_inherited_user_preset), with no printer and no slicer GUI.
#include <catch2/catch.hpp>

#include "libslic3r/FilamentSyncMatch.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/filesystem.hpp>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = boost::filesystem;
using namespace Slic3r;
using namespace Slic3r::FilamentSyncMatch;

namespace {

Candidate sys(const std::string &name, const std::string &vendor, const std::string &type, const std::string &colour = "")
{
    Candidate c;
    c.name      = name;
    c.vendor    = vendor;
    c.type      = type;
    c.colour    = colour;
    c.is_system = true;
    c.is_base   = true;
    return c;
}

Candidate user(const std::string &name, const std::string &vendor, const std::string &type, bool detached, const std::string &colour = "")
{
    Candidate c = sys(name, vendor, type, colour);
    c.is_system = false;
    c.is_base   = detached;
    return c;
}

// What a U1 with the 0.4 nozzle variant selected has: the Snapmaker and Generic presets.
std::vector<Candidate> u1_system_presets()
{
    return { sys("Generic PLA @U1 0.4 nozzle", "Generic", "PLA"), sys("Generic PLA Silk @U1 0.4 nozzle", "Generic", "PLA", "#C0C0C0"),
             sys("Generic PETG @U1 0.4 nozzle", "Generic", "PETG"), sys("Snapmaker PLA Matte @U1 0.4 nozzle", "Snapmaker", "PLA", "#FFFFFF") };
}

} // namespace

TEST_CASE("U1 tag: a Snapmaker spool still matches its system preset", "[FilamentSyncMatch][U1Tag]")
{
    const auto m = match_preset(u1_system_presets(), "Snapmaker PLA Matte", "PLA", "Snapmaker", "#D93A2B");
    REQUIRE(m.kind == MatchKind::ExactName);
    CHECK(u1_system_presets()[size_t(m.index)].name == "Snapmaker PLA Matte @U1 0.4 nozzle");
}

TEST_CASE("U1 tag: a third-party spool with no preset falls back to Generic of its type", "[FilamentSyncMatch][U1Tag]")
{
    const auto presets = u1_system_presets();
    const auto m       = match_preset(presets, "iBoss PLA Silk", "PLA", "iBoss", "#E0E0E0");
    REQUIRE(m.kind == MatchKind::Generic);
    CHECK(presets[size_t(m.index)].name == "Generic PLA @U1 0.4 nozzle");
}

TEST_CASE("U1 tag: the user's own preset of the reported name is matched", "[FilamentSyncMatch][U1Tag]")
{
    SECTION("Inheriting Generic PLA, named like the U1's presets")
    {
        auto presets = u1_system_presets();
        presets.push_back(user("iBoss PLA Silk @U1 0.4 nozzle", "iBoss", "PLA", false));
        const auto m = match_preset(presets, "iBoss PLA Silk", "PLA", "iBoss", "#E0E0E0");
        REQUIRE(m.kind == MatchKind::ExactName);
        CHECK(presets[size_t(m.index)].name == "iBoss PLA Silk @U1 0.4 nozzle");
    }
    SECTION("Made by Create filament (detached, named after the printer preset)")
    {
        auto presets = u1_system_presets();
        presets.push_back(user("iBoss PLA Silk @Snapmaker U1 (0.4 nozzle)", "iBoss", "PLA", true));
        const auto m = match_preset(presets, "iboss pla silk", "PLA", "iBoss", "#E0E0E0");
        REQUIRE(m.kind == MatchKind::ExactName);
        CHECK(presets[size_t(m.index)].name == "iBoss PLA Silk @Snapmaker U1 (0.4 nozzle)");
    }
    SECTION("Incompatible with the selected printer: not matched")
    {
        auto presets = u1_system_presets();
        presets.push_back(user("iBoss PLA Silk @U1 0.2 nozzle", "iBoss", "PLA", false));
        presets.back().is_compatible = false;
        CHECK(match_preset(presets, "iBoss PLA Silk", "PLA", "iBoss", "#E0E0E0").kind == MatchKind::Generic);
    }
}

TEST_CASE("U1 tag: a system preset wins over a user preset of the same name", "[FilamentSyncMatch][U1Tag]")
{
    auto presets = u1_system_presets();
    presets.insert(presets.begin(), user("Generic PLA @U1 0.4 nozzle copy", "Generic", "PLA", false));
    const auto m = match_preset(presets, "Generic PLA", "PLA", "Generic", "#FFFFFF");
    REQUIRE(m.kind == MatchKind::ExactName);
    CHECK(presets[size_t(m.index)].is_system);
}

TEST_CASE("U1 tag: vendor and type pick the closest colour, inheriting user presets included", "[FilamentSyncMatch][U1Tag]")
{
    auto presets = u1_system_presets();
    presets.push_back(user("My iBoss white", "iBoss", "PLA", false, "#FFFFFF"));
    presets.push_back(user("My iBoss red", "iBoss", "PLA", false, "#FF0000"));
    presets.push_back(user("My iBoss PETG", "iBoss", "PETG", true, "#FF0000"));
    const auto m = match_preset(presets, "iBoss PLA Basic", "PLA", "iBoss", "#EE1010");
    REQUIRE(m.kind == MatchKind::VendorType);
    CHECK(presets[size_t(m.index)].name == "My iBoss red");
}

TEST_CASE("U1 tag: a user preset still on vendor Generic cannot shadow the Generic presets", "[FilamentSyncMatch][U1Tag]")
{
    auto presets = u1_system_presets();
    presets.push_back(user("My red PLA", "Generic", "PLA", false, "#FF0000"));
    const auto m = match_preset(presets, "Generic PLA Glow", "PLA", "Generic", "#FF0000");
    REQUIRE(m.index >= 0);
    CHECK(presets[size_t(m.index)].name != "My red PLA");
}

TEST_CASE("U1 tag: an empty slot matches nothing", "[FilamentSyncMatch][U1Tag]")
{
    CHECK(match_preset(u1_system_presets(), "  ", "", "", "").kind == MatchKind::None);
    CHECK(match_preset(u1_system_presets(), "NONE NONE", "NONE", "NONE", "#000000").kind == MatchKind::None);
}

TEST_CASE("U1 tag: names and stock vendors", "[FilamentSyncMatch][U1Tag]")
{
    CHECK(base_name("Generic PLA @U1 0.4 nozzle") == "Generic PLA");
    CHECK(base_name("  iBoss PLA+  ") == "iBoss PLA+");
    CHECK(third_party_preset_name("iBoss PLA+ Silk", "Generic PLA @U1 0.4 nozzle") == "iBoss PLA+ Silk @U1 0.4 nozzle");
    CHECK(third_party_preset_name("a/b:c@d", "Generic PLA @U1 0.4 nozzle") == "a-b-c-d @U1 0.4 nozzle");
    CHECK(third_party_preset_name("", "Generic PLA @U1 0.4 nozzle").empty());
    CHECK(is_stock_vendor("Generic"));
    CHECK(is_stock_vendor(" snapmaker "));
    CHECK(is_stock_vendor(""));
    CHECK_FALSE(is_stock_vendor("iBoss"));
}

// ---- The user preset a third-party spool gets ---------------------------------------------------

namespace {

struct ScopedTemporaryDir
{
    fs::path root;
    ScopedTemporaryDir()
    {
        root = fs::temp_directory_path() / "snorca_tests" / fs::unique_path("u1_tag_%%%%%%%%");
        fs::create_directories(root);
    }
    ~ScopedTemporaryDir()
    {
        boost::system::error_code ec;
        fs::remove_all(root, ec);
    }
};

struct TestFilaments : public PresetCollection
{
    TestFilaments()
        : PresetCollection(Preset::TYPE_FILAMENT, Preset::filament_options(),
                           static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults()), "Default Filament")
    {}
};

const char *k_parent = "Generic PLA @U1 0.4 nozzle";

// The U1 parent as a fixture: compatible with the U1 only, pressure advance off (as every U1 preset).
void write_parent(const fs::path &folder)
{
    fs::create_directories(folder);
    std::ofstream(fs::path(folder / (std::string(k_parent) + ".json")).string())
        << R"({"type":"filament","name":")" << k_parent << R"(","from":"User","version":"1.0.0","inherits":"",)"
        << R"("filament_vendor":["Generic"],"filament_type":["PLA"],"enable_pressure_advance":["0"],"nozzle_temperature":["220"],)"
        << R"("compatible_printers":["Snapmaker U1 (0.4 nozzle)"]})";
}

// Loads the parent from its own folder and makes it a system preset, then the user folder.
void load(TestFilaments &filaments, const fs::path &system_dir, const fs::path &user_dir)
{
    PresetsConfigSubstitutions substitutions;
    filaments.load_presets(system_dir.string(), PRESET_FILAMENT_NAME, substitutions, ForwardCompatibilitySubstitutionRule::EnableSilent);
    Preset *parent = filaments.find_preset(k_parent, false);
    REQUIRE(parent != nullptr);
    parent->is_system = true;
    filaments.load_presets(user_dir.string(), PRESET_FILAMENT_NAME, substitutions, ForwardCompatibilitySubstitutionRule::EnableSilent);
}

std::string slurp(const fs::path &file)
{
    std::ifstream     in(file.string());
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

TEST_CASE("U1 tag: a third-party spool's preset inherits the U1 Generic and survives a restart", "[Preset][U1Tag]")
{
    ScopedTemporaryDir temp;
    const fs::path     system_dir = temp.root / "system";
    const fs::path     user_dir   = temp.root / "user";
    write_parent(system_dir / PRESET_FILAMENT_NAME);
    fs::create_directories(user_dir / PRESET_FILAMENT_NAME);

    const std::string name = third_party_preset_name("iBoss PLA Silk", k_parent);
    {
        TestFilaments filaments;
        load(filaments, system_dir, user_dir);
        const std::string selected_before = filaments.get_selected_preset_name();

        DynamicPrintConfig overrides;
        overrides.set_key_value("filament_vendor", new ConfigOptionStrings({ "iBoss" }));
        overrides.set_key_value("default_filament_colour", new ConfigOptionStrings({ "#E0E0E0" }));
        const Preset *created = filaments.create_inherited_user_preset(*filaments.find_preset(k_parent, false), name, overrides);
        REQUIRE(created != nullptr);
        CHECK(created->name == "iBoss PLA Silk @U1 0.4 nozzle");
        CHECK(created->inherits() == k_parent);
        CHECK_FALSE(created->is_system);
        CHECK(created->config.opt_string("filament_vendor", 0u) == "iBoss");
        CHECK(filaments.get_selected_preset_name() == selected_before);

        // Saved in the user folder (not base/: it has a parent), as a difference against the parent.
        const fs::path file = user_dir / PRESET_FILAMENT_NAME / (name + ".json");
        REQUIRE(fs::exists(file));
        const std::string json = slurp(file);
        CHECK(json.find("iBoss") != std::string::npos);
        CHECK(json.find(k_parent) != std::string::npos);
        CHECK(json.find("nozzle_temperature") == std::string::npos);

        // A second sync of the same spool does not make another one.
        CHECK(filaments.create_inherited_user_preset(*filaments.find_preset(k_parent, false), name, overrides) == nullptr);
    }

    // Next start: the preset is loaded back with its parent's U1-only compatibility and PA off.
    TestFilaments filaments;
    load(filaments, system_dir, user_dir);
    const Preset *reloaded = filaments.find_preset(name, false);
    REQUIRE(reloaded != nullptr);
    CHECK(reloaded->inherits() == k_parent);
    CHECK(reloaded->config.opt_string("filament_vendor", 0u) == "iBoss");
    CHECK(reloaded->config.option<ConfigOptionBools>("enable_pressure_advance")->values.front() == false);
    CHECK(reloaded->config.option<ConfigOptionStrings>("compatible_printers")->values == std::vector<std::string>{ "Snapmaker U1 (0.4 nozzle)" });

    // ...and the next sync finds it by name instead of falling back to Generic PLA.
    std::vector<Candidate> candidates = { sys(k_parent, "Generic", "PLA"),
                                          user(reloaded->name, "iBoss", "PLA", false) };
    const auto m = match_preset(candidates, "iBoss PLA Silk", "PLA", "iBoss", "#E0E0E0");
    CHECK(m.kind == MatchKind::ExactName);
    CHECK(m.index == 1);
}

TEST_CASE("U1 tag: no preset is made over an existing name or from a user parent", "[Preset][U1Tag]")
{
    ScopedTemporaryDir temp;
    const fs::path     system_dir = temp.root / "system";
    const fs::path     user_dir   = temp.root / "user";
    write_parent(system_dir / PRESET_FILAMENT_NAME);
    fs::create_directories(user_dir / PRESET_FILAMENT_NAME);
    TestFilaments filaments;
    load(filaments, system_dir, user_dir);

    DynamicPrintConfig overrides;
    const Preset      &parent = *filaments.find_preset(k_parent, false);
    CHECK(filaments.create_inherited_user_preset(parent, k_parent, overrides) == nullptr);
    CHECK(filaments.create_inherited_user_preset(parent, "", overrides) == nullptr);

    Preset user_parent = parent;
    user_parent.is_system = false;
    CHECK(filaments.create_inherited_user_preset(user_parent, "X @U1 0.4 nozzle", overrides) == nullptr);
}
