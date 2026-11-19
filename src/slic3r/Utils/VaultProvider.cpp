#include "VaultProvider.hpp"

#include "libslic3r/UntrustedInput.hpp"

#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <set>
#include <stdexcept>

namespace Slic3r {
namespace Vaults {

using json = nlohmann::json;

namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return std::string();
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool starts_with(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }
bool ends_with(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0; }
bool iequals(const std::string& a, const char* b) { return lower(a) == lower(std::string(b)); }

std::string base64(const std::string& in)
{
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string        out;
    size_t             i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned n = (unsigned char) in[i] << 16 | (unsigned char) in[i + 1] << 8 | (unsigned char) in[i + 2];
        out += tbl[n >> 18 & 63]; out += tbl[n >> 12 & 63]; out += tbl[n >> 6 & 63]; out += tbl[n & 63];
    }
    if (i + 1 == in.size()) {
        const unsigned n = (unsigned char) in[i] << 16;
        out += tbl[n >> 18 & 63]; out += tbl[n >> 12 & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        const unsigned n = (unsigned char) in[i] << 16 | (unsigned char) in[i + 1] << 8;
        out += tbl[n >> 18 & 63]; out += tbl[n >> 12 & 63]; out += tbl[n >> 6 & 63]; out += '=';
    }
    return out;
}

std::string hex(const std::string& raw)
{
    static const char* h = "0123456789abcdef";
    std::string        out;
    for (unsigned char c : raw) {
        out += h[c >> 4];
        out += h[c & 15];
    }
    return out;
}

// ---- civil dates (Howard Hinnant's algorithms) ----
int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t  era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + int64_t(doe) - 719468;
}
void civil_from_days(int64_t z, int64_t& y, unsigned& m, unsigned& d)
{
    z += 719468;
    const int64_t  era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = unsigned(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y                  = int64_t(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp  = (5 * doy + 2) / 153;
    d                  = doy - (153 * mp + 2) / 5 + 1;
    m                  = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
}
std::string format_utc(int64_t t)
{
    int64_t  y;
    unsigned m, d;
    int64_t  days = t >= 0 ? t / 86400 : (t - 86399) / 86400;
    civil_from_days(days, y, m, d);
    const int64_t s = t - days * 86400;
    char          buf[40];
    std::snprintf(buf, sizeof(buf), "%04lld-%02u-%02u %02d:%02d UTC", (long long) y, m, d, int(s / 3600), int(s / 60 % 60));
    return buf;
}

// ---- a small XML reader: elements by local name (namespace prefixes dropped), their text ----
struct Xml
{
    std::string      name;
    std::string      text;
    std::vector<Xml> children;

    const Xml* child(const char* n) const
    {
        for (const Xml& c : children)
            if (iequals(c.name, n))
                return &c;
        return nullptr;
    }
    std::vector<const Xml*> all(const char* n) const
    {
        std::vector<const Xml*> out;
        for (const Xml& c : children)
            if (iequals(c.name, n))
                out.push_back(&c);
        return out;
    }
    std::string child_text(const char* n) const
    {
        const Xml* c = child(n);
        return c ? trim(c->text) : std::string();
    }
};

void append_utf8(std::string& out, unsigned long cp)
{
    if (cp < 0x80) out += char(cp);
    else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x110000) {
        out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F));
        out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F));
    }
}

std::string xml_decode(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        const size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) {
            out += s[i];
            continue;
        }
        const std::string e = s.substr(i + 1, semi - i - 1);
        if (e == "amp") out += '&';
        else if (e == "lt") out += '<';
        else if (e == "gt") out += '>';
        else if (e == "quot") out += '"';
        else if (e == "apos") out += '\'';
        else if (e.size() > 1 && e[0] == '#') {
            char* end = nullptr;
            const unsigned long cp = (e[1] == 'x' || e[1] == 'X') ? std::strtoul(e.c_str() + 2, &end, 16) : std::strtoul(e.c_str() + 1, &end, 10);
            if (end != nullptr && *end == '\0')
                append_utf8(out, cp);
        } else {
            out += s.substr(i, semi - i + 1); // an entity we do not know stays as it is (no DTD is read)
        }
        i = semi;
    }
    return out;
}

std::string local_name(const std::string& qname)
{
    const size_t colon = qname.rfind(':');
    return colon == std::string::npos ? qname : qname.substr(colon + 1);
}

bool parse_xml(const std::string& s, Xml& doc, std::string& error)
{
    doc = Xml();
    doc.name = "#document";
    std::vector<Xml*> stack { &doc };
    size_t            i = 0;
    const size_t      n = s.size();
    while (i < n) {
        if (s[i] != '<') {
            const size_t lt = s.find('<', i);
            const size_t end = lt == std::string::npos ? n : lt;
            if (stack.size() > 1)
                stack.back()->text += xml_decode(s.substr(i, end - i));
            i = end;
            continue;
        }
        if (s.compare(i, 4, "<!--") == 0) {
            const size_t e = s.find("-->", i + 4);
            if (e == std::string::npos) break;
            i = e + 3;
        } else if (s.compare(i, 9, "<![CDATA[") == 0) {
            const size_t e = s.find("]]>", i + 9);
            if (e == std::string::npos) break;
            if (stack.size() > 1)
                stack.back()->text += s.substr(i + 9, e - i - 9);
            i = e + 3;
        } else if (s.compare(i, 2, "<?") == 0 || s.compare(i, 2, "<!") == 0) {
            const size_t e = s.find('>', i + 2);
            if (e == std::string::npos) break;
            i = e + 1;
        } else if (s.compare(i, 2, "</") == 0) {
            const size_t e = s.find('>', i + 2);
            if (e == std::string::npos) break;
            const std::string name = local_name(trim(s.substr(i + 2, e - i - 2)));
            if (stack.size() < 2 || !iequals(stack.back()->name, name.c_str())) {
                error = "the answer is not well-formed XML";
                return false;
            }
            stack.pop_back();
            i = e + 1;
        } else {
            // A start tag; attributes are skipped (quoted values may hold '>').
            size_t j = i + 1;
            while (j < n && !std::isspace((unsigned char) s[j]) && s[j] != '>' && s[j] != '/')
                ++j;
            const std::string name = local_name(s.substr(i + 1, j - i - 1));
            char              quote = 0;
            while (j < n && (quote != 0 || s[j] != '>')) {
                if (quote != 0) {
                    if (s[j] == quote) quote = 0;
                } else if (s[j] == '"' || s[j] == '\'')
                    quote = s[j];
                ++j;
            }
            if (j >= n || name.empty()) break;
            const bool self_closing = s[j - 1] == '/';
            if (stack.size() > 64) {
                error = "the answer is nested too deeply";
                return false;
            }
            stack.back()->children.emplace_back();
            Xml& child = stack.back()->children.back();
            child.name = name;
            if (!self_closing)
                stack.push_back(&child);
            i = j + 1;
        }
    }
    if (stack.size() != 1 || doc.children.empty()) {
        error = "the answer is not well-formed XML";
        return false;
    }
    return true;
}

