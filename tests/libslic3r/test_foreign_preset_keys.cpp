// Keys that bundled or user preset files carry but presets deliberately do not hold.
//
// User process presets saved while mixed filaments were in use carry mixed_filament_definitions, a
// project key that is not part of the preset option lists, so the loader drops it. That used to log
// one error per file. PrintConfigDef::unsupported_foreign_key() names it, and
// Preset::remove_invalid_keys() drops it as before but only counts it; the totals are written once,
// at info level. A key that is genuinely unknown must still be an error.
//
// The Bambu Studio flush keys (filament_flush_temp, filament_flush_temp_fast,
// filament_flush_volumetric_speed, filament_cooling_before_tower) were in that list too; filament
// presets hold them now (test_bambu_flush_keys.cpp), so they load like any other filament setting.

#include <catch2/catch.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
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

// boost::log::trivial severities, as handed to the log observer.
constexpr int SEV_INFO  = 2;
constexpr int SEV_ERROR = 4;

// Collects log records from the moment it is created (info and worse) until it goes out of scope.
class LogCapture
{
public:
    LogCapture()
    {
        set_logging_level(3); // info
        set_log_observer(
            [this](int severity, const std::string &message) {
                // Errors, and the foreign-key summary; a full profile load logs many thousands of other infos.
                if (severity < SEV_ERROR && message.find("Ignored") == std::string::npos)
                    return;
                std::lock_guard<std::mutex> lock(m_mutex);
                m_records.emplace_back(severity, message);
            },
            SEV_INFO);
    }
    ~LogCapture()
    {
        set_log_observer(nullptr, SEV_INFO);
        set_logging_level(2); // the level the test binary starts with
    }

    size_t count(int severity) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return size_t(std::count_if(m_records.begin(), m_records.end(), [&](const auto &r) { return r.first == severity; }));
    }

    std::vector<std::string> messages(int severity, const std::string &needle) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<std::string> out;
        for (const auto &r : m_records)
            if (r.first == severity && r.second.find(needle) != std::string::npos)
                out.push_back(r.second);
        return out;
    }

private:
    mutable std::mutex                         m_mutex;
    std::vector<std::pair<int, std::string>>   m_records;
};

// One system vendor with a base filament and a few derived ones, plus a user process preset.
//   extra_filament_keys: raw JSON members appended to every filament preset of the vendor.
//   extra_user_keys:     raw JSON members appended to the user process preset.
struct TreeSpec
{
    std::string extra_filament_keys;
    std::string extra_user_keys;
};

constexpr const char *VENDOR = "KeyNoiseVendor";

void build_tree(const fs::path &datadir, const TreeSpec &spec)
{
    const fs::path system_dir = datadir / PRESET_SYSTEM_DIR;
    const std::vector<std::string> filaments = {"KeyNoise PLA @K", "KeyNoise PETG @K", "KeyNoise TPU @K"};

    std::ostringstream manifest;
    manifest << "{\n  \"name\": \"" << VENDOR << "\",\n  \"version\": \"2.0.0.0\",\n";
    manifest << "  \"machine_model_list\": [ { \"name\": \"KeyNoise-M\", \"sub_path\": \"machine/KeyNoise-M.json\" } ],\n";
    manifest << "  \"process_list\": [],\n  \"filament_list\": [\n";
    for (size_t i = 0; i < filaments.size(); ++i)
        manifest << "    { \"name\": \"" << filaments[i] << "\", \"sub_path\": \"filament/" << filaments[i] << ".json\" }"
                 << (i + 1 < filaments.size() ? "," : "") << "\n";
    manifest << "  ],\n  \"machine_list\": []\n}\n";
    write_file(system_dir / (std::string(VENDOR) + ".json"), manifest.str());

    write_file(system_dir / VENDOR / "machine" / "KeyNoise-M.json",
               "{ \"name\": \"KeyNoise-M\", \"model_id\": \"KeyNoise-M\", \"nozzle_diameter\": \"0.4\", \"family\": \"KeyNoise\" }\n");

    for (size_t i = 0; i < filaments.size(); ++i) {
        std::ostringstream p;
        p << "{\n  \"type\": \"filament\",\n  \"name\": \"" << filaments[i] << "\",\n  \"from\": \"system\",\n"
          << "  \"instantiation\": \"true\",\n  \"filament_id\": \"KN" << i << "\",\n"
          << "  \"filament_flow_ratio\": [\"0.9" << (10 + i) << "\"]";
        if (!spec.extra_filament_keys.empty())
            p << ",\n  " << spec.extra_filament_keys;
        p << "\n}\n";
        write_file(system_dir / VENDOR / "filament" / (filaments[i] + ".json"), p.str());
    }

    std::ostringstream u;
    u << "{\n  \"type\": \"process\",\n  \"name\": \"KeyNoise User Process\",\n  \"from\": \"User\",\n"
      << "  \"inherits\": \"\",\n  \"version\": \"1.0.0.0\",\n  \"layer_height\": \"0.16\"";
    if (!spec.extra_user_keys.empty())
        u << ",\n  " << spec.extra_user_keys;
    u << "\n}\n";
    write_file(datadir / PRESET_USER_DIR / "default" / PRESET_PRINT_NAME / "KeyNoise User Process.json", u.str());
}

