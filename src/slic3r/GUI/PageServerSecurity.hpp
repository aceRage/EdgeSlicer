#ifndef slic3r_GUI_PageServerSecurity_hpp_
#define slic3r_GUI_PageServerSecurity_hpp_

// Access rules for the local page server (HttpServer with page security on, 127.0.0.1:13619+).
//
// That server hands the flutter pages and a few app-produced files to the app's own web views.
// Any website open in the user's normal browser can also reach 127.0.0.1, so every request has to
// prove it comes from one of our web views, and the file routes may only return files the app
// itself handed to a page. Kept free of wx/GUI types so the rules can be unit tested directly.

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace page_server {

// Query parameter the app appends to the URLs it loads into its web views.
constexpr const char* TOKEN_PARAM = "edge_page_token";
// Header alternative to the query parameter / cookie, for non-browser callers.
constexpr const char* TOKEN_HEADER = "X-Edge-Page-Token";

// 64 hex chars from the OS CSPRNG.
std::string generate_secret();

bool constant_time_equals(const std::string& a, const std::string& b);

// Cookie carrying the secret after the first tokened page load. The port is part of the name
// because cookies are shared between ports of one host, and several app instances (each with its
// own port and secret) can share one WebView2 cookie jar.
std::string cookie_name(uint16_t port);
std::string set_cookie_header_value(uint16_t port, const std::string& secret);

// Returns url with ?/&edge_page_token=<secret> appended (before any #fragment).
std::string append_token(const std::string& url, const std::string& secret);

// Removes every edge_page_token parameter from a raw request target; *token receives the last value.
std::string strip_token_param(const std::string& target, std::string* token = nullptr);

struct RequestInfo
{
    std::string method;
    std::string target;          // raw request target, not URL-decoded
    std::string host;            // Host header
    std::string origin;          // Origin header ("" when absent)
    std::string sec_fetch_site;  // Sec-Fetch-Site header ("" when absent)
    std::string cookie;          // Cookie header
    std::string header_token;    // X-Edge-Page-Token header
};

struct Decision
{
    bool        allowed    = false;
    int         status     = 403;
    std::string reason;          // short machine-readable reason for the log / 403 body
    bool        set_cookie = false;
    std::string target;          // request target with the token parameter removed
};

// port: the port this server listens on; secret: this process's page secret.
Decision authorize(const RequestInfo& req, uint16_t port, const std::string& secret);

// ---- files -------------------------------------------------------------------------------------

// Lexical screening of an absolute path taken from a URL (UTF-8, '/' or '\' separators).
// Rejects: relative / drive-relative / rooted-without-drive paths, UNC and device paths (\\server,
// \\?\, \\.\), '..' and '.' segments, NTFS alternate data streams (any ':' past the drive letter),
// Windows reserved characters, control characters, and segments ending in '.' or ' ' (Windows
// silently strips those, so "a.conf." would alias "a.conf").
bool is_acceptable_request_path(const std::string& utf8_path);

// Resolves symlinks, junctions, 8.3 short names and case to the file's real path, so two spellings
// of one file compare equal. Returns false when the file does not exist or cannot be opened.
// *key is a comparison key (lower-cased where the file system is case-insensitive), *display the
// real path in UTF-8. Directories resolve too; *is_dir tells them apart.
bool resolve_final_path(const std::string& utf8_path, std::string* key, std::string* display, bool* is_dir = nullptr);

// True if `key` (from resolve_final_path) is `dir_key` or lies below it.
bool key_is_within(const std::string& key, const std::string& dir_key);

// Files the app hands to its pages (the active G-code/3MF and its zip). Only these, plus the
// installed resources, can be read through /localfile/ and /wcp_download/.
class FileGrants
{
public:
    // Registers a file; returns an unguessable id usable as /localfile/cap/<id>/<name>.
    // Keeps the newest `capacity` grants.
    std::string grant(const std::string& utf8_path);

    // Path registered under id, or "" if unknown.
    std::string path_for_id(const std::string& id) const;

    // True if utf8_path resolves to the same file as a granted path.
    bool is_granted(const std::string& utf8_path) const;

    void clear();
    size_t size() const;

    static constexpr size_t capacity = 64;

private:
    struct Entry { std::string id; std::string path; };
    mutable std::mutex m_mutex;
    std::vector<Entry> m_entries; // oldest first
};

FileGrants& file_grants();

// Paths the file routes never serve, granted or not: *.conf / *.conf.* anywhere, and inside the
// data dir its top-level files plus the user/, system/ and hub/ trees (presets hold printer access
// codes and API keys). protected_dir_key = resolve_final_path key of the data dir ("" = none).
bool is_protected_file(const std::string& key, const std::string& protected_dir_key);

struct FileRoots
{
    std::string resources_dir;   // UTF-8, installed resources (always readable)
    std::string data_dir;        // UTF-8, app data dir (protected)
};

// Decides what /localfile/<rest> may serve. `rest` is URL-decoded. Accepts "cap/<id>[/<name>]"
// (a grant id) or an absolute path that is granted or inside the resources dir.
// Returns the real path to serve, or "" to refuse.
std::string resolve_localfile(const std::string& rest, const FileRoots& roots, const FileGrants& grants);

// Same check for a path that arrived through /wcp_download/<base64>.
std::string resolve_granted_path(const std::string& utf8_path, const FileRoots& roots, const FileGrants& grants);

// Decides what a plain resources URL ("/web/...", "/profiles/...") may serve: the real path when
// it stays inside the resources dir, "" otherwise.
std::string resolve_resource(const std::string& url_path, const std::string& resources_dir);

}}} // namespace Slic3r::GUI::page_server

#endif
