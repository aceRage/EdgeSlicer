#include "Layerling.hpp"

#include "UntrustedInput.hpp"
#include "miniz_extension.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <set>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#ifdef _WIN32
#include <boost/nowide/convert.hpp>
#endif

#include <expat.h>
#include <nlohmann/json.hpp>
#include <openssl/sha.h>

namespace Slic3r {
namespace layerling {

namespace fs = boost::filesystem;

namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

bool ends_with_ci(const std::string &s, const std::string &suffix)
{
    return s.size() >= suffix.size() && lower(s.substr(s.size() - suffix.size())) == lower(suffix);
}

bool starts_with(const std::string &s, const std::string &prefix) { return s.compare(0, prefix.size(), prefix) == 0; }

std::string trim(const std::string &s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char) s[b]))
        ++b;
    while (e > b && std::isspace((unsigned char) s[e - 1]))
        --e;
    return s.substr(b, e - b);
}

// UTF-8 for miniz_extension's open_zip_reader (which takes UTF-8 and widens it on Windows).
std::string path_utf8(const fs::path &p)
{
#ifdef _WIN32
    return boost::nowide::narrow(p.wstring());
#else
    return p.string();
#endif
}

std::string extension_of(const std::string &file_name)
{
    const size_t slash = file_name.find_last_of("/\\");
    const std::string leaf = slash == std::string::npos ? file_name : file_name.substr(slash + 1);
    const size_t dot = leaf.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? std::string() : lower(leaf.substr(dot));
}

} // namespace

// ------------------------------------------------------------------------------------ releases ----

std::string parse_sha256_digest(const std::string &digest)
{
    const std::string d = trim(digest);
    if (!starts_with(lower(d), "sha256:"))
        return std::string();
    std::string hex = lower(d.substr(7));
    if (hex.size() != 64)
        return std::string();
    for (char c : hex)
        if (!std::isxdigit((unsigned char) c))
            return std::string();
    return hex;
}

std::string normalize_version(const std::string &version)
{
    std::string v = trim(version);
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V'))
        v.erase(0, 1);
    return v;
}

bool is_safe_version(const std::string &version)
{
    if (version.empty() || version.size() > 64 || version == "." || version == "..")
        return false;
    if (!std::isalnum((unsigned char) version[0]))
        return false;
    if (version.find("..") != std::string::npos)
        return false;
    for (char c : version)
        if (!(std::isalnum((unsigned char) c) || c == '.' || c == '-' || c == '_'))
            return false;
    return true;
}

int compare_versions(const std::string &a_in, const std::string &b_in)
{
    auto split = [](const std::string &v, std::vector<long long> &nums, std::string &pre) {
        std::string s = normalize_version(v);
        const size_t cut = s.find_first_of("-+");
        if (cut != std::string::npos) {
            // "+build" metadata does not order; "-pre" does.
            if (s[cut] == '-') {
                const size_t plus = s.find('+', cut);
                pre = s.substr(cut + 1, plus == std::string::npos ? std::string::npos : plus - cut - 1);
            }
            s = s.substr(0, cut);
        }
        size_t pos = 0;
        while (pos <= s.size()) {
            const size_t dot = s.find('.', pos);
            const std::string part = s.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos);
            long long n = 0;
            for (char c : part) {
                if (!std::isdigit((unsigned char) c))
                    break;
                n = n * 10 + (c - '0');
                if (n > 1000000000LL)
                    break;
            }
            nums.push_back(n);
            if (dot == std::string::npos)
                break;
            pos = dot + 1;
        }
    };
    std::vector<long long> na, nb;
    std::string            pa, pb;
    split(a_in, na, pa);
    split(b_in, nb, pb);
    const size_t n = std::max(na.size(), nb.size());
    for (size_t i = 0; i < n; ++i) {
        const long long x = i < na.size() ? na[i] : 0;
        const long long y = i < nb.size() ? nb[i] : 0;
        if (x != y)
            return x < y ? -1 : 1;
    }
    if (pa == pb)
        return 0;
    if (pa.empty())
        return 1; // a release is newer than its pre-releases
    if (pb.empty())
        return -1;
    return pa < pb ? -1 : 1;
}

bool update_check_due(std::int64_t last_check, std::int64_t now)
{
    if (last_check <= 0 || last_check > now)
        return true;
    return now - last_check >= 24 * 60 * 60;
}