// The XML error code of an S3 / Azure answer ("SignatureDoesNotMatch", "AuthenticationFailed"), if any.
std::string error_code(const std::string& body)
{
    Xml         doc;
    std::string err;
    if (body.empty() || body.size() > 64 * 1024 || !parse_xml(body, doc, err))
        return std::string();
    const Xml* e = doc.child("Error");
    std::string code = e ? e->child_text("Code") : std::string();
    if (code.size() > 60) code.resize(60);
    for (char& c : code)
        if (!std::isalnum((unsigned char) c)) c = '_';
    return code;
}

int64_t to_int(const std::string& s)
{
    try {
        return s.empty() ? 0 : std::stoll(s);
    } catch (...) {
        return 0;
    }
}

const std::string EMPTY_SHA256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

// Folder of a '/'-separated path, and its last segment.
void split_path(const std::string& path, std::string& folder, std::string& name)
{
    const size_t slash = path.rfind('/');
    folder = slash == std::string::npos ? std::string() : path.substr(0, slash);
    name   = slash == std::string::npos ? path : path.substr(slash + 1);
}

bool has_dot_segment(const std::string& path)
{
    size_t pos = 0;
    while (pos <= path.size()) {
        const size_t slash = path.find('/', pos);
        const std::string seg = path.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        if (seg == "." || seg == "..")
            return true;
        if (slash == std::string::npos)
            break;
        pos = slash + 1;
    }
    return false;
}

// ---- WebDAV ----
const char* const PROPFIND_BODY =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<D:propfind xmlns:D=\"DAV:\"><D:prop><D:resourcetype/><D:getcontentlength/><D:getlastmodified/><D:getetag/></D:prop></D:propfind>";

class WebDavProvider : public Provider
{
public:
    explicit WebDavProvider(Spec spec) : m_spec(std::move(spec))
    {
        untrusted::Url u;
        if (untrusted::parse_url(m_spec.url, u)) {
            m_origin = origin_of(m_spec.url);
            m_root   = percent_decode(u.path.empty() ? std::string("/") : u.path);
            if (!ends_with(m_root, "/"))
                m_root += '/';
        }
    }

    void authorize(Request& r, const Secrets& s) const
    {
        auto get = [&s](const char* k) { auto it = s.find(k); return it == s.end() ? std::string() : it->second; };
        if (m_spec.auth == "basic")
            r.headers.emplace_back("Authorization", "Basic " + base64(get("user") + ":" + get("password")));
        else if (m_spec.auth == "bearer")
            r.headers.emplace_back("Authorization", "Bearer " + get("token"));
    }

    std::string missing(const Secrets& s) const
    {
        auto empty = [&s](const char* k) { auto it = s.find(k); return it == s.end() || it->second.empty(); };
        if (m_spec.auth == "basic" && (empty("user") || empty("password")))
            return "Set the user name and password first.";
        if (m_spec.auth == "bearer" && empty("token"))
            return "Set the token first.";
        return std::string();
    }

