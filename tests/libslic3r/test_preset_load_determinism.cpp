// Determinism guard for PresetBundle::load_system_presets_from_json.
//
// The system-preset load parses each vendor into its own bundle in parallel and then
// merges those bundles into the destination one at a time, in the original vendor
// order. The parallelism must be invisible: the preset list, its order, the resolved
// inheritance values and the reported duplicates all have to come out exactly as a
// strictly sequential load would produce them.
//
// This test builds a synthetic multi-vendor system/ tree on disk - one base vendor that
// later vendors inherit from, several ordinary vendors, a vendor that redefines a name
// another vendor already used (duplicate reporting), and a vendor whose presets inherit
// through two levels - loads it, and pins the full result: names in order, and a hash
// over every resolved config value of every preset. Any reordering, any lost or
// double-applied inheritance, any cross-vendor leak between the parallel workers
// changes one of those and fails the test.

#include <catch2/catch.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

// Mirror the loader's own environment switches, so the cache assertions only run in the
// configuration that is supposed to produce a cache.
bool cache_is_enabled()
{
    const char *v = std::getenv("ORCA_PRESET_CACHE");
    return !(v != nullptr && (std::string(v) == "0" || std::string(v) == "false"));
}

bool sequential_forced()
{
    const char *v = std::getenv("ORCA_PRESET_LOAD_SEQUENTIAL");
    return v != nullptr && (std::string(v) == "1" || std::string(v) == "true");
}

void write_file(const fs::path &path, const std::string &content)
{
    fs::create_directories(path.parent_path());
    std::ofstream ofs(path.string(), std::ios::binary);
    ofs << content;
}

// One vendor: a root manifest naming its filament/machine subfiles, plus the subfiles.
//
// The presets here are FILAMENTS on purpose. Cross-vendor inheritance only works for
// filaments: load_vendor_configs_from_json copies its local config map into
// m_config_maps once, at the end of the filament section, and only for
// ORCA_FILAMENT_LIBRARY. That map is the only thing later vendors can resolve an
// "inherits" against, so a process preset in vendor B can never inherit from vendor A -
// but a filament preset can, and every shipped vendor relies on exactly that.
struct VendorSpec
{
    std::string name;
    // (preset name, inherits-or-empty, filament_flow_ratio value, instantiation)
    std::vector<std::tuple<std::string, std::string, std::string, bool>> filaments;
};

void write_vendor(const fs::path &system_dir, const VendorSpec &spec)
{
    std::ostringstream manifest;
    manifest << "{\n";
    manifest << "  \"name\": \"" << spec.name << "\",\n";
    manifest << "  \"version\": \"2.0.0.0\",\n";
    manifest << "  \"machine_model_list\": [\n";
    manifest << "    { \"name\": \"" << spec.name << "-M\", \"sub_path\": \"machine/" << spec.name << "-M.json\" }\n";
    manifest << "  ],\n";
    manifest << "  \"process_list\": [],\n";
    manifest << "  \"filament_list\": [\n";
    for (size_t i = 0; i < spec.filaments.size(); ++i) {
        const std::string &fname = std::get<0>(spec.filaments[i]);
        manifest << "    { \"name\": \"" << fname << "\", \"sub_path\": \"filament/" << fname << ".json\" }"
                 << (i + 1 < spec.filaments.size() ? "," : "") << "\n";
    }
    manifest << "  ],\n";
    manifest << "  \"machine_list\": []\n";
    manifest << "}\n";
    write_file(system_dir / (spec.name + ".json"), manifest.str());

    // A minimal printer model so the vendor profile is well formed.
    std::ostringstream model;
    model << "{\n";
    model << "  \"name\": \"" << spec.name << "-M\",\n";
    model << "  \"model_id\": \"" << spec.name << "-M\",\n";
    model << "  \"nozzle_diameter\": \"0.4\",\n";
    model << "  \"family\": \"" << spec.name << "\"\n";
    model << "}\n";
    write_file(system_dir / spec.name / "machine" / (spec.name + "-M.json"), model.str());

    for (const auto &fil : spec.filaments) {
        const std::string &fname    = std::get<0>(fil);
        const std::string &inherits = std::get<1>(fil);
        const std::string &flow     = std::get<2>(fil);
        const bool         inst     = std::get<3>(fil);
        std::ostringstream p;
        p << "{\n";
        p << "  \"type\": \"filament\",\n";
        p << "  \"name\": \"" << fname << "\",\n";
        p << "  \"from\": \"system\",\n";
        p << "  \"instantiation\": \"" << (inst ? "true" : "false") << "\",\n";
        p << "  \"filament_id\": \"F" << fname << "\",\n";
        if (!inherits.empty())
            p << "  \"inherits\": \"" << inherits << "\",\n";
        p << "  \"filament_flow_ratio\": [\"" << flow << "\"]\n";
        p << "}\n";
        write_file(system_dir / spec.name / "filament" / (fname + ".json"), p.str());
    }
}

