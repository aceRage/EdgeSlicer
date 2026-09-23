#include "PageServerSecurity.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <random>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <climits>
#include <cstdlib>
#include <sys/stat.h>
#endif

namespace Slic3r { namespace GUI { namespace page_server {

namespace {

std::string to_lower_ascii(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return s;
}

std::string trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

bool is_hex(const std::string& s)
{
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

// Value of cookie `name` in a Cookie header ("a=1; b=2"), "" if absent.
std::string cookie_value(const std::string& header, const std::string& name)
{
    size_t pos = 0;
    while (pos <= header.size()) {
        size_t end = header.find(';', pos);
        if (end == std::string::npos) end = header.size();
        const std::string pair = trim(header.substr(pos, end - pos));
        const size_t      eq   = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == name)
            return trim(pair.substr(eq + 1));
        pos = end + 1;
    }
    return "";
}

bool is_own_host(const std::string& host, uint16_t port)
{
    const std::string h = to_lower_ascii(trim(host));
    const std::string p = ":" + std::to_string(port);
    return h == "127.0.0.1" + p || h == "localhost" + p;
}

bool is_own_origin(const std::string& origin, uint16_t port)
{
    const std::string o = to_lower_ascii(trim(origin));
    const std::string p = ":" + std::to_string(port);
    return o == "http://127.0.0.1" + p || o == "http://localhost" + p;
}

#ifdef _WIN32
std::wstring widen(const std::string& utf8, UINT cp)
{
    if (utf8.empty()) return std::wstring();
    const DWORD flags = cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
    const int   n     = ::MultiByteToWideChar(cp, flags, utf8.data(), (int) utf8.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(n, L'\0');
    ::MultiByteToWideChar(cp, flags, utf8.data(), (int) utf8.size(), &w[0], n);
    return w;
}

std::string narrow_utf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int) w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int) w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
#endif

// Path with '\' turned into '/', and a trailing '/' removed (except for a bare root).
std::string key_dir_form(std::string k)
{
    while (k.size() > 1 && k.back() == '/') k.pop_back();
    return k;
}

} // namespace

std::string generate_secret()
{
    // std::random_device draws from the OS CSPRNG on every toolchain we build with
    // (RtlGenRandom on MSVC, getrandom()/urandom on libstdc++, arc4random on libc++).
    std::random_device rd;
    static const char* hex = "0123456789abcdef";
    std::string        s;
    s.reserve(64);
    for (int i = 0; i < 8; ++i) {
        uint32_t v = rd();
        for (int j = 0; j < 8; ++j) {
            s.push_back(hex[v & 0xF]);
            v >>= 4;
        }
    }
    return s;
}

bool constant_time_equals(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        diff |= (unsigned char) (a[i] ^ b[i]);
    return diff == 0;
}

std::string cookie_name(uint16_t port) { return "edge_page_" + std::to_string(port); }

std::string set_cookie_header_value(uint16_t port, const std::string& secret)
{
    // Session cookie; HttpOnly so page script never sees it; SameSite=Strict so no other site's
    // request carries it (the query-string token is only accepted on top of the other checks).
    return cookie_name(port) + "=" + secret + "; Path=/; HttpOnly; SameSite=Strict";
}

std::string append_token(const std::string& url, const std::string& secret)
{
    const size_t hash = url.find('#');
    std::string  base = hash == std::string::npos ? url : url.substr(0, hash);
    std::string  frag = hash == std::string::npos ? std::string() : url.substr(hash);
    base += (base.find('?') == std::string::npos ? "?" : "&");
    base += TOKEN_PARAM;
    base += "=";
    base += secret;
    return base + frag;
}

std::string strip_token_param(const std::string& target, std::string* token)
{
    const size_t q = target.find('?');
    if (q == std::string::npos)
        return target;
    const size_t      hash  = target.find('#', q);
    const std::string query = target.substr(q + 1, hash == std::string::npos ? std::string::npos : hash - q - 1);
    const std::string frag  = hash == std::string::npos ? std::string() : target.substr(hash);
    const std::string key   = std::string(TOKEN_PARAM) + "=";

    std::string kept;
    size_t      pos = 0;
    while (pos <= query.size()) {
        size_t end = query.find('&', pos);
        if (end == std::string::npos) end = query.size();
        const std::string part = query.substr(pos, end - pos);
        if (part.compare(0, key.size(), key) == 0) {
            if (token) *token = part.substr(key.size());
        } else if (part == TOKEN_PARAM) {
            if (token) *token = std::string();
        } else if (!part.empty()) {
            if (!kept.empty()) kept += "&";
            kept += part;
        }
        pos = end + 1;
    }
    return target.substr(0, q) + (kept.empty() ? std::string() : "?" + kept) + frag;
}

Decision authorize(const RequestInfo& req, uint16_t port, const std::string& secret)
{
    Decision d;
    std::string query_token;
    d.target = strip_token_param(req.target, &query_token);

    if (req.method != "GET" && req.method != "HEAD") {
        d.status = 405;
        d.reason = "method";
        return d;
    }
    // DNS rebinding: a foreign name resolving to 127.0.0.1 still carries its own Host.
    if (!is_own_host(req.host, port)) {
        d.reason = "host";
        return d;
    }
    // A request that states an origin must state ours. "null" (sandboxed frames, file:, data:)
    // is refused too: any website can produce it.
    if (!req.origin.empty() && !is_own_origin(req.origin, port)) {
        d.reason = "origin";
        return d;
    }
    if (to_lower_ascii(trim(req.sec_fetch_site)) == "cross-site") {
        d.reason = "cross-site";
        return d;
    }
    if (secret.empty()) {
        d.reason = "no-secret";
        return d;
    }
    if (!query_token.empty() && constant_time_equals(query_token, secret)) {
        d.allowed    = true;
        d.status     = 200;
        d.set_cookie = true;
        return d;
    }
    if (!req.header_token.empty() && constant_time_equals(trim(req.header_token), secret)) {
        d.allowed = true;
        d.status  = 200;
        return d;
    }
    const std::string cookie = cookie_value(req.cookie, cookie_name(port));
    if (!cookie.empty() && constant_time_equals(cookie, secret)) {
        d.allowed = true;
        d.status  = 200;
        return d;
    }
    d.reason = query_token.empty() && req.header_token.empty() && cookie.empty() ? "no-token" : "bad-token";
    return d;
}

bool is_acceptable_request_path(const std::string& p)
{
    if (p.empty() || p.size() > 4096)
        return false;
    for (unsigned char c : p)
        if (c < 0x20 || c == 0x7f)
            return false;

    std::string s = p;
    std::replace(s.begin(), s.end(), '\\', '/');

#ifdef _WIN32
    // Absolute drive path only: "X:/...". Rules out UNC (//server), device paths (//?/, //./),
    // rooted-without-drive (/foo) and drive-relative (C:foo).
    if (s.size() < 3 || !std::isalpha((unsigned char) s[0]) || s[1] != ':' || s[2] != '/')
        return false;
    // Any further ':' is an alternate data stream ("file.gcode:secret", "::$DATA").
    if (s.find(':', 2) != std::string::npos)
        return false;
    if (s.find_first_of("*?\"<>|") != std::string::npos)
        return false;
    const size_t first = 3;
#else
    if (s[0] != '/')
        return false;
    const size_t first = 1;
#endif
    size_t pos = first;
    while (pos <= s.size()) {
        size_t end = s.find('/', pos);
        if (end == std::string::npos) end = s.size();
        const std::string seg = s.substr(pos, end - pos);
        if (seg == "." || seg == "..")
            return false;
#ifdef _WIN32
        // Windows drops trailing dots and spaces from a name, so "a.conf." opens "a.conf".
        if (!seg.empty() && (seg.back() == '.' || seg.back() == ' '))
            return false;
#endif
        pos = end + 1;
    }
    return true;
}

bool resolve_final_path(const std::string& utf8_path, std::string* key, std::string* display, bool* is_dir)
{
    if (utf8_path.empty())
        return false;
#ifdef _WIN32
    std::wstring w = widen(utf8_path, CP_UTF8);
    if (w.empty())
        w = widen(utf8_path, CP_ACP); // not UTF-8: a path in the system code page
    if (w.empty())
        return false;
    std::replace(w.begin(), w.end(), L'/', L'\\');

    // Opening the file (no data access needed) and asking the handle for its name resolves
    // symlinks, junctions, mount points, 8.3 short names and case in one go.
    HANDLE h = ::CreateFileW(w.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    BY_HANDLE_FILE_INFORMATION info{};
    const bool   have_info = ::GetFileInformationByHandle(h, &info) != 0;
    std::wstring buf(1024, L'\0');
    DWORD        n = ::GetFinalPathNameByHandleW(h, &buf[0], (DWORD) buf.size(), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n >= buf.size()) {
        buf.assign(n + 1, L'\0');
        n = ::GetFinalPathNameByHandleW(h, &buf[0], (DWORD) buf.size(), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    }
    ::CloseHandle(h);
    if (n == 0 || n >= buf.size() || !have_info)
        return false;
    buf.resize(n);
    if (buf.compare(0, 8, L"\\\\?\\UNC\\") == 0)
        buf = L"\\\\" + buf.substr(8);
    else if (buf.compare(0, 4, L"\\\\?\\") == 0)
        buf = buf.substr(4);
    std::replace(buf.begin(), buf.end(), L'\\', L'/');

    std::wstring lower = buf;
    if (!lower.empty())
        ::CharLowerBuffW(&lower[0], (DWORD) lower.size());
    if (key) *key = key_dir_form(narrow_utf8(lower));
    if (display) *display = narrow_utf8(buf);
    if (is_dir) *is_dir = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return true;
#else
    char        buf[PATH_MAX];
    const char* r = ::realpath(utf8_path.c_str(), buf);
    if (!r)
        return false;
    struct stat st;
    if (::stat(buf, &st) != 0)
        return false;
    std::string real(buf);
#ifdef __APPLE__
    if (key) *key = key_dir_form(to_lower_ascii(real)); // APFS/HFS+ default to case-insensitive
#else
    if (key) *key = key_dir_form(real);
#endif
    if (display) *display = real;
    if (is_dir) *is_dir = S_ISDIR(st.st_mode);
    return true;
#endif
}

bool key_is_within(const std::string& key, const std::string& dir_key)
{
    if (dir_key.empty() || key.size() < dir_key.size())
        return false;
    if (key.compare(0, dir_key.size(), dir_key) != 0)
        return false;
    return key.size() == dir_key.size() || key[dir_key.size()] == '/' || dir_key.back() == '/';
}

std::string FileGrants::grant(const std::string& utf8_path)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        if (it->path == utf8_path) {
            Entry e = *it;
            m_entries.erase(it);
            m_entries.push_back(e); // refresh: newest last
            return e.id;
        }
    }
    Entry e{generate_secret().substr(0, 32), utf8_path};
    m_entries.push_back(e);
    if (m_entries.size() > capacity)
        m_entries.erase(m_entries.begin());
    return e.id;
}

std::string FileGrants::path_for_id(const std::string& id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const Entry& e : m_entries)
        if (constant_time_equals(e.id, id))
            return e.path;
    return "";
}

bool FileGrants::is_granted(const std::string& utf8_path) const
{
    std::string key;
    if (!resolve_final_path(utf8_path, &key, nullptr))
        return false;
    std::vector<std::string> paths;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const Entry& e : m_entries) paths.push_back(e.path);
    }
    // Resolved at check time, not at grant time: a zip may be granted before it is written.
    for (const std::string& p : paths) {
        std::string gkey;
        if (resolve_final_path(p, &gkey, nullptr) && gkey == key)
            return true;
    }
    return false;
}

