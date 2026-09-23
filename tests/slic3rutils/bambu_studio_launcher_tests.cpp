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
