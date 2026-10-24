// The shipped profile set (resources/profiles, every vendor) loaded the way the application
// loads it at startup: copied into a scratch data dir's system/ folder, plus a few user presets,
// then PresetBundle::load_presets(). Package 4B-2 (OrcaSlicer #13667 / #14155).
//
// 1. Determinism: the parallel load (vendors and files in parallel, with and without the CBOR
//    parse cache) must give exactly what a strictly sequential load gives - every preset of every
//    collection in the same order with the same name, alias, inherits, vendor, ids and resolved
//    config; the same alias table; the same vendor/model list; the same user presets resolved
//    against the same system parents (including a child of a detached user preset, #653); and
//    the same selection afterwards. test_preset_load_determinism.cpp guards the same property on
//    a small synthetic tree; this one runs the real 50+ vendors, aliases and inherits chains.
//
// 2. Benchmark (hidden, [.][Benchmark]): times load_presets() on the full set in each load mode,
//    N runs each (ORCA_BENCH_RUNS, default 5), and prints median / min. Run it explicitly:
//      libslic3r_tests "[PresetLoadBenchmark]"

#include <catch2/catch.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
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

fs::path shipped_profiles_dir()
{
    return fs::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles";
}

// FNV-1a 64: stable across runs and platforms, unlike std::hash.
struct Fnv64
{
    std::uint64_t h = 1469598103934665603ull;
    void add(const std::string &s)
    {
        for (unsigned char c : s) {
            h ^= c;
            h *= 1099511628211ull;
        }
        // Field separator, so ("ab","c") and ("a","bc") differ.
        h ^= 0xff;
        h *= 1099511628211ull;
    }
};

// Restores the environment-driven load mode however the test exits.
struct LoadModeGuard
{
    ~LoadModeGuard()
    {
        PresetBundle::set_sequential_load_override(std::nullopt);
        PresetBundle::set_parse_cache_override(std::nullopt);
    }
};

struct ScratchDataDir
{
    std::string saved_data_dir = data_dir();
    fs::path    root;

    ScratchDataDir()
    {
        root = fs::temp_directory_path() / "snorca_tests" / fs::unique_path("full_preset_set_%%%%%%%%");
        fs::create_directories(root);
    }
    ~ScratchDataDir()
    {
        set_data_dir(saved_data_dir);
        boost::system::error_code ec;
        fs::remove_all(root, ec);
    }
};

void copy_tree(const fs::path &from, const fs::path &to)
{
    fs::create_directories(to);
    for (fs::recursive_directory_iterator it(from), end; it != end; ++it) {
        const fs::path rel = fs::relative(it->path(), from);
        if (fs::is_directory(it->path()))
            fs::create_directories(to / rel);
        else
            fs::copy_file(it->path(), to / rel, fs::copy_option::overwrite_if_exists);
    }
}

void write_file(const fs::path &path, const std::string &content)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path.string(), std::ios::binary) << content;
}

// A user preset file in the default user folder; empty `inherits` makes a detached preset.
void write_user_preset(const fs::path &datadir, const std::string &section, const std::string &type, const std::string &name,
                       const std::string &inherits, const std::string &extra, bool in_base = false)
{
    fs::path folder = datadir / PRESET_USER_DIR / DEFAULT_USER_FOLDER_NAME / section;
    if (in_base)
        folder /= "base";
    std::ostringstream p;
    p << R"({"type":")" << type << R"(","name":")" << name << R"(","from":"User","version":"2.0.0.0","inherits":")" << inherits
      << '"' << extra << '}';
    write_file(folder / (name + ".json"), p.str());
}

// The data dir the app would have: every shipped vendor in system/, and user presets that
// inherit from system presets, a detached user preset and a child of it.
fs::path build_datadir(const fs::path &root)
{
    const fs::path datadir = root / "dd";
    copy_tree(shipped_profiles_dir(), datadir / PRESET_SYSTEM_DIR);

    write_user_preset(datadir, PRESET_FILAMENT_NAME, "filament", "Full Set User PLA", "Bambu PLA Basic @BBL X1C",
                      R"(,"filament_flow_ratio":["0.97"])");
    // Detached (standalone) parent in base/, and a child of it in the main folder (#653).
    write_user_preset(datadir, PRESET_FILAMENT_NAME, "filament", "Full Set Detached Parent", "",
                      R"(,"filament_type":["PETG"],"nozzle_temperature":["241"],"filament_vendor":["Edge"])", true);
    write_user_preset(datadir, PRESET_FILAMENT_NAME, "filament", "Full Set Detached Child", "Full Set Detached Parent",
                      R"(,"nozzle_temperature":["243"])");
    write_user_preset(datadir, PRESET_PRINT_NAME, "process", "Full Set User Process", "0.20mm Standard @BBL X1C",
                      R"(,"wall_loops":"4")");
    write_user_preset(datadir, PRESET_PRINTER_NAME, "machine", "Full Set User Printer", "Bambu Lab X1 Carbon 0.4 nozzle",
                      R"(,"printer_notes":"full set")");
    return datadir;
}

