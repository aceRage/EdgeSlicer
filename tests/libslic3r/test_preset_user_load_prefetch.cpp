// Startup round 2 (F3): the user preset loader parses the first pass's JSON files ahead on TBB workers
// and then consumes the results in directory order. The result must be the same as the sequential
// loader's (ORCA_PRESET_LOAD_SEQUENTIAL=1, PresetBundle::set_sequential_load_override(true)) for a
// folder that crosses several prefetch chunks and has the awkward cases: children sorting before their
// detached parent (deferred to a later round), a parent in base/, an orphan, a file of another type,
// a duplicate name and a file that does not parse (quarantined).
#include <catch2/catch.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PresetQuarantine.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/filesystem.hpp>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace fs = boost::filesystem;
using namespace Slic3r;

namespace {

struct ScopedTemporaryDir
{
    fs::path root;
    ScopedTemporaryDir()
    {
        root = fs::temp_directory_path() / "snorca_tests" / fs::unique_path("user_load_prefetch_%%%%%%%%");
        fs::create_directories(root);
    }
    ~ScopedTemporaryDir()
    {
        boost::system::error_code ec;
        fs::remove_all(root, ec);
    }
};

struct SequentialOverride
{
    explicit SequentialOverride(bool sequential) { PresetBundle::set_sequential_load_override(sequential); }
    ~SequentialOverride() { PresetBundle::set_sequential_load_override(std::nullopt); }
};

struct TestFilaments : public PresetCollection
{
    TestFilaments()
        : PresetCollection(Preset::TYPE_FILAMENT, Preset::filament_options(),
                           static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults()), "Default Filament")
    {}
};

void write_file(const fs::path &file, const std::string &body)
{
    fs::create_directories(file.parent_path());
    std::ofstream(file.string()) << body;
}

void write_filament(const fs::path &folder, const std::string &file_name, const std::string &name, const std::string &inherits,
                    const std::string &extra = {}, const std::string &type = "filament")
{
    write_file(folder / (file_name + ".json"), R"({"type":")" + type + R"(","name":")" + name +
                                                   R"(","from":"User","version":"1.0.0","inherits":")" + inherits + '"' + extra + '}');
}

// 200 files: more than three prefetch chunks of 64.
void write_user_folder(const fs::path &root)
{
    const fs::path folder = root / PRESET_FILAMENT_NAME;
    char           buf[64];
    for (int i = 0; i < 170; ++i) {
        std::snprintf(buf, sizeof(buf), "M %03d", i);
        const std::string temp = std::to_string(190 + i % 60);
        const std::string flow = std::to_string(0.80 + 0.001 * i);
        write_filament(folder, buf, buf, "", R"(,"nozzle_temperature":[")" + temp + R"("],"filament_flow_ratio":[")" + flow + R"("])");
    }
    // Children whose detached parent sorts after them: deferred to the second round.
    for (int i = 0; i < 12; ++i) {
        std::snprintf(buf, sizeof(buf), "A child %02d", i);
        char parent[32];
        std::snprintf(parent, sizeof(parent), "M %03d", 160 + i % 10);
        write_filament(folder, buf, buf, parent, R"(,"filament_flow_ratio":["0.9)" + std::to_string(i % 10) + R"("])");
    }
    // Children after their parent in the listing.
    for (int i = 0; i < 8; ++i) {
        std::snprintf(buf, sizeof(buf), "N child %02d", i);
        char parent[32];
        std::snprintf(parent, sizeof(parent), "M %03d", i * 7);
        write_filament(folder, buf, buf, parent, R"(,"nozzle_temperature":["2)" + std::to_string(10 + i) + R"("])");
    }
    // A detached parent in base/ and its child.
    write_filament(folder / "base", "Base parent", "Base parent", "", R"(,"nozzle_temperature":["222"])");
    write_filament(folder, "Base child", "Base child", "Base parent", R"(,"filament_flow_ratio":["0.97"])");
    // An orphan, a process preset in the filament folder, a duplicate logical name, a broken file.
    write_filament(folder, "Orphan", "Orphan", "No such parent");
    write_filament(folder, "Process in the wrong folder", "Process in the wrong folder", "", {}, "process");
    write_filament(folder, "Z duplicate", "M 005", "");
    write_file(folder / "Broken.json", "{\"type\":\"filament\",\"name\":\"Broken\", this is not json");
}

