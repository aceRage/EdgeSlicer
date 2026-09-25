#ifndef slic3r_Utils_BambuStudioLauncher_hpp_
#define slic3r_Utils_BambuStudioLauncher_hpp_

// "Export & Open in Bambu Studio": after Plater::export_bambu_3mf() writes the file, find an
// installed Bambu Studio and hand it the exported path.
//
// Discovery and command-line construction are pure, GUI-independent logic so they can be unit
// tested without wxWidgets or a real registry/filesystem (tests/slic3rutils/bambu_studio_launcher_tests.cpp).
// The actual registry reads / filesystem probes / process launch are thin platform adapters that
// GUI code (Plater.cpp) wires up; tests inject fakes instead.

#include <functional>
#include <string>
#include <vector>

namespace Slic3r {
namespace BambuStudioLauncher {

// -------------------------------------------------------------------------------------------
// Discovery (Windows)
// -------------------------------------------------------------------------------------------

// One registry hive/view combination to probe, in the precedence order Windows searches would
// naturally suggest: HKLM 64-bit, HKLM 32-bit (WOW6432Node), HKCU 64-bit, HKCU 32-bit.
enum class RegHive { HKLM, HKCU };
enum class RegView { Default, Wow6432 };

struct RegLookup
{
    RegHive     hive;
    RegView     view;
    std::string subkey; // e.g. "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{...}"
};

// A source of registry string values, injected so tests can supply a fixed map instead of the
// real Windows registry. Returns false when the key/value does not exist.
using RegReader = std::function<bool(const RegLookup &where, const std::string &value_name, std::string &out)>;

// A source of "does this path exist as a file" checks, injected for the same reason.
using FileExists = std::function<bool(const std::string &path)>;

// Windows: the uninstall-key subpaths under each of Uninstall\ and
// Uninstall\{GUID-or-name} that we recognize as "Bambu Studio". Real subkeys carry a random
// GUID or a product name; this lists the DisplayName substrings we match on a full enumeration
// (the caller enumerates subkeys; this module only defines the match predicate), plus the
// well-known fixed App Paths key.
bool looks_like_bambu_studio_display_name(const std::string &display_name);

// App Paths\bambu-studio.exe (HKLM and HKCU, both registry views) - the direct, non-enumerated
// lookup used before falling back to scanning Uninstall.
extern const RegLookup APP_PATHS_LOOKUPS[4];

// Windows default install location, tried last.
extern const char *const DEFAULT_INSTALL_PATH;

// One candidate the discovery pass considered, kept for diagnostics/tests: where it came from
// and whether the path it produced actually exists on disk.
struct Candidate
{
    std::string source; // "AppPaths", "Uninstall", "AssocQueryString", "DefaultPath"
    std::string path;
    bool        exists = false;
};

struct DiscoveryResult
{
    bool                    found = false;
    std::string             exe_path; // valid only when found
    std::vector<Candidate>  candidates; // every candidate considered, in precedence order, for diagnostics
};

// Uninstall-key entry as read by the caller's enumeration (InstallLocation and/or DisplayIcon,
// whichever the real key has; either may be empty).
struct UninstallEntry
{
    std::string display_name;
    std::string install_location; // directory, may be empty
    std::string display_icon;     // often "<path>\bambu-studio.exe,0" or similar, may be empty
};

// Pure precedence logic, given already-read inputs (no registry/filesystem access here):
//   1. App Paths value for bambu-studio.exe (HKLM 64, HKLM 32, HKCU 64, HKCU 32).
//   2. Uninstall entries recognized as Bambu Studio (first match wins; InstallLocation preferred
//      over parsing DisplayIcon).
//   3. The program associated with .3mf, only if that program's path is recognized as Bambu
//      Studio (never launches an unrelated .3mf handler).
//   4. The hardcoded default Program Files path.
// Every candidate is checked with `exists`; the first that exists wins. `app_paths_value` is the
// App Paths lookup result per entry of APP_PATHS_LOOKUPS (empty string = not found). `assoc_exe`
// is the resolved .3mf association's target executable path (empty = none / lookup failed).
DiscoveryResult discover_windows(const std::vector<std::string>        &app_paths_values,
                                  const std::vector<UninstallEntry>     &uninstall_entries,
                                  const std::string                     &assoc_exe,
                                  const FileExists                      &exists);

// -------------------------------------------------------------------------------------------
// Command construction (all platforms)
// -------------------------------------------------------------------------------------------

// Windows: quote a single argument for CreateProcess/wxExecute's lpCommandLine the way the
// Win32 argv parser expects (backslash-escaping only matters directly before a closing quote).
// Handles spaces and embedded quotes; leaves already-safe tokens unquoted only when they
// contain no space, tab or quote.
std::string win_quote_argument(const std::string &arg);

// Windows: full command line to launch `exe_path` with the single argument `file_path`, both
// quoted. This is what gets handed to CreateProcess/wxExecute.
std::string win_build_command_line(const std::string &exe_path, const std::string &file_path);

// macOS: argv (including argv[0] "open") for `open -a "BambuStudio" <file>` (the bundle-name
// form tried first).
std::vector<std::string> macos_open_by_name_args(const std::string &file_path, const std::string &app_name = "BambuStudio");
// macOS: argv (including argv[0] "open") for the bundle-id fallback, `open -b <bundle id> <file>`.
std::vector<std::string> macos_open_by_bundle_id_args(const std::string &file_path, const std::string &bundle_id = "com.bambulab.bambu-studio");

// Linux: argv (including argv[0], the executable) to run the PATH executable directly.
std::vector<std::string> linux_path_exec_args(const std::string &file_path, const std::string &exe_name = "bambu-studio");
// Linux: argv (including argv[0] "flatpak") for `flatpak run com.bambulab.BambuStudio <file>`.
std::vector<std::string> linux_flatpak_args(const std::string &file_path, const std::string &app_id = "com.bambulab.BambuStudio");

// -------------------------------------------------------------------------------------------
// Platform entry point (real registry / filesystem / process launch - not unit tested, thin
// glue over the pure logic above; see BambuStudioLauncherTest for that).
// -------------------------------------------------------------------------------------------

enum class LaunchOutcome {
    Launched,  // a Bambu Studio process was started
    NotFound,  // no installation could be located
    LaunchFailed, // found but the process failed to start
};

struct LaunchResult
{
    LaunchOutcome outcome = LaunchOutcome::NotFound;
    std::string   exe_path; // set when an install was found (Launched or LaunchFailed)
};

// Optional override, e.g. from Preferences > Other > "Bambu Studio path". When non-empty and it
// exists on disk, this is used directly and normal discovery is skipped.
LaunchResult open_in_bambu_studio(const std::string &file_path, const std::string &custom_exe_path = {});

} // namespace BambuStudioLauncher
} // namespace Slic3r

#endif // slic3r_Utils_BambuStudioLauncher_hpp_
