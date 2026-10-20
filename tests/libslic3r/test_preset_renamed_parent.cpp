// User presets whose system parent was renamed (renamed_from) or had its printer list narrowed by a profile
// update. OrcaSlicer #16367 (legacy_compatible_printers.json and the filament preset migration), adapted to
// this tree, plus the Edge re-parenting of user and project presets onto a renamed parent.
#include <catch2/catch.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/miniz_extension.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = boost::filesystem;
using namespace Slic3r;

namespace {

// A scratch directory that removes itself. Never points at a user's data directory.
struct ScopedTemporaryDir
{
    fs::path root;
    ScopedTemporaryDir()
    {
        root = fs::temp_directory_path() / "snorca_tests" / fs::unique_path("renamed_parent_%%%%%%%%");
        fs::create_directories(root);
    }
    ~ScopedTemporaryDir()
    {
        boost::system::error_code ec;
        fs::remove_all(root, ec);
    }
    const fs::path &path() const { return root; }
};

// Points data_dir() somewhere else for the scope.
struct ScopedDataDir
{
    std::string saved;
    explicit ScopedDataDir(const fs::path &dir) : saved(data_dir()) { set_data_dir(dir.string()); }
    ~ScopedDataDir() { set_data_dir(saved); }
};

std::string read_file(const fs::path &file)
{
    boost::nowide::ifstream in(file.string(), std::ios::binary);
    std::stringstream       ss;
    ss << in.rdbuf();
    return ss.str();
}

// A filament collection that exposes the rename map update, which PresetBundle runs after loading system presets.
struct RenameTestFilaments : public PresetCollection
{
    RenameTestFilaments()
        : PresetCollection(Preset::TYPE_FILAMENT, Preset::filament_options(),
                           static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults()), "Default Filament")
    {}
    using PresetCollection::update_map_system_profile_renamed;
};

// A system filament preset that lists the given printers and is known by the given former names.
void add_system_filament(PresetCollection &filaments, const std::string &name, std::vector<std::string> printers,
                         std::vector<std::string> renamed_from = {})
{
    DynamicPrintConfig config = filaments.default_preset().config;
    config.option<ConfigOptionStrings>("compatible_printers", true)->values = std::move(printers);
    filaments.load_preset("", name, config, false);
    // load_preset() inserts into a deque: look the preset up again rather than keep its reference.
    Preset *preset       = filaments.find_preset(name, false, true);
    preset->is_system    = true;
    preset->is_visible   = true;
    preset->renamed_from = std::move(renamed_from);
}

// A user filament preset file as the settings tab saves it: only its diff, so unless extra sets
// compatible_printers, the printer list comes from the parent.
fs::path write_user_filament(const fs::path &dir, const std::string &name, const std::string &inherits, const std::string &extra = {})
{
    const fs::path file = dir / PRESET_FILAMENT_NAME / (name + ".json");
    fs::create_directories(file.parent_path());
    std::ofstream(file.string()) << R"({"type":"filament","name":")" << name << R"(","from":"User","version":"1.0.0","inherits":")"
                                 << inherits << '"' << extra << '}';
    return file;
}

void load_user_filaments(PresetCollection &filaments, const fs::path &dir)
{
    PresetsConfigSubstitutions substitutions;
    filaments.load_presets(dir.string(), PRESET_FILAMENT_NAME, substitutions, ForwardCompatibilitySubstitutionRule::EnableSilent);
}

const std::vector<std::string> &printers_of(const Preset &preset)
{
    static const std::vector<std::string> none;
    const auto *opt = preset.config.option<ConfigOptionStrings>("compatible_printers");
    return opt ? opt->values : none;
}

const std::string k1_parent     = "Generic PETG @Creality K1-all";
const std::string k1_old_parent = "Creality Generic PETG @K1-all";

} // namespace

TEST_CASE("User filaments made against an older release that lost printers their narrowed system parent listed are found",
          "[Preset][RenamedParent]")
{
    ScopedTemporaryDir  temp_dir;
    RenameTestFilaments filaments;
    add_system_filament(filaments, k1_parent, { "Creality K1C 0.6 nozzle" }, { k1_old_parent });
    filaments.update_map_system_profile_renamed();
    write_user_filament(temp_dir.path(), "Old PETG", k1_old_parent);
    write_user_filament(temp_dir.path(), "New PETG", k1_parent);
    write_user_filament(temp_dir.path(), "Own list PETG", k1_old_parent, R"(,"compatible_printers":["Creality K1C 0.6 nozzle"])");
    write_user_filament(temp_dir.path(), "Narrowed PETG", k1_old_parent, R"(,"compatible_printers":["Creality K1C 0.8 nozzle"])");
    write_user_filament(temp_dir.path(), "Empty list PETG", k1_old_parent, R"(,"compatible_printers":[])");
    load_user_filaments(filaments, temp_dir.path());

    // The map is keyed by the parent's old name, which only the file still holds: a preset made against the
    // parent's current name is left alone, as is one whose file sets compatible_printers at all, whether to the
    // parent's list, to other printers or to an empty list. A printer the parent still lists is not reported again.
    const std::vector<LegacyUserFilamentPreset> legacy_presets =
        filaments.legacy_user_filament_presets({ { k1_old_parent, { "Creality K1C 0.4 nozzle", "Creality K1C 0.6 nozzle" } } });
    REQUIRE(legacy_presets.size() == 1);
    CHECK(legacy_presets[0].preset == "Old PETG");
    CHECK(legacy_presets[0].parent == k1_parent);
    CHECK(legacy_presets[0].inherits == k1_old_parent);
    CHECK(legacy_presets[0].printers == std::vector<std::string>{ "Creality K1C 0.4 nozzle" });
    CHECK(legacy_presets[0].file == (temp_dir.path() / PRESET_FILAMENT_NAME / "Old PETG.json").string());
    CHECK(legacy_presets[0].compatible_printers == std::vector<std::string>{ "Creality K1C 0.6 nozzle", "Creality K1C 0.4 nozzle" });
}