    Listing list(const Secrets& secrets, const HttpFn& http, const std::atomic<bool>& cancel, int64_t,
                 const ListOptions& options) const override
    {
        Listing out;
        if (m_origin.empty()) {
            out.error = "The vault's address is not valid.";
            return out;
        }
        if (std::string m = missing(secrets); !m.empty()) {
            out.error = m;
            return out;
        }
        std::vector<std::pair<std::string, int>> queue { {m_root, 0} };
        std::set<std::string>                    seen { m_root };
        for (size_t q = 0; q < queue.size(); ++q) {
            if (cancel) { out.partial = true; break; }
            if (out.requests >= options.max_requests) { out.partial = true; break; }
            const std::string folder = queue[q].first;
            const int         depth  = queue[q].second;
            Request req;
            req.method = "PROPFIND";
            req.url    = m_origin + uri_encode(folder, true);
            req.headers.emplace_back("Depth", "1");
            req.headers.emplace_back("Content-Type", "application/xml; charset=utf-8");
            req.body = PROPFIND_BODY;
            authorize(req, secrets);
            const Response r = fetch(http, req);
            ++out.requests;
            if (r.status != 207 && r.status != 200) {
                if (q == 0) {
                    out.error = http_error(r);
                    return out;
                }
                out.partial = true; // a folder we may not read: the rest is still listed
                continue;
            }
            Xml         doc;
            std::string err;
            if (!parse_xml(r.body, doc, err) || doc.child("multistatus") == nullptr) {
                if (q == 0) {
                    out.error = "The server's answer is not a WebDAV listing (" + err + ").";
                    return out;
                }
                out.partial = true;
                continue;
            }
            for (const Xml* resp : doc.child("multistatus")->all("response")) {
                std::string href = resp->child_text("href");
                if (starts_with(lower(href), "http://") || starts_with(lower(href), "https://")) {
                    if (origin_of(href) != m_origin)
                        continue; // another server: never followed
                    untrusted::Url hu;
                    if (!untrusted::parse_url(href, hu))
                        continue;
                    href = hu.path;
                }
                href = href.substr(0, href.find_first_of("?#"));
                const std::string path = percent_decode(href);
                if (!starts_with(path, m_root) || has_dot_segment(path))
                    continue; // outside the vault's folder
                std::string rel = path.substr(m_root.size());
                const bool  trailing = ends_with(rel, "/");
                while (ends_with(rel, "/")) rel.pop_back();
                const std::string folder_rel = folder.substr(m_root.size());
                std::string       self = folder_rel;
                while (ends_with(self, "/")) self.pop_back();
                if (rel == self)
                    continue; // the folder itself
                // The successful propstat (or the only one).
                const Xml* prop = nullptr;
                for (const Xml* ps : resp->all("propstat")) {
                    const std::string st = ps->child_text("status");
                    if (prop == nullptr || st.find(" 200") != std::string::npos)
                        prop = ps->child("prop");
                }
                const Xml* rt         = prop ? prop->child("resourcetype") : nullptr;
                const bool collection = (rt && rt->child("collection")) || (prop == nullptr && trailing);
                if (collection) {
                    if (m_spec.recursive && depth + 1 <= m_spec.max_depth) {
                        const std::string sub = m_root + rel + "/";
                        if (seen.insert(sub).second)
                            queue.emplace_back(sub, depth + 1);
                    }
                    continue;
                }
                File f;
                f.id = rel;
                split_path(rel, f.folder, f.name);
                if (!is_listed_file(f.name))
                    continue;
                if (prop) {
                    f.size  = to_int(prop->child_text("getcontentlength"));
                    f.mtime = parse_time(prop->child_text("getlastmodified"));
                    f.etag  = prop->child_text("getetag");
                }
                out.files.push_back(std::move(f));
                if (out.files.size() >= options.max_files) {
                    out.partial = true;
                    out.ok      = true;
                    return out;
                }
            }
        }
        out.ok = true;
        return out;
    }

    Request download_request(const Secrets& secrets, const File& file, int64_t) const override
    {
        Request r;
        r.url = m_origin + uri_encode(m_root + file.id, true);
        authorize(r, secrets);
        return r;
    }

private:
    Spec        m_spec;
    std::string m_origin; // scheme://host[:port]
    std::string m_root;   // the vault folder's decoded path, ending in '/'
};

// ---- Azure Blob ----
class AzureBlobProvider : public Provider
{
public:
    explicit AzureBlobProvider(Spec spec) : m_spec(std::move(spec))
    {
        m_container = m_spec.url;
        while (ends_with(m_container, "/")) m_container.pop_back();
        m_prefix = m_spec.prefix;
        while (starts_with(m_prefix, "/")) m_prefix.erase(0, 1);
    }

    std::string token(const Secrets& s) const
    {
        if (m_spec.auth != "sas")
            return std::string();
        auto it = s.find("sas");
        std::string t = it == s.end() ? std::string() : it->second;
        while (starts_with(t, "?")) t.erase(0, 1);
        return t;
    }

    Listing list(const Secrets& secrets, const HttpFn& http, const std::atomic<bool>& cancel, int64_t,
                 const ListOptions& options) const override
    {
        Listing           out;
        const std::string sas = token(secrets);
        if (m_spec.auth == "sas" && sas.empty()) {
            out.error = "Set the SAS link first.";
            return out;
        }
        std::string marker;
        for (;;) {
            if (cancel || out.requests >= options.max_requests) { out.partial = true; break; }
            std::string q = "restype=container&comp=list&maxresults=5000";
            if (!m_prefix.empty()) q += "&prefix=" + uri_encode(m_prefix, false);
            if (!m_spec.recursive) q += "&delimiter=%2F";
            if (!marker.empty()) q += "&marker=" + uri_encode(marker, false);
            if (!sas.empty()) q += "&" + sas;
            Request req;
            req.url = m_container + "?" + q;
            req.headers.emplace_back("x-ms-version", "2021-12-02");
            const Response r = fetch(http, req);
            ++out.requests;
            if (r.status != 200) {
                if (out.requests == 1) {
                    out.error = http_error(r);
                    return out;
                }
                out.partial = true;
                break;
            }
            Xml         doc;
            std::string err;
            const Xml*  res = parse_xml(r.body, doc, err) ? doc.child("EnumerationResults") : nullptr;
            if (res == nullptr) {
                out.error = "The answer is not an Azure blob listing.";
                return out;
            }
            if (const Xml* blobs = res->child("Blobs"))
                for (const Xml* b : blobs->all("Blob")) {
                    const std::string key = b->child_text("Name");
                    if (key.empty() || !starts_with(key, m_prefix) || has_dot_segment(key))
                        continue;
                    const std::string rel = key.substr(m_prefix.size());
                    if (m_spec.recursive && int(std::count(rel.begin(), rel.end(), '/')) > m_spec.max_depth)
                        continue;
                    File f;
                    f.id = key;
                    split_path(rel, f.folder, f.name);
                    while (starts_with(f.folder, "/")) f.folder.erase(0, 1);
                    if (!is_listed_file(f.name))
                        continue;
                    if (const Xml* p = b->child("Properties")) {
                        f.size  = to_int(p->child_text("Content-Length"));
                        f.mtime = parse_time(p->child_text("Last-Modified"));
                        f.etag  = p->child_text("Etag");
                    }
                    out.files.push_back(std::move(f));
                    if (out.files.size() >= options.max_files) {
                        out.partial = true;
                        out.ok      = true;
                        return out;
                    }
                }
            marker = res->child_text("NextMarker");
            if (marker.empty())
                break;
        }
        out.ok = true;
        return out;
    }