void FileGrants::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.clear();
}

size_t FileGrants::size() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries.size();
}

FileGrants& file_grants()
{
    static FileGrants grants;
    return grants;
}

bool is_protected_file(const std::string& key, const std::string& protected_dir_key)
{
    const size_t      slash = key.find_last_of('/');
    const std::string name  = to_lower_ascii(slash == std::string::npos ? key : key.substr(slash + 1));
    const size_t      conf  = name.find(".conf");
    if (conf != std::string::npos && (conf + 5 == name.size() || name[conf + 5] == '.'))
        return true;
    if (protected_dir_key.empty() || !key_is_within(key, protected_dir_key) || key.size() == protected_dir_key.size())
        return false;
    const std::string rel = key.substr(protected_dir_key.size() + (protected_dir_key.back() == '/' ? 0 : 1));
    if (rel.find('/') == std::string::npos)
        return true; // a top-level file of the data dir: the app config and its backups
    for (const char* sub : {"user/", "system/", "hub/"})
        if (rel.compare(0, std::strlen(sub), sub) == 0)
            return true;
    return false;
}

namespace {

std::string dir_key(const std::string& dir)
{
    std::string key;
    bool        is_dir = false;
    if (dir.empty() || !resolve_final_path(dir, &key, nullptr, &is_dir) || !is_dir)
        return "";
    return key;
}

// Common tail of the file routes: resolve, refuse directories and protected files.
std::string serve_if(const std::string& path, const FileRoots& roots, bool (*extra)(const std::string&, const std::string&, const FileRoots&, const FileGrants&),
                     const FileGrants& grants)
{
    std::string key, display;
    bool        is_dir = false;
    if (!resolve_final_path(path, &key, &display, &is_dir) || is_dir)
        return "";
    if (is_protected_file(key, dir_key(roots.data_dir)))
        return "";
    if (!extra(path, key, roots, grants))
        return "";
    return display;
}

bool granted_or_resource(const std::string& path, const std::string& key, const FileRoots& roots, const FileGrants& grants)
{
    const std::string res = dir_key(roots.resources_dir);
    if (!res.empty() && key_is_within(key, res))
        return true;
    return grants.is_granted(path);
}

} // namespace