TEST_CASE("Migrating a user filament backs up the user folder to a zip and writes the legacy printers to its file",
          "[Preset][RenamedParent]")
{
    ScopedTemporaryDir temp_dir;
    ScopedDataDir      data(temp_dir.path());
    const fs::path     user_dir = temp_dir.path() / PRESET_USER_DIR / "default";
    const fs::path     file     = write_user_filament(user_dir, "My PETG", k1_parent);
    const std::string  original = read_file(file);
    PresetBundle       bundle;
    add_system_filament(bundle.filaments, k1_parent, { "Creality K1C 0.6 nozzle" });
    load_user_filaments(bundle.filaments, user_dir);
    const std::map<std::string, std::vector<std::string>> map{ { k1_parent, { "Creality K1C 0.4 nozzle" } } };
    const std::vector<LegacyUserFilamentPreset> legacy_presets = bundle.filaments.legacy_user_filament_presets(map);
    REQUIRE(legacy_presets.size() == 1);

    const PresetBundle::LegacyUserFilamentPresetMigration migration = bundle.migrate_legacy_user_filament_presets(legacy_presets);
    const std::string &backup = migration.backup;
    CHECK(migration.locked);
    REQUIRE_FALSE(backup.empty());
    CHECK(std::regex_match(fs::path(backup).filename().string(), std::regex("filament_preset_migration_\\d{8}T\\d{6}Z_backup\\.zip")));
    CHECK(migration.failed.empty());
    const fs::path extracted = temp_dir.path() / "extracted";
    std::string    err;
    REQUIRE(extract_archive_confined(fs::path(backup), extracted, err));
    CHECK(read_file(extracted / PRESET_USER_DIR / "default" / PRESET_FILAMENT_NAME / "My PETG.json") == original);

    // Reloaded from disk, the preset carries both printers itself and needs no further migration.
    PresetBundle reloaded;
    add_system_filament(reloaded.filaments, k1_parent, { "Creality K1C 0.6 nozzle" });
    load_user_filaments(reloaded.filaments, user_dir);
    const std::vector<std::string> expected{ "Creality K1C 0.6 nozzle", "Creality K1C 0.4 nozzle" };
    REQUIRE(reloaded.filaments.find_preset("My PETG") != nullptr);
    CHECK(printers_of(*reloaded.filaments.find_preset("My PETG")) == expected);
    CHECK(reloaded.filaments.legacy_user_filament_presets(map).empty());
    // The file now sets its own list, so the migration found earlier no longer applies to it.
    CHECK(reloaded.migrate_legacy_user_filament_presets(legacy_presets).failed == std::vector<std::string>{ "My PETG" });
}

TEST_CASE("Migrating a user filament keeps the old name of its renamed parent in the file", "[Preset][RenamedParent]")
{
    ScopedTemporaryDir  temp_dir;
    RenameTestFilaments filaments;
    add_system_filament(filaments, k1_parent, { "Creality K1C 0.6 nozzle" }, { k1_old_parent });
    filaments.update_map_system_profile_renamed();
    const fs::path file = write_user_filament(temp_dir.path(), "My PETG", k1_old_parent);
    load_user_filaments(filaments, temp_dir.path());

    const std::vector<LegacyUserFilamentPreset> legacy_presets =
        filaments.legacy_user_filament_presets({ { k1_old_parent, { "Creality K1C 0.4 nozzle" } } });
    REQUIRE(legacy_presets.size() == 1);
    REQUIRE(PresetCollection::write_legacy_user_filament_preset(legacy_presets[0]));
    // An older release knows the parent only by its old name.
    const std::string saved = read_file(file);
    CHECK(saved.find("\"inherits\": \"" + k1_old_parent + "\"") != std::string::npos);
    CHECK(saved.find("Creality K1C 0.4 nozzle") != std::string::npos);
}

TEST_CASE("Migrating a user filament leaves a file another instance changed after it was found", "[Preset][RenamedParent]")
{
    ScopedTemporaryDir  temp_dir;
    RenameTestFilaments filaments;
    add_system_filament(filaments, k1_parent, { "Creality K1C 0.6 nozzle" });
    const fs::path file = write_user_filament(temp_dir.path(), "My PETG", k1_parent);
    load_user_filaments(filaments, temp_dir.path());
    const std::vector<LegacyUserFilamentPreset> legacy_presets =
        filaments.legacy_user_filament_presets({ { k1_parent, { "Creality K1C 0.4 nozzle" } } });
    REQUIRE(legacy_presets.size() == 1);
    CHECK(legacy_presets[0].inherits == k1_parent);

    SECTION("to set its own printers") {
        write_user_filament(temp_dir.path(), "My PETG", k1_parent, R"(,"compatible_printers":["Creality K1C 0.8 nozzle"])");
    }
    SECTION("to inherit another parent") {
        write_user_filament(temp_dir.path(), "My PETG", "Generic PETG @System");
    }
    const std::string edited = read_file(file);
    CHECK_FALSE(PresetCollection::write_legacy_user_filament_preset(legacy_presets[0]));
    CHECK(read_file(file) == edited);
}