    Request download_request(const Secrets& secrets, const File& file, int64_t) const override
    {
        Request           r;
        const std::string sas = token(secrets);
        r.url = m_container + "/" + uri_encode(file.id, true) + (sas.empty() ? std::string() : "?" + sas);
        r.headers.emplace_back("x-ms-version", "2021-12-02");
        return r;
    }

private:
    Spec        m_spec;
    std::string m_container; // https://account.blob.core.windows.net/container
    std::string m_prefix;
};

// ---- S3-compatible ----
class S3Provider : public Provider
{
public:
    explicit S3Provider(Spec spec) : m_spec(std::move(spec))
    {
        untrusted::Url u;
        if (untrusted::parse_url(m_spec.url, u)) {
            m_scheme = u.scheme;
            m_host   = u.host;
            if (u.port > 0 && !((u.scheme == "https" && u.port == 443) || (u.scheme == "http" && u.port == 80)))
                m_port = ":" + std::to_string(u.port);
            m_base = u.path;
            while (ends_with(m_base, "/")) m_base.pop_back();
        }
        m_prefix = m_spec.prefix;
        while (starts_with(m_prefix, "/")) m_prefix.erase(0, 1);
        m_region = m_spec.region.empty() ? std::string("us-east-1") : m_spec.region;
    }

    // scheme://host[:port] and the path of the bucket's root ("/bucket" or "").
    std::string authority() const { return m_spec.addressing == "virtual" ? m_spec.bucket + "." + m_host + m_port : m_host + m_port; }
    std::string bucket_path() const { return m_base + (m_spec.addressing == "virtual" ? std::string() : "/" + m_spec.bucket); }

    std::string missing(const Secrets& s) const
    {
        auto empty = [&s](const char* k) { auto it = s.find(k); return it == s.end() || it->second.empty(); };
        if (m_spec.auth == "sigv4" && (empty("access_key_id") || empty("secret_access_key")))
            return "Set the access key ID and secret access key first.";
        return std::string();
    }

    void sign(Request& r, const Secrets& s, int64_t now) const
    {
        if (m_spec.auth != "sigv4")
            return;
        sign_v4(r, s.at("access_key_id"), s.at("secret_access_key"), m_region, amz_date(now));
    }

    Listing list(const Secrets& secrets, const HttpFn& http, const std::atomic<bool>& cancel, int64_t now,
                 const ListOptions& options) const override
    {
        Listing out;
        if (m_host.empty() || m_spec.bucket.empty()) {
            out.error = "The vault's address or bucket is not valid.";
            return out;
        }
        if (std::string m = missing(secrets); !m.empty()) {
            out.error = m;
            return out;
        }
        std::string token;
        for (;;) {
            if (cancel || out.requests >= options.max_requests) { out.partial = true; break; }
            std::map<std::string, std::string> params { {"list-type", "2"}, {"max-keys", "1000"} };
            if (!m_prefix.empty()) params["prefix"] = m_prefix;
            if (!m_spec.recursive) params["delimiter"] = "/";
            if (!token.empty()) params["continuation-token"] = token;
            std::string q;
            for (const auto& [k, v] : params)
                q += (q.empty() ? "" : "&") + uri_encode(k, false) + "=" + uri_encode(v, false);
            Request req;
            req.url = m_scheme + "://" + authority() + bucket_path() + "/?" + q;
            sign(req, secrets, now);
            const Response r = fetch(http, req);
            ++out.requests;
            if (r.status != 200) {
                if (out.requests == 1) {
                    out.error = http_error(r);
                    return out;
                }
                out.partial = true;
                break;
            }
            Xml         doc;
            std::string err;
            const Xml*  res = parse_xml(r.body, doc, err) ? doc.child("ListBucketResult") : nullptr;
            if (res == nullptr) {
                out.error = "The answer is not an S3 bucket listing.";
                return out;
            }
            for (const Xml* c : res->all("Contents")) {
                const std::string key = c->child_text("Key");
                if (key.empty() || !starts_with(key, m_prefix) || has_dot_segment(key))
                    continue;
                const std::string rel = key.substr(m_prefix.size());
                if (m_spec.recursive && int(std::count(rel.begin(), rel.end(), '/')) > m_spec.max_depth)
                    continue;
                File f;
                f.id = key;
                split_path(rel, f.folder, f.name);
                while (starts_with(f.folder, "/")) f.folder.erase(0, 1);
                if (!is_listed_file(f.name))
                    continue;
                f.size  = to_int(c->child_text("Size"));
                f.mtime = parse_time(c->child_text("LastModified"));
                f.etag  = c->child_text("ETag");
                out.files.push_back(std::move(f));
                if (out.files.size() >= options.max_files) {
                    out.partial = true;
                    out.ok      = true;
                    return out;
                }
            }
            token = res->child_text("NextContinuationToken");
            if (!iequals(res->child_text("IsTruncated"), "true") || token.empty())
                break;
        }
        out.ok = true;
        return out;
    }

    Request download_request(const Secrets& secrets, const File& file, int64_t now) const override
    {
        Request r;
        r.url = m_scheme + "://" + authority() + bucket_path() + "/" + uri_encode(file.id, true);
        if (missing(secrets).empty())
            sign(r, secrets, now);
        return r;
    }

private:
    Spec        m_spec;
    std::string m_scheme, m_host, m_port, m_base, m_prefix, m_region;
};

} // namespace

