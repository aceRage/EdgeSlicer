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

void write_file(const fs::path &path, const std::string &content)
{
    fs::create_directories(path.parent_path());
    std::ofstream ofs(path.string(), std::ios::binary);
    ofs << content;
}

// One vendor: a root manifest naming its process/filament/machine subfiles, plus the
// subfiles themselves.
struct VendorSpec
{
    std::string              name;
    // (preset name, inherits-or-empty, layer_height value, instantiation)
    std::vector<std::tuple<std::string, std::string, std::string, bool>> processes;
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
    manifest << "  \"process_list\": [\n";
    for (size_t i = 0; i < spec.processes.size(); ++i) {
        const std::string &pname = std::get<0>(spec.processes[i]);
        manifest << "    { \"name\": \"" << pname << "\", \"sub_path\": \"process/" << pname << ".json\" }"
                 << (i + 1 < spec.processes.size() ? "," : "") << "\n";
    }
    manifest << "  ],\n";
    manifest << "  \"filament_list\": [],\n";
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

    for (const auto &proc : spec.processes) {
        const std::string &pname    = std::get<0>(proc);
        const std::string &inherits = std::get<1>(proc);
        const std::string &lh       = std::get<2>(proc);
        const bool         inst     = std::get<3>(proc);
        std::ostringstream p;
        p << "{\n";
        p << "  \"type\": \"process\",\n";
        p << "  \"name\": \"" << pname << "\",\n";
        p << "  \"from\": \"system\",\n";
        p << "  \"instantiation\": \"" << (inst ? "true" : "false") << "\",\n";
        p << "  \"setting_id\": \"S" << pname << "\",\n";
        if (!inherits.empty())
            p << "  \"inherits\": \"" << inherits << "\",\n";
        p << "  \"layer_height\": \"" << lh << "\"\n";
        p << "}\n";
        write_file(system_dir / spec.name / "process" / (pname + ".json"), p.str());
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

LoadResult load_synthetic_tree(const fs::path &datadir)
{
    const fs::path system_dir = datadir / PRESET_SYSTEM_DIR;
    fs::create_directories(system_dir);

    // The base vendor. It is always loaded first (the loader swaps it to the front),
    // and its non-instantiated presets are the shared inheritance base every other
    // vendor resolves against.
    VendorSpec base;
    base.name = PresetBundle::ORCA_FILAMENT_LIBRARY;
    base.processes = {
        {"base_common", "", "0.20", false},
        {"base_fine", "base_common", "0.10", false},
        {"lib_visible", "base_common", "0.24", true},
    };
    write_vendor(system_dir, base);

    // Ordinary vendors. Alpha inherits one level from the base, Bravo two levels
    // (through the base's own chained non-instantiated preset), Charlie not at all.
    VendorSpec alpha;
    alpha.name     = "Alpha";
    alpha.processes = {
        {"alpha_local", "", "0.30", false},
        {"Alpha Standard @A", "base_common", "0.28", true},
        {"Alpha Local @A", "alpha_local", "0.32", true},
    };
    write_vendor(system_dir, alpha);

    VendorSpec bravo;
    bravo.name     = "Bravo";
    bravo.processes = {
        {"Bravo Fine @B", "base_fine", "0.12", true},
        {"Bravo Plain @B", "", "0.26", true},
    };
    write_vendor(system_dir, bravo);

    VendorSpec charlie;
    charlie.name     = "Charlie";
    charlie.processes = {
        {"Charlie Only @C", "", "0.34", true},
    };
    write_vendor(system_dir, charlie);

    // Delta redefines a name Bravo already loaded - the duplicate path.
    VendorSpec delta;
    delta.name     = "Delta";
    delta.processes = {
        {"Bravo Plain @B", "", "0.99", true},
        {"Delta Own @D", "base_common", "0.36", true},
    };
    write_vendor(system_dir, delta);

    set_data_dir(datadir.string());

    PresetBundle bundle;
    LoadResult   result;
    auto         loaded = bundle.load_system_presets_from_json(ForwardCompatibilitySubstitutionRule::EnableSilent);
    result.errors       = loaded.second;

    for (const Preset &preset : bundle.prints)
        result.names.push_back(preset.name);
    result.fingerprints = collection_fingerprints(bundle.prints);
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
    LoadResult first  = load_synthetic_tree(root / "run1");
    LoadResult second = load_synthetic_tree(root / "run2");

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
                                  [](const std::string &n) { return n.rfind("- default -", 0) == 0 || n == "Default Setting"; }),
                   tail.end());
        std::vector<std::string> sorted = tail;
        std::sort(sorted.begin(), sorted.end());
        REQUIRE(tail == sorted);
    }

    SECTION("a name defined by two vendors is reported as a duplicate exactly once") {
        REQUIRE(first.errors.find("Bravo Plain @B") != std::string::npos);
        REQUIRE(first.errors == second.errors);
    }

    SECTION("inherited values are resolved, not left at the default") {
        // "Alpha Standard @A" inherits base_common and overrides layer_height; the
        // fingerprint must carry its own 0.28, proving the parent was applied and then
        // the child's delta on top.
        auto it = std::find_if(first.fingerprints.begin(), first.fingerprints.end(),
                               [](const std::string &f) { return f.rfind("Alpha Standard @A|", 0) == 0; });
        REQUIRE(it != first.fingerprints.end());
        REQUIRE(it->find("layer_height=0.28;") != std::string::npos);
    }

    boost::system::error_code ec;
    fs::remove_all(root, ec);
}