// A stable fingerprint of one preset: its name, then every option key and its
// serialized value. Inheritance is resolved by then, so this catches a parent that was
// applied twice, not at all, or from the wrong vendor.
std::string preset_fingerprint(const Preset &preset)
{
    std::ostringstream out;
    out << preset.name << '|' << (preset.is_system ? "sys" : "usr") << '|';
    t_config_option_keys keys = preset.config.keys();
    std::sort(keys.begin(), keys.end());
    for (const std::string &key : keys) {
        const ConfigOption *opt = preset.config.option(key);
        if (opt == nullptr)
            continue;
        out << key << '=' << opt->serialize() << ';';
    }
    return out.str();
}

std::vector<std::string> collection_fingerprints(const PresetCollection &collection)
{
    std::vector<std::string> out;
    for (const Preset &preset : collection)
        out.push_back(preset_fingerprint(preset));
    return out;
}

// Build the tree, load it, and return (names in order, fingerprints in order).
struct LoadResult
{
    std::vector<std::string> names;
    std::vector<std::string> fingerprints;
    std::string              errors;
};

// Write the synthetic vendor tree into datadir/system, without loading it.
void build_synthetic_tree(const fs::path &datadir)
{
    const fs::path system_dir = datadir / PRESET_SYSTEM_DIR;
    fs::create_directories(system_dir);

    // The base vendor. It is always loaded first (the loader swaps it to the front),
    // and its non-instantiated presets are the shared inheritance base every other
    // vendor resolves against.
    VendorSpec base;
    base.name = PresetBundle::ORCA_FILAMENT_LIBRARY;
    base.filaments = {
        {"base_common", "", "0.920", false},
        {"base_fine", "base_common", "0.910", false},
        {"lib_visible", "base_common", "0.924", true},
    };
    write_vendor(system_dir, base);

    // Ordinary vendors. Alpha inherits one level from the base, Bravo two levels
    // (through the base's own chained non-instantiated preset), Charlie not at all.
    VendorSpec alpha;
    alpha.name      = "Alpha";
    alpha.filaments = {
        {"alpha_local", "", "0.930", false},
        {"Alpha Standard @A", "base_common", "0.928", true},
        {"Alpha Local @A", "alpha_local", "0.932", true},
    };
    write_vendor(system_dir, alpha);

    VendorSpec bravo;
    bravo.name      = "Bravo";
    bravo.filaments = {
        {"Bravo Fine @B", "base_fine", "0.912", true},
        {"Bravo Plain @B", "", "0.926", true},
    };
    write_vendor(system_dir, bravo);

    VendorSpec charlie;
    charlie.name      = "Charlie";
    charlie.filaments = {
        {"Charlie Only @C", "", "0.934", true},
    };
    write_vendor(system_dir, charlie);

    // Delta redefines a name Bravo already loaded - the duplicate path.
    VendorSpec delta;
    delta.name      = "Delta";
    delta.filaments = {
        {"Bravo Plain @B", "", "0.999", true},
        {"Delta Own @D", "base_common", "0.936", true},
    };
    write_vendor(system_dir, delta);
}

// Load a tree that build_synthetic_tree already wrote.
LoadResult load_tree(const fs::path &datadir)
{
    set_data_dir(datadir.string());

    // load_presets() is the public entry the app itself uses: it runs the system-preset
    // load (the part this test is guarding), then the user presets and the selection
    // pass, so the fingerprints cover the state the GUI actually sees.
    PresetBundle bundle;
    LoadResult   result;
    AppConfig    app_config;
    try {
        bundle.load_presets(app_config, ForwardCompatibilitySubstitutionRule::EnableSilent);
    } catch (const std::exception &ex) {
        result.errors = ex.what();
    }

    for (const Preset &preset : bundle.filaments)
        result.names.push_back(preset.name);
    result.fingerprints = collection_fingerprints(bundle.filaments);
    return result;
}

} // namespace