// ------------------------------------------------------------------------------------------- spec ----

std::string kind_name(Kind kind)
{
    switch (kind) {
    case Kind::WebDav: return "webdav";
    case Kind::AzureBlob: return "azure_blob";
    case Kind::S3: return "s3";
    }
    return "webdav";
}

bool kind_from_name(const std::string& name, Kind& kind)
{
    if (name == "webdav") kind = Kind::WebDav;
    else if (name == "azure_blob") kind = Kind::AzureBlob;
    else if (name == "s3") kind = Kind::S3;
    else return false;
    return true;
}

static std::string str(const json& j, const char* key)
{
    return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
}

static bool plain_id(const std::string& s, size_t max)
{
    if (s.size() > max)
        return false;
    for (unsigned char c : s)
        if (!(std::islower(c) || std::isdigit(c) || c == '-'))
            return false;
    return true;
}

Spec spec_from_json(const json& j)
{
    if (!j.is_object())
        throw std::runtime_error("A vault is a JSON object.");
    if (j.contains("format") && !(j["format"].is_string() && j["format"].get<std::string>() == "edgeslicer-vault"))
        throw std::runtime_error("That is not an EdgeSlicer vault.");
    if (j.contains("version") && j["version"].is_number() && j["version"].get<double>() > 1)
        throw std::runtime_error("That vault was made by a newer EdgeSlicer.");
    Spec s;
    s.id = str(j, "id");
    if (!plain_id(s.id, 64))
        throw std::runtime_error("The vault's id is not valid.");
    s.name = trim(str(j, "name"));
    if (s.name.empty())
        throw std::runtime_error("Give the vault a name.");
    if (s.name.size() > 80)
        s.name.resize(80);
    if (!kind_from_name(str(j, "kind"), s.kind))
        throw std::runtime_error("Unknown kind of vault: \"" + str(j, "kind").substr(0, 40) + "\".");
    s.url = trim(str(j, "url"));
    if (s.url.empty())
        throw std::runtime_error("Enter the vault's address.");
    if (s.kind == Kind::AzureBlob && s.url.find('?') != std::string::npos)
        throw std::runtime_error("Enter the container's address without the ?sv=... part. Paste the whole SAS link with Set SAS link "
                                 "on the vault's card: its token is kept in your system's credential store.");
    std::string why;
    if (address_check(s.url, &why) == Transport::Refused)
        throw std::runtime_error(why);
    if (s.kind == Kind::WebDav) {
        if (!ends_with(s.url, "/"))
            s.url += '/';
    } else
        while (ends_with(s.url, "/"))
            s.url.pop_back();
    if (s.kind == Kind::AzureBlob) {
        untrusted::Url u;
        if (!untrusted::parse_url(s.url, u) || u.path.size() < 2)
            throw std::runtime_error("Add the container's name to the address: https://<account>.blob.core.windows.net/<container>.");
    }
    s.prefix = trim(str(j, "prefix"));
    while (starts_with(s.prefix, "/")) s.prefix.erase(0, 1);
    if (s.prefix.size() > 512 || has_dot_segment(s.prefix))
        throw std::runtime_error("The prefix is not valid.");
    const std::string auth = lower(trim(str(j, "auth")));
    switch (s.kind) {
    case Kind::WebDav: s.auth = auth.empty() ? "basic" : auth; if (s.auth != "none" && s.auth != "basic" && s.auth != "bearer") throw std::runtime_error("Unknown sign-in for WebDAV."); break;
    case Kind::AzureBlob: s.auth = auth.empty() ? "sas" : auth; if (s.auth != "none" && s.auth != "sas") throw std::runtime_error("Unknown sign-in for Azure Blob."); break;
    case Kind::S3: s.auth = auth.empty() ? "sigv4" : auth; if (s.auth != "none" && s.auth != "sigv4") throw std::runtime_error("Unknown sign-in for S3."); break;
    }
    if (s.kind == Kind::S3) {
        s.bucket = lower(trim(str(j, "bucket")));
        if (s.bucket.size() < 3 || s.bucket.size() > 63)
            throw std::runtime_error("Enter the bucket's name (3 to 63 characters).");
        for (unsigned char c : s.bucket)
            if (!(std::islower(c) || std::isdigit(c) || c == '-' || c == '.' || c == '_'))
                throw std::runtime_error("The bucket's name may only have letters, digits, '-', '_' and '.'.");
        s.region = lower(trim(str(j, "region")));
        if (!plain_id(s.region, 40))
            throw std::runtime_error("The region is not valid (for example us-east-1, or auto for Cloudflare R2).");
        s.addressing = str(j, "addressing") == "virtual" ? "virtual" : "path";
    }
    s.recursive = !(j.contains("recursive") && j["recursive"].is_boolean() && !j["recursive"].get<bool>());
    if (j.contains("max_depth") && j["max_depth"].is_number())
        s.max_depth = std::max(0, std::min(20, j["max_depth"].get<int>()));
    s.builtin = str(j, "builtin");
    if (!plain_id(s.builtin, 32))
        s.builtin.clear();
    s.downloads_dir = trim(str(j, "downloads_dir"));
    if (s.downloads_dir.size() > 1024)
        throw std::runtime_error("The downloads folder's path is too long.");
    return s;
}

json spec_to_json(const Spec& s)
{
    json j = {{"format", "edgeslicer-vault"}, {"version", 1}, {"id", s.id}, {"name", s.name}, {"kind", kind_name(s.kind)},
              {"url", s.url}, {"auth", s.auth}, {"recursive", s.recursive}, {"max_depth", s.max_depth}, {"builtin", s.builtin},
              {"downloads_dir", s.downloads_dir}};
    if (s.kind != Kind::WebDav)
        j["prefix"] = s.prefix;
    if (s.kind == Kind::S3) {
        j["bucket"]     = s.bucket;
        j["region"]     = s.region;
        j["addressing"] = s.addressing;
    }
    return j;
}

