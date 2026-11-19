#include <catch2/catch.hpp>

#include <string>
#include <vector>

#include "slic3r/Utils/BambuStudioLauncher.hpp"

using namespace Slic3r::BambuStudioLauncher;

namespace {

FileExists exists_set(const std::vector<std::string> &existing)
{
    return [existing](const std::string &path) {
        for (const auto &p : existing)
            if (p == path)
                return true;
        return false;
    };
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Windows discovery precedence
// ---------------------------------------------------------------------------------------------

TEST_CASE("App Paths HKLM 64-bit wins when it exists", "[BambuStudioLauncher]")
{
    std::vector<std::string> app_paths = {
        "C:\\Program Files\\Bambu Studio\\bambu-studio.exe", // HKLM 64
        "",                                                  // HKLM 32
        "",                                                  // HKCU 64
        "",                                                  // HKCU 32
    };
    auto result = discover_windows(app_paths, {}, "", exists_set({ app_paths[0] }));
    REQUIRE(result.found);
    CHECK(result.exe_path == app_paths[0]);
    CHECK(result.candidates.front().source == "AppPaths");
}

TEST_CASE("App Paths precedence: HKLM before HKCU, 64-bit before 32-bit view", "[BambuStudioLauncher]")
{
    // Only the HKCU 32-bit entry actually exists on disk; earlier candidates are considered
    // (and recorded) but skipped because they don't exist.
    std::vector<std::string> app_paths = {
        "C:\\stale\\hklm64.exe",
        "C:\\stale\\hklm32.exe",
        "C:\\stale\\hkcu64.exe",
        "C:\\Users\\me\\AppData\\Local\\BambuStudio\\bambu-studio.exe",
    };
    auto result = discover_windows(app_paths, {}, "", exists_set({ app_paths[3] }));
    REQUIRE(result.found);
    CHECK(result.exe_path == app_paths[3]);
    // All four App Paths candidates were tried before falling through.
    CHECK(result.candidates.size() == 4);
    for (size_t i = 0; i < 3; ++i)
        CHECK_FALSE(result.candidates[i].exists);
    CHECK(result.candidates[3].exists);
}

TEST_CASE("falls back to Uninstall InstallLocation when App Paths is absent", "[BambuStudioLauncher]")
{
    std::vector<std::string> app_paths = { "", "", "", "" };
    std::vector<UninstallEntry> uninstall = {
        { "Some Other App", "C:\\Other\\", "" },
        { "Bambu Studio", "C:\\Program Files\\Bambu Studio", "" },
    };
    std::string expected = "C:\\Program Files\\Bambu Studio\\bambu-studio.exe";
    auto result = discover_windows(app_paths, uninstall, "", exists_set({ expected }));
    REQUIRE(result.found);
    CHECK(result.exe_path == expected);
    CHECK(result.candidates.back().source == "Uninstall");
}

TEST_CASE("Uninstall DisplayIcon is parsed and its icon-index suffix stripped", "[BambuStudioLauncher]")
{
    std::vector<std::string> app_paths = { "", "", "", "" };
    std::vector<UninstallEntry> uninstall = {
        { "Bambu Studio", "", "C:\\Program Files\\Bambu Studio\\bambu-studio.exe,0" },
    };
    std::string expected = "C:\\Program Files\\Bambu Studio\\bambu-studio.exe";
    auto result = discover_windows(app_paths, uninstall, "", exists_set({ expected }));
    REQUIRE(result.found);
    CHECK(result.exe_path == expected);
}

TEST_CASE("Uninstall entries not named Bambu Studio are ignored", "[BambuStudioLauncher]")
{
    std::vector<std::string> app_paths = { "", "", "", "" };
    std::vector<UninstallEntry> uninstall = {
        { "Bambu Handy Companion", "C:\\Bambu Handy\\", "" }, // contains "Bambu" but not "Bambu Studio"
    };
    auto result = discover_windows(app_paths, uninstall, "", exists_set({ "C:\\Bambu Handy\\bambu-studio.exe" }));
    // Should NOT match the unrelated app; falls through to the default path, which doesn't exist either.
    CHECK_FALSE(result.found);
    for (const auto &c : result.candidates)
        CHECK(c.source != "Uninstall");
}

TEST_CASE("3mf association is used only when it points at Bambu Studio's own exe", "[BambuStudioLauncher]")
{
    std::vector<std::string> app_paths = { "", "", "", "" };
    SECTION("recognized bambu-studio.exe association is accepted")
    {
        std::string assoc = "C:\\Custom\\Path\\bambu-studio.exe";
        auto result = discover_windows(app_paths, {}, assoc, exists_set({ assoc }));
        REQUIRE(result.found);
        CHECK(result.exe_path == assoc);
        CHECK(result.candidates.back().source == "AssocQueryString");
    }
    SECTION("an unrelated .3mf handler is never launched")
    {
        std::string assoc = "C:\\Program Files\\Other Slicer\\other_slicer.exe";
        auto result = discover_windows(app_paths, {}, assoc, exists_set({ assoc, DEFAULT_INSTALL_PATH }));
        // Default path exists in this fake filesystem, association does not qualify.
        REQUIRE(result.found);
        CHECK(result.exe_path == DEFAULT_INSTALL_PATH);
        for (const auto &c : result.candidates)
            CHECK(c.source != "AssocQueryString");
    }
}

TEST_CASE("falls back to the hardcoded default path", "[BambuStudioLauncher]")
{
    std::vector<std::string> app_paths = { "", "", "", "" };
    auto result = discover_windows(app_paths, {}, "", exists_set({ DEFAULT_INSTALL_PATH }));
    REQUIRE(result.found);
    CHECK(result.exe_path == DEFAULT_INSTALL_PATH);
    CHECK(result.candidates.back().source == "DefaultPath");
}

TEST_CASE("nothing found when no candidate exists on disk", "[BambuStudioLauncher]")
{
    std::vector<std::string> app_paths = { "", "", "", "" };
    auto result = discover_windows(app_paths, {}, "", exists_set({}));
    CHECK_FALSE(result.found);
    CHECK(result.exe_path.empty());
    // Still records the default-path candidate as considered (and not existing).
    CHECK_FALSE(result.candidates.empty());
    CHECK_FALSE(result.candidates.back().exists);
}

TEST_CASE("looks_like_bambu_studio_display_name is tolerant but not over-broad", "[BambuStudioLauncher]")
{
    CHECK(looks_like_bambu_studio_display_name("Bambu Studio"));
    CHECK(looks_like_bambu_studio_display_name("Bambu Studio 2.1.1"));
    CHECK(looks_like_bambu_studio_display_name("BambuStudio"));
    CHECK(looks_like_bambu_studio_display_name("bambu studio")); // case-insensitive
    CHECK_FALSE(looks_like_bambu_studio_display_name("Bambu Handy"));
    CHECK_FALSE(looks_like_bambu_studio_display_name("Bambu Connect"));
    CHECK_FALSE(looks_like_bambu_studio_display_name(""));
}

// ---------------------------------------------------------------------------------------------
// Windows argument quoting / command construction
// ---------------------------------------------------------------------------------------------

TEST_CASE("win_quote_argument leaves simple tokens unquoted", "[BambuStudioLauncher]")
{
    CHECK(win_quote_argument("C:\\Program.exe") == "C:\\Program.exe");
    CHECK(win_quote_argument("plain") == "plain");
}

TEST_CASE("win_quote_argument quotes arguments containing spaces", "[BambuStudioLauncher]")
{
    CHECK(win_quote_argument("C:\\Program Files\\Bambu Studio\\bambu-studio.exe")
          == "\"C:\\Program Files\\Bambu Studio\\bambu-studio.exe\"");
}

TEST_CASE("win_quote_argument handles the empty string", "[BambuStudioLauncher]")
{
    CHECK(win_quote_argument("") == "\"\"");
}

TEST_CASE("win_quote_argument doubles backslashes only right before the closing quote", "[BambuStudioLauncher]")
{
    // A path ending in a backslash, when it must be quoted (has a space), needs the trailing
    // backslash doubled so it isn't read as escaping the closing quote.
    CHECK(win_quote_argument("C:\\some dir\\") == "\"C:\\some dir\\\\\"");
    // Backslashes NOT immediately before a quote are passed through untouched.
    CHECK(win_quote_argument("C:\\some dir\\file.3mf") == "\"C:\\some dir\\file.3mf\"");
}

TEST_CASE("win_quote_argument escapes embedded quotes", "[BambuStudioLauncher]")
{
    CHECK(win_quote_argument("he said \"hi\"") == "\"he said \\\"hi\\\"\"");
}

TEST_CASE("win_quote_argument round-trips unicode paths (UTF-8 bytes pass through)", "[BambuStudioLauncher]")
{
    // "C:\Users\\Ace 测试\\model.3mf" -- multi-byte sequences are opaque bytes to the quoting
    // logic, which only special-cases the ASCII space/tab/quote/backslash characters.
    std::string path = "C:\\Users\\Ace \xE6\xB5\x8B\xE8\xAF\x95\\model.3mf";
    std::string quoted = win_quote_argument(path);
    CHECK(quoted.front() == '"');
    CHECK(quoted.back() == '"');
    CHECK(quoted.find("\xE6\xB5\x8B\xE8\xAF\x95") != std::string::npos);
}

TEST_CASE("win_build_command_line quotes both exe and file path", "[BambuStudioLauncher]")
{
    std::string cmd = win_build_command_line("C:\\Program Files\\Bambu Studio\\bambu-studio.exe",
                                              "C:\\Users\\me\\Documents\\my model.3mf");
    CHECK(cmd == "\"C:\\Program Files\\Bambu Studio\\bambu-studio.exe\" \"C:\\Users\\me\\Documents\\my model.3mf\"");
}

TEST_CASE("win_build_command_line with no-space paths stays readable", "[BambuStudioLauncher]")
{
    std::string cmd = win_build_command_line("C:\\BambuStudio\\bambu-studio.exe", "C:\\out\\a.3mf");
    CHECK(cmd == "C:\\BambuStudio\\bambu-studio.exe C:\\out\\a.3mf");
}

// ---------------------------------------------------------------------------------------------
// macOS / Linux argv construction
// ---------------------------------------------------------------------------------------------

TEST_CASE("macos_open_by_name_args builds open -a BambuStudio <file>", "[BambuStudioLauncher]")
{
    auto args = macos_open_by_name_args("/Users/me/model.3mf");
    CHECK(args == std::vector<std::string>{ "open", "-a", "BambuStudio", "/Users/me/model.3mf" });
}

TEST_CASE("macos_open_by_bundle_id_args builds the bundle-id fallback", "[BambuStudioLauncher]")
{
    auto args = macos_open_by_bundle_id_args("/Users/me/model.3mf");
    CHECK(args == std::vector<std::string>{ "open", "-b", "com.bambulab.bambu-studio", "/Users/me/model.3mf" });
}

TEST_CASE("linux_path_exec_args runs bambu-studio from PATH", "[BambuStudioLauncher]")
{
    auto args = linux_path_exec_args("/home/me/model.3mf");
    CHECK(args == std::vector<std::string>{ "bambu-studio", "/home/me/model.3mf" });
}

TEST_CASE("linux_flatpak_args builds flatpak run com.bambulab.BambuStudio <file>", "[BambuStudioLauncher]")
{
    auto args = linux_flatpak_args("/home/me/model.3mf");
    CHECK(args == std::vector<std::string>{ "flatpak", "run", "com.bambulab.BambuStudio", "/home/me/model.3mf" });
}

// ---------------------------------------------------------------------------------------------
// macOS / Linux launch decisions (fake process calls; nothing is started)
// ---------------------------------------------------------------------------------------------

namespace {

// Records every call and answers run_and_wait from a table of exit statuses by argv.
struct FakeProcesses
{
    std::vector<std::pair<std::vector<std::string>, int>> exit_status; // argv -> exit status
    std::vector<std::string>                              on_path;
    bool                                                  start_ok = true;
    std::vector<std::vector<std::string>>                 waited, started;

    LaunchOps ops()
    {
        return LaunchOps{
            [this](const std::vector<std::string> &argv) {
                waited.push_back(argv);
                for (const auto &e : exit_status)
                    if (e.first == argv)
                        return e.second;
                return -1;
            },
            [this](const std::vector<std::string> &argv) {
                started.push_back(argv);
                return start_ok;
            },
            [this](const std::string &name) {
                for (const auto &p : on_path)
                    if (p == name)
                        return true;
                return false;
            },
        };
    }
};

const std::string FILE_3MF = "/Users/me/Desktop/part.3mf";

} // namespace

TEST_CASE("macOS: open -a BambuStudio succeeding is a launch", "[BambuStudioLauncher]")
{
    FakeProcesses p;
    p.exit_status = { { macos_open_by_name_args(FILE_3MF), 0 } };
    const LaunchResult r = launch_macos(FILE_3MF, "", p.ops());
    CHECK(r.outcome == LaunchOutcome::Launched);
    CHECK(p.waited.size() == 1);
}

TEST_CASE("macOS: a failed open -a falls back to the bundle id", "[BambuStudioLauncher]")
{
    // `open -a BambuStudio` exits 1 ("Unable to find application named 'BambuStudio'") when no
    // application has that name; the bundle-id form finds a renamed bundle.
    FakeProcesses p;
    p.exit_status = { { macos_open_by_name_args(FILE_3MF), 1 }, { macos_open_by_bundle_id_args(FILE_3MF), 0 } };
    const LaunchResult r = launch_macos(FILE_3MF, "", p.ops());
    CHECK(r.outcome == LaunchOutcome::Launched);
    REQUIRE(p.waited.size() == 2);
    CHECK(p.waited[1] == macos_open_by_bundle_id_args(FILE_3MF));
}

TEST_CASE("macOS: Bambu Studio not installed is reported as not found", "[BambuStudioLauncher]")
{
    FakeProcesses p;
    p.exit_status = { { macos_open_by_name_args(FILE_3MF), 1 }, { macos_open_by_bundle_id_args(FILE_3MF), 1 } };
    CHECK(launch_macos(FILE_3MF, "", p.ops()).outcome == LaunchOutcome::NotFound);
    // `open` could not even be run: still nothing launched.
    FakeProcesses none;
    CHECK(launch_macos(FILE_3MF, "", none.ops()).outcome == LaunchOutcome::NotFound);
}

TEST_CASE("macOS: a custom path that open refuses is a failed launch", "[BambuStudioLauncher]")
{
    const std::string custom = "/Applications/Tools/BambuStudio.app";
    FakeProcesses     ok;
    ok.exit_status = { { { "open", "-a", custom, FILE_3MF }, 0 } };
    LaunchResult r = launch_macos(FILE_3MF, custom, ok.ops());
    CHECK(r.outcome == LaunchOutcome::Launched);
    CHECK(r.exe_path == custom);

    FakeProcesses refused; // e.g. the path is not an application
    refused.exit_status = { { { "open", "-a", custom, FILE_3MF }, 1 } };
    r = launch_macos(FILE_3MF, custom, refused.ops());
    CHECK(r.outcome == LaunchOutcome::LaunchFailed);
    CHECK(r.exe_path == custom);
    CHECK(refused.waited.size() == 1); // no discovery after a custom path
}

TEST_CASE("Linux: bambu-studio on PATH is started without waiting", "[BambuStudioLauncher]")
{
    FakeProcesses p;
    p.on_path = { "bambu-studio", "flatpak" };
    const LaunchResult r = launch_linux(FILE_3MF, "", p.ops());
    CHECK(r.outcome == LaunchOutcome::Launched);
    CHECK(p.waited.empty());
    REQUIRE(p.started.size() == 1);
    CHECK(p.started[0] == linux_path_exec_args(FILE_3MF));
}

TEST_CASE("Linux: the Flatpak app is run only when flatpak info finds it", "[BambuStudioLauncher]")
{
    const std::vector<std::string> info = { "flatpak", "info", "com.bambulab.BambuStudio" };

    FakeProcesses installed;
    installed.on_path     = { "flatpak" };
    installed.exit_status = { { info, 0 } };
    LaunchResult r = launch_linux(FILE_3MF, "", installed.ops());
    CHECK(r.outcome == LaunchOutcome::Launched);
    REQUIRE(installed.started.size() == 1);
    CHECK(installed.started[0] == linux_flatpak_args(FILE_3MF));

    FakeProcesses missing; // flatpak present, Bambu Studio not installed through it
    missing.on_path     = { "flatpak" };
    missing.exit_status = { { info, 1 } };
    r = launch_linux(FILE_3MF, "", missing.ops());
    CHECK(r.outcome == LaunchOutcome::NotFound);
    CHECK(missing.started.empty());

    FakeProcesses nothing;
    CHECK(launch_linux(FILE_3MF, "", nothing.ops()).outcome == LaunchOutcome::NotFound);
}

TEST_CASE("Linux: a custom executable that cannot start is a failed launch", "[BambuStudioLauncher]")
{
    FakeProcesses p;
    p.start_ok = false;
    const LaunchResult r = launch_linux(FILE_3MF, "/opt/bambu/bambu-studio", p.ops());
    CHECK(r.outcome == LaunchOutcome::LaunchFailed);
    CHECK(r.exe_path == "/opt/bambu/bambu-studio");
}