TEST_CASE("Migrating user filaments backs up and writes nothing while it can not take the user presets lock", "[Preset][RenamedParent]")
{
    ScopedTemporaryDir temp_dir;
    ScopedDataDir      data(temp_dir.path());
    const fs::path     user_dir = temp_dir.path() / PRESET_USER_DIR / "default";
    const fs::path     file     = write_user_filament(user_dir, "My PETG", k1_parent);
    const std::string  original = read_file(file);
    PresetBundle       bundle;
    add_system_filament(bundle.filaments, k1_parent, { "Creality K1C 0.6 nozzle" });
    load_user_filaments(bundle.filaments, user_dir);
    const std::vector<LegacyUserFilamentPreset> legacy_presets =
        bundle.filaments.legacy_user_filament_presets({ { k1_parent, { "Creality K1C 0.4 nozzle" } } });
    REQUIRE(legacy_presets.size() == 1);

    // A folder in the lock file's place can not be opened as the lock; a fresh data dir, so no earlier
    // guard has the lock file open. The backup is taken before the lock, then one hold covers every write,
    // so a lock held elsewhere is one outcome, not a failure per preset.
    const fs::path other_data = temp_dir.path() / "data";
    fs::create_directories(other_data / PRESET_USER_DIR);
    ScopedDataDir data_for_migration(other_data);
    fs::create_directories(fs::path(user_presets_lock_path()));
    const PresetBundle::LegacyUserFilamentPresetMigration migration = bundle.migrate_legacy_user_filament_presets(legacy_presets);
    CHECK_FALSE(migration.backup.empty());
    CHECK_FALSE(migration.locked);
    CHECK(migration.failed.empty());
    CHECK(read_file(file) == original);
}

TEST_CASE("Applying a migration adds the printers to the list the preset holds now and keeps a pending cloud sync",
          "[Preset][RenamedParent]")
{
    ScopedTemporaryDir  temp_dir;
    RenameTestFilaments filaments;
    add_system_filament(filaments, k1_parent, { "Creality K1C 0.6 nozzle" });
    write_user_filament(temp_dir.path(), "My PETG", k1_parent);
    load_user_filaments(filaments, temp_dir.path());
    const std::vector<LegacyUserFilamentPreset> legacy_presets =
        filaments.legacy_user_filament_presets({ { k1_parent, { "Creality K1C 0.4 nozzle" } } });
    REQUIRE(legacy_presets.size() == 1);

    // Between detection and the write, the cloud sync gave the preset a newer list that is still to be saved.
    Preset *preset = filaments.find_preset("My PETG", false, true);
    preset->config.option<ConfigOptionStrings>("compatible_printers", true)->values = { "Creality K1C 0.8 nozzle" };
    preset->setting_id = "GFSA01";
    preset->sync_info  = "save";
    filaments.apply_legacy_user_filament_presets(legacy_presets);
    CHECK(printers_of(*preset) == std::vector<std::string>{ "Creality K1C 0.8 nozzle", "Creality K1C 0.4 nozzle" });
    CHECK(preset->sync_info == "save");
}

TEST_CASE("Migrating the selected user filament keeps a printer list edit pending in the settings tab", "[Preset][RenamedParent]")
{
    ScopedTemporaryDir  temp_dir;
    RenameTestFilaments filaments;
    add_system_filament(filaments, k1_parent, { "Creality K1C 0.6 nozzle" });
    write_user_filament(temp_dir.path(), "My PETG", k1_parent);
    load_user_filaments(filaments, temp_dir.path());
    REQUIRE(filaments.select_preset_by_name("My PETG", true));
    const std::vector<LegacyUserFilamentPreset> legacy_presets =
        filaments.legacy_user_filament_presets({ { k1_parent, { "Creality K1C 0.4 nozzle" } } });
    REQUIRE(legacy_presets.size() == 1);
    const std::vector<std::string> migrated{ "Creality K1C 0.6 nozzle", "Creality K1C 0.4 nozzle" };
    auto edited_printers = [&filaments]() -> std::vector<std::string> & {
        return filaments.get_edited_preset().config.option<ConfigOptionStrings>("compatible_printers", true)->values;
    };

    std::vector<std::string> expected_edited = migrated;
    bool                     expected_dirty  = false;
    SECTION("an unedited list follows the saved one") {}
    SECTION("an edited list stays as edited") {
        edited_printers() = { "Creality K1C 0.8 nozzle" };
        expected_edited   = edited_printers();
        expected_dirty    = true;
    }
    REQUIRE(PresetCollection::write_legacy_user_filament_preset(legacy_presets[0]));
    filaments.apply_legacy_user_filament_presets(legacy_presets);
    // find_preset() returns the edited copy of the selected preset unless asked for the real one.
    CHECK(printers_of(*filaments.find_preset("My PETG", false, true)) == migrated);
    CHECK(edited_printers() == expected_edited);
    // What marks the project as having unsaved preset changes.
    CHECK(filaments.saved_is_dirty() == expected_dirty);
}

