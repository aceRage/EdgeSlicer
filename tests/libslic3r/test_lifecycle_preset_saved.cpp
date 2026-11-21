// PresetSaved, fired from PresetCollection::save_current_preset (ported from OrcaSlicer).
// Compiled only with EDGE_PYTHON_PLUGINS ON (tests/libslic3r/CMakeLists.txt): with the flag OFF
// the fire site compiles to nothing.
#include <catch2/catch.hpp>

#include "libslic3r/LifecycleEvents.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/filesystem.hpp>

#include <fstream>
#include <string>
#include <vector>

namespace fs = boost::filesystem;
using namespace Slic3r;

namespace {

struct LifecyclePresetTempDir
{
    fs::path root;
    LifecyclePresetTempDir()
    {
        root = fs::temp_directory_path() / "snorca_tests" / fs::unique_path("lifecycle_preset_%%%%%%%%");
        fs::create_directories(root);
    }
    ~LifecyclePresetTempDir()
    {
        boost::system::error_code ec;
        fs::remove_all(root, ec);
    }
};

struct LifecycleTestFilaments : public PresetCollection
{
    LifecycleTestFilaments()
        : PresetCollection(Preset::TYPE_FILAMENT, Preset::filament_options(),
                           static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults()), "Default Filament")
    {}
};

struct PresetSavedRecord
{
    LifecycleEvent   event;
    std::string      name;
    std::string      msg;
    LifecycleEvtCode code;
};

} // namespace

TEST_CASE("Saving a preset fires PresetSaved once with new / overwrite", "[LifecycleFire][Preset]")
{
    LifecyclePresetTempDir temp;
    const fs::path         folder = temp.root / PRESET_FILAMENT_NAME;
    fs::create_directories(folder);
    std::ofstream((folder / "Mine.json").string())
        << R"({"type":"filament","name":"Mine","from":"User","version":"1.0.0","inherits":""})";

    LifecycleTestFilaments     filaments;
    PresetsConfigSubstitutions substitutions;
    filaments.load_presets(temp.root.string(), PRESET_FILAMENT_NAME, substitutions, ForwardCompatibilitySubstitutionRule::EnableSilent);
    REQUIRE(filaments.find_preset("Mine", false) != nullptr);

    std::vector<PresetSavedRecord> events;
    set_lifecycle_hook_fn([&events](LifecycleEvent event, const LifecycleEventContext &ctx) {
        events.push_back({event, ctx.name, ctx.msg, ctx.code});
    });

    Preset edited = *filaments.find_preset("Mine", false);
    filaments.save_current_preset("Mine Copy", false, false, &edited);
    REQUIRE(events.size() == 1);
    CHECK(events[0].event == LifecycleEvent::PresetSaved);
    CHECK(events[0].name == "Mine Copy");
    CHECK(events[0].msg == "new");
    CHECK(events[0].code == LifecycleEvtCode::Ok);

    Preset again = *filaments.find_preset("Mine Copy", false);
    filaments.save_current_preset("Mine Copy", false, false, &again);
    REQUIRE(events.size() == 2);
    CHECK(events[1].event == LifecycleEvent::PresetSaved);
    CHECK(events[1].name == "Mine Copy");
    CHECK(events[1].msg == "overwrite");

    set_lifecycle_hook_fn(nullptr);
}

TEST_CASE("Refusing to overwrite a system preset fires no PresetSaved", "[LifecycleFire][Preset]")
{
    LifecycleTestFilaments filaments;
    {
        DynamicPrintConfig config = filaments.default_preset().config;
        filaments.load_preset("", "System One", config, false);
        Preset *system      = filaments.find_preset("System One", false, true);
        REQUIRE(system != nullptr);
        system->is_system   = true;
        system->is_visible  = true;
    }

    int calls = 0;
    set_lifecycle_hook_fn([&calls](LifecycleEvent, const LifecycleEventContext &) { ++calls; });
    Preset edited = *filaments.find_preset("System One", false, true);
    filaments.save_current_preset("System One", false, false, &edited);
    set_lifecycle_hook_fn(nullptr);
    CHECK(calls == 0);
}