bool parse_release_json(const std::string &json, ReleaseInfo &out, std::string *error)
{
    auto fail = [error](const std::string &why) {
        if (error)
            *error = why;
        return false;
    };
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json);
    } catch (const std::exception &) {
        return fail("the release information is not valid JSON");
    }
    if (!j.is_object())
        return fail("the release information is not a JSON object");
    auto str = [](const nlohmann::json &o, const char *key) -> std::string {
        auto it = o.find(key);
        return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    auto flag = [](const nlohmann::json &o, const char *key) {
        auto it = o.find(key);
        return it != o.end() && it->is_boolean() && it->get<bool>();
    };
    if (flag(j, "draft") || flag(j, "prerelease"))
        return fail("the latest release is a draft or a pre-release");

    ReleaseInfo r;
    r.tag          = str(j, "tag_name");
    r.version      = normalize_version(r.tag);
    r.published_at = str(j, "published_at");
    r.html_url     = str(j, "html_url");
    if (!is_safe_version(r.version))
        return fail("the release has no usable version number");

    auto assets = j.find("assets");
    if (assets == j.end() || !assets->is_array())
        return fail("the release lists no files");

    const std::string versioned = "layerling-" + r.version + "-static.zip";
    const nlohmann::json *chosen = nullptr;
    int                   rank   = 0;
    for (const nlohmann::json &a : *assets) {
        if (!a.is_object())
            continue;
        const std::string name = str(a, "name");
        int r_ = 0;
        if (name == versioned)
            r_ = 3;
        else if (name == "layerling-static.zip")
            r_ = 2;
        else if (ends_with_ci(name, "-static.zip"))
            r_ = 1;
        if (r_ > rank) {
            rank   = r_;
            chosen = &a;
        }
    }
    if (chosen == nullptr)
        return fail("the release has no static web build (layerling-<version>-static.zip)");

    r.asset_name = str(*chosen, "name");
    r.asset_url  = str(*chosen, "browser_download_url");
    if (!starts_with(r.asset_url, ASSET_URL_PREFIX) || r.asset_url.find("..") != std::string::npos ||
        r.asset_url.find_first_of(" \t\r\n\\?#") != std::string::npos)
        return fail("the download address is not on Layerling's GitHub releases");
    auto size = chosen->find("size");
    if (size == chosen->end() || !size->is_number_unsigned() || size->get<std::uint64_t>() == 0)
        return fail("the release does not say how large the download is");
    r.asset_size = size->get<std::uint64_t>();
    if (r.asset_size > MAX_ARCHIVE_BYTES)
        return fail("the download is larger than expected");
    const std::string digest = str(*chosen, "digest");
    if (!digest.empty()) {
        r.sha256 = parse_sha256_digest(digest);
        if (r.sha256.empty() && starts_with(lower(trim(digest)), "sha256:"))
            return fail("the release's checksum is malformed");
    }
    out = std::move(r);
    return true;
}

// ------------------------------------------------------------------------------------- archive ----

ArchiveCheck check_archive(const fs::path &zip_path)
{
    ArchiveCheck   res;
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!open_zip_reader(&zip, path_utf8(zip_path))) {
        res.error = "the download is not a zip archive";
        return res;
    }
    const mz_uint num = mz_zip_reader_get_num_files(&zip);
    res.entries       = num;
    if (num == 0 || num > MAX_ARCHIVE_ENTRIES) {
        close_zip_reader(&zip);
        res.error = num == 0 ? "the archive is empty" : "the archive has too many files";
        return res;
    }
    bool                  root_index = false;
    std::set<std::string> top_folders;
    std::set<std::string> nested_index; // top folders that hold an index.html
    bool                  loose_files = false;
    for (mz_uint i = 0; i < num; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) {
            close_zip_reader(&zip);
            res.error = "the archive is damaged";
            return res;
        }
        if (zip_entry_is_symlink(st)) {
            close_zip_reader(&zip);
            res.error = "the archive contains a symbolic link";
            return res;
        }
        res.unpacked += st.m_uncomp_size;
        if (res.unpacked > MAX_UNPACKED_BYTES) {
            close_zip_reader(&zip);
            res.error = "the archive unpacks to more than expected";
            return res;
        }
        std::string name;
        const untrusted::ArchiveEntryName verdict = untrusted::normalize_archive_entry_path(st.m_filename, name);
        if (verdict == untrusted::ArchiveEntryName::Skip)
            continue;
        if (verdict == untrusted::ArchiveEntryName::Reject) {
            close_zip_reader(&zip);
            res.error = std::string("the archive contains an unsafe file name: ") + st.m_filename;
            return res;
        }
        const size_t slash = name.find('/');
        if (slash == std::string::npos) {
            if (!st.m_is_directory)
                loose_files = true;
            if (name == "index.html" && !st.m_is_directory)
                root_index = true;
            if (st.m_is_directory)
                top_folders.insert(name);
        } else {
            const std::string top = name.substr(0, slash);
            top_folders.insert(top);
            if (name == top + "/index.html")
                nested_index.insert(top);
        }
    }
    close_zip_reader(&zip);
    if (root_index) {
        res.ok = true;
        return res;
    }
    if (!loose_files && top_folders.size() == 1 && nested_index.size() == 1) {
        res.root_prefix = *nested_index.begin() + "/";
        res.ok          = true;
        return res;
    }
    res.error = "the archive is not a Layerling web build (no index.html)";
    return res;
}