std::string resolve_localfile(const std::string& rest, const FileRoots& roots, const FileGrants& grants)
{
    if (rest.compare(0, 4, "cap/") == 0) {
        const size_t      slash = rest.find('/', 4);
        const std::string id    = rest.substr(4, slash == std::string::npos ? std::string::npos : slash - 4);
        if (id.size() != 32 || !is_hex(id))
            return "";
        const std::string path = grants.path_for_id(id);
        if (path.empty())
            return "";
        std::string key, display;
        bool        is_dir = false;
        if (!resolve_final_path(path, &key, &display, &is_dir) || is_dir || is_protected_file(key, dir_key(roots.data_dir)))
            return "";
        return display;
    }
    if (!is_acceptable_request_path(rest))
        return "";
    return serve_if(rest, roots, &granted_or_resource, grants);
}

std::string resolve_granted_path(const std::string& utf8_path, const FileRoots& roots, const FileGrants& grants)
{
    if (!is_acceptable_request_path(utf8_path))
        return "";
    return serve_if(utf8_path, roots, &granted_or_resource, grants);
}

std::string resolve_resource(const std::string& url_path, const std::string& resources_dir)
{
    if (url_path.empty() || url_path[0] != '/' || resources_dir.empty())
        return "";
    // The URL part is appended to the resources dir, so it is screened as a path below it.
    std::string rel = url_path;
    std::replace(rel.begin(), rel.end(), '\\', '/');
    for (unsigned char c : rel)
        if (c < 0x20 || c == 0x7f)
            return "";
    if (rel.find(':') != std::string::npos || rel.compare(0, 2, "//") == 0)
        return "";
    size_t pos = 1;
    while (pos <= rel.size()) {
        size_t end = rel.find('/', pos);
        if (end == std::string::npos) end = rel.size();
        const std::string seg = rel.substr(pos, end - pos);
        if (seg == "." || seg == "..")
            return "";
#ifdef _WIN32
        if (!seg.empty() && (seg.back() == '.' || seg.back() == ' '))
            return "";
#endif
        pos = end + 1;
    }
    const std::string res = dir_key(resources_dir);
    if (res.empty())
        return "";
    std::string key, display;
    bool        is_dir = false;
    if (!resolve_final_path(resources_dir + rel, &key, &display, &is_dir) || is_dir)
        return "";
    if (!key_is_within(key, res))
        return ""; // a link inside resources pointing out of it
    return display;
}

}}} // namespace Slic3r::GUI::page_server