std::string make_id(const std::string& name, uint32_t random)
{
    std::string id;
    for (unsigned char c : name) {
        if (std::isalnum(c))
            id += char(std::tolower(c));
        else if (!id.empty() && id.back() != '-')
            id += '-';
        if (id.size() >= 24)
            break;
    }
    while (!id.empty() && id.back() == '-')
        id.pop_back();
    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), "%08x", random);
    return (id.empty() ? std::string("vault") : id) + "-" + suffix;
}

std::vector<std::pair<std::string, std::string>> secret_slots(const Spec& s)
{
    std::vector<std::pair<std::string, std::string>> out;
    switch (s.kind) {
    case Kind::WebDav:
        if (s.auth == "basic") {
            out.emplace_back("user", "User name");
            out.emplace_back("password", "Password");
        } else if (s.auth == "bearer")
            out.emplace_back("token", "Token");
        break;
    case Kind::AzureBlob:
        if (s.auth == "sas")
            out.emplace_back("sas", "SAS link");
        break;
    case Kind::S3:
        if (s.auth == "sigv4") {
            out.emplace_back("access_key_id", "Access key ID");
            out.emplace_back("secret_access_key", "Secret access key");
        }
        break;
    }
    return out;
}

// ------------------------------------------------------------------------------------ addresses ----

bool is_lan_host(const std::string& host_in)
{
    std::string host = lower(trim(host_in));
    if (host.empty())
        return false;
    if (host.front() == '[' && host.back() == ']')
        host = host.substr(1, host.size() - 2);
    if (host.find(':') != std::string::npos) { // IPv6
        if (host == "::1")
            return true;
        return starts_with(host, "fe8") || starts_with(host, "fe9") || starts_with(host, "fea") || starts_with(host, "feb") ||
               starts_with(host, "fc") || starts_with(host, "fd");
    }
    bool numeric = true;
    for (char c : host)
        if (!(std::isdigit((unsigned char) c) || c == '.'))
            numeric = false;
    if (numeric) {
        unsigned a[4];
        char     tail;
        if (std::sscanf(host.c_str(), "%u.%u.%u.%u%c", &a[0], &a[1], &a[2], &a[3], &tail) != 4)
            return false;
        for (unsigned v : a)
            if (v > 255)
                return false;
        return a[0] == 10 || a[0] == 127 || (a[0] == 169 && a[1] == 254) || (a[0] == 172 && a[1] >= 16 && a[1] <= 31) ||
               (a[0] == 192 && a[1] == 168) || (a[0] == 100 && a[1] >= 64 && a[1] <= 127);
    }
    if (host == "localhost" || host.find('.') == std::string::npos)
        return true;
    for (const char* suffix : {".localhost", ".local", ".lan", ".home.arpa", ".internal"})
        if (ends_with(host, suffix))
            return true;
    return false;
}

Transport address_check(const std::string& url, std::string* error)
{
    auto refuse = [error](const std::string& why) {
        if (error) *error = why;
        return Transport::Refused;
    };
    untrusted::Url u;
    if (!untrusted::parse_url(url, u) || u.host.empty())
        return refuse("The address is not valid. It looks like https://nas.example.com:5006/models/.");
    if (u.has_userinfo)
        return refuse("Put the user name and password in the vault's credentials, not in the address.");
    if (!u.query.empty() || url.find('?') != std::string::npos || url.find('#') != std::string::npos)
        return refuse("The address must not have a ?query or #fragment.");
    if (u.scheme == "https")
        return Transport::Https;
    if (u.scheme == "http") {
        if (is_lan_host(u.host))
            return Transport::LanHttp;
        return refuse("Plain http is only allowed for a device on your local network. Use https for " + u.host + ".");
    }
    return refuse("The address must start with https:// (or http:// for a device on your local network).");
}

std::string origin_of(const std::string& url)
{
    untrusted::Url u;
    if (!untrusted::parse_url(url, u) || u.host.empty())
        return std::string();
    std::string o = u.scheme + "://" + u.host;
    if (u.port > 0 && !((u.scheme == "https" && u.port == 443) || (u.scheme == "http" && u.port == 80)))
        o += ":" + std::to_string(u.port);
    return o;
}

std::string display_address(const std::string& url)
{
    untrusted::Url u;
    if (!untrusted::parse_url(url, u))
        return std::string();
    return origin_of(url) + u.path;
}

// ----------------------------------------------------------------------------------------- HTTP ----

Response fetch(const HttpFn& http, Request req, int max_redirects)
{
    const std::string origin = origin_of(req.url);
    for (int hop = 0;; ++hop) {
        Response r = http(req);
        if (!(r.status == 301 || r.status == 302 || r.status == 303 || r.status == 307 || r.status == 308))
            return r;
        auto loc = r.headers.find("location");
        if (loc == r.headers.end() || loc->second.empty())
            return r;
        if (hop >= max_redirects) {
            Response e;
            e.error = "too many redirects";
            return e;
        }
        std::string next = trim(loc->second);
        if (starts_with(next, "//"))
            next = req.url.substr(0, req.url.find("://") + 1) + next;
        else if (starts_with(next, "/"))
            next = origin_of(req.url) + next;
        else if (!starts_with(lower(next), "http://") && !starts_with(lower(next), "https://")) {
            const size_t q = req.url.find('?');
            const std::string base = req.url.substr(0, q);
            next = base.substr(0, base.rfind('/') + 1) + next;
        }
        // A redirect may carry a query (a signed link); only the rest of the address is checked.
        if (address_check(next.substr(0, next.find_first_of("?#")), nullptr) == Transport::Refused) {
            Response e;
            e.error = "redirected to an address that is not allowed";
            return e;
        }
        if (origin_of(next) != origin) {
            auto& h = req.headers;
            h.erase(std::remove_if(h.begin(), h.end(), [](const auto& kv) {
                        const std::string n = lower(kv.first);
                        return n == "authorization" || starts_with(n, "x-amz-") || starts_with(n, "x-ms-");
                    }),
                    h.end());
        }
        if (r.status == 303) {
            req.method = "GET";
            req.body.clear();
        }
        req.url = next;
    }
}