fs::path find_app_root(const fs::path &version_dir)
{
    boost::system::error_code ec;
    if (fs::is_regular_file(version_dir / "index.html", ec))
        return version_dir;
    fs::path only;
    int      dirs = 0;
    for (fs::directory_iterator it(version_dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (fs::is_directory(it->symlink_status()))
            only = it->path(), ++dirs;
        else if (fs::is_regular_file(it->symlink_status()))
            return fs::path(); // loose files but no index.html at the top
    }
    if (dirs == 1 && fs::is_regular_file(only / "index.html", ec))
        return only;
    return fs::path();
}

bool install_archive(const fs::path &zip_path, const fs::path &versions_dir, const std::string &version, fs::path &app_root,
                     std::string &error)
{
    if (!is_safe_version(version)) {
        error = "unusable version number";
        return false;
    }
    const ArchiveCheck check = check_archive(zip_path);
    if (!check.ok) {
        error = check.error;
        return false;
    }
    boost::system::error_code ec;
    fs::create_directories(versions_dir, ec);
    if (ec) {
        error = "cannot create " + path_utf8(versions_dir) + ": " + ec.message();
        return false;
    }
    const fs::path staging = versions_dir / fs::unique_path(".install-" + version + "-%%%%%%%%");
    if (!untrusted::is_path_within_root(versions_dir, staging)) {
        error = "the install folder is not inside the Layerling folder";
        return false;
    }
    std::string err;
    if (!extract_archive_confined(zip_path, staging, err)) {
        fs::remove_all(staging, ec);
        error = "the archive could not be unpacked: " + err;
        return false;
    }
    if (find_app_root(staging).empty()) {
        fs::remove_all(staging, ec);
        error = "the unpacked files have no index.html";
        return false;
    }
    const fs::path target = versions_dir / version;
    if (!untrusted::is_path_within_root(versions_dir, target)) {
        fs::remove_all(staging, ec);
        error = "the version folder is not inside the Layerling folder";
        return false;
    }
    if (fs::exists(fs::symlink_status(target, ec))) {
        // A reinstall of the same version: move the old copy aside first, so the rename below
        // cannot fail on a non-empty folder, and drop it once the new one is in place.
        const fs::path old = versions_dir / fs::unique_path(".old-" + version + "-%%%%%%%%");
        fs::rename(target, old, ec);
        if (ec) {
            fs::remove_all(staging, ec);
            error = "the installed copy of this version is in use";
            return false;
        }
        fs::rename(staging, target, ec);
        if (ec) {
            boost::system::error_code ec2;
            fs::rename(old, target, ec2);
            fs::remove_all(staging, ec2);
            error = "the new files could not be moved into place: " + ec.message();
            return false;
        }
        fs::remove_all(old, ec);
    } else {
        fs::rename(staging, target, ec);
        if (ec) {
            boost::system::error_code ec2;
            fs::remove_all(staging, ec2);
            error = "the new files could not be moved into place: " + ec.message();
            return false;
        }
    }
    app_root = find_app_root(target);
    if (app_root.empty()) {
        error = "the installed files have no index.html";
        return false;
    }
    return true;
}

// -------------------------------------------------------------------------------------- server ----

const char *mime_type_for(const std::string &file_name)
{
    static const std::map<std::string, const char *> types = {
        {".html", "text/html; charset=utf-8"},
        {".htm", "text/html; charset=utf-8"},
        {".js", "text/javascript; charset=utf-8"},
        {".mjs", "text/javascript; charset=utf-8"},
        {".css", "text/css; charset=utf-8"},
        {".json", "application/json; charset=utf-8"},
        {".map", "application/json; charset=utf-8"},
        {".webmanifest", "application/manifest+json; charset=utf-8"},
        {".wasm", "application/wasm"},
        {".svg", "image/svg+xml"},
        {".png", "image/png"},
        {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".webp", "image/webp"},
        {".gif", "image/gif"},
        {".avif", "image/avif"},
        {".ico", "image/x-icon"},
        {".woff", "font/woff"},
        {".woff2", "font/woff2"},
        {".ttf", "font/ttf"},
        {".otf", "font/otf"},
        {".txt", "text/plain; charset=utf-8"},
        {".md", "text/plain; charset=utf-8"},
        {".xml", "application/xml; charset=utf-8"},
        {".pdf", "application/pdf"},
        {".mp4", "video/mp4"},
        {".webm", "video/webm"},
        {".bin", "application/octet-stream"},
    };
    auto it = types.find(extension_of(file_name));
    return it == types.end() ? "application/octet-stream" : it->second;
}

const char *cache_control_for(const std::string &relative_path)
{
    if (starts_with(relative_path, "_next/static/") && relative_path.find("/chunks/") != std::string::npos)
        return "public, max-age=31536000, immutable";
    if (starts_with(relative_path, "_next/static/"))
        return "public, max-age=86400";
    return "no-cache";
}

std::vector<std::string> candidate_files(const std::string &target, bool *forbidden)
{
    auto refuse = [forbidden]() {
        if (forbidden)
            *forbidden = true;
        return std::vector<std::string>();
    };
    if (forbidden)
        *forbidden = false;
    std::string path = target.substr(0, target.find_first_of("?#"));
    if (path.empty() || path[0] != '/')
        return refuse();
    for (unsigned char c : path)
        if (c < 0x20 || c == 0x7f || c == '\\' || c == ':')
            return refuse();
    std::vector<std::string> segments;
    size_t                   pos = 1;
    while (pos <= path.size()) {
        const size_t      slash = path.find('/', pos);
        const std::string seg   = path.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        if (seg == "." || seg == "..")
            return refuse();
        if (!seg.empty()) // "a//b": the empty segment is skipped
            segments.push_back(seg);
        if (slash == std::string::npos)
            break;
        pos = slash + 1;
    }
    // Segments made only of dots and spaces ("... ", Windows trims them to "..").
    for (const std::string &seg : segments)
        if (seg.find_first_not_of(". ") == std::string::npos)
            return refuse();
    std::string rel;
    for (const std::string &seg : segments)
        rel += (rel.empty() ? "" : "/") + seg;
    if (ends_with_ci(rel, ".php") || ends_with_ci(rel, ".phtml") || lower(rel).find(".php/") != std::string::npos)
        return refuse();
    const bool dir_like = path.back() == '/';
    if (rel.empty())
        return {"index.html"};
    if (dir_like)
        return {rel + "/index.html"};
    std::vector<std::string> out{rel};
    if (extension_of(rel).empty())
        out.push_back(rel + ".html");
    out.push_back(rel + "/index.html");
    return out;
}

std::string origin_of(const std::string &url)
{
    untrusted::Url u;
    if (!untrusted::parse_url(url, u) || (u.scheme != "http" && u.scheme != "https") || u.host.empty() || u.has_userinfo)
        return std::string();
    std::string o = u.scheme + "://" + u.host;
    if (u.port > 0 && !((u.scheme == "http" && u.port == 80) || (u.scheme == "https" && u.port == 443)))
        o += ":" + std::to_string(u.port);
    return o;
}

// ------------------------------------------------------------------------------------ web view ----

Navigation decide_navigation(const std::string &url, const std::string &allowed_origin)
{
    const std::string l = lower(url);
    if (l == "about:blank" || l == "about:srcdoc")
        return Navigation::Allow;
    if (starts_with(l, "blob:") || starts_with(l, "data:"))
        return Navigation::Allow;
    const std::string o = origin_of(url);
    if (!o.empty() && !allowed_origin.empty() && o == allowed_origin)
        return Navigation::Allow;
    if ((!o.empty() || starts_with(l, "mailto:")) && untrusted::is_safe_to_open_externally(url))
        return Navigation::External;
    return Navigation::Block;
}

const char *hide_save_picker_script()
{
    return "(function(){try{Object.defineProperty(window,'showSaveFilePicker',{value:undefined,configurable:true,writable:true});}"
           "catch(e){try{delete window.showSaveFilePicker;}catch(e2){}}})();";
}

const char *suppress_install_hint_script()
{
    return "(function(){try{if(window.top!==window)return;var k='layerling.installHintDismissed';"
           "if(window.localStorage.getItem(k)!=='1')window.localStorage.setItem(k,'1');}catch(e){}})();";
}

std::string document_start_script()
{
    return std::string(hide_save_picker_script()) + suppress_install_hint_script();
}

std::string start_url(const std::string &base_url)
{
    const std::string o = origin_of(base_url);
    if (o.empty())
        return std::string();
    untrusted::Url u;
    untrusted::parse_url(base_url, u);
    if (u.path.empty() && u.query.empty())
        return o + "/";
    return base_url.substr(0, base_url.find('#'));
}

Sha256::Sha256() : m_ctx(new SHA256_CTX) { SHA256_Init(static_cast<SHA256_CTX *>(m_ctx)); }
Sha256::~Sha256() { delete static_cast<SHA256_CTX *>(m_ctx); }
void Sha256::update(const void *data, std::size_t size)
{
    if (size > 0)
        SHA256_Update(static_cast<SHA256_CTX *>(m_ctx), data, size);
}
std::string Sha256::hex_digest()
{
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256_Final(digest, static_cast<SHA256_CTX *>(m_ctx));
    static const char *hex = "0123456789abcdef";
    std::string        out;
    for (unsigned char c : digest) {
        out += hex[c >> 4];
        out += hex[c & 15];
    }
    return out;
}

// ------------------------------------------------------------------------------------- funding ----

namespace {

bool plain_account_name(const std::string &s)
{
    if (s.empty() || s.size() > 100)
        return false;
    for (char c : s)
        if (!(std::isalnum((unsigned char) c) || c == '-' || c == '_' || c == '.'))
            return false;
    return s.find("..") == std::string::npos;
}

std::string unquote(std::string s)
{
    s = trim(s);
    if (s.size() >= 2 && ((s.front() == '\'' && s.back() == '\'') || (s.front() == '"' && s.back() == '"')))
        s = s.substr(1, s.size() - 2);
    return trim(s);
}

// Drops a YAML comment: a '#' at the start or after a blank, outside quotes.
std::string strip_comment(const std::string &line)
{
    char quote = 0;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quote) {
            if (c == quote)
                quote = 0;
        } else if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == '#' && (i == 0 || std::isspace((unsigned char) line[i - 1]))) {
            return line.substr(0, i);
        }
    }
    return line;
}

