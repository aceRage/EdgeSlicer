#include "BambuStudioLauncher.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>

#ifdef _WIN32
#include <windows.h>
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")
#elif defined(__APPLE__)
#include <boost/process/spawn.hpp>
#include <boost/process/args.hpp>
#include <boost/process/search_path.hpp>
#else
#include <cstdlib>
#include <boost/process/spawn.hpp>
#include <boost/process/args.hpp>
#include <boost/process/search_path.hpp>
#endif

#include <wx/wx.h>

namespace Slic3r {
namespace BambuStudioLauncher {

// -------------------------------------------------------------------------------------------
// Discovery (Windows)
// -------------------------------------------------------------------------------------------

const RegLookup APP_PATHS_LOOKUPS[4] = {
    { RegHive::HKLM, RegView::Default, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\bambu-studio.exe" },
    { RegHive::HKLM, RegView::Wow6432, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\bambu-studio.exe" },
    { RegHive::HKCU, RegView::Default, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\bambu-studio.exe" },
    { RegHive::HKCU, RegView::Wow6432, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\bambu-studio.exe" },
};

const char *const DEFAULT_INSTALL_PATH = "C:\\Program Files\\Bambu Studio\\bambu-studio.exe";

namespace {

std::string to_lower(const std::string &s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool contains_ci(const std::string &haystack, const std::string &needle)
{
    return to_lower(haystack).find(to_lower(needle)) != std::string::npos;
}

// DisplayIcon is typically "<path>\bambu-studio.exe" or "<path>\bambu-studio.exe,0" (icon index
// suffix). Strip a trailing ",<digits>" and return the path portion, or empty if it doesn't look
// like an .exe reference at all.
std::string strip_icon_index(const std::string &display_icon)
{
    if (display_icon.empty())
        return {};
    std::string path = display_icon;
    auto comma = path.rfind(',');
    if (comma != std::string::npos) {
        bool all_digits = comma + 1 < path.size();
        for (size_t i = comma + 1; all_digits && i < path.size(); ++i)
            all_digits = std::isdigit(static_cast<unsigned char>(path[i])) != 0;
        if (all_digits)
            path = path.substr(0, comma);
    }
    return path;
}

bool ends_with_ci(const std::string &s, const std::string &suffix)
{
    if (suffix.size() > s.size())
        return false;
    return to_lower(s.substr(s.size() - suffix.size())) == to_lower(suffix);
}

// Join a directory and the known exe filename, tolerating a missing/extra trailing separator.
std::string join_exe(const std::string &dir)
{
    if (dir.empty())
        return {};
    std::string d = dir;
    char        last = d.back();
    if (last != '\\' && last != '/')
        d += '\\';
    return d + "bambu-studio.exe";
}

} // namespace

bool looks_like_bambu_studio_display_name(const std::string &display_name)
{
    // Real installers use "Bambu Studio"; be tolerant of a missing space or case variation but
    // don't match on "Bambu" alone since Bambu Lab ships other apps (Handy, Connect, etc.) whose
    // DisplayName also starts with "Bambu".
    return contains_ci(display_name, "Bambu Studio") || contains_ci(display_name, "BambuStudio");
}

DiscoveryResult discover_windows(const std::vector<std::string>    &app_paths_values,
                                  const std::vector<UninstallEntry> &uninstall_entries,
                                  const std::string                 &assoc_exe,
                                  const FileExists                  &exists)
{
    DiscoveryResult result;
    auto             consider = [&](const std::string &source, const std::string &path) -> bool {
        if (path.empty())
            return false;
        bool ok = exists && exists(path);
        result.candidates.push_back({ source, path, ok });
        if (ok && !result.found) {
            result.found    = true;
            result.exe_path = path;
        }
        return ok;
    };

    // 1. App Paths, in precedence order (HKLM 64, HKLM 32, HKCU 64, HKCU 32).
    for (size_t i = 0; i < app_paths_values.size() && i < 4; ++i)
        if (consider("AppPaths", app_paths_values[i]) && result.found)
            return result;

    // 2. Uninstall entries recognized as Bambu Studio: InstallLocation (joined with the known
    // exe name) preferred, then DisplayIcon parsed for an .exe path.
    for (const UninstallEntry &entry : uninstall_entries) {
        if (!looks_like_bambu_studio_display_name(entry.display_name))
            continue;
        if (!entry.install_location.empty()) {
            std::string candidate = join_exe(entry.install_location);
            if (consider("Uninstall", candidate) && result.found)
                return result;
        }
        if (!entry.display_icon.empty()) {
            std::string candidate = strip_icon_index(entry.display_icon);
            if (ends_with_ci(candidate, ".exe") && consider("Uninstall", candidate) && result.found)
                return result;
        }
    }

    // 3. .3mf file association, only if it resolves to something that looks like Bambu Studio's
    // own executable (never hand off to an unrelated registered .3mf handler).
    if (!assoc_exe.empty() && (contains_ci(assoc_exe, "bambu-studio.exe") || contains_ci(assoc_exe, "bambustudio.exe"))) {
        consider("AssocQueryString", assoc_exe);
        if (result.found)
            return result;
    }

    // 4. Hardcoded default.
    consider("DefaultPath", DEFAULT_INSTALL_PATH);
    return result;
}

// -------------------------------------------------------------------------------------------
// Command construction (all platforms)
// -------------------------------------------------------------------------------------------

// Win32 CommandLineToArgvW-compatible quoting (the rules MSDN documents for building
// lpCommandLine): wrap in quotes; a backslash run is only escaped (doubled) when it immediately
// precedes a literal quote we are about to emit (either an embedded '"' or the closing quote).
std::string win_quote_argument(const std::string &arg)
{
    bool needs_quotes = arg.empty() || arg.find_first_of(" \t\"") != std::string::npos;
    if (!needs_quotes)
        return arg;

    std::string out = "\"";
    size_t      backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            // Escape all pending backslashes, then the quote itself.
            out.append(backslashes * 2 + 1, '\\');
            out += '"';
            backslashes = 0;
            continue;
        }
        if (backslashes) {
            out.append(backslashes, '\\');
            backslashes = 0;
        }
        out += c;
    }
    // Trailing backslashes must be doubled since they precede the closing quote.
    out.append(backslashes * 2, '\\');
    out += '"';
    return out;
}

std::string win_build_command_line(const std::string &exe_path, const std::string &file_path)
{
    return win_quote_argument(exe_path) + " " + win_quote_argument(file_path);
}

std::vector<std::string> macos_open_by_name_args(const std::string &file_path, const std::string &app_name)
{
    return { "open", "-a", app_name, file_path };
}

std::vector<std::string> macos_open_by_bundle_id_args(const std::string &file_path, const std::string &bundle_id)
{
    return { "open", "-b", bundle_id, file_path };
}

std::vector<std::string> linux_path_exec_args(const std::string &file_path, const std::string &exe_name)
{
    return { exe_name, file_path };
}

std::vector<std::string> linux_flatpak_args(const std::string &file_path, const std::string &app_id)
{
    return { "flatpak", "run", app_id, file_path };
}

// -------------------------------------------------------------------------------------------
// Platform entry point
// -------------------------------------------------------------------------------------------

#ifdef _WIN32

namespace {

bool file_exists_win(const std::string &path)
{
    return boost::filesystem::exists(boost::filesystem::path(boost::nowide::widen(path)));
}

// Reads one App Paths\bambu-studio.exe default value from the given hive/view.
bool read_app_path(const RegLookup &where, std::string &out)
{
    HKEY root = (where.hive == RegHive::HKLM) ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    REGSAM flags = KEY_READ | ((where.view == RegView::Wow6432) ? KEY_WOW64_32KEY : KEY_WOW64_64KEY);
    std::wstring subkey = boost::nowide::widen(where.subkey);
    HKEY  hKey = nullptr;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, flags, &hKey) != ERROR_SUCCESS)
        return false;
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD   size = sizeof(buf);
    DWORD   type = 0;
    LONG    res  = RegQueryValueExW(hKey, nullptr, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &size);
    RegCloseKey(hKey);
    if (res != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
        return false;
    out = boost::nowide::narrow(buf);
    return !out.empty();
}

// Enumerates Uninstall subkeys of one hive/view, reading DisplayName / InstallLocation /
// DisplayIcon for each, and appends every entry to `out` (filtering to plausible Bambu Studio
// names happens in discover_windows()).
void enumerate_uninstall(HKEY root, REGSAM extra_flags, std::vector<UninstallEntry> &out)
{
    const wchar_t *base = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    HKEY           hUninstall = nullptr;
    if (RegOpenKeyExW(root, base, 0, KEY_READ | extra_flags, &hUninstall) != ERROR_SUCCESS)
        return;
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD name_len = 256;
        LONG  res      = RegEnumKeyExW(hUninstall, i, name, &name_len, nullptr, nullptr, nullptr, nullptr);
        if (res == ERROR_NO_MORE_ITEMS)
            break;
        if (res != ERROR_SUCCESS)
            continue;
        HKEY hSub = nullptr;
        if (RegOpenKeyExW(hUninstall, name, 0, KEY_READ | extra_flags, &hSub) != ERROR_SUCCESS)
            continue;
        auto read_value = [&](const wchar_t *value_name) -> std::string {
            wchar_t buf[1024] = {};
            DWORD   size = sizeof(buf);
            DWORD   type = 0;
            if (RegQueryValueExW(hSub, value_name, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS &&
                (type == REG_SZ || type == REG_EXPAND_SZ))
                return boost::nowide::narrow(buf);
            return {};
        };
        UninstallEntry entry;
        entry.display_name     = read_value(L"DisplayName");
        entry.install_location = read_value(L"InstallLocation");
        entry.display_icon     = read_value(L"DisplayIcon");
        RegCloseKey(hSub);
        if (!entry.display_name.empty())
            out.push_back(std::move(entry));
    }
    RegCloseKey(hUninstall);
}

// .3mf ProgId's associated executable, via AssocQueryString(ASSOCSTR_EXECUTABLE).
std::string assoc_exe_for_3mf()
{
    wchar_t buf[MAX_PATH * 2];
    DWORD   size = static_cast<DWORD>(std::size(buf));
    HRESULT hr   = AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_EXECUTABLE, L".3mf", nullptr, buf, &size);
    if (FAILED(hr))
        return {};
    return boost::nowide::narrow(buf);
}

} // namespace

// Real discovery: reads the registry/filesystem and delegates the precedence logic to
// discover_windows() above.
static DiscoveryResult discover_windows_real()
{
    std::vector<std::string> app_paths_values;
    for (const RegLookup &where : APP_PATHS_LOOKUPS) {
        std::string value;
        read_app_path(where, value);
        app_paths_values.push_back(value);
    }

    // HKCU\...\Uninstall is not redirected by WOW64 the way HKLM is, but the flag is harmless to
    // pass and keeps this symmetric with the HKLM scan / the App Paths lookups above.
    std::vector<UninstallEntry> uninstall_entries;
    enumerate_uninstall(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, uninstall_entries);
    enumerate_uninstall(HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, uninstall_entries);
    enumerate_uninstall(HKEY_CURRENT_USER, KEY_WOW64_64KEY, uninstall_entries);
    enumerate_uninstall(HKEY_CURRENT_USER, KEY_WOW64_32KEY, uninstall_entries);

    std::string assoc_exe = assoc_exe_for_3mf();

    return discover_windows(app_paths_values, uninstall_entries, assoc_exe, &file_exists_win);
}

LaunchResult open_in_bambu_studio(const std::string &file_path, const std::string &custom_exe_path)
{
    LaunchResult result;

    std::string exe_path;
    if (!custom_exe_path.empty() && file_exists_win(custom_exe_path)) {
        exe_path = custom_exe_path;
    } else {
        DiscoveryResult discovered = discover_windows_real();
        if (!discovered.found) {
            BOOST_LOG_TRIVIAL(warning) << "BambuStudioLauncher: Bambu Studio not found on this machine.";
            result.outcome = LaunchOutcome::NotFound;
            return result;
        }
        exe_path = discovered.exe_path;
    }

    result.exe_path = exe_path;
    std::string cmdline = win_build_command_line(exe_path, file_path);
    wxString    wx_cmd  = wxString::FromUTF8(cmdline.c_str());
    long        pid     = ::wxExecute(wx_cmd, wxEXEC_ASYNC, nullptr);
    if (pid <= 0) {
        BOOST_LOG_TRIVIAL(error) << "BambuStudioLauncher: failed to launch \"" << cmdline << "\"";
        result.outcome = LaunchOutcome::LaunchFailed;
        return result;
    }
    BOOST_LOG_TRIVIAL(info) << "BambuStudioLauncher: launched \"" << cmdline << "\" (pid " << pid << ")";
    result.outcome = LaunchOutcome::Launched;
    return result;
}

#elif defined(__APPLE__)

namespace {
bool run_argv(const std::vector<std::string> &argv)
{
    try {
        boost::process::spawn(boost::process::search_path(argv.front()), boost::process::args(std::vector<std::string>(argv.begin() + 1, argv.end())));
        return true;
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << "BambuStudioLauncher: failed to spawn \"" << argv.front() << "\": " << ex.what();
        return false;
    }
}
} // namespace

LaunchResult open_in_bambu_studio(const std::string &file_path, const std::string &custom_exe_path)
{
    LaunchResult result;
    if (!custom_exe_path.empty() && boost::filesystem::exists(custom_exe_path)) {
        // A custom override is a path to the .app bundle or the executable inside it; `open -a`
        // takes either.
        if (run_argv({ "open", "-a", custom_exe_path, file_path })) {
            result.outcome  = LaunchOutcome::Launched;
            result.exe_path = custom_exe_path;
        } else {
            result.outcome  = LaunchOutcome::LaunchFailed;
            result.exe_path = custom_exe_path;
        }
        return result;
    }

    // Try the bundle name first, then the bundle id. `open -a` fails (non-zero exit) when the
    // name isn't registered with Launch Services, but boost::process::spawn on "open" itself
    // rarely fails to start - the app-not-found case is a "look for by name" miss we cannot
    // distinguish from here without waiting on the child, so we optimistically report Launched
    // once `open` itself starts, same as the existing start_new_slicer() pattern in Process.cpp.
    if (run_argv(macos_open_by_name_args(file_path))) {
        result.outcome = LaunchOutcome::Launched;
        return result;
    }
    if (run_argv(macos_open_by_bundle_id_args(file_path))) {
        result.outcome = LaunchOutcome::Launched;
        return result;
    }
    result.outcome = LaunchOutcome::NotFound;
    return result;
}

#else // Linux

namespace {
bool has_in_path(const std::string &exe_name)
{
    return !boost::process::search_path(exe_name).empty();
}
bool run_argv(const std::vector<std::string> &argv)
{
    try {
        boost::process::spawn(argv.front().find('/') != std::string::npos ? argv.front() : boost::process::search_path(argv.front()).string(),
                               boost::process::args(std::vector<std::string>(argv.begin() + 1, argv.end())));
        return true;
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << "BambuStudioLauncher: failed to spawn \"" << argv.front() << "\": " << ex.what();
        return false;
    }
}
} // namespace

LaunchResult open_in_bambu_studio(const std::string &file_path, const std::string &custom_exe_path)
{
    LaunchResult result;
    if (!custom_exe_path.empty() && boost::filesystem::exists(custom_exe_path)) {
        if (run_argv({ custom_exe_path, file_path })) {
            result.outcome  = LaunchOutcome::Launched;
            result.exe_path = custom_exe_path;
        } else {
            result.outcome  = LaunchOutcome::LaunchFailed;
            result.exe_path = custom_exe_path;
        }
        return result;
    }

    if (has_in_path("bambu-studio")) {
        result.exe_path = "bambu-studio";
        result.outcome  = run_argv(linux_path_exec_args(file_path)) ? LaunchOutcome::Launched : LaunchOutcome::LaunchFailed;
        return result;
    }
    if (has_in_path("flatpak")) {
        // We cannot cheaply tell whether the Flatpak app itself is installed without invoking
        // `flatpak run`, which is also the launch step - so a missing app surfaces as
        // LaunchFailed (flatpak exits non-zero) rather than NotFound. The caller's message
        // covers both the same way.
        result.exe_path = "flatpak run com.bambulab.BambuStudio";
        result.outcome  = run_argv(linux_flatpak_args(file_path)) ? LaunchOutcome::Launched : LaunchOutcome::LaunchFailed;
        return result;
    }
    // AppImage is not discoverable (no install location, no PATH entry, no package registry).
    result.outcome = LaunchOutcome::NotFound;
    return result;
}

#endif

} // namespace BambuStudioLauncher
} // namespace Slic3r