std::string http_error(const Response& r)
{
    if (r.status == 0)
        return "Could not connect: " + r.error.substr(0, 200);
    const std::string code   = error_code(r.body);
    const std::string detail = code.empty() ? std::string() : ", " + code;
    switch (r.status) {
    case 401: return "The server refused the sign-in (HTTP 401" + detail + "). Check the credentials.";
    case 403: return "Access denied (HTTP 403" + detail + "). Check the credentials and what they are allowed to read.";
    case 404: return "Not found (HTTP 404" + detail + "). Check the address, container or bucket.";
    case 405: return "The server does not offer WebDAV at this address (HTTP 405).";
    default: return "The server answered HTTP " + std::to_string(r.status) + detail + ".";
    }
}

// ---------------------------------------------------------------------------------------- files ----

json file_to_json(const File& f)
{
    return {{"id", f.id}, {"name", f.name}, {"folder", f.folder}, {"size", f.size}, {"mtime", f.mtime}, {"etag", f.etag}};
}

File file_from_json(const json& j)
{
    File f;
    if (!j.is_object())
        return f;
    f.id     = str(j, "id");
    f.name   = str(j, "name");
    f.folder = str(j, "folder");
    f.etag   = str(j, "etag");
    if (j.contains("size") && j["size"].is_number_integer()) f.size = j["size"].get<int64_t>();
    if (j.contains("mtime") && j["mtime"].is_number_integer()) f.mtime = j["mtime"].get<int64_t>();
    return f;
}

bool is_listed_file(const std::string& name) { return untrusted::has_model_extension(name); }

std::unique_ptr<Provider> make_provider(const Spec& spec)
{
    switch (spec.kind) {
    case Kind::WebDav: return std::make_unique<WebDavProvider>(spec);
    case Kind::AzureBlob: return std::make_unique<AzureBlobProvider>(spec);
    case Kind::S3: return std::make_unique<S3Provider>(spec);
    }
    return nullptr;
}

TestResult test_connection(const Spec& spec, const Secrets& secrets, const HttpFn& http, int64_t now)
{
    TestResult        t;
    std::atomic<bool> cancel { false };
    ListOptions       o;
    o.max_requests = 1;
    const Listing l = make_provider(spec)->list(secrets, http, cancel, now, o);
    t.ok    = l.ok;
    t.error = l.error;
    t.files = l.files.size();
    return t;
}

// ------------------------------------------------------------------------------------------ SAS ----

bool split_sas_url(const std::string& pasted, std::string& base, std::string& token)
{
    const std::string s = trim(pasted);
    base.clear();
    token.clear();
    const std::string l = lower(s);
    if (starts_with(l, "https://") || starts_with(l, "http://")) {
        const size_t q = s.find('?');
        if (q == std::string::npos)
            return false;
        base  = s.substr(0, q);
        token = s.substr(q + 1);
        while (ends_with(base, "/"))
            base.pop_back();
    } else
        token = starts_with(s, "?") ? s.substr(1) : s;
    token = token.substr(0, token.find('#'));
    return !token.empty() && token.find('=') != std::string::npos;
}

SasCheck check_sas(const std::string& token, int64_t now)
{
    SasCheck                           c;
    std::map<std::string, std::string> p;
    size_t                             pos = 0;
    while (pos <= token.size()) {
        const size_t amp = token.find('&', pos);
        const std::string kv = token.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        const size_t eq = kv.find('=');
        if (eq != std::string::npos)
            p[lower(kv.substr(0, eq))] = percent_decode(kv.substr(eq + 1));
        if (amp == std::string::npos)
            break;
        pos = amp + 1;
    }
    if (p["sig"].empty()) {
        c.error = "This is not a SAS link: it has no signature (sig=).";
        return c;
    }
    if (p.count("ss") || p.count("srt")) {
        c.error = "This is an account SAS: it opens every container of the storage account. Make a SAS for this one container "
                  "with Read and List.";
        return c;
    }
    const std::string sr = p["sr"];
    if (sr == "b" || sr == "bs" || sr == "bv") {
        c.error = "This SAS is for one file, not a container. Make a SAS for the container with Read and List.";
        return c;
    }
    const std::string sp = p["sp"];
    if (sp.find('r') == std::string::npos || sp.find('l') == std::string::npos) {
        c.error = "This SAS needs the Read and List permissions (it has \"" + sp.substr(0, 20) + "\").";
        return c;
    }
    if (sp.find_first_of("wdacxytmeopi") != std::string::npos)
        c.warnings.push_back("This SAS can also change or delete files (permissions \"" + sp.substr(0, 20) +
                             "\"). EdgeSlicer only needs Read and List: a read-only SAS is safer.");
    if (p.count("se")) {
        c.expires_at = parse_time(p["se"]);
        if (c.expires_at == 0)
            c.warnings.push_back("The SAS's expiry date could not be read.");
        else if (c.expires_at <= now) {
            c.error = "This SAS expired on " + format_utc(c.expires_at) + ".";
            return c;
        } else if (c.expires_at - now < 7 * 86400)
            c.warnings.push_back("This SAS expires on " + format_utc(c.expires_at) + ".");
    }
    c.ok = true;
    return c;
}