// Everything a loaded preset holds, as one string per preset, in collection order.
std::vector<std::string> fingerprints(const PresetCollection &collection)
{
    std::vector<std::string> out;
    for (const Preset &preset : collection) {
        if (preset.name.find("KeyNoise") == std::string::npos)
            continue;
        std::ostringstream o;
        o << preset.name << '|';
        t_config_option_keys keys = preset.config.keys();
        std::sort(keys.begin(), keys.end());
        for (const std::string &key : keys)
            if (const ConfigOption *opt = preset.config.option(key))
                o << key << '=' << opt->serialize() << ';';
        out.push_back(o.str());
    }
    return out;
}

struct Loaded
{
    std::vector<std::string> filament_prints; // fingerprints of the filament presets
    std::vector<std::string> process_prints;  // fingerprints of the process presets
    std::vector<std::string> error_messages;
    std::vector<std::string> info_messages;
};

Loaded load_tree(const fs::path &datadir, const TreeSpec &spec)
{
    build_tree(datadir, spec);
    const std::string saved = data_dir();
    set_data_dir(datadir.string());
    Preset::reset_ignored_foreign_keys();

    Loaded result;
    {
        LogCapture log;
        PresetBundle bundle;
        AppConfig    app_config;
        bundle.load_presets(app_config, ForwardCompatibilitySubstitutionRule::EnableSilent);
        result.filament_prints = fingerprints(bundle.filaments);
        result.process_prints  = fingerprints(bundle.prints);
        result.error_messages  = log.messages(SEV_ERROR, "");
        result.info_messages   = log.messages(SEV_INFO, "occurrences of");
    }
    set_data_dir(saved);
    return result;
}

bool any_mentions(const std::vector<std::string> &messages, const std::string &needle)
{
    return std::any_of(messages.begin(), messages.end(),
                       [&](const std::string &m) { return m.find(needle) != std::string::npos; });
}

// Bambu Studio's flush keys, with values that differ from their defaults.
const char *const BAMBU_FLUSH_KEYS =
    "\"filament_flush_temp\": [\"200\"],\n  \"filament_flush_temp_fast\": [\"210\"],\n"
    "  \"filament_flush_volumetric_speed\": [\"3\"],\n  \"filament_cooling_before_tower\": [\"5\"]";

const char *const BAMBU_FLUSH_KEY_NAMES[] = {"filament_flush_temp", "filament_flush_temp_fast", "filament_flush_volumetric_speed",
                                             "filament_cooling_before_tower"};

} // namespace

TEST_CASE("The known foreign keys are named, an unknown key is not", "[Preset][ForeignKeys]")
{
    PrintConfigDef::ForeignKeyOrigin origin;
    // Supported now: filament presets hold them.
    for (const char *key : BAMBU_FLUSH_KEY_NAMES) {
        INFO("key: " << key);
        REQUIRE_FALSE(PrintConfigDef::unsupported_foreign_key(key, &origin));
    }
    REQUIRE(PrintConfigDef::unsupported_foreign_key("mixed_filament_definitions", &origin));
    REQUIRE(origin == PrintConfigDef::ForeignKeyOrigin::ProjectScoped);

    REQUIRE_FALSE(PrintConfigDef::unsupported_foreign_key("totally_unknown_key"));
    REQUIRE_FALSE(PrintConfigDef::unsupported_foreign_key("filament_flow_ratio"));
    REQUIRE_FALSE(PrintConfigDef::unsupported_foreign_key(""));
}