void drop_parse_cache(const fs::path &datadir)
{
    boost::system::error_code ec;
    fs::remove_all(datadir / "cache" / "presets", ec);
}

struct LoadSnapshot
{
    // One line per preset, in collection order; the config goes in as a hash.
    std::vector<std::string> presets;
    // alias -> the preset names that hold it, in table order.
    std::map<std::string, std::vector<std::string>> filament_aliases;
    std::vector<std::string> vendors;
    std::vector<std::string> selection;
    std::string              error;
    size_t                   system_presets = 0;
    size_t                   vendor_count   = 0;
};

std::string preset_line(const Preset &preset)
{
    Fnv64                h;
    t_config_option_keys keys = preset.config.keys();
    std::sort(keys.begin(), keys.end());
    for (const std::string &key : keys)
        if (const ConfigOption *opt = preset.config.option(key); opt != nullptr) {
            h.add(key);
            h.add(opt->serialize());
        }
    std::ostringstream out;
    out << Preset::get_type_string(preset.type) << '|' << preset.name << "|alias=" << preset.alias << "|inherits=" << preset.inherits()
        << "|vendor=" << (preset.vendor ? preset.vendor->name : std::string()) << "|sys=" << preset.is_system << "|def=" << preset.is_default
        << "|vis=" << preset.is_visible << "|setting_id=" << preset.setting_id << "|filament_id=" << preset.filament_id
        << "|base_id=" << preset.base_id << "|renamed_from=" << preset.renamed_from.size() << "|cfg=" << std::hex << h.h;
    return out.str();
}

LoadSnapshot load_snapshot(const fs::path &datadir)
{
    set_data_dir(datadir.string());

    LoadSnapshot snap;
    PresetBundle bundle;
    AppConfig    app_config;
    try {
        bundle.load_presets(app_config, ForwardCompatibilitySubstitutionRule::EnableSilent);
    } catch (const std::exception &ex) {
        snap.error = ex.what();
    }

    for (const PresetCollection *collection : {static_cast<const PresetCollection *>(&bundle.prints),
                                               static_cast<const PresetCollection *>(&bundle.filaments),
                                               static_cast<const PresetCollection *>(&bundle.printers)})
        for (const Preset &preset : *collection) {
            snap.presets.push_back(preset_line(preset));
            if (preset.is_system)
                ++snap.system_presets;
        }

    // The alias table as the GUI queries it: every holder of each alias, in lookup order.
    std::set<std::string> aliases;
    for (const Preset &preset : bundle.filaments)
        if (!preset.alias.empty())
            aliases.insert(preset.alias);
    for (const std::string &alias : aliases) {
        std::vector<std::string> &holders = snap.filament_aliases[alias];
        bundle.filaments.get_preset_name_by_alias(alias, [&holders](const Preset &p) {
            holders.push_back(p.name);
            return false;
        });
    }

    for (const auto &[name, vendor] : bundle.vendors) {
        std::ostringstream v;
        v << name << '|' << vendor.id << '|' << vendor.config_version.to_string();
        for (const auto &model : vendor.models) {
            v << '|' << model.id << ':' << model.family;
            for (const auto &variant : model.variants)
                v << ',' << variant.name;
        }
        snap.vendors.push_back(v.str());
    }
    snap.vendor_count = bundle.vendors.size();

    snap.selection = {bundle.printers.get_selected_preset_name(), bundle.prints.get_selected_preset_name(),
                      bundle.filaments.get_selected_preset_name()};
    return snap;
}

// The first difference between two snapshots, for the failure message.
std::string first_difference(const LoadSnapshot &a, const LoadSnapshot &b)
{
    if (a.presets.size() != b.presets.size())
        return "preset count " + std::to_string(a.presets.size()) + " vs " + std::to_string(b.presets.size());
    for (size_t i = 0; i < a.presets.size(); ++i)
        if (a.presets[i] != b.presets[i])
            return "preset #" + std::to_string(i) + ":\n  " + a.presets[i] + "\n  " + b.presets[i];
    if (a.filament_aliases != b.filament_aliases)
        return "filament alias table";
    if (a.vendors != b.vendors)
        return "vendor/model list";
    if (a.selection != b.selection)
        return "selection";
    if (a.error != b.error)
        return "error text";
    return {};
}

const std::string *find_line(const LoadSnapshot &snap, const std::string &prefix)
{
    for (const std::string &line : snap.presets)
        if (line.rfind(prefix, 0) == 0)
            return &line;
    return nullptr;
}

} // namespace