struct LoadResult
{
    std::vector<std::string>   names;
    std::vector<const Preset*> presets;
    size_t                     substitutions = 0;
    size_t                     quarantined   = 0;
};

LoadResult load(TestFilaments &filaments, const fs::path &root, bool sequential)
{
    SequentialOverride         mode(sequential);
    PresetsConfigSubstitutions substitutions;
    PresetQuarantine::take(); // start from an empty accumulator
    filaments.load_presets(root.string(), PRESET_FILAMENT_NAME, substitutions, ForwardCompatibilitySubstitutionRule::EnableSilent);
    LoadResult result;
    for (const Preset &preset : filaments.get_presets()) {
        result.names.push_back(preset.name);
        result.presets.push_back(&preset);
    }
    result.substitutions = substitutions.size();
    result.quarantined   = PresetQuarantine::take().size();
    return result;
}

} // namespace

TEST_CASE("Parsing user presets ahead loads the same presets as parsing them one by one", "[Preset][UserLoadPrefetch]")
{
    // Two copies of the same folder: a quarantined file is moved away by the first load.
    ScopedTemporaryDir sequential_dir, prefetch_dir;
    write_user_folder(sequential_dir.root);
    write_user_folder(prefetch_dir.root);

    TestFilaments sequential_filaments, prefetch_filaments;
    const LoadResult sequential = load(sequential_filaments, sequential_dir.root, true);
    const LoadResult prefetch   = load(prefetch_filaments, prefetch_dir.root, false);

    // The folder loaded at all: the standalone presets, both kinds of children and the base/ pair.
    CHECK(sequential_filaments.find_preset("M 169", false) != nullptr);
    CHECK(sequential_filaments.find_preset("A child 11", false) != nullptr);
    CHECK(sequential_filaments.find_preset("N child 07", false) != nullptr);
    CHECK(sequential_filaments.find_preset("Base child", false) != nullptr);
    CHECK(sequential_filaments.find_preset("Orphan", false) == nullptr);
    CHECK(sequential.quarantined == 1);

    REQUIRE(prefetch.names == sequential.names);
    CHECK(prefetch.substitutions == sequential.substitutions);
    CHECK(prefetch.quarantined == sequential.quarantined);
    for (size_t i = 0; i < sequential.presets.size(); ++i) {
        const Preset &a = *sequential.presets[i];
        const Preset &b = *prefetch.presets[i];
        INFO("preset " << a.name);
        CHECK(a.is_visible == b.is_visible);
        CHECK(a.filament_id == b.filament_id);
        CHECK(a.version == b.version);
        CHECK(a.inherits() == b.inherits());
        CHECK(a.config.diff(b.config).empty());
        CHECK(a.config.keys() == b.config.keys());
    }
}

TEST_CASE("A deferred child still takes its values from a parent loaded ahead", "[Preset][UserLoadPrefetch]")
{
    ScopedTemporaryDir temp;
    write_user_folder(temp.root);
    TestFilaments    filaments;
    const LoadResult result = load(filaments, temp.root, false);
    (void) result;

    const Preset *child = filaments.find_preset("A child 03", false);
    REQUIRE(child != nullptr);
    // Its parent "M 163": nozzle temperature 190 + 163 % 60 = 233; its own flow ratio 0.93.
    CHECK(child->config.option<ConfigOptionInts>("nozzle_temperature")->values.front() == 233);
    CHECK(child->config.option<ConfigOptionFloats>("filament_flow_ratio")->values.front() == Approx(0.93));
    const Preset *base_child = filaments.find_preset("Base child", false);
    REQUIRE(base_child != nullptr);
    CHECK(base_child->config.option<ConfigOptionInts>("nozzle_temperature")->values.front() == 222);
}
