// User presets built on a detached user preset. A detached preset is a standalone user preset (no
// "inherits"); a child that inherits it sits in the same folder, so the loader has to resolve the
// parent among the presets of its own pass, whatever the directory order. Overwriting a preset with
// "detach" moves its file into base/, where standalone user presets belong.
#include <catch2/catch.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/filesystem.hpp>

#include <fstream>
#include <string>

namespace fs = boost::filesystem;
using namespace Slic3r;

namespace {

struct ScopedTemporaryDir
{
    fs::path root;
    ScopedTemporaryDir()
    {
        root = fs::temp_directory_path() / "snorca_tests" / fs::unique_path("detached_children_%%%%%%%%");
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

// A user filament file; "inherits" empty writes a standalone preset.
fs::path write_user_filament(const fs::path &folder, const std::string &name, const std::string &inherits, const std::string &extra = {})
{
    const fs::path file = folder / (name + ".json");
    fs::create_directories(folder);
    std::ofstream(file.string()) << R"({"type":"filament","name":")" << name << R"(","from":"User","version":"1.0.0","inherits":")"
                                 << inherits << '"' << extra << '}';
    return file;
}

void load_user_filaments(PresetCollection &filaments, const fs::path &dir)
{
    PresetsConfigSubstitutions substitutions;
    filaments.load_presets(dir.string(), PRESET_FILAMENT_NAME, substitutions, ForwardCompatibilitySubstitutionRule::EnableSilent);
}

} // namespace

TEST_CASE("A user preset inheriting a detached user preset of the same folder is loaded", "[Preset][DetachedChildren]")
{
    ScopedTemporaryDir temp;
    const fs::path     folder = temp.root / PRESET_FILAMENT_NAME;
    // Names chosen so that the child sorts both before and after its parent in a directory listing.
    write_user_filament(folder, "A Child", "Z Parent", R"(,"filament_flow_ratio":["0.91"])");
    write_user_filament(folder, "Z Parent", "", R"(,"filament_flow_ratio":["0.95"],"nozzle_temperature":["231"])");
    write_user_filament(folder, "Z Child", "Z Parent");

    TestFilaments filaments;
    load_user_filaments(filaments, temp.root);

    REQUIRE(filaments.find_preset("Z Parent", false) != nullptr);
    const Preset *a_child = filaments.find_preset("A Child", false);
    const Preset *z_child = filaments.find_preset("Z Child", false);
    REQUIRE(a_child != nullptr);
    REQUIRE(z_child != nullptr);
    // The child starts from its parent's values and applies its own on top.
    CHECK(a_child->config.option<ConfigOptionFloats>("filament_flow_ratio")->values.front() == Approx(0.91));
    CHECK(a_child->config.option<ConfigOptionInts>("nozzle_temperature")->values.front() == 231);
    CHECK(z_child->config.option<ConfigOptionFloats>("filament_flow_ratio")->values.front() == Approx(0.95));
}

TEST_CASE("A chain of three user presets loads in any directory order", "[Preset][DetachedChildren]")
{
    ScopedTemporaryDir temp;
    const fs::path     folder = temp.root / PRESET_FILAMENT_NAME;
    write_user_filament(folder, "A Grandchild", "B Child", R"(,"nozzle_temperature":["240"])");
    write_user_filament(folder, "B Child", "C Root", R"(,"filament_flow_ratio":["0.9"])");
    write_user_filament(folder, "C Root", "", R"(,"nozzle_temperature":["233"])");

    TestFilaments filaments;
    load_user_filaments(filaments, temp.root);

    const Preset *grandchild = filaments.find_preset("A Grandchild", false);
    const Preset *child      = filaments.find_preset("B Child", false);
    REQUIRE(filaments.find_preset("C Root", false) != nullptr);
    REQUIRE(child != nullptr);
    REQUIRE(grandchild != nullptr);
    CHECK(child->config.option<ConfigOptionInts>("nozzle_temperature")->values.front() == 233);
    CHECK(grandchild->config.option<ConfigOptionInts>("nozzle_temperature")->values.front() == 240);
    CHECK(grandchild->config.option<ConfigOptionFloats>("filament_flow_ratio")->values.front() == Approx(0.9));
}

TEST_CASE("A detached parent in base/ still serves its children", "[Preset][DetachedChildren]")
{
    ScopedTemporaryDir temp;
    const fs::path     folder = temp.root / PRESET_FILAMENT_NAME;
    write_user_filament(folder / "base", "My Parent", "");
    write_user_filament(folder, "My Child", "My Parent");

    TestFilaments filaments;
    load_user_filaments(filaments, temp.root);

    CHECK(filaments.find_preset("My Parent", false) != nullptr);
    CHECK(filaments.find_preset("My Child", false) != nullptr);
}

TEST_CASE("A user preset whose parent does not exist is still skipped", "[Preset][DetachedChildren]")
{
    ScopedTemporaryDir temp;
    const fs::path     folder = temp.root / PRESET_FILAMENT_NAME;
    write_user_filament(folder, "Orphan", "No Such Parent");
    write_user_filament(folder, "Orphan Child", "Orphan");
    write_user_filament(folder, "Fine", "");

    TestFilaments filaments;
    load_user_filaments(filaments, temp.root);

    CHECK(filaments.find_preset("Orphan", false) == nullptr);
    CHECK(filaments.find_preset("Orphan Child", false) == nullptr);
    CHECK(filaments.find_preset("Fine", false) != nullptr);
}

TEST_CASE("Overwriting a preset with detach moves its files into base/", "[Preset][DetachedChildren]")
{
    ScopedTemporaryDir temp;
    const fs::path     folder = temp.root / PRESET_FILAMENT_NAME;

    TestFilaments filaments;
    {
        DynamicPrintConfig config = filaments.default_preset().config;
        filaments.load_preset("", "System Parent", config, false);
        Preset *parent      = filaments.find_preset("System Parent", false, true);
        parent->is_system   = true;
        parent->is_visible  = true;
    }
    const fs::path old_json = write_user_filament(folder, "Attached", "System Parent");
    const fs::path old_info = fs::path(old_json).replace_extension(".info");
    std::ofstream(old_info.string()) << "sync_info = \nuser_id = \nsetting_id = \nbase_id = \nupdated_time = 0\n";
    load_user_filaments(filaments, temp.root);
    REQUIRE(filaments.find_preset("Attached", false) != nullptr);
    REQUIRE(fs::exists(old_json));

    Preset edited = *filaments.find_preset("Attached", false);
    filaments.save_current_preset("Attached", true, false, &edited);

    const fs::path new_json = folder / "base" / "Attached.json";
    CHECK(fs::exists(new_json));
    CHECK(fs::exists(fs::path(new_json).replace_extension(".info")));
    CHECK_FALSE(fs::exists(old_json));
    CHECK_FALSE(fs::exists(old_info));
    CHECK(filaments.find_preset("Attached", false)->inherits().empty());

    // A fresh start finds it once, in base/, and a child saved on top of it loads beside it.
    write_user_filament(folder, "Child Of Attached", "Attached");
    TestFilaments reloaded;
    {
        DynamicPrintConfig config = reloaded.default_preset().config;
        reloaded.load_preset("", "System Parent", config, false);
        Preset *parent     = reloaded.find_preset("System Parent", false, true);
        parent->is_system  = true;
        parent->is_visible = true;
    }
    load_user_filaments(reloaded, temp.root);
    CHECK(reloaded.find_preset("Attached", false) != nullptr);
    CHECK(reloaded.find_preset("Child Of Attached", false) != nullptr);
}
