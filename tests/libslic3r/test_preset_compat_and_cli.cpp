#include <catch2/catch.hpp>

#include "libslic3r/CliNamedPresets.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Time.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <string>

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

// The Snapmaker vendor of the shipped profiles, read into a bundle of its own with a data folder of
// its own under the system temp folder (never the user's).
struct SnapmakerBundle
{
    std::string  saved_data_dir;
    fs::path     scratch;
    PresetBundle library;
    PresetBundle bundle;

    SnapmakerBundle()
    {
        saved_data_dir = data_dir();
        scratch        = fs::temp_directory_path() / fs::unique_path("preset_compat_%%%%-%%%%");
        fs::create_directories(scratch);
        set_data_dir(scratch.string());
        const std::string profiles = (fs::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
        library.load_vendor_configs_from_json(profiles, PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem,
                                              ForwardCompatibilitySubstitutionRule::EnableSilent);
        bundle.load_vendor_configs_from_json(profiles, "Snapmaker", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent,
                                             &library);
        bundle.prints.update_user_presets_directory(scratch.string(), "process");
        bundle.printers.update_user_presets_directory(scratch.string(), "machine");
    }
    ~SnapmakerBundle()
    {
        set_data_dir(saved_data_dir);
        boost::system::error_code ec;
        fs::remove_all(scratch, ec);
    }

    // A user printer called `name` inheriting `parent` (`inherits` empty: a root of its own). Presets are
    // found by name afterwards: loading one moves the others in the collection.
    void add_user_printer(const std::string &name, const std::string &parent, const std::string &inherits)
    {
        DynamicPrintConfig config = bundle.printers.find_preset(parent)->config;
        config.option<ConfigOptionString>("inherits", true)->value = inherits;
        bundle.printers.load_preset((scratch / (name + ".json")).string(), name, std::move(config), false);
    }

    int compatible_system_processes(const std::string &printer_name)
    {
        const Preset *printer = bundle.printers.find_preset(printer_name);
        REQUIRE(printer != nullptr);
        const PresetWithVendorProfile active = bundle.printers.get_preset_with_vendor_profile(*printer);
        int count = 0;
        for (const Preset &process : bundle.prints.get_presets())
            if (process.is_system && process.is_visible && is_compatible_with_printer(bundle.prints.get_preset_with_vendor_profile(process), active))
                ++count;
        return count;
    }
};

const std::string U1 = "Snapmaker U1 (0.4 nozzle)";

} // namespace

TEST_CASE("A user printer made from another user printer is offered its system printer's processes", "[Preset][Compatibility][Regression]")
{
    SnapmakerBundle f;
    REQUIRE(f.bundle.printers.find_preset(U1) != nullptr);
    f.add_user_printer("My U1", U1, U1);
    f.add_user_printer("My U1 copy", U1, "My U1");
    // A user printer whose chain ends in a user printer with no parent.
    f.add_user_printer("Root", U1, "");
    f.add_user_printer("On root", U1, "Root");
    for (const char *name : { "My U1", "My U1 copy", "Root", "On root" })
        REQUIRE_FALSE(f.bundle.printers.find_preset(name)->is_system);

    const int for_system = f.compatible_system_processes(U1);
    const int for_mine   = f.compatible_system_processes("My U1");
    const int for_copy   = f.compatible_system_processes("My U1 copy");
    INFO("system " << for_system << ", user printer " << for_mine << ", user printer of the user printer " << for_copy);
    REQUIRE(for_system > 0);
    // A user printer on a system printer: unchanged, its parent's processes.
    CHECK(for_mine == for_system);
    // One more step up: the processes of the system printer it comes from (none before).
    CHECK(for_copy == for_system);
    // No system printer up the chain: none, as before.
    CHECK(f.compatible_system_processes("On root") == 0);
}

TEST_CASE("A new preset is saved with the time it was made", "[Preset][Regression]")
{
    SnapmakerBundle f;
    const std::string process = "0.20mm Standard @Snapmaker U1 (0.4 nozzle)";
    REQUIRE(f.bundle.prints.find_preset(process) != nullptr);
    REQUIRE(f.bundle.prints.select_preset_by_name(process, true));

    const long long before = (long long) Slic3r::Utils::get_current_time_utc();
    f.bundle.prints.save_current_preset("My process");
    const Preset *saved = f.bundle.prints.find_preset("My process");
    REQUIRE(saved != nullptr);
    CHECK(saved->updated_time >= before);

    // The .info file written with it says so too (it said 0 until the next start).
    fs::path info = saved->file;
    info.replace_extension(".info");
    REQUIRE(fs::exists(info));
    boost::nowide::ifstream ifs(info.string());
    std::string line, time;
    while (std::getline(ifs, line))
        if (line.rfind("updated_time = ", 0) == 0)
            time = line.substr(15);
    CHECK(time != "0");
    CHECK(std::atoll(time.c_str()) >= before);
}

TEST_CASE("--printer-preset with a user printer stands for its system printer", "[CLI][Config][Regression]")
{
    // The flat file for a user printer said "from": "system" and "inherits": "", so the CLI took the user
    // printer's own name for the system printer and no process listed it: exit 239, not compatible.
    const CliNamedPresetOrigin user_printer = cli_named_preset_origin(true, true, U1);
    CHECK(user_printer.from == "User");
    CHECK(user_printer.inherits == U1);
    CHECK(cli_config_system_name(user_printer.from, "My U1", user_printer.inherits) == U1);

    // A system printer, and a user printer with no system parent, as before: its own name.
    const CliNamedPresetOrigin system_printer = cli_named_preset_origin(true, false, "");
    CHECK(system_printer.from == "system");
    CHECK(system_printer.inherits.empty());
    CHECK(cli_config_system_name(system_printer.from, U1, system_printer.inherits) == U1);
    const CliNamedPresetOrigin root_printer = cli_named_preset_origin(true, true, "");
    CHECK(cli_config_system_name(root_printer.from, "My root", root_printer.inherits) == "My root");

    // Processes and filaments are written as before.
    const CliNamedPresetOrigin user_process = cli_named_preset_origin(false, true, "0.20mm Standard @Snapmaker U1 (0.4 nozzle)");
    CHECK(user_process.from == "system");
    CHECK(user_process.inherits.empty());
}