TEST_CASE("remove_invalid_keys drops foreign keys silently and unknown keys as errors", "[Preset][ForeignKeys]")
{
    Preset::reset_ignored_foreign_keys();

    // What a filament preset is made of.
    DynamicPrintConfig defaults;
    defaults.apply(FullPrintConfig::defaults(), true);
    DynamicPrintConfig filament_defaults;
    for (const std::string &key : Preset::filament_options())
        if (const ConfigOption *opt = defaults.option(key))
            filament_defaults.set_key_value(key, opt->clone());
    REQUIRE(filament_defaults.has("filament_flow_ratio"));
    for (const char *key : BAMBU_FLUSH_KEY_NAMES) {
        INFO("key: " << key);
        REQUIRE(filament_defaults.has(key));
    }

    DynamicPrintConfig config = filament_defaults;
    config.set_key_value("filament_flush_temp", new ConfigOptionIntsNullable({200}));
    config.set_key_value("filament_flush_volumetric_speed", new ConfigOptionFloatsNullable({3.}));
    config.set_key_value("filament_cooling_before_tower", new ConfigOptionFloatsNullable({5.}));
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString("1,2,1,0,50,0,0"));

    SECTION("only foreign keys: nothing is reported as incorrect, they are gone, the flush keys stay")
    {
        DynamicPrintConfig expected = config;
        expected.erase("mixed_filament_definitions");
        REQUIRE(Preset::remove_invalid_keys(config, filament_defaults).empty());
        REQUIRE(config == expected);
        REQUIRE_FALSE(config.has("mixed_filament_definitions"));
        REQUIRE(Preset::ignored_foreign_key_count("mixed_filament_definitions") == 1);
        REQUIRE(config.option<ConfigOptionIntsNullable>("filament_flush_temp")->values == std::vector<int>{200});
        REQUIRE(config.option<ConfigOptionFloatsNullable>("filament_cooling_before_tower")->values == std::vector<double>{5.});
        for (const char *key : BAMBU_FLUSH_KEY_NAMES) {
            INFO("key: " << key);
            REQUIRE(Preset::ignored_foreign_key_count(key) == 0);
        }
    }

    SECTION("a genuinely unknown key next to them is still reported, and only it")
    {
        config.set_key_value("totally_unknown_key", new ConfigOptionString("x"));
        const std::string incorrect = Preset::remove_invalid_keys(config, filament_defaults);
        REQUIRE(incorrect == "totally_unknown_key");
        REQUIRE_FALSE(config.has("totally_unknown_key"));
        REQUIRE_FALSE(config.has("mixed_filament_definitions"));
        REQUIRE(config.has("filament_flush_temp"));
        REQUIRE(Preset::ignored_foreign_key_count("totally_unknown_key") == 0);
    }

    SECTION("a config without such keys is left alone")
    {
        DynamicPrintConfig clean = filament_defaults;
        REQUIRE(Preset::remove_invalid_keys(clean, filament_defaults).empty());
        REQUIRE(clean == filament_defaults);
        REQUIRE(Preset::ignored_foreign_key_count("mixed_filament_definitions") == 0);
    }

    Preset::reset_ignored_foreign_keys();
}

