#ifndef slic3r_Utils_WebView2UserData_hpp_
#define slic3r_Utils_WebView2UserData_hpp_

// Which WebView2 user data folder this copy of EdgeSlicer uses.
//
// wx creates every WebView2 environment in GetUserLocalDataDir() (%LOCALAPPDATA%\EdgeSlicer), so
// the main install and every test copy share one browser process. WebView2 only lets a second
// process join that browser process when its environment options match exactly, and ours don't
// across builds: the additional browser arguments carry --disable-features=HideCursorWhileTyping
// (#743, added 2026-10-28) and --user-agent="... EdgeSlicer/v<version> (dark|light) ...". Whichever
// copy starts second gets HRESULT_FROM_WIN32(ERROR_INVALID_STATE) from
// CreateCoreWebView2Controller, which wx only logs, and every web page in that copy stays blank
// (Home, Snapmaker sign-in, device pages).
//
// A copy marked as a test copy (the edgeslicer_no_hub marker edgebuild writes next to every
// --install copy, or an edgeslicer_own_webview marker / EDGESLICER_OWN_WEBVIEW=1) gets its own
// folder under %LOCALAPPDATA%\EdgeSlicer\WebView2Copies, keyed by its install directory so it
// keeps its sign-ins across rebuilds. The main install keeps the shared default folder and its
// cookies. EDGESLICER_SHARED_WEBVIEW=1 forces the shared folder.
//
// Header-only and free of wx / WebView2 so tests/slic3rutils/webview2_userdata_tests.cpp can pin
// the rules.

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>

namespace Slic3r {
namespace WebView2UserData {

inline constexpr const char *ENV_NAME         = "WEBVIEW2_USER_DATA_FOLDER";
inline constexpr const char *OWN_MARKER       = "edgeslicer_own_webview";
inline constexpr const char *NO_HUB_MARKER    = "edgeslicer_no_hub";
inline constexpr const char *OWN_ENV          = "EDGESLICER_OWN_WEBVIEW";
inline constexpr const char *SHARED_ENV       = "EDGESLICER_SHARED_WEBVIEW";
inline constexpr const char *COPIES_SUBFOLDER = "WebView2Copies";

inline bool env_on(const char *v)
{
    if (v == nullptr)
        return false;
    std::string s(v);
    for (char &c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s == "1" || s == "true" || s == "yes" || s == "on";
}

struct Inputs
{
    bool        own_marker { false };    // edgeslicer_own_webview next to the exe
    bool        no_hub_marker { false }; // edgeslicer_no_hub next to the exe (every edgebuild test copy)
    const char *own_env { nullptr };     // EDGESLICER_OWN_WEBVIEW
    const char *shared_env { nullptr };  // EDGESLICER_SHARED_WEBVIEW
    const char *udf_env { nullptr };     // WEBVIEW2_USER_DATA_FOLDER already set by the user
};

// True when this copy should use its own user data folder instead of the shared default.
inline bool wants_own_folder(const Inputs &in)
{
    if (in.udf_env != nullptr && *in.udf_env != '\0')
        return false; // the user picked a folder; WebView2 honours it already
    if (env_on(in.shared_env))
        return false;
    return env_on(in.own_env) || in.own_marker || in.no_hub_marker;
}

// Stable, filesystem-safe folder name for an install directory: "<leaf>-<fnv1a32 hex>".
// The hash is over the lower-cased path with '\' turned into '/' and trailing slashes dropped,
// so the same copy maps to the same folder however the path was spelled.
inline std::string copy_folder_name(const std::string &exe_dir_utf8)
{
    std::string norm = exe_dir_utf8;
    for (char &c : norm) {
        if (c == '\\')
            c = '/';
        else
            c = char(std::tolower(static_cast<unsigned char>(c)));
    }
    while (norm.size() > 1 && norm.back() == '/')
        norm.pop_back();

    uint32_t h = 2166136261u;
    for (unsigned char c : norm) {
        h ^= c;
        h *= 16777619u;
    }

    // Leaf from the original spelling, so the folder keeps the copy's case.
    std::string original = exe_dir_utf8;
    while (original.size() > 1 && (original.back() == '/' || original.back() == '\\'))
        original.pop_back();
    const size_t slash   = original.find_last_of("/\\");
    std::string  leaf_in = (slash == std::string::npos) ? original : original.substr(slash + 1);

    std::string leaf;
    for (unsigned char c : leaf_in) {
        if (leaf.size() >= 40)
            break;
        leaf += (std::isalnum(c) || c == '-' || c == '_') ? char(c) : '_';
    }
    if (leaf.empty())
        leaf = "copy";

    char hex[9];
    std::snprintf(hex, sizeof(hex), "%08x", h);
    return leaf + "-" + hex;
}

} // namespace WebView2UserData
} // namespace Slic3r

#endif // slic3r_Utils_WebView2UserData_hpp_