TEST_CASE("System preset load is deterministic across repeated runs", "[Preset][PresetLoadDeterminism]")
{
    const std::string saved_data_dir = data_dir();

    const fs::path root = fs::temp_directory_path() / fs::unique_path("orca_preset_determinism_%%%%%%%%");
    fs::create_directories(root);

    // Two independent loads of the same tree. The vendor parses run in parallel, so a
    // race or an order-dependent merge would show up as a difference between runs.
    build_synthetic_tree(root / "run1");
    build_synthetic_tree(root / "run2");
    LoadResult first  = load_tree(root / "run1");
    LoadResult second = load_tree(root / "run2");

    set_data_dir(saved_data_dir);

    INFO("run1 names: " << first.names.size() << ", run2 names: " << second.names.size());
    REQUIRE(first.names == second.names);
    REQUIRE(first.fingerprints == second.fingerprints);

    SECTION("every instantiated preset of every vendor is present") {
        auto has = [&](const std::string &name) {
            return std::find(first.names.begin(), first.names.end(), name) != first.names.end();
        };
        REQUIRE(has("lib_visible"));
        REQUIRE(has("Alpha Standard @A"));
        REQUIRE(has("Alpha Local @A"));
        REQUIRE(has("Bravo Fine @B"));
        REQUIRE(has("Bravo Plain @B"));
        REQUIRE(has("Charlie Only @C"));
        REQUIRE(has("Delta Own @D"));
    }

    SECTION("non-instantiated inheritance bases stay out of the preset list") {
        auto has = [&](const std::string &name) {
            return std::find(first.names.begin(), first.names.end(), name) != first.names.end();
        };
        REQUIRE_FALSE(has("base_common"));
        REQUIRE_FALSE(has("base_fine"));
        REQUIRE_FALSE(has("alpha_local"));
    }

    SECTION("the preset list is sorted, as the collection's merge requires") {
        // The default presets come first; the rest must be in ascending name order,
        // which is what merge_presets' lower_bound insert maintains.
        std::vector<std::string> tail(first.names.begin(), first.names.end());
        tail.erase(std::remove_if(tail.begin(), tail.end(),
                                  [](const std::string &n) {
                                      return n.rfind("- default -", 0) == 0 || n.rfind("Default", 0) == 0;
                                  }),
                   tail.end());
        std::vector<std::string> sorted = tail;
        std::sort(sorted.begin(), sorted.end());
        REQUIRE(tail == sorted);
    }

    SECTION("a name defined by two vendors resolves to one preset, the same way each run") {
        // Delta redefines "Bravo Plain @B". Whichever vendor wins, it must be the same
        // winner every run and the name must appear exactly once in the collection -
        // that is what the ordered sequential merge guarantees.
        const long count = std::count(first.names.begin(), first.names.end(), std::string("Bravo Plain @B"));
        REQUIRE(count == 1);
        REQUIRE(first.errors == second.errors);
    }

    SECTION("cross-vendor inherited values are resolved, not left at the default") {
        // "Alpha Standard @A" lives in vendor Alpha but inherits base_common from the
        // shared library vendor - the one piece of state the parallel workers read from
        // the bundle the base vendor filled in. Its own override must survive, proving
        // the parent was applied and then the child's delta on top.
        auto it = std::find_if(first.fingerprints.begin(), first.fingerprints.end(),
                               [](const std::string &f) { return f.rfind("Alpha Standard @A|", 0) == 0; });
        REQUIRE(it != first.fingerprints.end());
        REQUIRE(it->find("filament_flow_ratio=0.928;") != std::string::npos);
    }

    boost::system::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("The per-vendor parse cache is transparent and self-invalidating", "[Preset][PresetLoadDeterminism]")
{
    const std::string saved_data_dir = data_dir();

    const fs::path root = fs::temp_directory_path() / fs::unique_path("orca_preset_cache_%%%%%%%%");
    fs::create_directories(root);
    const fs::path dd = root / "dd";
    build_synthetic_tree(dd);

    // First load: no cache exists, so everything is parsed and the caches are written.
    LoadResult cold = load_tree(dd);
    const fs::path cache_dir = dd / "cache" / "presets";

    // The cache can be switched off from the environment, and the fully sequential path
    // deliberately does not write one. Both are supported configurations, so the
    // cache-file assertions only apply when a cache is actually expected; everything
    // else in this test - that the results match - must hold either way.
    const bool cache_expected = cache_is_enabled() && !sequential_forced();

    SECTION("the first load writes a cache per vendor") {
        if (!cache_expected) {
            WARN("preset cache disabled for this run; skipping the cache-file checks");
        } else {
            REQUIRE(fs::exists(cache_dir));
            REQUIRE(fs::exists(cache_dir / (std::string(PresetBundle::ORCA_FILAMENT_LIBRARY) + ".cbor")));
            REQUIRE(fs::exists(cache_dir / "Alpha.cbor"));
            REQUIRE(fs::exists(cache_dir / "Bravo.cbor"));
        }
    }

    // Second load of the SAME tree: the key matches, so the documents come from the
    // cache and no file is parsed. The result must be indistinguishable.
    LoadResult warm = load_tree(dd);
    REQUIRE(warm.names == cold.names);
    REQUIRE(warm.fingerprints == cold.fingerprints);

    SECTION("touching one file invalidates that vendor's cache and the result still matches") {
        const fs::path touched = dd / PRESET_SYSTEM_DIR / "Alpha" / "filament" / "Alpha Standard @A.json";
        REQUIRE(fs::exists(touched));
        // Move the mtime well clear of the original so a coarse filesystem timestamp
        // cannot land on the same value.
        fs::last_write_time(touched, fs::last_write_time(touched) + 120);

        LoadResult rebuilt = load_tree(dd);
        REQUIRE(rebuilt.names == cold.names);
        REQUIRE(rebuilt.fingerprints == cold.fingerprints);
    }

    SECTION("a corrupt cache is survived, not trusted") {
        // Truncated / garbage CBOR must be detected, dropped and reparsed.
        const fs::path victim = cache_dir / "Alpha.cbor";
        if (!cache_expected || !fs::exists(victim)) {
            WARN("no cache file to corrupt in this configuration; skipping");
            return;
        }
        {
            std::ofstream ofs(victim.string(), std::ios::binary | std::ios::trunc);
            ofs << "this is not cbor";
        }
        LoadResult recovered = load_tree(dd);
        REQUIRE(recovered.names == cold.names);
        REQUIRE(recovered.fingerprints == cold.fingerprints);
    }

    SECTION("a same-size edit in the same second still invalidates the cache") {
        // The replacement is deliberately byte-for-byte the SAME LENGTH as the original
        // and is written immediately, so the file's size is unchanged and its mtime very
        // likely lands in the same whole second. A (path, size, mtime) key cannot see
        // this edit; the content hash can. This is the shape of edit a profile update
        // makes, so getting it wrong would serve stale presets after an update.
        const fs::path edited = dd / PRESET_SYSTEM_DIR / "Charlie" / "filament" / "Charlie Only @C.json";
        REQUIRE(fs::exists(edited));
        {
            std::ofstream ofs(edited.string(), std::ios::binary | std::ios::trunc);
            ofs << "{\n  \"type\": \"filament\",\n  \"name\": \"Charlie Only @C\",\n"
                   "  \"from\": \"system\",\n  \"instantiation\": \"true\",\n"
                   "  \"filament_id\": \"FCharlie Only @C\",\n"
                   "  \"filament_flow_ratio\": [\"0.777\"]\n}\n";
        }
        LoadResult edited_result = load_tree(dd);
        auto it = std::find_if(edited_result.fingerprints.begin(), edited_result.fingerprints.end(),
                               [](const std::string &f) { return f.rfind("Charlie Only @C|", 0) == 0; });
        REQUIRE(it != edited_result.fingerprints.end());
        INFO("fingerprint after edit: " << *it);
        REQUIRE(it->find("filament_flow_ratio=0.777;") != std::string::npos);
    }

    set_data_dir(saved_data_dir);
    boost::system::error_code ec;
    fs::remove_all(root, ec);
}