// ---------------------------------------------------------------------------------------- SigV4 ----

std::string sha256_hex(const std::string& data)
{
    unsigned char out[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), out);
    return hex(std::string(reinterpret_cast<const char*>(out), sizeof(out)));
}

std::string hmac_sha256(const std::string& key, const std::string& data)
{
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int  len = 0;
    HMAC(EVP_sha256(), key.data(), int(key.size()), reinterpret_cast<const unsigned char*>(data.data()), data.size(), out, &len);
    return std::string(reinterpret_cast<const char*>(out), len);
}

std::string amz_date(int64_t utc)
{
    int64_t  y;
    unsigned m, d;
    const int64_t days = utc >= 0 ? utc / 86400 : (utc - 86399) / 86400;
    civil_from_days(days, y, m, d);
    const int64_t s = utc - days * 86400;
    char          buf[24];
    std::snprintf(buf, sizeof(buf), "%04lld%02u%02uT%02d%02d%02dZ", (long long) y, m, d, int(s / 3600), int(s / 60 % 60), int(s % 60));
    return buf;
}

std::string uri_encode(const std::string& s, bool keep_slash)
{
    static const char* h = "0123456789ABCDEF";
    std::string        out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || (keep_slash && c == '/'))
            out += char(c);
        else {
            out += '%';
            out += h[c >> 4];
            out += h[c & 15];
        }
    }
    return out;
}

void sign_v4(Request& req, const std::string& access_key, const std::string& secret_key, const std::string& region,
             const std::string& amz_date_value, const std::string& service)
{
    untrusted::Url u;
    if (!untrusted::parse_url(req.url, u))
        return;
    std::string host = u.host;
    if (u.port > 0 && !((u.scheme == "https" && u.port == 443) || (u.scheme == "http" && u.port == 80)))
        host += ":" + std::to_string(u.port);
    req.headers.emplace_back("x-amz-date", amz_date_value);
    req.headers.emplace_back("x-amz-content-sha256", EMPTY_SHA256);

    std::map<std::string, std::string> headers { {"host", host} };
    for (const auto& [k, v] : req.headers)
        headers[lower(k)] = trim(v);
    std::string canonical_headers, signed_headers;
    for (const auto& [k, v] : headers) {
        canonical_headers += k + ":" + v + "\n";
        signed_headers += (signed_headers.empty() ? "" : ";") + k;
    }
    // The query, already encoded by the caller, sorted by name then value.
    std::vector<std::pair<std::string, std::string>> params;
    size_t                                           pos = 0;
    while (!u.query.empty() && pos <= u.query.size()) {
        const size_t amp = u.query.find('&', pos);
        const std::string kv = u.query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        const size_t eq = kv.find('=');
        params.emplace_back(eq == std::string::npos ? kv : kv.substr(0, eq), eq == std::string::npos ? std::string() : kv.substr(eq + 1));
        if (amp == std::string::npos)
            break;
        pos = amp + 1;
    }
    std::sort(params.begin(), params.end());
    std::string query;
    for (const auto& [k, v] : params)
        query += (query.empty() ? "" : "&") + k + "=" + v;

    const std::string creq = req.method + "\n" + (u.path.empty() ? std::string("/") : u.path) + "\n" + query + "\n" + canonical_headers +
                             "\n" + signed_headers + "\n" + EMPTY_SHA256;
    const std::string day   = amz_date_value.substr(0, 8);
    const std::string scope = day + "/" + region + "/" + service + "/aws4_request";
    const std::string sts   = "AWS4-HMAC-SHA256\n" + amz_date_value + "\n" + scope + "\n" + sha256_hex(creq);
    std::string       k     = hmac_sha256("AWS4" + secret_key, day);
    k                       = hmac_sha256(k, region);
    k                       = hmac_sha256(k, service);
    k                       = hmac_sha256(k, "aws4_request");
    const std::string sig   = hex(hmac_sha256(k, sts));
    req.headers.emplace_back("Authorization", "AWS4-HMAC-SHA256 Credential=" + access_key + "/" + scope + ",SignedHeaders=" + signed_headers +
                                                  ",Signature=" + sig);
}

// --------------------------------------------------------------------------------------- parsers ----

int64_t parse_time(const std::string& text_in)
{
    const std::string text = trim(text_in);
    int  y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    char c1 = 0;
    // ISO 8601: 2026-10-13, 2026-10-13T08:00Z, 2026-10-13T08:00:00.123Z
    if (text.size() >= 10 && std::sscanf(text.c_str(), "%4d-%2d-%2d", &y, &mo, &d) == 3) {
        if (text.size() > 10 && (text[10] == 'T' || text[10] == ' ')) {
            const int got = std::sscanf(text.c_str() + 11, "%2d:%2d%c%2d", &h, &mi, &c1, &s);
            if (got < 2)
                return 0;
            if (got < 4 || c1 != ':')
                s = 0;
        }
    } else {
        // HTTP-date: Sun, 06 Nov 1994 08:49:37 GMT
        char mon[4] = {0};
        if (std::sscanf(text.c_str(), "%*3s, %2d %3s %4d %2d:%2d:%2d", &d, mon, &y, &h, &mi, &s) != 6)
            return 0;
        static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        for (int i = 0; i < 12; ++i)
            if (iequals(mon, months[i]))
                mo = i + 1;
    }
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60)
        return 0;
    return days_from_civil(y, unsigned(mo), unsigned(d)) * 86400 + h * 3600 + mi * 60 + s;
}

std::string percent_decode(const std::string& s) { return untrusted::percent_decode(s); }

} // namespace Vaults
} // namespace Slic3r