std::vector<std::string> values_of(const std::string &raw)
{
    std::vector<std::string> out;
    const std::string v = trim(raw);
    if (v.empty())
        return out;
    if (v.front() == '[' && v.back() == ']') {
        const std::string inner = v.substr(1, v.size() - 2);
        std::string       cur;
        char              quote = 0;
        for (char c : inner) {
            if (quote) {
                if (c == quote)
                    quote = 0;
                cur += c;
            } else if (c == '\'' || c == '"') {
                quote = c;
                cur += c;
            } else if (c == ',') {
                out.push_back(unquote(cur));
                cur.clear();
            } else {
                cur += c;
            }
        }
        out.push_back(unquote(cur));
    } else {
        out.push_back(unquote(v));
    }
    out.erase(std::remove_if(out.begin(), out.end(), [](const std::string &s) { return s.empty(); }), out.end());
    return out;
}

std::string custom_label(const std::string &url)
{
    untrusted::Url u;
    if (!untrusted::parse_url(url, u))
        return url;
    std::string host = u.host;
    if (starts_with(host, "www."))
        host = host.substr(4);
    if (host == "paypal.com" || host == "paypal.me" || ends_with_ci(host, ".paypal.com"))
        return "PayPal";
    if (host == "buymeacoffee.com")
        return "Buy Me a Coffee";
    if (host == "ko-fi.com")
        return "Ko-fi";
    if (host == "patreon.com")
        return "Patreon";
    return host;
}

} // namespace