TEST_CASE("The full shipped profile set loads identically in parallel and sequentially", "[Preset][PresetLoadDeterminism][PresetLoadFullSet]")
{
    REQUIRE(fs::exists(shipped_profiles_dir()));
    LoadModeGuard  mode_guard;
    ScratchDataDir scratch;
    const fs::path datadir = build_datadir(scratch.root);

    // Sequential, no cache: one vendor and one file at a time - the reference.
    PresetBundle::set_sequential_load_override(true);
    PresetBundle::set_parse_cache_override(false);
    drop_parse_cache(datadir);
    const LoadSnapshot sequential = load_snapshot(datadir);

    // Parallel, cache on, cold: parses everything and writes the per-vendor CBOR cache.
    PresetBundle::set_sequential_load_override(false);
    PresetBundle::set_parse_cache_override(true);
    drop_parse_cache(datadir);
    const LoadSnapshot parallel_cold = load_snapshot(datadir);
    const bool cache_written = fs::exists(datadir / "cache" / "presets");

    // Parallel again with the cache in place: the normal app start on unchanged profiles.
    const LoadSnapshot parallel_warm = load_snapshot(datadir);

    // Parallel, no cache, twice: two runs of the racy path against each other.
    PresetBundle::set_parse_cache_override(false);
    drop_parse_cache(datadir);
    const LoadSnapshot parallel_a = load_snapshot(datadir);
    const LoadSnapshot parallel_b = load_snapshot(datadir);

    INFO("vendors: " << sequential.vendor_count << ", presets: " << sequential.presets.size() << " (" << sequential.system_presets
                     << " system), filament aliases: " << sequential.filament_aliases.size());
    REQUIRE(sequential.error.empty());
    // The shipped set really is the big one (59 vendor manifests in 2.4.x).
    CHECK(sequential.vendor_count >= 49);
    CHECK(sequential.system_presets > 5000);
    CHECK(cache_written);

    CHECK(first_difference(sequential, parallel_cold) == "");
    CHECK(first_difference(sequential, parallel_warm) == "");
    CHECK(first_difference(sequential, parallel_a) == "");
    CHECK(first_difference(parallel_a, parallel_b) == "");

    SECTION("user presets load after the system ones, on their system and detached parents") {
        const std::string *user_pla = find_line(sequential, "filament|Full Set User PLA|");
        REQUIRE(user_pla != nullptr);
        CHECK(user_pla->find("|inherits=Bambu PLA Basic @BBL X1C|") != std::string::npos);
        CHECK(user_pla->find("|sys=0|") != std::string::npos);
        const std::string *detached = find_line(sequential, "filament|Full Set Detached Parent|");
        const std::string *child    = find_line(sequential, "filament|Full Set Detached Child|");
        REQUIRE(detached != nullptr);
        REQUIRE(child != nullptr);
        CHECK(detached->find("|inherits=|") != std::string::npos);
        CHECK(child->find("|inherits=Full Set Detached Parent|") != std::string::npos);
        CHECK(find_line(sequential, "process|Full Set User Process|") != nullptr);
        CHECK(find_line(sequential, "machine|Full Set User Printer|") != nullptr);
    }
}

TEST_CASE("Benchmark: full profile set load time by load mode", "[.][Benchmark][PresetLoadBenchmark]")
{
    REQUIRE(fs::exists(shipped_profiles_dir()));
    LoadModeGuard  mode_guard;
    ScratchDataDir scratch;
    const fs::path datadir = build_datadir(scratch.root);

    int runs = 5;
    if (const char *v = std::getenv("ORCA_BENCH_RUNS"); v != nullptr)
        runs = std::max(1, std::atoi(v));

    struct Mode
    {
        const char *name;
        bool        sequential;
        bool        cache;
        bool        drop_cache_each_run;
    };
    const std::vector<Mode> modes = {
        // Upstream before #13667 had no per-file parallelism or cache either: this is that load.
        {"sequential_nocache", true, false, true},
        // #13667's vendor-parallel load plus our per-file parallel parse, no cache.
        {"parallel_nocache", false, false, true},
        // First start after a profile update: parse in parallel and write the cache.
        {"parallel_cache_cold", false, true, true},
        // Ordinary start on unchanged profiles: the cache is hit.
        {"parallel_cache_warm", false, true, false},
    };

    // One untimed warm-up load, so the first timed mode does not pay for a cold OS file cache.
    PresetBundle::set_sequential_load_override(false);
    PresetBundle::set_parse_cache_override(false);
    load_snapshot(datadir);

    for (const Mode &mode : modes) {
        PresetBundle::set_sequential_load_override(mode.sequential);
        PresetBundle::set_parse_cache_override(mode.cache);
        drop_parse_cache(datadir);
        if (!mode.drop_cache_each_run)
            load_snapshot(datadir); // prime the cache, untimed
        std::vector<long long> ms;
        for (int i = 0; i < runs; ++i) {
            if (mode.drop_cache_each_run)
                drop_parse_cache(datadir);
            set_data_dir(datadir.string());
            PresetBundle bundle;
            AppConfig    app_config;
            const auto   start = std::chrono::steady_clock::now();
            bundle.load_presets(app_config, ForwardCompatibilitySubstitutionRule::EnableSilent);
            ms.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
        }
        std::vector<long long> sorted = ms;
        std::sort(sorted.begin(), sorted.end());
        std::ostringstream all;
        for (long long m : ms)
            all << (all.tellp() > 0 ? "," : "") << m;
        std::cout << "PRESET_LOAD_BENCH mode=" << mode.name << " runs=" << runs << " median_ms=" << sorted[sorted.size() / 2]
                  << " min_ms=" << sorted.front() << " max_ms=" << sorted.back() << " all=" << all.str() << std::endl;
    }
    SUCCEED();
}
