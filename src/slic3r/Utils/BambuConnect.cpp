#include "BambuConnect.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Slic3r {
namespace BambuConnect {

const char *const URL_SCHEME       = "bambu-connect";
const char *const PROTOCOL_VERSION = "1.0.0";
const char *const INFO_URL         = "https://wiki.bambulab.com/en/software/bambu-connect";

namespace {

bool is_uri_component_unreserved(unsigned char c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
        return true;
    switch (c) {
    case '-': case '_': case '.': case '!': case '~': case '*': case '\'': case '(': case ')':
        return true;
    default:
        return false;
    }
}

std::string to_lower_ascii(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

bool ends_with_ci(const std::string &s, const std::string &suffix)
{
    return s.size() >= suffix.size() && to_lower_ascii(s.substr(s.size() - suffix.size())) == to_lower_ascii(suffix);
}

std::string trim_ascii_space(const std::string &s)
{
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t'))
        ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t'))
        --e;
    return s.substr(b, e - b);
}

// Cut a UTF-8 string to at most `max_bytes`, never inside a multi-byte character.
std::string utf8_truncate(const std::string &s, size_t max_bytes)
{
    if (s.size() <= max_bytes)
        return s;
    size_t end = max_bytes;
    // Step back over continuation bytes (10xxxxxx) so the cut lands on a lead byte.
    while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80)
        --end;
    return s.substr(0, end);
}

bool is_windows_device_name(const std::string &stem)
{
    const std::string lower = to_lower_ascii(stem);
    static const char *const names[] = { "con", "prn", "aux", "nul" };
    for (const char *n : names)
        if (lower == n)
            return true;
    if (lower.size() == 4 && (lower.compare(0, 3, "com") == 0 || lower.compare(0, 3, "lpt") == 0) && lower[3] >= '1' && lower[3] <= '9')
        return true;
    return false;
}

} // namespace

// -------------------------------------------------------------------------------------------
// URL building
// -------------------------------------------------------------------------------------------

std::string encode_uri_component(const std::string &utf8)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(utf8.size() * 3);
    for (unsigned char c : utf8) {
        if (is_uri_component_unreserved(c)) {
            out += char(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

std::string build_import_url(const std::string &abs_path_utf8, const std::string &display_name_utf8)
{
    return std::string(URL_SCHEME) + "://import-file?path=" + encode_uri_component(abs_path_utf8) +
           "&name=" + encode_uri_component(display_name_utf8) + "&version=" + PROTOCOL_VERSION;
}

// -------------------------------------------------------------------------------------------
// Naming
// -------------------------------------------------------------------------------------------

std::string display_name_for(const std::string &export_file_name_utf8)
{
    std::string name = export_file_name_utf8;
    // Leaf only, whichever separator the caller's path used.
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos)
        name = name.substr(slash + 1);
    for (const char *ext : { ".gcode.3mf", ".3mf", ".gcode" }) {
        if (ends_with_ci(name, ext)) {
            name.resize(name.size() - std::string(ext).size());
            break;
        }
    }
    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name) {
        if (c < 0x20 || c == 0x7F)
            continue;
        switch (c) {
        case '&': case '=': case '?': case '#': case '%': case '+':
            out += '_';
            break;
        default:
            out += char(c);
        }
    }
    out = trim_ascii_space(out);
    return out.empty() ? std::string("plate") : out;
}

std::string temp_file_name_for(const std::string &display_name_utf8)
{
    std::string stem;
    stem.reserve(display_name_utf8.size());
    for (unsigned char c : display_name_utf8) {
        if (c < 0x20 || c == 0x7F)
            continue;
        switch (c) {
        case '<': case '>': case ':': case '"': case '/': case '\\': case '|': case '?': case '*':
        case '&': case '=': case '#': case '%': case '+':
            stem += '_';
            break;
        default:
            stem += char(c);
        }
    }
    stem = utf8_truncate(stem, 100);
    // Windows silently drops trailing dots and spaces; trim them so the name we pass is the name on disk.
    while (!stem.empty() && (stem.back() == '.' || stem.back() == ' '))
        stem.pop_back();
    while (!stem.empty() && stem.front() == ' ')
        stem.erase(stem.begin());
    if (stem.empty())
        stem = "plate";
    if (is_windows_device_name(stem))
        stem += "_";
    return stem + ".gcode.3mf";
}

// -------------------------------------------------------------------------------------------
// Installation detection
// -------------------------------------------------------------------------------------------

std::string executable_from_command(const std::string &command)
{
    const std::string cmd = trim_ascii_space(command);
    if (cmd.empty())
        return {};
    if (cmd.front() == '"') {
        const size_t close = cmd.find('"', 1);
        return close == std::string::npos ? cmd.substr(1) : cmd.substr(1, close - 1);
    }
    const std::string lower = to_lower_ascii(cmd);
    const size_t      exe   = lower.find(".exe");
    if (exe != std::string::npos)
        return cmd.substr(0, exe + 4);
    const size_t space = cmd.find(' ');
    return space == std::string::npos ? cmd : cmd.substr(0, space);
}

Installed detect_windows(const WindowsRegistration &reg, const FileExists &exists)
{
    if (!reg.key_exists || !reg.has_url_protocol)
        return Installed::No;
    if (trim_ascii_space(reg.open_command).empty())
        return Installed::No;
    const std::string exe = executable_from_command(reg.open_command);
    // Environment-variable paths (%LOCALAPPDATA%\...) cannot be checked without expanding them;
    // the real reader expands REG_EXPAND_SZ, so one left here is trusted rather than rejected.
    if (exe.empty() || exe.find('%') != std::string::npos || !exists)
        return Installed::Yes;
    return exists(exe) ? Installed::Yes : Installed::No;
}

Installed detect_macos(const std::string &handler_app_path)
{
    return trim_ascii_space(handler_app_path).empty() ? Installed::No : Installed::Yes;
}

Installed detect_linux(bool query_ran, const std::string &xdg_mime_output)
{
    if (!query_ran)
        return Installed::Unknown;
    std::string out = xdg_mime_output;
    out.erase(std::remove_if(out.begin(), out.end(), [](unsigned char c) { return std::isspace(c) != 0; }), out.end());
    return out.empty() ? Installed::No : Installed::Yes;
}

#ifdef _WIN32
namespace {
WindowsRegistration read_windows_registration()
{
    WindowsRegistration reg;
    HKEY                key = nullptr;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, L"bambu-connect", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        reg.key_exists       = true;
        reg.has_url_protocol = RegQueryValueExW(key, L"URL Protocol", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
        RegCloseKey(key);
    }
    if (!reg.key_exists)
        return reg;
    wchar_t buf[4096];
    DWORD   size = sizeof(buf);
    if (RegGetValueW(HKEY_CLASSES_ROOT, L"bambu-connect\\shell\\open\\command", nullptr, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                     nullptr, buf, &size) == ERROR_SUCCESS)
        reg.open_command = boost::nowide::narrow(buf);
    return reg;
}
} // namespace
#endif

Installed detect_installed()
{
#if defined(_WIN32)
    const WindowsRegistration reg    = read_windows_registration();
    const Installed           result = detect_windows(reg, [](const std::string &path) {
        boost::system::error_code ec;
        return boost::filesystem::exists(boost::filesystem::path(boost::nowide::widen(path)), ec);
    });
    BOOST_LOG_TRIVIAL(info) << "Bambu Connect: protocol key " << reg.key_exists << ", URL Protocol " << reg.has_url_protocol
                            << ", installed " << (result == Installed::Yes);
    return result;
#elif defined(__APPLE__)
    const std::string handler = macos_url_handler_app_path();
    BOOST_LOG_TRIVIAL(info) << "Bambu Connect: URL handler " << (handler.empty() ? std::string("none") : handler);
    return detect_macos(handler);
#else
    std::string output;
    bool        ran  = false;
    FILE       *pipe = popen("xdg-mime query default x-scheme-handler/bambu-connect 2>/dev/null", "r");
    if (pipe != nullptr) {
        char buf[512];
        while (fgets(buf, sizeof(buf), pipe) != nullptr)
            output += buf;
        ran = pclose(pipe) == 0;
    }
    BOOST_LOG_TRIVIAL(info) << "Bambu Connect: xdg-mime ran " << ran << ", handler '" << output << "'";
    return detect_linux(ran, output);
#endif
}

// -------------------------------------------------------------------------------------------
// Temp folder housekeeping
// -------------------------------------------------------------------------------------------

std::vector<std::string> stale_temp_files(const std::vector<TempEntry> &entries, std::time_t now, std::time_t max_age_s)
{
    std::vector<std::string> out;
    for (const TempEntry &e : entries) {
        if (!ends_with_ci(e.file_name, ".gcode.3mf"))
            continue;
        if (e.modified > 0 && now - e.modified > max_age_s)
            out.push_back(e.file_name);
    }
    return out;
}

std::string prepare_temp_dir()
{
    namespace fs = boost::filesystem;
    boost::system::error_code ec;
    const fs::path            dir = fs::temp_directory_path(ec) / "EdgeSlicer" / "bambu-connect";
    if (ec)
        return {};
    fs::create_directories(dir, ec);
    if (ec || !fs::is_directory(dir, ec))
        return {};

    std::vector<TempEntry> entries;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!fs::is_regular_file(it->path(), ec))
            continue;
        TempEntry e;
#ifdef _WIN32
        e.file_name = boost::nowide::narrow(it->path().filename().wstring());
#else
        e.file_name = it->path().filename().string();
#endif
        e.modified = fs::last_write_time(it->path(), ec);
        if (ec) {
            ec.clear();
            continue;
        }
        entries.push_back(e);
    }
    ec.clear();
    for (const std::string &leaf : stale_temp_files(entries, std::time(nullptr), TEMP_MAX_AGE_S)) {
#ifdef _WIN32
        fs::remove(dir / boost::nowide::widen(leaf), ec);
#else
        fs::remove(dir / leaf, ec);
#endif
        ec.clear();
    }
#ifdef _WIN32
    return boost::nowide::narrow(dir.wstring());
#else
    return dir.string();
#endif
}

} // namespace BambuConnect
} // namespace Slic3r