// The shipped map: valid JSON with the shape the loader reads, every entry naming at least one printer.
TEST_CASE("The shipped legacy_compatible_printers.json has the shape the loader reads", "[Preset][RenamedParent]")
{
    const fs::path path = fs::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "info" / "legacy_compatible_printers.json";
    REQUIRE(fs::exists(path));
    const std::string text = read_file(path);
    std::map<std::string, std::vector<std::string>> presets;
    REQUIRE_NOTHROW(presets = nlohmann::json::parse(text).at("presets").get<std::map<std::string, std::vector<std::string>>>());
    for (const auto &[name, printers] : presets) {
        INFO(name);
        CHECK_FALSE(name.empty());
        CHECK_FALSE(printers.empty());
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Edge: a renamed parent is resolved on every path (user folder, project 3MF, save), while the preset keeps
// the old parent name, as upstream's migration does, so an older release sharing the data folder (a test copy
// beside the installed build) or opening the project still finds the parent it knows.
// ---------------------------------------------------------------------------------------------------------------

namespace {

// A renamed system parent that differs from the default preset, so a preset rebuilt on the default shows.
void add_renamed_parent(RenameTestFilaments &filaments)
{
    add_system_filament(filaments, k1_parent, { "Creality K1C 0.6 nozzle", "Creality K1C 0.8 nozzle" }, { k1_old_parent });
    filaments.find_preset(k1_parent, false, true)->config.option<ConfigOptionFloats>("filament_flow_ratio", true)->values = { 0.95 };
    filaments.update_map_system_profile_renamed();
}

double flow_ratio(const Preset &preset) { return preset.config.option<ConfigOptionFloats>("filament_flow_ratio")->values.front(); }
int    nozzle_temperature(const Preset &preset) { return preset.config.option<ConfigOptionInts>("nozzle_temperature")->values.front(); }

// A project-inside filament as the 3MF reader hands it over: only the keys it sets itself.
std::unique_ptr<Preset> project_filament(const std::string &name, const std::string &inherits)
{
    auto preset = std::make_unique<Preset>(Preset::TYPE_FILAMENT, name, false);
    preset->is_project_embedded = true;
    preset->config.set_key_value("inherits", new ConfigOptionString(inherits));
    preset->config.set_key_value("filament_settings_id", new ConfigOptionStrings({ name }));
    preset->config.set_key_value("nozzle_temperature", new ConfigOptionInts({ 245 }));
    return preset;
}

void load_project_filaments(PresetCollection &filaments, std::vector<Preset *> presets)
{
    PresetsConfigSubstitutions substitutions;
    filaments.load_project_embedded_presets(presets, PRESET_FILAMENT_NAME, substitutions, ForwardCompatibilitySubstitutionRule::EnableSilent);
}

} // namespace

TEST_CASE("A user filament on a renamed parent loads on the new parent", "[Preset][RenamedParent]")
{
    ScopedTemporaryDir  temp_dir;
    RenameTestFilaments filaments;
    add_renamed_parent(filaments);
    const fs::path    file   = write_user_filament(temp_dir.path(), "My PETG", k1_old_parent, R"(,"nozzle_temperature":["245"])");
    const std::string before = read_file(file);
    load_user_filaments(filaments, temp_dir.path());

    const Preset *preset = filaments.find_preset("My PETG", false, true);
    REQUIRE(preset != nullptr);
    CHECK(preset->inherits() == k1_old_parent);
    REQUIRE(filaments.get_preset_parent(*preset) != nullptr);
    CHECK(filaments.get_preset_parent(*preset)->name == k1_parent);
    // Its own value, and everything else (printers included) from the renamed parent.
    CHECK(nozzle_temperature(*preset) == 245);
    CHECK(flow_ratio(*preset) == Approx(0.95));
    CHECK(printers_of(*preset) == std::vector<std::string>{ "Creality K1C 0.6 nozzle", "Creality K1C 0.8 nozzle" });
    // Loading writes nothing: an older release sharing the data folder still reads the old parent name.
    CHECK(read_file(file) == before);
}

TEST_CASE("Saving a user filament on a renamed parent writes only its difference to that parent", "[Preset][RenamedParent]")
{
    ScopedTemporaryDir  temp_dir;
    RenameTestFilaments filaments;
    add_renamed_parent(filaments);
    const fs::path file = write_user_filament(temp_dir.path(), "My PETG", k1_old_parent, R"(,"nozzle_temperature":["245"])");
    load_user_filaments(filaments, temp_dir.path());
    REQUIRE(filaments.select_preset_by_name("My PETG", true));
    filaments.get_edited_preset().config.option<ConfigOptionInts>("nozzle_temperature")->values = { 250 };

    filaments.save_current_preset("My PETG");

    // Without the parent the whole config was written (save(nullptr)), freezing every inherited value.
    const nlohmann::json j = nlohmann::json::parse(read_file(file));
    CHECK(j.value("inherits", std::string()) == k1_old_parent);
    CHECK(j.at("nozzle_temperature") == nlohmann::json::array({ "250" }));
    CHECK_FALSE(j.contains("filament_flow_ratio"));
    CHECK_FALSE(j.contains("compatible_printers"));
}

TEST_CASE("A project filament on a renamed parent loads on the new parent", "[Preset][RenamedParent][ProjectPreset]")
{
    RenameTestFilaments filaments;
    add_renamed_parent(filaments);
    std::unique_ptr<Preset> old_name = project_filament("Project PETG", k1_old_parent);
    std::unique_ptr<Preset> new_name = project_filament("Project PETG 2", k1_parent);
    load_project_filaments(filaments, { old_name.get(), new_name.get() });

    for (const std::string name : { "Project PETG", "Project PETG 2" }) {
        INFO(name);
        const Preset *preset = filaments.find_preset(name, false, true);
        REQUIRE(preset != nullptr);
        CHECK(preset->is_project_embedded);
        REQUIRE(filaments.get_preset_parent(*preset) != nullptr);
        CHECK(filaments.get_preset_parent(*preset)->name == k1_parent);
        CHECK(nozzle_temperature(*preset) == 245);
        // Not rebuilt on the default preset: the parent's values and its printers, not every printer.
        CHECK(flow_ratio(*preset) == Approx(0.95));
        CHECK(printers_of(*preset) == std::vector<std::string>{ "Creality K1C 0.6 nozzle", "Creality K1C 0.8 nozzle" });
    }
    // The project's next save stores it as its difference to the renamed parent, under the name it came with.
    Preset *saved = filaments.get_preset_differed_for_save(*filaments.find_preset("Project PETG", false, true));
    REQUIRE(saved != nullptr);
    CHECK(saved->config.opt_string("inherits") == k1_old_parent);
    CHECK_FALSE(saved->config.has("filament_flow_ratio"));
    delete saved;
}

TEST_CASE("A name nothing was renamed from still fails to load as before", "[Preset][RenamedParent]")
{
    ScopedTemporaryDir  temp_dir;
    RenameTestFilaments filaments;
    add_renamed_parent(filaments);
    write_user_filament(temp_dir.path(), "Orphan PETG", "Some Vendor PETG @Gone");
    load_user_filaments(filaments, temp_dir.path());
    CHECK(filaments.find_preset("Orphan PETG", false, true) == nullptr);
}

// ---------------------------------------------------------------------------------------------------------------
// The shipped renames: Panchroma PLA Stain -> Satin (#491 for @System, this PR for the BBL vendor's stale
// "Stain @BBL ..." copies) and the U1 renamed_from coverage (v2.4.0).
// ---------------------------------------------------------------------------------------------------------------

namespace {

const std::string profiles_dir() { return (fs::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string(); }

// A vendor bundle from this tree's resources/profiles (OrcaFilamentLibrary behind it), with the system maps the
// app builds after loading (renamed_from, aliases).
struct ShippedVendor
{
    ScopedTemporaryDir            scratch;
    ScopedDataDir                 data{ scratch.path() };
    std::unique_ptr<PresetBundle> library;
    std::unique_ptr<PresetBundle> bundle;

    explicit ShippedVendor(const std::string &vendor)
    {
        library = std::make_unique<PresetBundle>();
        library->load_vendor_configs_from_json(profiles_dir(), PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem,
                                               ForwardCompatibilitySubstitutionRule::EnableSilent);
        PresetBundle *target = library.get();
        if (vendor != PresetBundle::ORCA_FILAMENT_LIBRARY) {
            bundle = std::make_unique<PresetBundle>();
            bundle->load_vendor_configs_from_json(profiles_dir(), vendor, PresetBundle::LoadSystem,
                                                  ForwardCompatibilitySubstitutionRule::EnableSilent, library.get());
            target = bundle.get();
        }
        // load_installed_printers() is the public entry that runs update_system_maps().
        AppConfig app_config;
        target->load_installed_printers(app_config);
    }
    PresetBundle &get() { return bundle ? *bundle : *library; }
};

} // namespace

TEST_CASE("A filament made on Panchroma PLA Stain @System survives the rename to Satin", "[Preset][RenamedParent][Profiles]")
{
    // #491 renamed the library's "Panchroma PLA Stain @System" to "Panchroma PLA Satin @System" (renamed_from).
    ShippedVendor     shipped(PresetBundle::ORCA_FILAMENT_LIBRARY);
    PresetCollection &filaments = shipped.get().filaments;
    const std::string stain     = "Panchroma PLA Stain @System";
    const std::string satin     = "Panchroma PLA Satin @System";
    REQUIRE(filaments.find_preset(satin, false, true) != nullptr);
    REQUIRE(filaments.find_preset(stain, false, true) == nullptr);

    SECTION("in the user presets folder") {
        // As 2.4.5.5 saved it: only its difference, against the name the parent had then.
        ScopedTemporaryDir user;
        write_user_filament(user.path(), "My Panchroma", stain, R"(,"nozzle_temperature":["225"])");
        load_user_filaments(filaments, user.path());
    }
    SECTION("inside a project") {
        std::unique_ptr<Preset> preset = project_filament("My Panchroma", stain);
        preset->config.option<ConfigOptionInts>("nozzle_temperature")->values = { 225 };
        load_project_filaments(filaments, { preset.get() });
    }

    const Preset *preset = filaments.find_preset("My Panchroma", false, true);
    REQUIRE(preset != nullptr);
    REQUIRE(filaments.get_preset_parent(*preset) != nullptr);
    CHECK(filaments.get_preset_parent(*preset)->name == satin);
    CHECK(preset->inherits() == stain);
    CHECK(nozzle_temperature(*preset) == 225);
    // The parent's own values, not the defaults.
    CHECK(flow_ratio(*preset) == Approx(0.95));
    CHECK(preset->config.option<ConfigOptionFloats>("filament_max_volumetric_speed")->values.front() == Approx(16.));
    CHECK(preset->config.option<ConfigOptionStrings>("filament_vendor")->values == std::vector<std::string>{ "Polymaker" });
    // Like its parent, listed for every printer.
    CHECK(printers_of(*preset) == printers_of(*filaments.find_preset(satin, false, true)));
}

TEST_CASE("A filament made on a BBL Panchroma PLA Stain preset moves to the library's Satin preset",
          "[Preset][RenamedParent][Profiles]")
{
    // The BBL vendor shipped stale "Panchroma PLA Stain @BBL A1/A1M/P1P/X1/X1C" copies next to the library's
    // "Panchroma PLA Satin @BBL ..." presets, which already list the Stain names in renamed_from. They are gone:
    // a preset made on one now takes the library preset of the right name, with its values.
    ShippedVendor     shipped(PresetBundle::ORCA_FILAMENT_LIBRARY);
    PresetCollection &filaments = shipped.get().filaments;
    const std::string stain     = "Panchroma PLA Stain @BBL A1";
    const std::string satin     = "Panchroma PLA Satin @BBL A1";
    REQUIRE(filaments.find_preset(satin, false, true) != nullptr);

    SECTION("in the user presets folder") {
        ScopedTemporaryDir user;
        write_user_filament(user.path(), "My A1 Panchroma", stain, R"(,"nozzle_temperature":["225"])");
        load_user_filaments(filaments, user.path());
    }
    SECTION("inside a project") {
        std::unique_ptr<Preset> preset = project_filament("My A1 Panchroma", stain);
        preset->config.option<ConfigOptionInts>("nozzle_temperature")->values = { 225 };
        load_project_filaments(filaments, { preset.get() });
    }

    const Preset *preset = filaments.find_preset("My A1 Panchroma", false, true);
    REQUIRE(preset != nullptr);
    REQUIRE(filaments.get_preset_parent(*preset) != nullptr);
    CHECK(filaments.get_preset_parent(*preset)->name == satin);
    CHECK(nozzle_temperature(*preset) == 225);
    CHECK(preset->config.option<ConfigOptionInts>("cool_plate_temp")->values.front() == 60);
    CHECK(preset->config.option<ConfigOptionStrings>("filament_vendor")->values == std::vector<std::string>{ "Polymaker" });
    CHECK(printers_of(*preset) == std::vector<std::string>{ "Bambu Lab A1 0.4 nozzle", "Bambu Lab A1 0.6 nozzle", "Bambu Lab A1 0.8 nozzle" });
}

TEST_CASE("No shipped vendor still has a Panchroma PLA Stain preset", "[Preset][RenamedParent][Profiles]")
{
    // The typo lives on only in renamed_from. The vendor index lists every preset the app loads.
    for (const auto &entry : fs::directory_iterator(fs::path(profiles_dir()))) {
        if (entry.path().extension() != ".json")
            continue;
        INFO(entry.path().filename().string());
        const nlohmann::json index = nlohmann::json::parse(read_file(entry.path()), nullptr, false);
        if (!index.is_object() || !index.contains("filament_list"))
            continue;
        for (const auto &item : index.at("filament_list"))
            CHECK(item.value("name", std::string()).find("Panchroma PLA Stain") == std::string::npos);
    }
}

TEST_CASE("Every renamed U1 filament takes over user presets made on each of its old names", "[Preset][RenamedParent][Profiles]")
{
    ShippedVendor     shipped(PresetBundle::SM_BUNDLE);
    PresetCollection &filaments = shipped.get().filaments;

    // Old name -> the preset that lists it in renamed_from. Only instantiated (selectable) presets enter the
    // collection, and only those can be a user preset's parent. An old name some preset still carries
    // resolves to that preset instead, so it is left out.
    std::vector<std::pair<std::string, std::string>> renames;
    for (const Preset &preset : filaments)
        if (preset.is_system)
            for (const std::string &old_name : preset.renamed_from)
                if (filaments.find_preset(old_name, false, true) == nullptr) {
                    // The loader's map: on a clash between two presets claiming one old name, the first one.
                    const std::string *target = filaments.get_preset_name_renamed(old_name);
                    REQUIRE(target != nullptr);
                    renames.emplace_back(old_name, *target);
                }
    REQUIRE(renames.size() >= 5);

    ScopedTemporaryDir user;
    for (size_t i = 0; i < renames.size(); ++i)
        write_user_filament(user.path(), "U1 user " + std::to_string(i), renames[i].first);
    load_user_filaments(filaments, user.path());
    for (size_t i = 0; i < renames.size(); ++i) {
        INFO(renames[i].first << " -> " << renames[i].second);
        const Preset *preset = filaments.find_preset("U1 user " + std::to_string(i), false, true);
        REQUIRE(preset != nullptr);
        REQUIRE(filaments.get_preset_parent(*preset) != nullptr);
        CHECK(filaments.get_preset_parent(*preset)->name == renames[i].second);
        CHECK(printers_of(*preset) == printers_of(*filaments.find_preset(renames[i].second, false, true)));
    }
}

// Orca #7071 / #15173: "Detach from parent" in the Save preset dialog. A detached preset keeps every value
// and has no parent, so the renamed-parent lookups and the filament preset migration leave it alone.
TEST_CASE("A detached copy of a system filament keeps every value and has no parent", "[Preset][RenamedParent][Detach]")
{
    ScopedTemporaryDir  temp_dir;
    RenameTestFilaments filaments;
    add_renamed_parent(filaments);
    fs::create_directories(temp_dir.path() / PRESET_FILAMENT_NAME);
    load_user_filaments(filaments, temp_dir.path());
    REQUIRE(filaments.select_preset_by_name(k1_parent, true));
    filaments.get_edited_preset().config.option<ConfigOptionInts>("nozzle_temperature")->values = { 250 };

    filaments.save_current_preset("My detached PETG", /*detach*/ true);

    const Preset *preset = filaments.find_preset("My detached PETG", false, true);
    REQUIRE(preset != nullptr);
    CHECK_FALSE(preset->is_system);
    CHECK(preset->inherits().empty());
    CHECK(preset->renamed_from.empty());
    CHECK(filaments.get_preset_parent(*preset) == nullptr);
    CHECK(nozzle_temperature(*preset) == 250);
    CHECK(flow_ratio(*preset) == Approx(0.95));

    // The whole config is in the file, not a difference to a parent. A preset with no parent is a base
    // preset, which PresetCollection::path_for_preset() keeps in the "base" folder.
    CHECK(fs::equivalent(fs::path(preset->file), temp_dir.path() / PRESET_FILAMENT_NAME / "base" / "My detached PETG.json"));
    const nlohmann::json j = nlohmann::json::parse(read_file(fs::path(preset->file)));
    CHECK(j.value("inherits", std::string()).empty());
    CHECK(j.at("nozzle_temperature") == nlohmann::json::array({ "250" }));
    CHECK(j.contains("filament_flow_ratio"));

    // The migration only looks at presets with a parent.
    CHECK(filaments.legacy_user_filament_presets({ { k1_parent, { "Creality K1C 0.4 nozzle" } },
                                                   { k1_old_parent, { "Creality K1C 0.4 nozzle" } } }).empty());
}

TEST_CASE("Detaching a user filament on a renamed parent drops the parent and keeps its values", "[Preset][RenamedParent][Detach]")
{
    ScopedTemporaryDir  temp_dir;
    RenameTestFilaments filaments;
    add_renamed_parent(filaments);
    const fs::path file = write_user_filament(temp_dir.path(), "My PETG", k1_old_parent, R"(,"nozzle_temperature":["245"])");
    load_user_filaments(filaments, temp_dir.path());
    REQUIRE(filaments.select_preset_by_name("My PETG", true));

    filaments.save_current_preset("My PETG", /*detach*/ true);

    const Preset *preset = filaments.find_preset("My PETG", false, true);
    REQUIRE(preset != nullptr);
    CHECK(preset->inherits().empty());
    CHECK(filaments.get_preset_parent(*preset) == nullptr);
    CHECK(nozzle_temperature(*preset) == 245);
    CHECK(flow_ratio(*preset) == Approx(0.95));
    const nlohmann::json j = nlohmann::json::parse(read_file(file));
    CHECK(j.value("inherits", std::string()).empty());
    CHECK(j.contains("filament_flow_ratio"));
    CHECK(filaments.legacy_user_filament_presets({ { k1_old_parent, { "Creality K1C 0.4 nozzle" } } }).empty());
}

// Orca #12816: the 3MF reader allocates a project preset's loading_substitutions with new, and
// load_project_embedded_presets released it with free() - a heap mismatch a crafted 3MF (one whose
// embedded preset needs a substitution) reaches. Under ASan / the MSVC debug heap the old code
// fails here; in a release build this pins the hand-over and the release.
TEST_CASE("A project preset's loading substitutions are reported and released", "[Preset][ProjectPreset][Regression]")
{
    RenameTestFilaments filaments;
    add_renamed_parent(filaments);
    std::unique_ptr<Preset> preset = project_filament("Project PETG", k1_parent);

    preset->loading_substitutions = new ConfigSubstitutions();
    {
        ConfigSubstitution sub;
        sub.opt_def   = print_config_def.get("filament_type");
        sub.old_value = "NOT-A-FILAMENT-TYPE";
        sub.new_value.reset(new ConfigOptionStrings({ "PLA" }));
        preset->loading_substitutions->emplace_back(std::move(sub));
    }
    REQUIRE(preset->loading_substitutions->front().opt_def != nullptr);

    PresetsConfigSubstitutions substitutions;
    std::vector<Preset *>      project_presets{ preset.get() };
    filaments.load_project_embedded_presets(project_presets, PRESET_FILAMENT_NAME, substitutions,
                                            ForwardCompatibilitySubstitutionRule::EnableSilent);

    REQUIRE(substitutions.size() == 1);
    CHECK(substitutions.front().preset_name == "Project PETG");
    CHECK(substitutions.front().preset_source == PresetConfigSubstitutions::Source::ProjectFile);
    REQUIRE(substitutions.front().substitutions.size() == 1);
    CHECK(substitutions.front().substitutions.front().old_value == "NOT-A-FILAMENT-TYPE");
    CHECK(preset->loading_substitutions == nullptr);
    CHECK(filaments.find_preset("Project PETG", false, true) != nullptr);
}


// Owner hand test of #549: "Detach from parent" in the Save preset dialog. The dialog offers it when
// PresetCollection::detach_parent_name() is not empty and saves with detach = true; check both for process,
// filament and printer presets, saved from a system preset and from a user preset.
namespace {
struct DetachCase
{
    Preset::Type type;
    const char  *folder;
    const char  *edited_key;
    const char  *edited_value;
    const char  *other_key; // left alone: a detached preset's file still has it
};

void check_detach_from_parent(PresetCollection &presets, const DetachCase &c)
{
    ScopedTemporaryDir temp_dir;
    // A system preset, and the user folder the collection saves into.
    presets.load_preset("", "System preset", presets.default_preset().config, false);
    Preset *system = presets.find_preset("System preset", false, true);
    REQUIRE(system != nullptr);
    system->is_system  = true;
    system->is_visible = true;
    fs::create_directories(temp_dir.path() / c.folder);
    PresetsConfigSubstitutions substitutions;
    presets.load_presets(temp_dir.path().string(), c.folder, substitutions, ForwardCompatibilitySubstitutionRule::EnableSilent);

    // From the system preset the dialog offers to detach from that system preset.
    REQUIRE(presets.select_preset_by_name("System preset", true));
    CHECK(PresetCollection::detach_parent_name(presets.get_selected_preset()) == "System preset");

    SECTION("A detached copy of the system preset") {
        presets.get_edited_preset().config.set_deserialize_strict({{c.edited_key, c.edited_value}});
        presets.save_current_preset("Detached copy", /*detach*/ true);
        const Preset *saved = presets.find_preset("Detached copy", false, true);
        REQUIRE(saved != nullptr);
        CHECK(saved->inherits().empty());
        CHECK(presets.get_preset_parent(*saved) == nullptr);
        CHECK(PresetCollection::detach_parent_name(*saved).empty());
        CHECK(saved->config.opt_serialize(c.edited_key) == presets.get_edited_preset().config.opt_serialize(c.edited_key));
        const nlohmann::json j = nlohmann::json::parse(read_file(fs::path(saved->file)));
        CHECK(j.value("inherits", std::string()).empty());
        CHECK(j.contains(c.edited_key));
        CHECK(j.contains(c.other_key));
    }
    SECTION("A user preset, then detached") {
        presets.get_edited_preset().config.set_deserialize_strict({{c.edited_key, c.edited_value}});
        presets.save_current_preset("User preset");
        const Preset *user = presets.find_preset("User preset", false, true);
        REQUIRE(user != nullptr);
        CHECK(user->inherits() == "System preset");
        // From the user preset the dialog offers to detach from its parent.
        REQUIRE(presets.select_preset_by_name("User preset", true));
        CHECK(PresetCollection::detach_parent_name(presets.get_selected_preset()) == "System preset");
        const nlohmann::json before = nlohmann::json::parse(read_file(fs::path(user->file)));
        CHECK_FALSE(before.contains(c.other_key)); // only its difference to the parent

        presets.save_current_preset("User preset", /*detach*/ true);
        const Preset *detached = presets.find_preset("User preset", false, true);
        REQUIRE(detached != nullptr);
        CHECK(detached->inherits().empty());
        CHECK(PresetCollection::detach_parent_name(*detached).empty());
        const nlohmann::json after = nlohmann::json::parse(read_file(fs::path(detached->file)));
        CHECK(after.value("inherits", std::string()).empty());
        CHECK(after.contains(c.edited_key));
        CHECK(after.contains(c.other_key));
    }
}
} // namespace

TEST_CASE("Detach from parent works for process, filament and printer presets", "[Preset][Detach]")
{
    const auto &defaults = static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults());
    SECTION("process") {
        PresetCollection prints(Preset::TYPE_PRINT, Preset::print_options(), defaults);
        check_detach_from_parent(prints, {Preset::TYPE_PRINT, PRESET_PRINT_NAME, "layer_height", "0.28", "wall_loops"});
    }
    SECTION("filament") {
        PresetCollection filaments(Preset::TYPE_FILAMENT, Preset::filament_options(), defaults, "Default Filament");
        check_detach_from_parent(filaments, {Preset::TYPE_FILAMENT, PRESET_FILAMENT_NAME, "nozzle_temperature", "250", "filament_flow_ratio"});
    }
    SECTION("printer") {
        PrinterPresetCollection printers(Preset::TYPE_PRINTER, Preset::printer_options(), defaults, "Default Printer");
        check_detach_from_parent(printers, {Preset::TYPE_PRINTER, PRESET_PRINTER_NAME, "printable_height", "300", "nozzle_diameter"});
    }
}