TEST_CASE("Loading presets that carry foreign keys logs no error and loads the same values", "[Preset][ForeignKeys]")
{
    const fs::path root = fs::temp_directory_path() / fs::unique_path("orca_foreign_keys_%%%%%%%%");
    fs::create_directories(root);

    TreeSpec plain;
    TreeSpec foreign;
    foreign.extra_user_keys = "\"mixed_filament_definitions\": \"1,2,1,0,50,0,0\"";
    TreeSpec flush = foreign;
    flush.extra_filament_keys = BAMBU_FLUSH_KEYS;
    TreeSpec unknown = flush;
    unknown.extra_filament_keys += ",\n  \"layer_height\": \"0.3\"";

    const Loaded without    = load_tree(root / "without", plain);
    const Loaded with       = load_tree(root / "with", foreign);
    const Loaded with_flush = load_tree(root / "flush", flush);

    // The three bundled presets and the user process preset came through in every tree.
    for (const Loaded *l : {&without, &with, &with_flush}) {
        REQUIRE(l->filament_prints.size() == 3);
        REQUIRE(l->process_prints.size() == 1);
    }

    SECTION("same loaded values as without the foreign key")
    {
        REQUIRE(with.filament_prints == without.filament_prints);
        REQUIRE(with.process_prints == without.process_prints);
    }

    SECTION("the flush keys are kept with their values, the defaults where a preset has none")
    {
        for (size_t i = 0; i < 3; ++i) {
            INFO("preset: " << with_flush.filament_prints[i].substr(0, 40));
            REQUIRE(with_flush.filament_prints[i].find("filament_flush_temp=200;") != std::string::npos);
            REQUIRE(with_flush.filament_prints[i].find("filament_flush_temp_fast=210;") != std::string::npos);
            REQUIRE(with_flush.filament_prints[i].find("filament_flush_volumetric_speed=3;") != std::string::npos);
            REQUIRE(with_flush.filament_prints[i].find("filament_cooling_before_tower=5;") != std::string::npos);
            REQUIRE(without.filament_prints[i].find("filament_flush_temp=0;") != std::string::npos);
            REQUIRE(without.filament_prints[i].find("filament_flush_temp_fast=0;") != std::string::npos);
            REQUIRE(without.filament_prints[i].find("filament_flush_volumetric_speed=0;") != std::string::npos);
            // Bambu Studio's fdm_filament_common.json: no cooling for a filament that does not ask for it.
            REQUIRE(without.filament_prints[i].find("filament_cooling_before_tower=0;") != std::string::npos);
        }
        REQUIRE(with_flush.process_prints == without.process_prints);
    }

    SECTION("no error-level message names them or complains about incorrect keys")
    {
        for (const char *key : {"filament_flush_temp", "filament_flush_volumetric_speed", "filament_cooling_before_tower",
                                "mixed_filament_definitions", "incorrect keys"}) {
            INFO("key: " << key);
            REQUIRE_FALSE(any_mentions(with.error_messages, key));
            REQUIRE_FALSE(any_mentions(with_flush.error_messages, key));
        }
    }

    SECTION("the foreign key is reported once, with its count, at info level; the flush keys are not")
    {
        // The user process preset carries the project key.
        REQUIRE(with_flush.info_messages.size() == 1);
        const std::string &proj = with_flush.info_messages[0];
        REQUIRE(proj.find("Ignored 1 occurrences of project-level keys saved into presets (1 preset)") != std::string::npos);
        REQUIRE(proj.find("mixed_filament_definitions (1)") != std::string::npos);
        REQUIRE(proj.find("filament_flush") == std::string::npos);
        REQUIRE(without.info_messages.empty());
    }

    SECTION("a misplaced key still logs an error, naming it and only it")
    {
        const Loaded bad = load_tree(root / "unknown", unknown);
        // layer_height is a process option: defined, but not part of a filament preset, which is exactly what the
        // check exists for. (A key the config definition does not know at all is already dropped silently while the
        // file is parsed, see PrintConfigDef::handle_legacy.) One error per vendor filament that carries it.
        const auto incorrect = std::count_if(bad.error_messages.begin(), bad.error_messages.end(),
                                             [](const std::string &m) { return m.find("incorrect keys: layer_height,") != std::string::npos; });
        REQUIRE(incorrect == 3);
        REQUIRE_FALSE(any_mentions(bad.error_messages, "filament_flush_temp"));
        REQUIRE_FALSE(any_mentions(bad.error_messages, "filament_cooling_before_tower"));
        // And the values still match the tree without the misplaced key.
        REQUIRE(bad.filament_prints == with_flush.filament_prints);
    }

    boost::system::error_code ec;
    fs::remove_all(root, ec);
}

// Not run by default. Measures how many error lines a start logs with a real profile tree:
//   KEYNOISE_DATADIR=<dir containing system/<vendor>.json + system/<vendor>/...> libslic3r_tests "[KeyNoiseProbe]"
// The same load on a build without the foreign-key handling logged one error per file that carries one
// of the keys (each such file is counted in "ignored" below), so errors-before = errors-after + files.
TEST_CASE("Startup error lines with a real profile tree", "[.][KeyNoiseProbe]")
{
    const char *dd = std::getenv("KEYNOISE_DATADIR");
    if (dd == nullptr) {
        WARN("KEYNOISE_DATADIR is not set");
        return;
    }

    const std::string saved = data_dir();
    set_data_dir(dd);
    Preset::reset_ignored_foreign_keys();
    {
        LogCapture log;
        PresetBundle bundle;
        AppConfig    app_config;
        bundle.load_presets(app_config, ForwardCompatibilitySubstitutionRule::EnableSilent);
        std::cout << "KEYNOISE_ERRORS=" << log.count(SEV_ERROR) << "\n";
        for (const auto &m : log.messages(SEV_INFO, "occurrences of"))
            std::cout << "KEYNOISE_INFO=" << m << "\n";
        for (const auto &m : log.messages(SEV_ERROR, ""))
            std::cout << "KEYNOISE_ERRLINE=" << m.substr(0, 400) << "\n";
    }
    for (const char *key : {"mixed_filament_definitions"})
        std::cout << "KEYNOISE_IGNORED " << key << "=" << Preset::ignored_foreign_key_count(key) << "\n";
    set_data_dir(saved);
}