std::vector<FundingLink> parse_funding_yml(const std::string &text)
{
    struct Platform { const char *key; const char *label; const char *prefix; };
    static const Platform platforms[] = {
        {"github", "GitHub Sponsors", "https://github.com/sponsors/"},
        {"ko_fi", "Ko-fi", "https://ko-fi.com/"},
        {"patreon", "Patreon", "https://www.patreon.com/"},
        {"open_collective", "Open Collective", "https://opencollective.com/"},
        {"liberapay", "Liberapay", "https://liberapay.com/"},
        {"buy_me_a_coffee", "Buy Me a Coffee", "https://www.buymeacoffee.com/"},
        {"polar", "Polar", "https://polar.sh/"},
    };
    std::vector<FundingLink> out;
    std::set<std::string>    seen;
    auto add = [&out, &seen](const std::string &label, const std::string &url) {
        if (url.size() > 512 || !untrusted::is_safe_to_open_externally(url) || !starts_with(url, "https://"))
            return;
        if (seen.insert(url).second)
            out.push_back({label, url});
    };
    size_t pos = 0;
    while (pos < text.size()) {
        size_t      eol  = text.find('\n', pos);
        std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos              = eol == std::string::npos ? text.size() : eol + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || std::isspace((unsigned char) line[0]))
            continue; // only top-level keys
        line = strip_comment(line);
        const size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        const std::string key    = trim(line.substr(0, colon));
        const auto        values = values_of(line.substr(colon + 1));
        if (key == "custom") {
            for (const std::string &v : values)
                add(custom_label(v), v);
            continue;
        }
        for (const Platform &p : platforms) {
            if (key != p.key)
                continue;
            for (const std::string &v : values)
                if (plain_account_name(v))
                    add(p.label, std::string(p.prefix) + v);
        }
    }
    return out;
}

