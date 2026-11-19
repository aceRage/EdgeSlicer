#ifndef slic3r_VaultProvider_hpp_
#define slic3r_VaultProvider_hpp_

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

// The Home tab's Vaults: the user's own storage for model files (a WebDAV server or NAS, an Azure
// Blob container, an S3-compatible bucket). This is the engine: what a vault is (a JSON spec without
// secrets), how its files are listed and fetched, and the rules for addresses, SAS links and AWS
// signatures. Kept free of wx and of any HTTP library so it can be tested on its own with stand-in
// servers (tests/slic3rutils/vault_provider_tests.cpp); GUI/HomeVaults.cpp makes the HTTP calls and
// keeps the secrets in the OS credential store. Design: tests/design_vaults_home_tab.md.
//
// Secrets (passwords, tokens, SAS tokens, S3 keys) are never part of a spec: a spec names the slots
// it needs (secret_slots()); their values go into request headers, the query (SAS) or a signature
// only. Errors, listings and caches never carry them, nor a signed or SAS-carrying URL.
namespace Slic3r {
namespace Vaults {

enum class Kind { WebDav, AzureBlob, S3 };
std::string kind_name(Kind kind);                          // "webdav" "azure_blob" "s3"
bool        kind_from_name(const std::string& name, Kind& kind);

struct Spec
{
    std::string id;          // [a-z0-9-], made when the vault is created
    std::string name;        // shown to the user
    Kind        kind { Kind::WebDav };
    // webdav: the folder, "https://nas.local:5006/3d/"; azure_blob: the container,
    // "https://account.blob.core.windows.net/models"; s3: the endpoint, "https://s3.eu-west-1.amazonaws.com".
    std::string url;
    std::string prefix;      // azure_blob / s3: only keys under it ("models/"); "" = all
    std::string bucket;      // s3
    std::string region;      // s3: "us-east-1" when empty; "auto" for Cloudflare R2
    std::string addressing { "path" }; // s3: "path" (endpoint/bucket/key) or "virtual" (bucket.endpoint/key)
    // webdav: "none" "basic" "bearer"; azure_blob: "sas" "none"; s3: "sigv4" "none".
    std::string auth;
    bool        recursive { true };
    int         max_depth { 6 };   // folder levels below the vault's root, 0..20
    std::string builtin;           // the built-in template it was made from ("synology"), or ""
    std::string downloads_dir;     // where its downloads go; "" = the app's vault downloads folder
};

// A spec from JSON (app_config, or the vault editor). Throws std::runtime_error with a message for the
// user when it cannot be used: unknown format, a newer version, an unknown kind or sign-in, a bad
// address (address_check()), a missing bucket. Unknown keys are ignored. The JSON never holds a secret.
Spec           spec_from_json(const nlohmann::json& j);
nlohmann::json spec_to_json(const Spec& spec);
std::string    make_id(const std::string& name, uint32_t random);

// The secret values a spec needs: slot key -> what to ask the user for. Keys: "user", "password"
// (webdav basic), "token" (webdav bearer), "sas" (azure_blob), "access_key_id", "secret_access_key" (s3).
std::vector<std::pair<std::string, std::string>> secret_slots(const Spec& spec);
using Secrets = std::map<std::string, std::string>;

// ---- addresses -----------------------------------------------------------------------------------
// A host on the user's own network: localhost, a private, loopback, link-local or CGNAT (Tailscale)
// IPv4 address, an IPv6 loopback, link-local or unique-local address, or a .local / .lan / .home.arpa /
// .internal / single-label name.
bool is_lan_host(const std::string& host);
enum class Transport { Https, LanHttp, Refused };
// https to any host; plain http only to a LAN host (the user is warned before it is used); anything
// else (another scheme, user:password@, a query or fragment, http to a public host) is refused with
// `error` for the user.
Transport address_check(const std::string& url, std::string* error = nullptr);
// scheme://host[:port] in lower case, without a default port.
std::string origin_of(const std::string& url);
// What the page may show for an address: scheme://host[:port]/path, never a query.
std::string display_address(const std::string& url);

// ---- HTTP -------------------------------------------------------------------------------------------
struct Request
{
    std::string                                      method { "GET" };
    std::string                                      url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string                                      body;
    // Optional: a 2xx body goes here in pieces instead of into Response::body; false stops it.
    std::function<bool(const char* data, size_t size)> sink;
};
struct Response
{
    int                                status { 0 };   // 0 = no response
    std::map<std::string, std::string> headers;        // names lower-cased
    std::string                        body;
    std::string                        error;          // transport error
};
// Makes one request and must NOT follow redirects (fetch() does, per hop).
using HttpFn = std::function<Response(const Request&)>;

// Sends `req` through `http`, following up to `max_redirects` redirects (301 302 303 307 308). A hop to
// another origin than the request's own goes without the vault's credentials (the Authorization and
// x-amz-* headers are dropped; a SAS lives in the original URL only) and must pass address_check().
// 303 turns into a bare GET.
Response fetch(const HttpFn& http, Request req, int max_redirects = 5);

// ---- files and listings ----------------------------------------------------------------------------
struct File
{
    std::string id;       // the file's path in the vault, '/'-separated, decoded ("parts/arm v2.3mf")
    std::string name;     // its last segment
    std::string folder;   // the rest ("parts"), "" at the root
    int64_t     size { 0 };
    int64_t     mtime { 0 };  // seconds since 1970 (UTC), 0 when unknown
    std::string etag;
};
nlohmann::json file_to_json(const File& f);
File           file_from_json(const nlohmann::json& j);

// Model files a vault lists: the app's download types (.3mf .stl .step .stp .obj .zip).
bool is_listed_file(const std::string& name);

struct ListOptions
{
    size_t max_files { 50000 };
    int    max_requests { 2000 };
};
struct Listing
{
    bool              ok { false };
    bool              partial { false }; // stopped early (a limit, cancel): `files` is what came
    std::string       error;             // for the user; never a secret or a request URL
    std::vector<File> files;
    int               requests { 0 };
};

class Provider
{
public:
    virtual ~Provider() = default;
    // Every model file of the vault (below its root / prefix, as deep as the spec says).
    virtual Listing list(const Secrets& secrets, const HttpFn& http, const std::atomic<bool>& cancel, int64_t now,
                         const ListOptions& options = {}) const = 0;
    // The request that gives `file`'s bytes, with the credentials or the signature in it.
    virtual Request download_request(const Secrets& secrets, const File& file, int64_t now) const = 0;
};
std::unique_ptr<Provider> make_provider(const Spec& spec);

// One request's worth of listing, to check the address and the credentials.
struct TestResult
{
    bool        ok { false };
    std::string error;
    size_t      files { 0 };   // model files on the first page / in the root folder
};
TestResult test_connection(const Spec& spec, const Secrets& secrets, const HttpFn& http, int64_t now);

// The user's words for an HTTP failure ("The server refused the user name or password." ...).
std::string http_error(const Response& r);

// ---- Azure SAS links ---------------------------------------------------------------------------------
// "https://acct.blob.core.windows.net/models?sv=...&sig=..." -> base (without the query) and token (the
// query). A bare token ("sv=...&sig=..." or "?sv=...") gives an empty base. false when there is no token.
bool split_sas_url(const std::string& pasted, std::string& base, std::string& token);
struct SasCheck
{
    bool                     ok { false };
    std::string              error;     // why it cannot be used
    std::vector<std::string> warnings;  // usable, but the user should know
    int64_t                  expires_at { 0 }; // UTC seconds, 0 when the token has no expiry
};
// Refuses: no signature, an account SAS (ss/srt), a single-blob SAS (sr=b), no read or list permission,
// expired. Warns: write/delete/add/create/... permissions, expiry within 7 days.
SasCheck check_sas(const std::string& token, int64_t now);

// ---- AWS Signature Version 4 (exposed for the tests) --------------------------------------------------
std::string sha256_hex(const std::string& data);
std::string hmac_sha256(const std::string& key, const std::string& data); // raw 32 bytes
std::string amz_date(int64_t utc);                                          // "20130524T000000Z"
// RFC 3986 percent-encoding as SigV4 wants it; '/' kept when `keep_slash`.
std::string uri_encode(const std::string& s, bool keep_slash);
// Adds x-amz-date, x-amz-content-sha256 (of the empty body) and Authorization to `req`, signing the
// host, every header already on the request and those two.
void sign_v4(Request& req, const std::string& access_key, const std::string& secret_key, const std::string& region,
             const std::string& amz_date_value, const std::string& service = "s3");

// ---- parsers (exposed for the tests) ------------------------------------------------------------------
// HTTP-date ("Sun, 06 Nov 1994 08:49:37 GMT") or ISO 8601 ("2026-10-13T08:00:00Z", fractions allowed)
// -> UTC seconds; 0 when it is neither.
int64_t parse_time(const std::string& text);
// Percent-decoding of a URL path ("+" stays).
std::string percent_decode(const std::string& s);

} // namespace Vaults
} // namespace Slic3r

#endif
