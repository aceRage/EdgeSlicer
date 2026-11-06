// Phase 2 of the preset managers (tests/design_preset_manager_tables.md): renaming user presets
// (PresetCollection::rename_user_preset), the shared name rules (validate_preset_name, also used by the
// Save preset dialog), the rename plan of a filament family (plan_preset_rename) and what a rename or
// delete touches in the project (PresetBundle::preset_usage / repoint_preset_references).
#include <catch2/catch.hpp>

#include "libslic3r/NozzleProcess.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PresetManagerModel.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/filesystem.hpp>

#include <fstream>
#include <iterator>
#include <string>

namespace fs = boost::filesystem;
using namespace Slic3r;

namespace {

struct ScopedTemporaryDir
{
    fs::path root;
    ScopedTemporaryDir()
    {
        root = fs::temp_directory_path() / "snorca_tests" / fs::unique_path("preset_rename_%%%%%%%%");
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
    {
        // A system parent for the attached user presets ("System PLA @BBL X1C", alias "System PLA").
        DynamicPrintConfig config = this->default_preset().config;
        this->load_preset("", "System PLA @BBL X1C", config, false);
        Preset *parent     = this->find_preset("System PLA @BBL X1C", false, true);
        parent->is_system  = true;
        parent->is_visible = true;
        parent->alias      = "System PLA";
        this->update_map_alias_to_profile_name();
    }
    void load_user(const fs::path &dir)
    {
        PresetsConfigSubstitutions substitutions;
        this->load_presets(dir.string(), PRESET_FILAMENT_NAME, substitutions, ForwardCompatibilitySubstitutionRule::EnableSilent);
    }
};

fs::path write_user_filament(const fs::path &folder, const std::string &name, const std::string &inherits, const std::string &extra = {})
{
    const fs::path file = folder / (name + ".json");
    fs::create_directories(folder);
    std::ofstream(file.string()) << R"({"type":"filament","name":")" << name << R"(","from":"User","version":"1.0.0","inherits":")"
                                 << inherits << '"' << extra << '}';
    return file;
}

std::string read_file(const std::string &path)
{
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// A family: "My PLA" for two printers (one filament_id), a standalone root with a child, one more preset.
void write_family(const fs::path &root)
{
    const fs::path folder = root / PRESET_FILAMENT_NAME;
    write_user_filament(folder, "My PLA @BBL X1C", "System PLA @BBL X1C", R"(,"filament_id":"Pmy1","nozzle_temperature":["211"])");
    write_user_filament(folder, "My PLA @BBL P1S", "System PLA @BBL X1C", R"(,"filament_id":"Pmy1","nozzle_temperature":["212"])");
    write_user_filament(folder / "base", "Root", "", R"(,"filament_id":"Proot","nozzle_temperature":["233"])");
    write_user_filament(folder, "Root Child", "Root", R"(,"nozzle_temperature":["240"])");
    write_user_filament(folder, "Zebra", "System PLA @BBL X1C");
}

using Level = PresetNameCheck::Level;
using Issue = PresetNameCheck::Issue;

} // namespace

TEST_CASE("validate_preset_name keeps the Save preset dialog's rules", "[PresetRename]")
{
    ScopedTemporaryDir temp;
    write_family(temp.root);
    TestFilaments filaments;
    filaments.load_user(temp.root);
    REQUIRE(filaments.find_preset("Root", false) != nullptr);

    auto issue = [&](const std::string &name, PresetNameUse use = PresetNameUse::Save) { return validate_preset_name(filaments, name, "Root", use).issue; };
    CHECK(issue("A good name") == Issue::None);
    CHECK(validate_preset_name(filaments, "A good name", "Root").level == Level::Ok);
    for (const char c : std::string(preset_name_illegal_characters()))
        CHECK(issue(std::string("Bad") + c + "Name") == Issue::IllegalCharacters);
    CHECK(issue("PLA " + PresetCollection::get_suffix_modified()) == Issue::IllegalSuffix);
    CHECK(issue("Default Filament") == Issue::Reserved);
    CHECK(issue("Default Setting") == Issue::Reserved);
    CHECK(issue("Default Printer") == Issue::Reserved);
    CHECK(issue("System PLA @BBL X1C") == Issue::SystemExists);
    CHECK(issue("") == Issue::Empty);
    CHECK(issue(" Leading") == Issue::LeadingSpace);
    CHECK(issue("Trailing ") == Issue::TrailingSpace);
    CHECK(issue("System PLA") == Issue::AliasName);
    // The preset's own name is not "taken".
    CHECK(issue("Root") == Issue::None);
}

TEST_CASE("validate_preset_name: saving over a user preset warns, renaming onto one is refused", "[PresetRename]")
{
    ScopedTemporaryDir temp;
    write_family(temp.root);
    TestFilaments filaments;
    filaments.load_user(temp.root);

    const PresetNameCheck save = validate_preset_name(filaments, "Zebra", "Root", PresetNameUse::Save);
    CHECK(save.level == Level::Warning);
    CHECK(save.ok());
    CHECK((save.issue == Issue::Exists || save.issue == Issue::ExistsIncompatible));
    const PresetNameCheck rename = validate_preset_name(filaments, "Zebra", "Root", PresetNameUse::Rename);
    CHECK(rename.level == Level::Invalid);
    CHECK_FALSE(rename.ok());
    CHECK(rename.issue == save.issue);
}

TEST_CASE("rename_user_preset moves the file and re-points the presets that inherit it", "[PresetRename]")
{
    ScopedTemporaryDir temp;
    write_family(temp.root);
    TestFilaments filaments;
    filaments.load_user(temp.root);
    const Preset *root = filaments.find_preset("Root", false);
    REQUIRE(root != nullptr);
    const std::string old_file = root->file;
    REQUIRE(fs::exists(old_file));

    std::string error;
    REQUIRE(filaments.rename_user_preset("Root", "Root Renamed", &error));
    CHECK(error.empty());
    CHECK(filaments.find_preset("Root", false) == nullptr);
    const Preset *renamed = filaments.find_preset("Root Renamed", false);
    REQUIRE(renamed != nullptr);
    CHECK(renamed->filament_id == "Proot");
    CHECK(renamed->config.option<ConfigOptionInts>("nozzle_temperature")->values.front() == 233);
    CHECK(renamed->config.option<ConfigOptionStrings>("filament_settings_id")->values.front() == "Root Renamed");
    CHECK(fs::exists(renamed->file));
    CHECK(fs::path(renamed->file).filename().string() == "Root Renamed.json");
    CHECK(fs::path(renamed->file).parent_path() == fs::path(old_file).parent_path()); // same folder (base/)
    CHECK_FALSE(fs::exists(old_file));
    CHECK_FALSE(fs::exists(fs::path(old_file).replace_extension(".info")));

    // The child points at the new name, in memory and on disk.
    const Preset *child = filaments.find_preset("Root Child", false);
    REQUIRE(child != nullptr);
    CHECK(child->inherits() == "Root Renamed");
    CHECK(read_file(child->file).find("Root Renamed") != std::string::npos);
    CHECK(filaments.user_presets_inheriting("Root Renamed") == std::vector<std::string>{"Root Child"});
    CHECK(filaments.user_presets_inheriting("Root").empty());

    // A fresh start loads the renamed preset and its child, nothing under the old name.
    TestFilaments reloaded;
    reloaded.load_user(temp.root);
    CHECK(reloaded.find_preset("Root", false) == nullptr);
    REQUIRE(reloaded.find_preset("Root Renamed", false) != nullptr);
    const Preset *reloaded_child = reloaded.find_preset("Root Child", false);
    REQUIRE(reloaded_child != nullptr);
    CHECK(reloaded_child->inherits() == "Root Renamed");
    CHECK(reloaded_child->config.option<ConfigOptionInts>("nozzle_temperature")->values.front() == 240);
}

TEST_CASE("rename_user_preset keeps the collection sorted and the selection on its preset", "[PresetRename]")
{
    ScopedTemporaryDir temp;
    write_family(temp.root);
    TestFilaments filaments;
    filaments.load_user(temp.root);

    REQUIRE(filaments.select_preset_by_name("Zebra", true));
    // Another preset moves past the selected one in the sorted list.
    REQUIRE(filaments.rename_user_preset("My PLA @BBL X1C", "Zz PLA @BBL X1C"));
    CHECK(filaments.get_selected_preset_name() == "Zebra");
    CHECK(filaments.get_edited_preset().name == "Zebra");
    for (const Preset &p : filaments.get_presets())
        CHECK(filaments.find_preset(p.name, false) == &p); // findable by the sorted lookup
    CHECK(filaments.find_preset("Zz PLA @BBL X1C", false)->filament_id == "Pmy1");
    CHECK(filaments.find_preset("Zz PLA @BBL X1C", false)->inherits() == "System PLA @BBL X1C");

    // The selected preset itself: the selection and the edited copy follow, edits kept.
    filaments.get_edited_preset().config.option<ConfigOptionInts>("nozzle_temperature")->values.front() = 199;
    REQUIRE(filaments.rename_user_preset("Zebra", "Aardvark"));
    CHECK(filaments.get_selected_preset_name() == "Aardvark");
    CHECK(filaments.get_edited_preset().name == "Aardvark");
    CHECK(filaments.get_edited_preset().config.option<ConfigOptionInts>("nozzle_temperature")->values.front() == 199);
    CHECK(filaments.get_selected_preset().name == "Aardvark");
}

TEST_CASE("rename_user_preset refuses what it must not rename", "[PresetRename]")
{
    ScopedTemporaryDir temp;
    write_family(temp.root);
    TestFilaments filaments;
    filaments.load_user(temp.root);
    const size_t count = filaments.size();

    std::string error;
    CHECK_FALSE(filaments.rename_user_preset("Root", "Zebra", &error)); // taken
    CHECK_FALSE(error.empty());
    CHECK_FALSE(filaments.rename_user_preset("System PLA @BBL X1C", "Mine")); // system
    CHECK_FALSE(filaments.rename_user_preset("Default Filament", "Mine"));    // default
    CHECK_FALSE(filaments.rename_user_preset("No such preset", "Mine"));
    CHECK_FALSE(filaments.rename_user_preset("Root", "Root"));
    CHECK(filaments.size() == count);
    CHECK(filaments.find_preset("Root", false) != nullptr);
    CHECK(fs::exists(filaments.find_preset("Root", false)->file));
}

TEST_CASE("rename_user_preset: a change of letter case keeps the file", "[PresetRename]")
{
    ScopedTemporaryDir temp;
    write_family(temp.root);
    TestFilaments filaments;
    filaments.load_user(temp.root);
    REQUIRE(filaments.rename_user_preset("Zebra", "ZEBRA"));
    const Preset *renamed = filaments.find_preset("ZEBRA", false);
    REQUIRE(renamed != nullptr);
    CHECK(fs::exists(renamed->file));
    TestFilaments reloaded;
    reloaded.load_user(temp.root);
    CHECK(reloaded.find_preset("ZEBRA", false) != nullptr);
    CHECK(reloaded.find_preset("Zebra", false) == nullptr);
}

TEST_CASE("plan_preset_rename: a filament family keeps each printer part", "[PresetRename]")
{
    ScopedTemporaryDir temp;
    write_family(temp.root);
    TestFilaments filaments;
    filaments.load_user(temp.root);
    const std::vector<std::string> family = {"My PLA @BBL P1S", "My PLA @BBL X1C"};

    std::string base, suffix;
    split_preset_suffix("My PLA @BBL X1C", base, suffix);
    CHECK(base == "My PLA");
    CHECK(suffix == " @BBL X1C");
    split_preset_suffix("Root", base, suffix);
    CHECK(base == "Root");
    CHECK(suffix.empty());

    PresetRenamePlan all = plan_preset_rename(filaments, "My PLA @BBL X1C", "Our PLA", family);
    CHECK(all.check.ok());
    REQUIRE(all.renames.size() == 2);
    CHECK(all.renames[0] == std::make_pair(std::string("My PLA @BBL X1C"), std::string("Our PLA @BBL X1C"))); // the row first
    CHECK(all.renames[1] == std::make_pair(std::string("My PLA @BBL P1S"), std::string("Our PLA @BBL P1S")));

    PresetRenamePlan one = plan_preset_rename(filaments, "My PLA @BBL X1C", "Our PLA", {});
    REQUIRE(one.renames.size() == 1);
    CHECK(one.renames[0].second == "Our PLA @BBL X1C");

    CHECK(plan_preset_rename(filaments, "My PLA @BBL X1C", "My PLA", family).renames.empty()); // nothing changes
    CHECK(plan_preset_rename(filaments, "My PLA @BBL X1C", "Our @PLA", family).check.issue == Issue::PrinterSuffix);
    CHECK(plan_preset_rename(filaments, "My PLA @BBL X1C", "", family).check.issue == Issue::Empty);
    CHECK(plan_preset_rename(filaments, "My PLA @BBL X1C", "Our PLA ", family).check.issue == Issue::TrailingSpace);
    CHECK(plan_preset_rename(filaments, "My PLA @BBL X1C", "Our|PLA", family).check.issue == Issue::IllegalCharacters);
    // Onto a system preset's name: refused, and the plan names it.
    const PresetRenamePlan clash = plan_preset_rename(filaments, "My PLA @BBL X1C", "System PLA", family);
    CHECK_FALSE(clash.check.ok());
    CHECK(clash.check.issue == Issue::SystemExists);
    CHECK(clash.checked_name == "System PLA @BBL X1C");
    CHECK(clash.renames.empty());

    // A name without " @" (a process, a standalone filament) is edited whole by the shared rules.
    CHECK(plan_preset_rename(filaments, "Root", "Zebra", {}).check.level == Level::Invalid);
    const PresetRenamePlan whole = plan_preset_rename(filaments, "Root", "New Root", {});
    REQUIRE(whole.renames.size() == 1);
    CHECK(whole.renames[0].second == "New Root");

    // Carried out: both files renamed, filament_id kept.
    for (const auto &[from, to] : all.renames)
        REQUIRE(filaments.rename_user_preset(from, to));
    CHECK(filaments.find_preset("Our PLA @BBL X1C", false)->filament_id == "Pmy1");
    CHECK(filaments.find_preset("Our PLA @BBL P1S", false)->filament_id == "Pmy1");
    CHECK(filaments.find_preset("My PLA @BBL X1C", false) == nullptr);
}

TEST_CASE("Preset manager rows: user filament families and rename / delete flags", "[PresetRename]")
{
    auto item = [](const std::string &name, const std::string &filament_id, bool system) {
        PresetManagerItem it;
        it.name         = name;
        it.alias        = system ? name.substr(0, name.find(" @")) : std::string();
        it.vendor_id    = system ? "BBL" : "";
        it.is_system    = system;
        it.filament_id  = filament_id;
        it.favorite_key = preset_manager_favorite_key(name, it.alias, system);
        return it;
    };
    const PresetManagerModel model = build_preset_manager_model(PresetManagerKind::Filament,
        {item("My PLA @BBL X1C", "P1", false), item("My PLA @BBL P1S", "P1", false), item("My PLA @Other", "P2", false),
         item("Solo", "P3", false), item("Bambu PLA @BBL X1C", "GFA00", true)});
    const PresetManagerRow *x1c = model.find("u|My PLA @BBL X1C");
    REQUIRE(x1c != nullptr);
    CHECK(x1c->base == "My PLA");
    CHECK(x1c->suffix == " @BBL X1C");
    CHECK(x1c->family == std::vector<std::string>{"My PLA @BBL P1S", "My PLA @BBL X1C"});
    CHECK(model.find("u|My PLA @Other")->family == std::vector<std::string>{"My PLA @Other"}); // another filament_id
    CHECK(model.find("u|Solo")->suffix.empty());

    const nlohmann::json j = preset_manager_rows_json(model, {}, "");
    for (const nlohmann::json &r : j["rows"]) {
        const bool user = r["source"] == "User";
        CHECK(r["can_rename"] == user);
        CHECK(r["can_delete"] == user);
        CHECK(r.contains("base"));
        CHECK(r.contains("suffix"));
        CHECK(r["family"].is_array());
    }
}

TEST_CASE("preset_usage and repoint_preset_references: filament slots", "[PresetRename]")
{
    PresetBundle bundle;
    bundle.filament_presets = {"A PLA", "B PETG", "A PLA"};
    PresetBundle::PresetUsage usage = bundle.preset_usage(Preset::TYPE_FILAMENT, "A PLA");
    CHECK(usage.filament_slots == std::vector<size_t>{0, 2});
    CHECK(usage.in_use());
    CHECK_FALSE(bundle.preset_usage(Preset::TYPE_FILAMENT, "C ABS").in_use());

    CHECK(bundle.repoint_preset_references(Preset::TYPE_FILAMENT, "A PLA", "A2 PLA"));
    CHECK(bundle.filament_presets == std::vector<std::string>{"A2 PLA", "B PETG", "A2 PLA"});
    // A delete leaves the slots to the caller (they move to the next visible preset).
    CHECK_FALSE(bundle.repoint_preset_references(Preset::TYPE_FILAMENT, "B PETG", ""));
    CHECK(bundle.filament_presets[1] == "B PETG");
    CHECK_FALSE(bundle.repoint_preset_references(Preset::TYPE_FILAMENT, "Nothing", "Else"));
}

TEST_CASE("preset_usage and repoint_preset_references: per-nozzle processes", "[PresetRename]")
{
    PresetBundle bundle;
    bundle.project_config.set_key_value("nozzle_process_presets", new ConfigOptionStrings({"Fast", "", "Fast"}));
    const std::string slot_id = nozzle_tool_id(0) + ":0.4";
    bundle.project_config.set_key_value("nozzle_slot_process_presets", new ConfigOptionStrings({slot_id + "\nFast"}));

    PresetBundle::PresetUsage usage = bundle.preset_usage(Preset::TYPE_PRINT, "Fast");
    CHECK(usage.nozzles == std::vector<size_t>{0, 2});
    CHECK(usage.nozzle_slots == std::vector<std::string>{slot_id});
    CHECK(usage.in_use());

    // Renamed: every choice follows.
    CHECK(bundle.repoint_preset_references(Preset::TYPE_PRINT, "Fast", "Faster"));
    CHECK(bundle.project_config.option<ConfigOptionStrings>("nozzle_process_presets")->values == std::vector<std::string>{"Faster", "", "Faster"});
    CHECK(nozzle_slot_process_names(bundle.project_config).at(slot_id) == "Faster");
    CHECK_FALSE(bundle.preset_usage(Preset::TYPE_PRINT, "Fast").in_use());

    // Deleted: the nozzles go back to the plate's process (an all-empty list is stored as no list).
    CHECK(bundle.repoint_preset_references(Preset::TYPE_PRINT, "Faster", ""));
    CHECK(bundle.project_config.option<ConfigOptionStrings>("nozzle_process_presets")->values.empty());
    CHECK(nozzle_slot_process_names(bundle.project_config).empty());
    CHECK_FALSE(bundle.preset_usage(Preset::TYPE_PRINT, "Faster").in_use());
}