std::vector<FundingLink> default_funding_links()
{
    return {
        {"GitHub Sponsors", "https://github.com/sponsors/henmedia"},
        {"Ko-fi", "https://ko-fi.com/henmedia"},
        {"PayPal", "https://www.paypal.com/donate?business=TX5T9PET4XKWN&item_name=layerling&currency_code=EUR&no_recurring=0"},
    };
}

// ------------------------------------------------------------------------------------- exports ----

ExportKind classify_export(const std::string &file_name)
{
    const std::string ext = extension_of(file_name);
    if (ext == ".3mf" || ext == ".stl" || ext == ".obj" || ext == ".step" || ext == ".stp")
        return ExportKind::Model;
    if (ext == ".lyl" || ext == ".skf")
        return ExportKind::Project;
    return ExportKind::Other;
}

std::string sanitize_export_name(const std::string &suggested)
{
    std::string name = suggested;
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos)
        name = name.substr(slash + 1);
    std::string out;
    for (unsigned char c : name) {
        if (c < 0x20 || c == 0x7f || std::strchr("<>:\"|?*/\\", c) != nullptr)
            out += '_';
        else
            out += char(c);
    }
    out = trim(out);
    while (!out.empty() && (out.back() == '.' || out.back() == ' '))
        out.pop_back();
    // Split off a short, plain extension (".3mf"), also when nothing stands before it.
    std::string  stem = out, ext;
    const size_t dot  = out.find_last_of('.');
    if (dot != std::string::npos && out.size() - dot <= 11) {
        const std::string e = out.substr(dot);
        bool              plain = e.size() > 1;
        for (size_t i = 1; i < e.size(); ++i)
            plain = plain && std::isalnum((unsigned char) e[i]);
        if (plain) {
            stem = out.substr(0, dot);
            ext  = e;
        }
    }
    // No hidden files, no leading or trailing blanks and dots on the stem.
    while (!stem.empty() && (stem.front() == '.' || stem.front() == ' '))
        stem.erase(0, 1);
    while (!stem.empty() && (stem.back() == '.' || stem.back() == ' '))
        stem.pop_back();
    if (stem.size() + ext.size() > 120) {
        stem.resize(120 - ext.size());
        // Do not cut a UTF-8 sequence in half: drop trailing continuation bytes and their lead byte.
        while (!stem.empty() && (static_cast<unsigned char>(stem.back()) & 0xC0) == 0x80)
            stem.pop_back();
        if (!stem.empty() && (static_cast<unsigned char>(stem.back()) & 0x80))
            stem.pop_back();
    }
    if (stem.empty())
        stem = "layerling-export";
    return stem + ext;
}

// --------------------------------------------------------------------------------- 3MF colours ----

namespace {

std::string local_name(const char *name)
{
    const char *colon = std::strrchr(name, ':');
    return colon ? std::string(colon + 1) : std::string(name);
}

const char *attr(const char **atts, const char *key)
{
    for (int i = 0; atts[i] != nullptr; i += 2)
        if (std::strcmp(atts[i], key) == 0)
            return atts[i + 1];
    return nullptr;
}

// "#RRGGBB" or "#RRGGBBAA" -> "#RRGGBB" upper case; "" otherwise.
std::string normalize_colour(const char *c)
{
    if (c == nullptr)
        return std::string();
    std::string s = trim(c);
    if (s.size() != 7 && s.size() != 9)
        return std::string();
    if (s[0] != '#')
        return std::string();
    for (size_t i = 1; i < s.size(); ++i)
        if (!std::isxdigit((unsigned char) s[i]))
            return std::string();
    s = s.substr(0, 7);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return char(std::toupper(ch)); });
    return s;
}

struct ColourParser
{
    std::map<std::string, std::vector<std::string>> groups; // basematerials / colorgroup id -> colours
    std::string                                       group;  // the group being read
    struct Obj
    {
        std::string id, name, pid, pindex;
        std::string tri_pid, tri_p1; // first triangle's
        bool        has_triangle = false;
        bool        has_mesh     = false;
    };
    std::vector<Obj>         objects;
    bool                     in_object = false;
    std::vector<std::string> build_items;
    std::set<std::string>    components; // object ids used as components

    void start(const char *name, const char **atts)
    {
        const std::string n = local_name(name);
        if (n == "basematerials" || n == "colorgroup") {
            const char *id = attr(atts, "id");
            group          = id ? id : "";
            groups[group];
        } else if (n == "base" && !group.empty()) {
            groups[group].push_back(normalize_colour(attr(atts, "displaycolor")));
        } else if (n == "color" && !group.empty()) {
            groups[group].push_back(normalize_colour(attr(atts, "color")));
        } else if (n == "object") {
            Obj o;
            const char *v;
            if ((v = attr(atts, "id")))
                o.id = v;
            if ((v = attr(atts, "name")))
                o.name = v;
            if ((v = attr(atts, "pid")))
                o.pid = v;
            if ((v = attr(atts, "pindex")))
                o.pindex = v;
            objects.push_back(o);
            in_object = true;
        } else if (n == "mesh" && in_object) {
            objects.back().has_mesh = true;
        } else if (n == "triangle" && in_object && !objects.back().has_triangle) {
            Obj &o        = objects.back();
            o.has_triangle = true;
            const char *v;
            if ((v = attr(atts, "pid")))
                o.tri_pid = v;
            if ((v = attr(atts, "p1")))
                o.tri_p1 = v;
        } else if (n == "component" && in_object) {
            if (const char *v = attr(atts, "objectid"))
                components.insert(v);
        } else if (n == "item") {
            if (const char *v = attr(atts, "objectid"))
                build_items.push_back(v);
        }
    }
    void end(const char *name)
    {
        const std::string n = local_name(name);
        if (n == "basematerials" || n == "colorgroup")
            group.clear();
        else if (n == "object")
            in_object = false;
    }
    std::string colour_of(const std::string &pid, const std::string &index) const
    {
        auto it = groups.find(pid);
        if (it == groups.end())
            return std::string();
        char       *end = nullptr;
        const long  i   = index.empty() ? 0 : std::strtol(index.c_str(), &end, 10);
        if (!index.empty() && (end == nullptr || *end != '\0'))
            return std::string();
        if (i < 0 || size_t(i) >= it->second.size())
            return std::string();
        return it->second[size_t(i)];
    }
};

void XMLCALL on_start(void *data, const char *name, const char **atts) { static_cast<ColourParser *>(data)->start(name, atts); }
void XMLCALL on_end(void *data, const char *name) { static_cast<ColourParser *>(data)->end(name); }

} // namespace

bool parse_3mf_body_colours(const std::string &model_xml, std::vector<BodyColour> &out, std::string *error)
{
    out.clear();
    if (!untrusted::xml_entry_size_ok(model_xml.size())) {
        if (error)
            *error = "the model is too large";
        return false;
    }
    ColourParser p;
    XML_Parser   parser = XML_ParserCreate(nullptr);
    if (parser == nullptr) {
        if (error)
            *error = "out of memory";
        return false;
    }
    XML_SetUserData(parser, &p);
    XML_SetElementHandler(parser, on_start, on_end);
    const bool ok = XML_Parse(parser, model_xml.data(), int(model_xml.size()), 1) != XML_STATUS_ERROR;
    if (!ok && error)
        *error = std::string("the 3MF model is not valid XML: ") + XML_ErrorString(XML_GetErrorCode(parser));
    XML_ParserFree(parser);
    if (!ok)
        return false;

    const std::set<std::string> in_build(p.build_items.begin(), p.build_items.end());
    for (const ColourParser::Obj &o : p.objects) {
        // A body of its own: in the build (or, with no build at all, any object that is not a
        // component of another) and not only a component.
        const bool listed = in_build.empty() ? p.components.count(o.id) == 0 : in_build.count(o.id) > 0;
        if (!listed)
            continue;
        BodyColour b;
        b.object_id = o.id;
        b.name      = o.name;
        if (!o.pid.empty())
            b.colour = p.colour_of(o.pid, o.pindex);
        if (b.colour.empty() && !o.tri_pid.empty())
            b.colour = p.colour_of(o.tri_pid, o.tri_p1);
        out.push_back(std::move(b));
    }
    return true;
}

bool read_3mf_body_colours(const fs::path &file, std::vector<BodyColour> &out, std::string *error)
{
    out.clear();
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!open_zip_reader(&zip, path_utf8(file))) {
        if (error)
            *error = "not a 3MF (zip) file";
        return false;
    }
    const mz_uint num   = mz_zip_reader_get_num_files(&zip);
    mz_uint       index = num;
    for (mz_uint i = 0; i < num; ++i) {
        mz_zip_archive_file_stat st;
        if (mz_zip_reader_file_stat(&zip, i, &st) && lower(st.m_filename) == "3d/3dmodel.model") {
            index = i;
            break;
        }
    }
    if (index == num) {
        close_zip_reader(&zip);
        if (error)
            *error = "the 3MF has no 3D/3dmodel.model";
        return false;
    }
    std::string xml, why;
    const bool  read = read_zip_entry_capped(zip, index, std::uint64_t(std::numeric_limits<int>::max()), xml, &why);
    close_zip_reader(&zip);
    if (!read) {
        if (error)
            *error = "the 3MF model could not be read: " + why;
        return false;
    }
    return parse_3mf_body_colours(xml, out, error);
}

std::vector<std::string> distinct_colours(const std::vector<BodyColour> &bodies)
{
    std::vector<std::string> out;
    for (const BodyColour &b : bodies)
        if (!b.colour.empty() && std::find(out.begin(), out.end(), b.colour) == out.end())
            out.push_back(b.colour);
    return out;
}

std::vector<int> map_colours_to_filaments(const std::vector<std::string> &colours, const std::vector<std::string> &existing,
                                          std::size_t max_filaments, std::vector<std::string> &added)
{
    added.clear();
    auto key = [](const std::string &c) {
        std::string k = lower(trim(c));
        if (k.size() == 9 && k[0] == '#')
            k = k.substr(0, 7); // alpha ignored
        return k;
    };
    auto rgb = [](const std::string &c, int &r, int &g, int &b) {
        const std::string s = c.size() >= 7 && c[0] == '#' ? c.substr(1, 6) : std::string();
        if (s.size() != 6)
            return false;
        char *end = nullptr;
        const long v = std::strtol(s.c_str(), &end, 16);
        if (end == nullptr || *end != '\0')
            return false;
        r = int((v >> 16) & 0xFF), g = int((v >> 8) & 0xFF), b = int(v & 0xFF);
        return true;
    };
    std::vector<std::string> palette;
    for (const std::string &c : existing)
        palette.push_back(key(c));
    std::vector<int> out;
    for (const std::string &c : colours) {
        const std::string k  = key(c);
        auto              it = std::find(palette.begin(), palette.end(), k);
        if (it != palette.end()) {
            out.push_back(int(it - palette.begin()) + 1);
            continue;
        }
        if (palette.size() < max_filaments) {
            palette.push_back(k);
            added.push_back(c);
            out.push_back(int(palette.size()));
            continue;
        }
        // No room: the nearest colour already in the palette.
        int r, g, b, best = 1;
        long best_d = -1;
        if (rgb(k, r, g, b)) {
            for (size_t i = 0; i < palette.size(); ++i) {
                int r2, g2, b2;
                if (!rgb(palette[i], r2, g2, b2))
                    continue;
                const long d = long(r - r2) * (r - r2) + long(g - g2) * (g - g2) + long(b - b2) * (b - b2);
                if (best_d < 0 || d < best_d)
                    best_d = d, best = int(i) + 1;
            }
        }
        out.push_back(palette.empty() ? 1 : best);
    }
    return out;
}

} // namespace layerling
} // namespace Slic3r
