// Native app push (see AppPush.hpp). Hub process only; wx-free, like the rest of the hub server.
//
// This file owns the device store, the settings, the routes and the fan-out. The two platform
// contracts live next door in ApnsProvider.cpp and FcmProvider.cpp, behind the Provider seam, and
// the third implementation - the EdgeSlicer push service, for hubs without their own keys - is
// HostedProvider.cpp. Which one a device's notifications take is the "mode": "own" (this hub's own
// APNs .p8 / FCM service account) or "hosted"; with no mode saved, a hub that has own keys set up
// keeps using them and every other hub uses the hosted service.
//
// The crypto is not here either: the payload is encrypted by WebPush::encrypt, unchanged. An app
// that generates a P-256 key pair and a 16-byte auth secret on first launch and registers the
// public half is, as far as this code is concerned, a browser PushSubscription without an
// endpoint - so the same RFC 8291 call serves both and there is one implementation to get right.
#include "AppPush.hpp"

#include "RemoteEvents.hpp"

#include "AppPushProvider.hpp"
#include "HostedPush.hpp"
#include "LiveActivityPush.hpp"
#include "PushIds.hpp"
#include "WebPush.hpp"
#include "slic3r/Utils/Http.hpp"

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <set>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace Slic3r {
namespace GUI {
namespace AppPush {

using json = nlohmann::json;

static const size_t MAX_DEVICES   = 16;    // a phone per person, not a phone per launch
static const int    MAX_TRIES     = 3;     // one send plus two retries, like every other sender here
static const size_t MAX_PLAINTEXT = 2800;  // well inside APNs' 4096-byte and FCM's 4096-byte caps
static const size_t MASK_LEN      = 4;
static const char*  MASK          = "****";

// A "finished" that arrives two days later is noise, not news; a "started" is stale even sooner.
// A failure is the exception: a phone that was off or out of coverage when the print failed at
// 03:10 should still be told at 04:00 (research note H3). The push service accepts up to 86400.
static const int TTL_ALERT   = 1800;
static const int TTL_ROUTINE = 300;
static const int TTL_FAILURE = 14400;

// The phone-plane test route (test_device): one at a time, and not more often than this.
static const long long TEST_MIN_GAP_MS = 3000;
static const int       TEST_MAX_DELAY_S = 30;

// Hub-driven Live Activity updates for one phone (LiveActivityPush.hpp): off unless it asked.
struct LaPrefs
{
    bool        enabled { false };
    std::string start_token; // iOS 17.2+: the app's push-to-start token (hex)
    bool        frequent { false }; // ActivityAuthorizationInfo.frequentPushesEnabled, for the page
};

struct Device
{
    std::string id, platform, env, token, bundle, p256dh, auth, label, app, os;
    long long   added { 0 };
    long long   last_sent { 0 };
    int         last_status { 0 };
    int         failures { 0 };
    std::string last_error, last_host;
    // What this device asked for when it registered (notification levels; see AppPush.hpp).
    policy::DevicePrefs prefs;
    LaPrefs     la;
    // Per printer id: its activity's token and what it was last told.
    std::map<std::string, LiveActivity::Track> la_tracks;
};

static std::mutex           g_mutex;
static std::vector<Device>  g_devices;
static bool                 g_enabled { true };
static std::string          g_min_severity { "info" };
// Which event kinds the registered devices want; empty is every kind. One list for the channel,
// like Web Push - the devices belong to the same person and a per-device filter is a screen
// nobody has asked for.
static std::vector<std::string> g_kinds;
static json                 g_apns_cfg = json::object();
static json                 g_fcm_cfg  = json::object();
// "own", "hosted", or "" = decide from the settings (own when own keys are set up, else hosted).
static std::string          g_mode;
static json                 g_hosted_cfg = json::object(); // {"url": ...}; no url = the default service
static std::atomic<bool>    g_stopping { false };
static std::atomic<bool>    g_dirty { false };
static std::unique_ptr<Provider> g_apns, g_fcm;
static std::unique_ptr<Hosted::HostedProvider> g_hosted;
static Hosted::Identity     g_identity; // this hub's Ed25519 identity, handed over by RemoteHub
// The per-hub secret behind every cleartext id next to a push (PushIds.hpp): the thread id and the
// collapse id. Kept in settings.json with the device rows; minted the first time a hub starts.
static std::string          g_id_key;
// Hub-driven Live Activity updates, for the phones that turned them on. The hub page's switch.
static bool                 g_la_enabled { true };

// ------------------------------------------------------------------ small helpers ----

static long long now_ms()
{
    return (long long) std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

static std::string random_id()
{
    static const char* hx = "0123456789abcdef";
    unsigned char      b[8];
    if (RAND_bytes(b, sizeof(b)) != 1) return std::to_string(now_ms());
    std::string s = "d_";
    for (unsigned char c : b) { s += hx[c >> 4]; s += hx[c & 15]; }
    return s;
}

static std::string trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

// RemoteNotify's rule, and for the same reason: "****" plus the last four characters is enough to
// tell two of a person's phones apart and not enough to be worth stealing.
static std::string mask(const std::string& secret)
{
    if (secret.empty()) return "";
    if (secret.size() <= MASK_LEN) return MASK;
    return std::string(MASK) + secret.substr(secret.size() - MASK_LEN);
}

static bool is_masked(const std::string& v) { return v.compare(0, MASK_LEN, MASK) == 0; }

// A credential the page sent back: absent or masked means "keep the stored one", so a credential
// only ever travels inward. RemoteNotify::take_secret, restated for this module's own settings.
static void take_secret(const json& j, const char* key, std::string& out)
{
    if (!j.is_object() || !j.contains(key) || !j[key].is_string()) return;
    const std::string v = trim(j[key].get<std::string>());
    if (is_masked(v)) return;
    out = v;
}

static void take_string(const json& j, const char* key, std::string& out)
{
    if (j.is_object() && j.contains(key) && j[key].is_string()) out = trim(j[key].get<std::string>());
}

static void take_bool(const json& j, const char* key, bool& out)
{
    if (j.is_object() && j.contains(key) && j[key].is_boolean()) out = j[key].get<bool>();
}

// An ActivityKit push token (an activity's own, or the app's push-to-start token): hex, like an
// APNs device token but longer.
static bool la_token_ok(const std::string& t)
{
    if (t.size() < 64 || t.size() > 400 || t.size() % 2) return false;
    for (unsigned char c : t)
        if (!std::isxdigit(c)) return false;
    return true;
}

// A path is not a secret, but it names a person's home directory and the key's file name, and the
// hub page has no use for either. The basename alone is enough to say "yes, that is the file".
static const char* const PATH_ELLIPSIS = "\xE2\x80\xA6";

static std::string mask_path(const std::string& p)
{
    if (p.empty()) return "";
    const size_t slash = p.find_last_of("/\\");
    return slash == std::string::npos ? p : (std::string(PATH_ELLIPSIS) + p.substr(slash + 1));
}

// A path field posted back. take_secret's rule plus one more: a masked path is shown as its
// basename behind an ellipsis rather than as ****, so a client that echoed back what it was shown
// must not be able to overwrite the real path with that display form.
static void take_path(const json& j, const char* key, std::string& out)
{
    if (!j.is_object() || !j.contains(key) || !j[key].is_string()) return;
    const std::string v = trim(j[key].get<std::string>());
    if (is_masked(v) || v.compare(0, std::strlen(PATH_ELLIPSIS), PATH_ELLIPSIS) == 0) return;
    out = v;
}

static std::string header_safe(const std::string& s, size_t limit = 200)
{
    std::string out;
    for (unsigned char c : s) {
        if (c == '\r' || c == '\n' || c == '\t') { if (!out.empty() && out.back() != ' ') out += ' '; }
        else if (c < 0x20 || c >= 0x7f) out += '?';
        else out += (char) c;
        if (out.size() >= limit) break;
    }
    return trim(out);
}

// libcurl's error text carries the URL it failed on, and for APNs the URL *is* the device token.
// Nothing that reaches last_error or the log may contain one.
//
// Two entry points because most callers are already inside the lock (they are recording a result
// against a row) and std::mutex is not recursive - taking it twice would deadlock the sender.
static std::string scrub_locked(std::string text, const Device& d)
{
    const std::string key_path = g_apns_cfg.value("key_path", "");
    const std::string sa_path  = g_fcm_cfg.value("service_account_path", "");
    for (const std::string& secret : { d.token, d.p256dh, d.auth, d.bundle, key_path, sa_path }) {
        if (secret.size() < 6) continue;
        for (size_t at = text.find(secret); at != std::string::npos; at = text.find(secret, at + 3))
            text.replace(at, secret.size(), "***");
    }
    if (text.size() > 300) text.resize(300);
    return text;
}

static std::string scrub(std::string text, const Device& d)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return scrub_locked(std::move(text), d);
}

static int severity_rank(const std::string& s)
{
    if (s == "error") return 2;
    if (s == "warning") return 1;
    return 0;
}

static std::string ev_str(const json& e, const char* key, const std::string& fallback = "")
{
    if (e.is_object() && e.contains(key) && e[key].is_string()) return e[key].get<std::string>();
    return fallback;
}

// ------------------------------------------------------- shared provider helpers ----

namespace detail {

static const char* const B64URL = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string b64url(const unsigned char* data, size_t len)
{
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        const unsigned a = data[i];
        const unsigned b = i + 1 < len ? data[i + 1] : 0;
        const unsigned c = i + 2 < len ? data[i + 2] : 0;
        const unsigned v = (a << 16) | (b << 8) | c;
        out += B64URL[(v >> 18) & 63];
        out += B64URL[(v >> 12) & 63];
        if (i + 1 < len) out += B64URL[(v >> 6) & 63];
        if (i + 2 < len) out += B64URL[v & 63];
    }
    return out;
}

std::string b64url(const std::string& s) { return b64url((const unsigned char*) s.data(), s.size()); }

void* load_pkcs8_pem(const std::string& pem, std::string& err)
{
    err.clear();
    if (pem.empty()) { err = "no key"; return nullptr; }
    BIO* bio = BIO_new_mem_buf(pem.data(), (int) pem.size());
    if (!bio) { err = "out of memory"; return nullptr; }
    // Apple's .p8 and the FCM service account's private_key are both unencrypted PKCS#8 PEM
    // ("-----BEGIN PRIVATE KEY-----"), so no password callback is wanted: if a key turns out to
    // be encrypted this must fail rather than block a worker thread on a prompt nobody will see.
    EVP_PKEY* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!key) err = "the key is not an unencrypted PKCS#8 PEM private key";
    return key;
}

void free_pkey(void* pkey)
{
    if (pkey) EVP_PKEY_free((EVP_PKEY*) pkey);
}

// ES256 comes out of OpenSSL as DER; JOSE wants the raw pair of 32-byte integers. Identical to
// WebPush.cpp's der_to_raw - the same conversion, because it is the same signature format.
static bool der_to_raw(const unsigned char* der, size_t len, std::string& out)
{
    const unsigned char* p   = der;
    ECDSA_SIG*           sig = d2i_ECDSA_SIG(nullptr, &p, (long) len);
    if (!sig) return false;
    const BIGNUM *r = nullptr, *s = nullptr;
    ECDSA_SIG_get0(sig, &r, &s);
    unsigned char raw[64] = { 0 };
    const bool    ok = BN_bn2binpad(r, raw, 32) == 32 && BN_bn2binpad(s, raw + 32, 32) == 32;
    ECDSA_SIG_free(sig);
    if (ok) out.assign((const char*) raw, sizeof(raw));
    return ok;
}

bool sign_jwt(void* pkey_v, bool es256, const std::string& header_json, const std::string& claims_json,
              std::string& out_jwt, std::string& err)
{
    out_jwt.clear();
    err.clear();
    EVP_PKEY* pkey = (EVP_PKEY*) pkey_v;
    if (!pkey) { err = "no signing key"; return false; }
    const std::string signing_input = b64url(header_json) + "." + b64url(claims_json);

    EVP_MD_CTX* md  = EVP_MD_CTX_new();
    size_t      len = 0;
    bool        ok  = md && EVP_DigestSignInit(md, nullptr, EVP_sha256(), nullptr, pkey) == 1 &&
              EVP_DigestSignUpdate(md, signing_input.data(), signing_input.size()) == 1 &&
              EVP_DigestSignFinal(md, nullptr, &len) == 1;
    std::vector<unsigned char> sig(len);
    ok = ok && EVP_DigestSignFinal(md, sig.data(), &len) == 1;
    if (md) EVP_MD_CTX_free(md);
    if (!ok) { err = "signing the token failed"; return false; }
    sig.resize(len);

    std::string raw;
    if (es256) {
        if (!der_to_raw(sig.data(), sig.size(), raw)) { err = "could not convert the ES256 signature"; return false; }
    } else {
        // RS256 signatures are already the raw value JOSE wants; there is no conversion step.
        raw.assign((const char*) sig.data(), sig.size());
    }
    out_jwt = signing_input + "." + b64url(raw);
    return true;
}

bool read_text_file(const std::string& path, std::string& out, std::string& err)
{
    out.clear();
    err.clear();
    if (path.empty()) { err = "no path"; return false; }
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "could not open the key file"; return false; }
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    if (out.empty()) { err = "the key file is empty"; return false; }
    if (out.size() > 128 * 1024) { err = "that file is far too large to be a key"; return false; }
    return true;
}

bool debug_routes_on()
{
    const char* on = std::getenv("SNORCA_DEBUG_ROUTES");
    return on && std::string(on) == "1";
}

} // namespace detail

// ------------------------------------------------------------------- the envelope ----

// The collapse id: a second "paused" for the same printer replaces the first on the lock screen
// instead of stacking. APNs, FCM and the hosted service all see it in the clear, so it is a keyed
// HMAC under this hub's own secret (PushIds.hpp) - an unkeyed hash of a Bambu serial number could
// be confirmed by anyone who knows the serial. 24 characters, far inside APNs' 64-byte cap.
static std::string collapse_for(const std::string& printer_id, const std::string& kind)
{
    return PushIds::collapse_id(PushIds::key(), printer_id, kind);
}

// The cleartext thread id (APNs aps.thread-id, the hosted service's "thread"): opaque and stable per
// printer, so iOS still groups one printer's alerts, but never the printer id itself. The app routes
// a tap with the printer_id inside the encrypted payload, and sets its own thread identifier from it
// after decrypting; this one only shows if the extension cannot decrypt.
static std::string thread_for(const std::string& printer_id)
{
    return PushIds::thread_id(PushIds::key(), printer_id);
}

// What the app decrypts and renders. Deliberately the same shape WebPush::payload_for produces,
// so the iOS extension, the Android service and the browser's service worker all read one format.
static std::string plaintext_for(const json& e)
{
    json        p;
    std::string title = ev_str(e, "title", "EdgeSlicer");
    std::string body  = ev_str(e, "text");
    std::string who;
    if (e.is_object() && e.contains("printer") && e["printer"].is_object()) who = ev_str(e["printer"], "name");
    if (body.empty()) body = title;
    if (!who.empty() && body.find(who) == std::string::npos) body = who + ": " + body;
    body = RemoteEvents::notification_body(body, ev_str(e, "code"));
    p["title"]    = title;
    p["body"]     = body;
    p["kind"]     = ev_str(e, "kind");
    p["severity"] = ev_str(e, "severity", "info");
    if (!who.empty()) p["printer"] = who;
    const std::string pid = e.is_object() && e.contains("printer") && e["printer"].is_object()
                                ? ev_str(e["printer"], "id") : std::string();
    if (!pid.empty()) p["printer_id"] = pid;
    p["tag"] = pid + ":" + ev_str(e, "kind");
    // The error code, when there is one. Small, and it is the key the app needs: a notification
    // that says "the toolhead camera is not working" is a sentence, and the buttons that go with
    // it are fetched from /api/printers or /summary by this code. Carried here rather than the
    // whole actions array, which would not survive the plaintext cap.
    {
        const std::string code = ev_str(e, "code");
        if (!code.empty()) p["code"] = code;
    }
    if (e.is_object() && e.contains("id") && e["id"].is_number_integer()) p["id"] = e["id"];
    if (e.is_object() && e.contains("time") && e["time"].is_number_integer()) p["time"] = e["time"];
    // The stable id (<hub instance>-<id>): the app keys its history on it, so the pushed copy and
    // the pulled copy of one event are one row even across a data-dir reset or a second hub.
    if (e.is_object() && e.contains("uid") && e["uid"].is_string()) p["uid"] = e["uid"];
    std::string out = p.dump();
    if (out.size() > MAX_PLAINTEXT) {
        p["body"] = body.substr(0, 400);
        out       = p.dump();
        if (out.size() > MAX_PLAINTEXT) out = json({ { "title", title }, { "body", "" } }).dump();
    }
    return out;
}

// ------------------------------------------------------- per-device policy ----

namespace policy {

DevicePrefs read_prefs(const json& in)
{
    DevicePrefs p;
    if (!in.is_object()) return p;
    if (in.contains("priority_kinds") && in["priority_kinds"].is_array()) {
        // Kept in the canonical order and deduplicated; a kind this hub does not know is dropped
        // (a newer app may name one), never a reason to refuse the registration.
        for (const std::string& k : RemoteEvents::all_kinds())
            for (const auto& v : in["priority_kinds"])
                if (v.is_string() && v.get<std::string>() == k) { p.priority_kinds.push_back(k); break; }
    }
    if (in.contains("all_events") && in["all_events"].is_boolean()) p.all_events = in["all_events"].get<bool>();
    if (in.contains("level_hint") && in["level_hint"].is_boolean()) p.level_hint = in["level_hint"].get<bool>();
    return p;
}

static bool listed(const DevicePrefs& d, const std::string& kind)
{
    return std::find(d.priority_kinds.begin(), d.priority_kinds.end(), kind) != d.priority_kinds.end();
}

bool wants(const DevicePrefs& d, const std::string& severity, const std::string& kind,
           const std::string& min_severity, const std::vector<std::string>& kinds)
{
    // The phone that asked for everything decides on the phone (decision D2); every other device
    // gets the hub's filter, severity and kind as an AND, as on every other channel.
    if (d.all_events) return true;
    return severity_rank(severity) >= severity_rank(min_severity) && RemoteEvents::kind_allowed(kinds, kind);
}

// Priority mirrors the Urgency rule WebPush.cpp already applies: anything the person needs to see
// now breaks through, a "started" can wait for the phone to wake on its own - unless this device
// made that kind urgent, in which case it must not ride a batched priority-5 push.
int priority(const DevicePrefs& d, const std::string& severity, const std::string& kind)
{
    if (severity_rank(severity) >= 1) return 10;
    return listed(d, kind) ? 10 : 5;
}

int ttl(const std::string& kind)
{
    if (kind == "started" || kind == "resumed") return TTL_ROUTINE;
    if (kind == "failed" || kind == "error" || kind == "runout") return TTL_FAILURE;
    return TTL_ALERT;
}

std::string interruption_level(const DevicePrefs& d, const std::string& kind)
{
    return d.level_hint && listed(d, kind) ? std::string("time-sensitive") : std::string();
}

bool wakes_app(const std::string& kind)
{
    return RemoteEvents::is_kind(kind);
}

} // namespace policy

static json prefs_json(const policy::DevicePrefs& p)
{
    json j;
    j["priority_kinds"] = p.priority_kinds;
    j["all_events"]     = p.all_events;
    j["level_hint"]     = p.level_hint;
    return j;
}

// ------------------------------------------------------------------- persistence ----

static json device_json(const Device& d, bool masked)
{
    json j;
    j["id"]       = d.id;
    j["platform"] = d.platform;
    j["env"]      = d.env;
    j["label"]    = d.label;
    j["app"]      = d.app;
    j["os"]       = d.os;
    j["added"]    = d.added;
    if (masked) {
        j["token"]  = mask(d.token);
        j["p256dh"] = mask(d.p256dh);
        j["auth"]   = mask(d.auth);
        j["bundle"] = d.bundle; // a bundle id is not a secret; it is how a person recognises the app
        j["status"] = d.failures >= 3 ? "failing" : (d.last_sent == 0 ? "new" : (d.failures ? "retrying" : "ok"));
    } else {
        j["token"]  = d.token;
        j["p256dh"] = d.p256dh;
        j["auth"]   = d.auth;
        j["bundle"] = d.bundle;
    }
    j["last_sent"]   = d.last_sent;
    j["last_status"] = d.last_status;
    j["last_error"]  = d.last_error;
    j["last_host"]   = d.last_host;
    j["failures"]    = d.failures;
    // Not secrets: which kinds the phone made urgent, and whether it takes every event. Persisted
    // so a hub restart keeps them until the app's next launch re-posts its registration.
    j["levels"]      = prefs_json(d.prefs);
    // Hub-driven Live Activity updates. The tokens are kept across a hub restart (a print outlives
    // one), masked like the device token everywhere else.
    json la;
    la["enabled"]  = d.la.enabled;
    la["frequent"] = d.la.frequent;
    int activities = 0;
    json acts      = json::array();
    for (const auto& kv : d.la_tracks) {
        if (kv.second.activity_token.empty()) continue;
        ++activities;
        if (!masked)
            acts.push_back(json{ { "printer", kv.first }, { "job", kv.second.job_key },
                                 { "token", kv.second.activity_token }, { "started", kv.second.started_ms } });
    }
    if (masked) {
        la["start_token"] = !d.la.start_token.empty();
        la["activities"]  = activities;
    } else {
        la["start_token"] = d.la.start_token;
        la["activities"]  = acts;
    }
    j["live_activity"] = la;
    return j;
}

// The APNs block as the hub page may see it. key_pem and the .p8's contents are never here in any
// form: only whether one is set.
static json apns_masked(const json& c)
{
    json j;
    j["enabled"]  = c.value("enabled", true);
    j["bundle"]   = c.value("bundle", "");                 // not a secret
    j["key_id"]   = mask(c.value("key_id", ""));           // not secret, but it identifies the account
    j["team_id"]  = mask(c.value("team_id", ""));
    j["key_path"] = mask_path(c.value("key_path", ""));
    j["has_key"]  = !c.value("key_path", "").empty() || !c.value("key_pem", "").empty();
    j["env"]      = c.value("env", "production");
    return j;
}

static json fcm_masked(const json& c)
{
    json j;
    j["enabled"]              = c.value("enabled", true);
    j["project_id"]           = c.value("project_id", "");  // it is in every send URL; not a secret
    j["service_account_path"] = mask_path(c.value("service_account_path", ""));
    j["client_email"]         = mask(c.value("client_email", ""));
    j["has_key"]              = !c.value("service_account_path", "").empty() ||
                               !c.value("service_account_json", "").empty();
    return j;
}

// Whether this hub has its own APNs or FCM credential configured - the owner's setup.
static bool own_keys_locked()
{
    return !g_apns_cfg.value("key_path", "").empty() || !g_apns_cfg.value("key_pem", "").empty() ||
           !g_fcm_cfg.value("service_account_path", "").empty() || !g_fcm_cfg.value("service_account_json", "").empty();
}

// The mode actually in force. An explicit choice from the hub page wins; without one, a hub that
// already has its own keys keeps them (nothing changes for it on upgrade) and every other hub uses
// the hosted service, which is the only way it can push at all.
static bool hosted_mode_locked()
{
    if (g_mode == "hosted") return true;
    if (g_mode == "own") return false;
    return !own_keys_locked();
}

static std::string hosted_url_locked()
{
    const std::string u = g_hosted_cfg.value("url", "");
    return u.empty() ? std::string(Hosted::DEFAULT_URL) : u;
}

json settings_json()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    json j;
    j["enabled"]      = g_enabled;
    j["min_severity"] = g_min_severity;
    j["kinds"]        = g_kinds;
    j["apns"]         = g_apns_cfg;
    j["fcm"]          = g_fcm_cfg;
    if (!g_mode.empty()) j["mode"] = g_mode;
    j["hosted"]       = g_hosted_cfg;
    // The secret behind the opaque thread and collapse ids. Only ever in settings.json: never in
    // masked_json(), never on the phone plane, never sent anywhere.
    if (!g_id_key.empty()) j["id_key"] = g_id_key;
    j["live_activity"] = g_la_enabled;
    j["devices"]      = json::array();
    for (const Device& d : g_devices) j["devices"].push_back(device_json(d, false));
    return j;
}

json providers_json()
{
    bool hosted;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        hosted = hosted_mode_locked();
    }
    json j;
    std::string why;
    if (hosted) {
        // One service carries both platforms, so both answer the same - and /pair tells the app it
        // may register for its platform's push exactly as it would with own keys.
        const bool ok = g_hosted && g_hosted->available(why);
        j["apns"] = ok;
        j["apns_reason"] = ok ? "" : why;
        j["fcm"] = ok;
        j["fcm_reason"] = ok ? "" : why;
        j["mode"] = "hosted";
        return j;
    }
    j["apns"] = g_apns && g_apns->available(why);
    j["apns_reason"] = j["apns"].get<bool>() ? "" : why;
    why.clear();
    j["fcm"] = g_fcm && g_fcm->available(why);
    j["fcm_reason"] = j["fcm"].get<bool>() ? "" : why;
    j["mode"] = "own";
    return j;
}

json masked_json()
{
    json prov = providers_json();
    json hosted_status = json::object();
    if (g_hosted) {
        try { hosted_status = json::parse(g_hosted->status_json()); } catch (...) {}
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    json j;
    // How pushes leave this PC. `mode` is what is in force, `mode_setting` what was chosen ("" =
    // decided from the settings), `own_keys` whether an APNs/FCM credential is configured - the
    // hub page offers the switch to the hosted service when it is.
    j["mode"]         = hosted_mode_locked() ? "hosted" : "own";
    j["mode_setting"] = g_mode;
    j["own_keys"]     = own_keys_locked();
    // The hosted URL is not a secret: it is shown in full, so a person can see where pushes go.
    hosted_status["url"] = hosted_url_locked();
    j["hosted"]       = hosted_status;
    j["enabled"]      = g_enabled;
    j["min_severity"] = g_min_severity;
    j["kinds"]        = g_kinds;
    j["events"]       = RemoteEvents::events_map(g_kinds); // the same filter as checkboxes
    j["all_kinds"]    = RemoteEvents::all_kinds();
    j["severities"]   = json::array({ "info", "warning", "error" });
    j["apns"]         = apns_masked(g_apns_cfg);
    j["fcm"]          = fcm_masked(g_fcm_cfg);
    j["providers"]    = prov;
    // Shown on the hub page so that "APNs is unavailable" reads as a build fact rather than a
    // mystery: without nghttp2 in the bundled libcurl there is no HTTP/2 and Apple cannot be
    // reached at all. See deps/NGHTTP2/NGHTTP2.cmake.
    j["http2"]        = Http::has_http2();
    j["count"]        = (int) g_devices.size();
    j["max_devices"]  = (int) MAX_DEVICES;
    // Hub-driven Live Activity updates: allowed by this hub, and how many phones turned them on.
    j["live_activity"] = g_la_enabled;
    j["live_activity_devices"] = (int) std::count_if(g_devices.begin(), g_devices.end(), [](const Device& d) { return d.la.enabled; });
    j["devices"]      = json::array();
    for (const Device& d : g_devices) j["devices"].push_back(device_json(d, true));
    return j;
}

bool has_devices()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_enabled && !g_devices.empty();
}

bool consume_dirty() { return g_dirty.exchange(false); }

// ------------------------------------------------------------------- the sender ----

static Provider* provider_for(const std::string& platform, bool hosted)
{
    if (platform != "apns" && platform != "fcm") return nullptr;
    // In hosted mode one provider carries both platforms: it puts the platform in the request body.
    if (hosted) return g_hosted.get();
    if (platform == "apns") return g_apns.get();
    return g_fcm.get();
}

static PushRequest request_for(const Device& d, const json& event, const std::string& ciphertext_b64u)
{
    PushRequest req;
    req.platform        = d.platform;
    req.device_token    = d.token;
    req.env             = d.env;
    req.bundle          = d.bundle;
    req.ciphertext_b64u = ciphertext_b64u;
    const std::string pid = event.is_object() && event.contains("printer") && event["printer"].is_object()
                                ? ev_str(event["printer"], "id") : std::string();
    const std::string kind = ev_str(event, "kind");
    req.collapse_id = collapse_for(pid, kind);
    req.thread_id   = thread_for(pid);
    req.priority    = policy::priority(d.prefs, ev_str(event, "severity", "info"), kind);
    req.ttl_seconds = policy::ttl(kind);
    if (d.platform == "apns") {
        req.interruption_level = policy::interruption_level(d.prefs, kind);
        req.content_available  = policy::wakes_app(kind);
    }
    return req;
}

// One line per push, so the owner can line the hub's sends up against the phone's Console log
// (category LiveActivity) when measuring the lag: which device (its short random id, never the
// token), the kind, priority, whether it asked for the background wake, the outcome, and how old
// the event was when it left (`age`: now minus the event's own time, i.e. watcher-to-send delay).
static void log_sent(const Device& d, const json& event, const PushRequest& req, const PushResult& r, bool hosted, bool queued)
{
    long long age_ms = 0;
    if (event.is_object() && event.contains("time") && event["time"].is_number_integer())
        age_ms = std::max(0LL, now_ms() - event["time"].get<long long>());
    BOOST_LOG_TRIVIAL(info) << "AppPush: sent " << ev_str(event, "kind") << " to " << d.platform << " device " << d.id
                            << " via " << (hosted ? "hosted" : "own keys") << ": priority " << req.priority
                            << ", wake " << (req.content_available ? 1 : 0) << ", "
                            << (queued ? std::string("queued") : (r.ok ? std::string("ok") : "failed")) << " (HTTP "
                            << r.status << "), event age " << age_ms / 1000 << " s";
}

// Encrypt for one device. The whole reason this module can exist without new crypto: an app's
// registered p256dh/auth mean exactly what a browser subscription's do.
static bool encrypt_for(const Device& d, const std::string& plaintext, std::string& out_b64u, std::string& err)
{
    std::string body;
    if (!WebPush::encrypt(d.p256dh, d.auth, plaintext, "", "", body, err)) return false;
    out_b64u = detail::b64url(body);
    return true;
}

static bool worth_retrying(const PushResult& r)
{
    if (r.ok || r.gone) return false;
    if (r.credential_expired) return false; // handled separately: refresh, then one more try
    if (r.status == 0) return true;         // a transport error: DNS, TLS, no route
    return r.status == 429 || r.status >= 500;
}

static PushResult send_with_retries(Provider* p, const PushRequest& req, const json& cfg)
{
    PushResult r;
    for (int attempt = 1; attempt <= MAX_TRIES; ++attempt) {
        r = p->send(req);
        if (r.ok || r.gone) return r;
        if (r.credential_expired) {
            // The provider token or the OAuth2 access token aged out. This must never prune a live
            // device: drop the cached credential, mint a fresh one and try exactly once more.
            BOOST_LOG_TRIVIAL(info) << "AppPush: " << p->name() << " reported an expired credential; re-minting";
            p->configure(cfg.dump());
            r = p->send(req);
            return r;
        }
        if (!worth_retrying(r) || attempt == MAX_TRIES) break;
        // Short enough that a "finished" is still news, long enough to ride out a hiccup - and
        // slept in slices, so quitting the hub does not have to wait out a backoff.
        for (int slept = 0; slept < (attempt == 1 ? 1000 : 3000); slept += 100) {
            if (g_stopping) return r;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    return r;
}

// Fold one result back into the stored row: prune a device the platform says is dead, otherwise
// record what happened so the hub page can show it.
static void record(const Device& sent_to, const PushResult& r)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (size_t i = 0; i < g_devices.size(); ++i) {
        if (g_devices[i].id != sent_to.id) continue;
        if (r.gone) {
            BOOST_LOG_TRIVIAL(info) << "AppPush: device " << sent_to.id << " (" << sent_to.platform
                                    << ") is gone (HTTP " << r.status << "); forgetting it";
            g_devices.erase(g_devices.begin() + i);
            g_dirty = true;
            return;
        }
        g_devices[i].last_sent   = now_ms();
        g_devices[i].last_status = r.status;
        g_devices[i].last_host   = r.host;
        if (r.ok) {
            g_devices[i].failures = 0;
            g_devices[i].last_error.clear();
        } else {
            ++g_devices[i].failures;
            g_devices[i].last_error = scrub_locked(r.error, sent_to);
            BOOST_LOG_TRIVIAL(warning) << "AppPush: push to a " << sent_to.platform << " device failed ("
                                       << g_devices[i].failures << " in a row): " << g_devices[i].last_error;
        }
        g_dirty = true;
        return;
    }
}

// A queued hosted notification's final outcome, from the hosted provider's worker.
static void record_by_id(const std::string& id, const PushResult& r)
{
    Device d;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = std::find_if(g_devices.begin(), g_devices.end(), [&](const Device& row) { return row.id == id; });
        if (it == g_devices.end()) return; // removed while its notification waited
        d = *it;
    }
    record(d, r);
}

// The first try reached no push service and the notification is queued: say so on the row without
// counting it as a failure yet - the outcome arrives through record_by_id.
static void note_waiting(const Device& sent_to, const PushResult& r)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (Device& row : g_devices)
        if (row.id == sent_to.id) {
            row.last_error = "waiting for the push service: " + scrub_locked(r.error, sent_to);
            g_dirty        = true;
        }
}

void deliver(const json& event)
{
    std::vector<Device>      targets;
    std::vector<std::string> kinds;
    std::string              min_sev;
    json                     apns_cfg, fcm_cfg;
    bool                     hosted;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_enabled || g_devices.empty()) return;
        targets  = g_devices;
        min_sev  = g_min_severity;
        kinds    = g_kinds;
        apns_cfg = g_apns_cfg;
        fcm_cfg  = g_fcm_cfg;
        hosted   = hosted_mode_locked();
    }
    const std::string severity = ev_str(event, "severity", "info");
    const std::string kind     = ev_str(event, "kind");
    // Severity and kind are an AND, as on every other channel - per device now, because a phone
    // with its own notification levels asked for every event and decides on the phone (D2).
    if (std::none_of(targets.begin(), targets.end(),
                     [&](const Device& d) { return policy::wants(d.prefs, severity, kind, min_sev, kinds); }))
        return;
    const std::string plaintext = plaintext_for(event);

    for (const Device& d : targets) {
        if (g_stopping) return;
        if (!policy::wants(d.prefs, severity, kind, min_sev, kinds)) continue;
        Provider* p = provider_for(d.platform, hosted);
        if (!p) continue;
        std::string why;
        if (!p->available(why)) {
            // Not a failure of this device - the hub simply cannot send through that provider at
            // all right now. Recorded once so the hub page can say why, never counted as a
            // failure that could eventually look like a dead phone.
            std::lock_guard<std::mutex> lock(g_mutex);
            for (Device& row : g_devices)
                if (row.id == d.id && row.last_error != why) { row.last_error = why; g_dirty = true; }
            continue;
        }
        std::string blob, err;
        if (!encrypt_for(d, plaintext, blob, err)) {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (Device& row : g_devices)
                if (row.id == d.id) { row.last_error = scrub_locked(err, d); ++row.failures; g_dirty = true; }
            continue;
        }
        if (hosted) {
            // No in-process 1 s / 3 s retries here: the hosted provider has its own bounded queue
            // (5 s, 30 s, 2 min, 5 min... up to 30 min or the TTL) that outlives a short outage.
            bool              queued = false;
            const PushRequest req    = request_for(d, event, blob);
            const PushResult  r      = g_hosted->deliver(d.id, req, queued);
            log_sent(d, event, req, r, true, queued);
            if (queued) note_waiting(d, r);
            else record(d, r);
            continue;
        }
        const PushRequest req = request_for(d, event, blob);
        const PushResult  r   = send_with_retries(p, req, d.platform == "apns" ? apns_cfg : fcm_cfg);
        log_sent(d, event, req, r, false, false);
        record(d, r);
    }
}

// ------------------------------------------------------------------- the routes ----

// Reconfigure both providers from the settings currently in memory. Called whenever anything they
// read might have changed, so a .p8 that moved is noticed at once rather than at 03:00.
static void reconfigure_locked()
{
    if (g_apns) g_apns->configure(g_apns_cfg.dump());
    if (g_fcm) g_fcm->configure(g_fcm_cfg.dump());
    if (g_hosted) g_hosted->configure(g_hosted_cfg.dump());
}

std::pair<int, std::string> register_device(const std::string& body)
{
    json in;
    try {
        in = json::parse(body);
    } catch (...) {
        return { 400, json({ { "error", "the body must be JSON" } }).dump() };
    }
    if (!in.is_object()) return { 400, json({ { "error", "the body must be a JSON object" } }).dump() };

    const std::string platform = trim(in.value("platform", ""));
    if (platform != "apns" && platform != "fcm")
        return { 400, json({ { "error", "platform must be apns or fcm" } }).dump() };
    const std::string token = trim(in.value("token", ""));
    if (token.empty() || token.size() > 1024)
        return { 400, json({ { "error", "the device token is missing or implausible" } }).dump() };
    // A device token goes into a URL path (APNs) or a JSON field (FCM); either way nothing that
    // could end a header or a path belongs in it.
    for (unsigned char c : token)
        if (c <= 0x20 || c >= 0x7f || c == '/' || c == '?' || c == '#')
            return { 400, json({ { "error", "the device token has characters that cannot be in one" } }).dump() };

    const std::string p256dh = trim(in.value("p256dh", ""));
    const std::string auth   = trim(in.value("auth", ""));
    // The same two fields a browser PushSubscription carries, checked the same way: without them
    // there is nothing to encrypt to and the whole privacy property is gone.
    std::string probe, err;
    if (!WebPush::encrypt(p256dh, auth, "probe", "", "", probe, err))
        return { 400, json({ { "error", "p256dh must be a 65-byte uncompressed P-256 point and auth 16 bytes" } }).dump() };

    std::string env = trim(in.value("env", ""));
    if (env != "sandbox" && env != "production") env = "production";

    // Absent fields mean the defaults, so an app that does not send them (or stopped sending
    // them) is treated exactly as before notification levels existed.
    const policy::DevicePrefs prefs = policy::read_prefs(in);
    // Hub-driven Live Activity updates: absent is off, like every other opt-in here.
    LaPrefs la;
    if (in.contains("live_activity") && in["live_activity"].is_object()) {
        const json& l = in["live_activity"];
        la.enabled  = l.value("enabled", false);
        la.frequent = l.value("frequent", false);
        const std::string st = trim(l.value("start_token", ""));
        if (platform == "apns" && la.enabled && la_token_ok(st)) la.start_token = st;
    }
    // What this hub understands, so the app can tell the person when the PC needs an update
    // before its own notification levels take full effect.
    // "wake": APNs alerts for print events carry content-available (policy::wakes_app).
    // "live_activity": the live_activity field here and /push/activity (hub-driven updates).
    const json features = json::array({ "priority_kinds", "all_events", "level_hint", "push_test", "wake", "live_activity" });

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const std::string bundle = header_safe(in.value("bundle", ""), 200);
        const std::string label  = header_safe(in.value("label", ""), 60);
        const std::string appv   = header_safe(in.value("app", ""), 32);
        const std::string osv    = header_safe(in.value("os", ""), 32);
        // Keyed on (platform, token): the app re-registers on every cold launch because push
        // tokens rotate, and sixteen copies of one phone would mean sixteen pushes per event.
        for (Device& d : g_devices)
            if (d.platform == platform && d.token == token) {
                d.env = env; d.bundle = bundle; d.p256dh = p256dh; d.auth = auth;
                d.label = label; d.app = appv; d.os = osv;
                d.prefs = prefs;
                if (!la.enabled) d.la_tracks.clear(); // off: forget every activity token at once
                if (la.enabled != d.la.enabled)
                    BOOST_LOG_TRIVIAL(info) << "LiveActivity: hub updates turned " << (la.enabled ? "on" : "off")
                                            << " by device " << d.id;
                d.la = la;
                d.failures = 0;
                d.last_error.clear();
                g_dirty = true;
                return { 200, json({ { "ok", true }, { "id", d.id }, { "count", (int) g_devices.size() },
                                     { "features", features } }).dump() };
            }
        if (g_devices.size() >= MAX_DEVICES)
            return { 429, json({ { "error", "this hub is already pushing to as many devices as it will" } }).dump() };
        Device d;
        d.id       = random_id();
        d.platform = platform;
        d.env      = env;
        d.token    = token;
        d.bundle   = bundle;
        d.p256dh   = p256dh;
        d.auth     = auth;
        d.label    = label;
        d.app      = appv;
        d.os       = osv;
        d.prefs    = prefs;
        d.la       = la;
        d.added    = now_ms();
        g_devices.push_back(d);
        g_dirty = true;
        BOOST_LOG_TRIVIAL(info) << "AppPush: a " << platform << " device registered (" << g_devices.size() << " total)";
        return { 200, json({ { "ok", true }, { "id", d.id }, { "count", (int) g_devices.size() },
                             { "features", features } }).dump() };
    }
}

std::pair<int, std::string> forget_device(const std::string& body)
{
    std::string platform, token;
    try {
        const json in = json::parse(body);
        if (in.is_object()) {
            platform = trim(in.value("platform", ""));
            token    = trim(in.value("token", ""));
        }
    } catch (...) {}
    if (!token.empty()) {
        std::string removed_id;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (size_t i = 0; i < g_devices.size(); ++i)
                if (g_devices[i].token == token && (platform.empty() || g_devices[i].platform == platform)) {
                    removed_id = g_devices[i].id;
                    g_devices.erase(g_devices.begin() + i);
                    g_dirty = true;
                    break;
                }
        }
        // Nothing more for an unpaired phone, not even what was waiting for the push service.
        if (!removed_id.empty() && g_hosted) g_hosted->forget(removed_id);
    }
    // Whether it was there is not the caller's business: answering "no such device" to an
    // unauthenticated caller would turn this into an oracle for guessing device tokens.
    return { 200, std::string("{\"ok\":true}") };
}

std::pair<int, std::string> remove(const std::string& id)
{
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (size_t i = 0; i < g_devices.size(); ++i)
            if (g_devices[i].id == id) { g_devices.erase(g_devices.begin() + i); found = true; g_dirty = true; break; }
    }
    if (!found) return { 404, json({ { "error", "no such device" } }).dump() };
    if (g_hosted) g_hosted->forget(id);
    BOOST_LOG_TRIVIAL(info) << "AppPush: device removed from the hub page";
    return { 200, masked_json().dump() };
}

int forget_all_devices()
{
    size_t n;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        n = g_devices.size();
        g_devices.clear();
        if (n) g_dirty = true;
    }
    // Including what is still queued for the hosted service: the old link's phones get nothing more.
    if (g_hosted) g_hosted->forget(std::string());
    if (n) BOOST_LOG_TRIVIAL(info) << "AppPush: " << n << " device(s) forgotten with the old phone link";
    return (int) n;
}

std::pair<int, std::string> set_options(const std::string& body)
{
    json in;
    try {
        in = json::parse(body);
    } catch (...) {
        return { 400, json({ { "error", "the body must be JSON" } }).dump() };
    }
    if (!in.is_object()) return { 400, json({ { "error", "the body must be a JSON object" } }).dump() };
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        take_bool(in, "enabled", g_enabled);
        take_bool(in, "live_activity", g_la_enabled);
        if (in.contains("min_severity") && in["min_severity"].is_string()) {
            const std::string s = in["min_severity"].get<std::string>();
            if (s != "info" && s != "warning" && s != "error")
                return { 400, json({ { "error", "min_severity must be info, warning or error" } }).dump() };
            g_min_severity = s;
        }
        if (RemoteEvents::has_kind_filter(in)) {
            std::string why;
            if (!RemoteEvents::read_kinds(in, g_kinds, why)) return { 400, json({ { "error", why } }).dump() };
        }
        if (in.contains("apns") && in["apns"].is_object()) {
            const json& a = in["apns"];
            bool        on = g_apns_cfg.value("enabled", true);
            take_bool(a, "enabled", on);
            g_apns_cfg["enabled"] = on;
            std::string v;
            v = g_apns_cfg.value("bundle", "");   take_string(a, "bundle", v);   g_apns_cfg["bundle"] = header_safe(v, 200);
            v = g_apns_cfg.value("key_id", "");   take_secret(a, "key_id", v);   g_apns_cfg["key_id"] = header_safe(v, 32);
            v = g_apns_cfg.value("team_id", "");  take_secret(a, "team_id", v);  g_apns_cfg["team_id"] = header_safe(v, 32);
            v = g_apns_cfg.value("key_path", ""); take_path(a, "key_path", v);   g_apns_cfg["key_path"] = v;
            // The PEM itself, for people who would rather paste the key than keep a file. It is a
            // credential in the fullest sense: stored only in settings.json, masked out of every
            // response, and never echoed back even masked.
            v = g_apns_cfg.value("key_pem", "");  take_secret(a, "key_pem", v);  g_apns_cfg["key_pem"] = v;
            std::string env = g_apns_cfg.value("env", std::string("production"));
            take_string(a, "env", env);
            g_apns_cfg["env"] = (env == "sandbox") ? "sandbox" : "production";
            if (detail::debug_routes_on()) {
                // Only ever honoured with SNORCA_DEBUG_ROUTES=1, and only so the gate can point
                // this sender at a mock on loopback. A shipped hub must not be talkable into
                // sending a device's notifications somewhere that is not Apple.
                v = g_apns_cfg.value("host_override", ""); take_string(a, "host_override", v);
                g_apns_cfg["host_override"] = v;
            }
        }
        if (in.contains("fcm") && in["fcm"].is_object()) {
            const json& f = in["fcm"];
            bool        on = g_fcm_cfg.value("enabled", true);
            take_bool(f, "enabled", on);
            g_fcm_cfg["enabled"] = on;
            std::string v;
            v = g_fcm_cfg.value("project_id", "");           take_string(f, "project_id", v); g_fcm_cfg["project_id"] = header_safe(v, 120);
            v = g_fcm_cfg.value("service_account_path", ""); take_path(f, "service_account_path", v); g_fcm_cfg["service_account_path"] = v;
            v = g_fcm_cfg.value("service_account_json", ""); take_secret(f, "service_account_json", v); g_fcm_cfg["service_account_json"] = v;
            if (detail::debug_routes_on()) {
                v = g_fcm_cfg.value("host_override", "");      take_string(f, "host_override", v);      g_fcm_cfg["host_override"] = v;
                v = g_fcm_cfg.value("token_uri_override", ""); take_string(f, "token_uri_override", v); g_fcm_cfg["token_uri_override"] = v;
            }
        }
        if (in.contains("mode") && in["mode"].is_string()) {
            const std::string m = trim(in["mode"].get<std::string>());
            if (m != "hosted" && m != "own" && m != "auto")
                return { 400, json({ { "error", "mode must be hosted, own or auto" } }).dump() };
            g_mode = m == "auto" ? std::string() : m;
            BOOST_LOG_TRIVIAL(info) << "AppPush: push mode set to " << (g_mode.empty() ? "auto" : g_mode)
                                    << " (in force: " << (hosted_mode_locked() ? "hosted" : "own") << ")";
        }
        if (in.contains("hosted") && in["hosted"].is_object()) {
            std::string url = g_hosted_cfg.value("url", "");
            take_string(in["hosted"], "url", url);
            while (!url.empty() && url.back() == '/') url.pop_back();
            if (url == Hosted::DEFAULT_URL) url.clear(); // the default is not pinned: it can move with a release
            std::string why;
            if (!url.empty() && !Hosted::url_allowed(url, why)) return { 400, json({ { "error", why } }).dump() };
            g_hosted_cfg["url"] = url;
        }
        reconfigure_locked();
        g_dirty = true;
    }
    return { 200, masked_json().dump() };
}

std::pair<int, std::string> hosted_check(const std::string& body)
{
    if (!g_hosted) return { 503, json({ { "error", "app push is not running" } }).dump() };
    bool do_register = false, with_quota = true;
    try {
        const json in = body.empty() ? json::object() : json::parse(body);
        take_bool(in, "register", do_register);
        take_bool(in, "quota", with_quota);
    } catch (...) {
        return { 400, json({ { "error", "the body must be JSON" } }).dump() };
    }
    // Synchronous, on the request thread, like the test button: somebody is watching for it.
    g_hosted->check(do_register, with_quota);
    return { 200, masked_json().dump() };
}

std::pair<int, std::string> hosted_unregister()
{
    if (!g_hosted) return { 503, json({ { "error", "app push is not running" } }).dump() };
    json out;
    try { out = json::parse(g_hosted->unregister()); } catch (...) { out = json::object(); }
    json j = masked_json();
    j["unregister"] = json{ { "ok", out.value("ok", false) }, { "status", out.value("status", 0) },
                            { "error", out.value("error", std::string()) } };
    return { 200, j.dump() };
}

void set_identity(const std::string& hubid, const std::string& public_hex, const std::string& private_hex)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_identity.hubid       = hubid;
    g_identity.public_hex  = public_hex;
    g_identity.private_hex = private_hex;
    if (g_hosted) g_hosted->set_identity(g_identity);
}

std::pair<int, std::string> test()
{
    std::vector<Device>      targets;
    std::vector<std::string> kinds;
    json                     apns_cfg, fcm_cfg;
    bool                     hosted;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        targets  = g_devices;
        kinds    = g_kinds;
        apns_cfg = g_apns_cfg;
        fcm_cfg  = g_fcm_cfg;
        hosted   = hosted_mode_locked();
    }
    if (targets.empty())
        return { 200, json({ { "ok", false }, { "error", "no app has registered a device yet" },
                             { "results", json::array() } }).dump() };

    json e;
    e["id"]       = 0;
    e["time"]     = now_ms();
    e["printer"]  = json{ { "id", "test" }, { "name", "Test" }, { "kind", "printhost" } };
    // Dressed as a kind the filter allows, so the notification looks like the real one. Sent
    // whatever the filters say: the button is here to prove the path to the device works.
    e["kind"]     = RemoteEvents::test_kind(kinds);
    e["severity"] = "info";
    e["title"]    = "EdgeSlicer test";
    e["text"]     = "This is a test push from the hub on your PC. If you can read it, app push works.";
    const std::string plaintext = plaintext_for(e);

    // Sent on this thread, once, with no retries: somebody is watching the page for the answer.
    json results = json::array();
    bool any     = false;
    for (const Device& d : targets) {
        json one;
        one["id"]       = d.id;
        one["platform"] = d.platform;
        one["label"]    = d.label;
        one["token"]    = mask(d.token);
        Provider*   p = provider_for(d.platform, hosted);
        std::string why;
        if (!p || !p->available(why)) {
            one["ok"]     = false;
            one["status"] = 0;
            one["host"]   = "";
            one["error"]  = p ? why : "no provider for that platform";
            results.push_back(one);
            continue;
        }
        std::string blob, err;
        if (!encrypt_for(d, plaintext, blob, err)) {
            one["ok"] = false; one["status"] = 0; one["host"] = ""; one["error"] = scrub(err, d);
            results.push_back(one);
            continue;
        }
        const PushResult r = p->send(request_for(d, e, blob));
        any = any || r.ok;
        one["ok"]     = r.ok;
        one["status"] = r.status;
        // Which host answered, so an APNs BadDeviceToken from an environment mismatch is
        // diagnosable from the hub page rather than from a debugger (risk R3).
        one["host"]   = r.host;
        one["error"]  = scrub(r.error, d);
        results.push_back(one);
        record(d, r);
    }
    // The filter goes back with the results so the page can say which kinds are on.
    return { 200, json({ { "ok", any }, { "results", results }, { "kind", e["kind"] },
                         { "mode", hosted ? "hosted" : "own" },
                         { "kinds", RemoteEvents::enabled_kinds(kinds) },
                         { "events", RemoteEvents::events_map(kinds) } }).dump() };
}

// The severity a real event of this kind carries (RemoteEvents.cpp's make_event calls), so a test
// is sent at the priority, and decided on the phone at the level, the real one would be.
static const char* severity_of_kind(const std::string& kind)
{
    if (kind == "failed" || kind == "error") return "error";
    if (kind == "paused" || kind == "runout" || kind == "cancelled") return "warning";
    return "info";
}

std::pair<int, std::string> test_device(const std::string& body)
{
    static std::atomic<bool>      busy { false };
    static std::atomic<long long> last_ms { 0 };

    json in;
    try {
        in = json::parse(body);
    } catch (...) {
        return { 400, json({ { "error", "the body must be JSON" } }).dump() };
    }
    if (!in.is_object()) return { 400, json({ { "error", "the body must be a JSON object" } }).dump() };
    const std::string platform = trim(in.value("platform", ""));
    const std::string token    = trim(in.value("token", ""));
    std::string       kind     = trim(in.value("kind", ""));
    if (kind.empty()) kind = "failed";
    if (!RemoteEvents::is_kind(kind)) return { 400, json({ { "error", "kind must be one of the event kinds" } }).dump() };
    int delay_s = 0;
    if (in.contains("delay_s") && in["delay_s"].is_number_integer()) delay_s = in["delay_s"].get<int>();
    delay_s = std::max(0, std::min(TEST_MAX_DELAY_S, delay_s));
    if (token.empty()) return { 400, json({ { "error", "name this device's push token" } }).dump() };

    Device d;
    bool   hosted = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = std::find_if(g_devices.begin(), g_devices.end(), [&](const Device& row) {
            return row.token == token && (platform.empty() || row.platform == platform);
        });
        // The caller already holds the hub token, and a push token is not guessable; this answer
        // says only that this phone has not registered (yet), which is what it needs to know.
        if (it == g_devices.end())
            return { 404, json({ { "error", "this device is not registered with the hub; open the app once so it registers" } }).dump() };
        d      = *it;
        hosted = hosted_mode_locked();
    }

    const long long now = now_ms();
    if (now - last_ms.load() < TEST_MIN_GAP_MS || busy.exchange(true))
        return { 429, json({ { "error", "one test at a time; try again in a few seconds" } }).dump() };
    struct Release { ~Release() { last_ms = now_ms(); busy = false; } } release;

    // Waited out on this request thread (the hub serves each connection on its own), in slices so
    // a hub that is quitting does not have to wait for it.
    for (int slept = 0; slept < delay_s * 1000; slept += 100) {
        if (g_stopping) return { 503, json({ { "error", "the hub is stopping" } }).dump() };
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    json e;
    // Id 0: no app stores it in its history (HubEvent.fromPush needs an id above 0).
    e["id"]       = 0;
    e["time"]     = now_ms();
    e["printer"]  = json{ { "id", "test" }, { "name", "Test printer" }, { "kind", "printhost" } };
    e["kind"]     = kind;
    e["severity"] = severity_of_kind(kind);
    e["title"]    = "Test: " + kind;
    e["text"]     = "A test \"" + kind + "\" notification from the hub, sent to this phone only.";
    const std::string plaintext = plaintext_for(e);

    Provider*   p = provider_for(d.platform, hosted);
    std::string why;
    if (!p || !p->available(why))
        return { 200, json({ { "ok", false }, { "status", 0 }, { "error", p ? why : "no provider for that platform" } }).dump() };
    std::string blob, err;
    if (!encrypt_for(d, plaintext, blob, err))
        return { 200, json({ { "ok", false }, { "status", 0 }, { "error", scrub(err, d) } }).dump() };
    const PushRequest req = request_for(d, e, blob);
    // Once, no retries, like the page's test button: somebody is watching for the answer.
    const PushResult  r   = p->send(req);
    record(d, r);
    return { 200, json({ { "ok", r.ok }, { "status", r.status }, { "host", r.host }, { "error", scrub(r.error, d) },
                         { "kind", kind }, { "severity", e["severity"] }, { "priority", req.priority },
                         { "ttl", req.ttl_seconds }, { "interruption_level", req.interruption_level },
                         { "content_available", req.content_available },
                         { "mode", hosted ? "hosted" : "own" } }).dump() };
}

// ------------------------------------------------- hub-driven Live Activity updates ----
//
// The rules and the payloads are LiveActivityPush.cpp's. Here: the per-phone tokens, the tick after
// every printer poll, and a worker thread of its own for the sends, so a slow push service never
// holds up the hub's loop (the poll that feeds the tick runs on it).

std::string push_id(const std::string& printer_id)
{
    return PushIds::thread_id(PushIds::key(), printer_id);
}

bool live_activity_wanted()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_enabled || !g_la_enabled) return false;
    return std::any_of(g_devices.begin(), g_devices.end(), [](const Device& d) { return d.la.enabled; });
}

// The printer ids the last tick saw, so an activity registered with a push_id (a push-started one
// the app could not name yet) resolves to the printer.
static std::set<std::string> g_la_known_printers;

static std::string la_resolve_printer_locked(const std::string& given)
{
    if (given.empty()) return given;
    if (g_la_known_printers.count(given)) return given;
    for (const std::string& id : g_la_known_printers)
        if (push_id(id) == given) return id;
    for (const Device& d : g_devices)
        for (const auto& kv : d.la_tracks)
            if (push_id(kv.first) == given) return kv.first;
    return given;
}

std::pair<int, std::string> register_activity(const std::string& body)
{
    json in;
    try {
        in = json::parse(body);
    } catch (...) {
        return { 400, json({ { "error", "the body must be JSON" } }).dump() };
    }
    if (!in.is_object()) return { 400, json({ { "error", "the body must be a JSON object" } }).dump() };
    const std::string platform = trim(in.value("platform", ""));
    const std::string token    = trim(in.value("token", ""));
    const std::string printer  = trim(in.value("printer", ""));
    const std::string atoken   = trim(in.value("activity_token", ""));
    const std::string job      = trim(in.value("job", ""));
    if (platform != "apns") return { 400, json({ { "error", "Live Activity tokens are an iOS thing: platform must be apns" } }).dump() };
    if (token.empty() || printer.empty() || printer.size() > 200)
        return { 400, json({ { "error", "name this phone's push token and the printer" } }).dump() };
    if (!la_token_ok(atoken)) return { 400, json({ { "error", "activity_token must be the activity's hex push token" } }).dump() };
    if (job.size() > 64) return { 400, json({ { "error", "job must be the app's job key" } }).dump() };
    long long started_ms = now_ms();
    if (in.contains("started_at") && in["started_at"].is_number()) {
        const long long s = (long long) in["started_at"].get<double>() * 1000;
        if (s > 0 && s <= started_ms) started_ms = s;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = std::find_if(g_devices.begin(), g_devices.end(), [&](const Device& d) { return d.platform == platform && d.token == token; });
    if (it == g_devices.end())
        return { 404, json({ { "error", "this phone is not registered with the hub; it registers on its next launch" } }).dump() };
    if (!it->la.enabled)
        return { 409, json({ { "error", "Lock Screen updates through this PC are off for this phone" } }).dump() };
    const std::string id = la_resolve_printer_locked(printer);
    if (!it->la_tracks.count(id) && it->la_tracks.size() >= 32)
        return { 429, json({ { "error", "too many printers for one phone" } }).dump() };
    LiveActivity::Track& t = it->la_tracks[id];
    t.activity_token = atoken;
    // A push-started activity does not know its job; the hub does (it asked for it).
    t.job_key        = !job.empty() ? job : t.start_sent_job;
    t.started_ms     = started_ms;
    t.has_sent       = false;
    g_dirty          = true;
    BOOST_LOG_TRIVIAL(info) << "LiveActivity: device " << it->id << " registered an activity for " << id;
    return { 200, json({ { "ok", true }, { "printer", id } }).dump() };
}

std::pair<int, std::string> forget_activity(const std::string& body)
{
    std::string platform, token, printer;
    bool        dismissed = false;
    try {
        const json in = json::parse(body);
        if (in.is_object()) {
            platform  = trim(in.value("platform", ""));
            token     = trim(in.value("token", ""));
            printer   = trim(in.value("printer", ""));
            dismissed = in.value("dismissed", false);
        }
    } catch (...) {}
    if (!token.empty() && !printer.empty()) {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (Device& d : g_devices) {
            if (d.token != token || (!platform.empty() && d.platform != platform)) continue;
            const std::string id = la_resolve_printer_locked(printer);
            auto t = d.la_tracks.find(id);
            if (t == d.la_tracks.end()) break;
            if (dismissed && !t->second.job_key.empty()) t->second.dismissed_job = t->second.job_key;
            t->second.activity_token.clear();
            t->second.has_sent   = false;
            t->second.started_ms = 0;
            g_dirty              = true;
            break;
        }
    }
    // Like DELETE /push/device: not an oracle for what is registered.
    return { 200, std::string("{\"ok\":true}") };
}

// One unit of work for the sender: one or two pushes (an end and a push-to-start) for one phone and
// one printer, and what to record when they went out.
struct LaJob
{
    Device                   dev;          // a copy: the row may change while this waits
    std::string              printer_id;
    LiveActivity::Decision   decision;
    LiveActivity::Row        row;
    bool                     has_row { false };
    json                     raw_row;      // Android: the row the progress update is built from
    std::vector<PushRequest> reqs;         // iOS; built in the tick
    std::vector<std::string> what;         // "update", "end", "start" - for the log
    bool                     hosted { false };
    json                     cfg;          // the provider settings, for a re-mint
    long long                made_ms { 0 };
};

static std::mutex              g_la_mutex;
static std::condition_variable g_la_cv;
static std::deque<LaJob>       g_la_queue;
static std::set<std::string>   g_la_inflight; // device id + "|" + printer id
static std::thread             g_la_thread;
static bool                    g_la_thread_running { false };
static const size_t            LA_QUEUE_MAX = 64;

static PushRequest la_apns_request(const Device& d, const std::string& token, const json& aps, int priority, int ttl)
{
    PushRequest r;
    r.platform     = "apns";
    r.device_token = token;
    r.env          = d.env;
    r.bundle       = d.bundle;
    r.push_type    = "liveactivity";
    r.la_aps       = aps.dump();
    r.priority     = priority;
    r.ttl_seconds  = ttl;
    return r;
}

static PushResult la_send(Provider* p, const PushRequest& req, bool hosted, const json& cfg)
{
    // One try (plus a re-mint): a progress push that arrives late is worse than none, and the next
    // poll re-decides from the state then.
    PushResult r = p->send(req);
    if (!hosted && r.credential_expired) {
        p->configure(cfg.dump());
        r = p->send(req);
    }
    return r;
}

// Fold a job's outcome back into the phone's track.
static void la_record(const LaJob& job, bool all_ok, bool end_ok, bool activity_gone, bool start_token_gone)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (Device& d : g_devices) {
        if (d.id != job.dev.id) continue;
        if (start_token_gone) d.la.start_token.clear();
        auto it = d.la_tracks.find(job.printer_id);
        if (it == d.la_tracks.end()) break;
        LiveActivity::Track& t = it->second;
        // The token may have been replaced by a newer registration while this was in flight.
        const bool same_token = job.decision.action == LiveActivity::Action::Start || t.virtual_activity ||
                                (!job.reqs.empty() && t.activity_token == job.reqs.front().device_token);
        if (!same_token) break;
        const LiveActivity::Row* row = job.has_row ? &job.row : nullptr;
        if (all_ok) {
            t = LiveActivity::apply(t, job.decision, row, job.made_ms);
        } else if (end_ok) {
            // End went out, the push-to-start did not: the next poll may try a start again.
            LiveActivity::Decision end = job.decision;
            end.action = LiveActivity::Action::End;
            t          = LiveActivity::apply(t, end, row, job.made_ms);
        } else if (activity_gone) {
            // APNs says the activity is gone: the person removed it (or iOS ended it). Do not start
            // this print's activity again by push.
            if (job.decision.action != LiveActivity::Action::End && !t.job_key.empty()) t.dismissed_job = t.job_key;
            t.activity_token.clear();
            t.has_sent   = false;
            t.started_ms = 0;
        }
        g_dirty = true;
        break;
    }
}

static void la_run(LaJob& job)
{
    Provider* p = provider_for(job.dev.platform, job.hosted);
    std::string why;
    if (!p || !p->available(why)) {
        BOOST_LOG_TRIVIAL(info) << "LiveActivity: cannot send for " << job.printer_id << ": " << (p ? why : std::string("no provider"));
        return;
    }
    const long long age_s = job.has_row ? job.row.age_ms / 1000 + (now_ms() - job.made_ms) / 1000 : -1;

    if (job.dev.platform == "fcm") {
        // Android: one encrypted progress update; the app moves its own notification along.
        const std::string plaintext = LiveActivity::progress_plaintext(job.raw_row, job.made_ms).dump();
        std::string       blob, err;
        if (plaintext.size() > MAX_PLAINTEXT || !encrypt_for(job.dev, plaintext, blob, err)) {
            BOOST_LOG_TRIVIAL(warning) << "LiveActivity: could not build the progress update for " << job.printer_id;
            return;
        }
        PushRequest r;
        r.platform        = "fcm";
        r.device_token    = job.dev.token;
        r.bundle          = job.dev.bundle;
        r.ciphertext_b64u = blob;
        r.collapse_id     = collapse_for(job.printer_id, "progress");
        r.priority        = job.decision.priority;
        r.ttl_seconds     = job.decision.priority >= 10 ? LiveActivity::Rules().ttl_p10_s : LiveActivity::Rules().ttl_p5_s;
        r.push_type       = "progress";
        const PushResult res = la_send(p, r, job.hosted, job.cfg);
        BOOST_LOG_TRIVIAL(info) << "LiveActivity: progress " << LiveActivity::action_name(job.decision.action) << " p" << r.priority
                                << " (" << job.decision.why << ") for " << job.printer_id << " to fcm device " << job.dev.id
                                << " via " << (job.hosted ? "hosted" : "own keys") << ": " << (res.ok ? "ok" : "failed")
                                << " (HTTP " << res.status << ")" << (res.ok ? std::string() : ", " + scrub(res.error, job.dev))
                                << ", data age " << age_s << " s";
        la_record(job, res.ok, false, false, false);
        return;
    }

    bool all_ok = true, end_ok = false, activity_gone = false, start_token_gone = false;
    for (size_t i = 0; i < job.reqs.size(); ++i) {
        if (g_stopping) return;
        const PushResult res = la_send(p, job.reqs[i], job.hosted, job.cfg);
        BOOST_LOG_TRIVIAL(info) << "LiveActivity: " << job.what[i] << " p" << job.reqs[i].priority << " (" << job.decision.why
                                << ") for " << job.printer_id << " to apns device " << job.dev.id << " via "
                                << (job.hosted ? "hosted" : "own keys") << ": " << (res.ok ? "ok" : "failed") << " (HTTP "
                                << res.status << ")" << (res.ok ? std::string() : ", " + scrub(res.error, job.dev))
                                << ", data age " << age_s << " s";
        if (res.ok) {
            if (job.what[i] == "end") end_ok = true;
            continue;
        }
        all_ok = false;
        if (res.gone) {
            if (job.what[i] == "start") start_token_gone = true;
            else activity_gone = true;
        }
        break;
    }
    la_record(job, all_ok, end_ok && !all_ok, activity_gone, start_token_gone);
}

static void la_worker()
{
    for (;;) {
        LaJob job;
        {
            std::unique_lock<std::mutex> lock(g_la_mutex);
            g_la_cv.wait(lock, [] { return g_stopping || !g_la_queue.empty(); });
            if (g_stopping) { g_la_queue.clear(); g_la_inflight.clear(); return; }
            job = std::move(g_la_queue.front());
            g_la_queue.pop_front();
        }
        try { la_run(job); } catch (...) {}
        std::lock_guard<std::mutex> lock(g_la_mutex);
        g_la_inflight.erase(job.dev.id + "|" + job.printer_id);
    }
}

static void la_enqueue(LaJob job)
{
    std::lock_guard<std::mutex> lock(g_la_mutex);
    if (g_stopping) return;
    const std::string key = job.dev.id + "|" + job.printer_id;
    if (g_la_inflight.count(key) || g_la_queue.size() >= LA_QUEUE_MAX) return;
    g_la_inflight.insert(key);
    g_la_queue.push_back(std::move(job));
    if (!g_la_thread_running) {
        g_la_thread         = std::thread(la_worker);
        g_la_thread_running = true;
    }
    g_la_cv.notify_one();
}

static void la_stop_worker()
{
    {
        std::lock_guard<std::mutex> lock(g_la_mutex);
        g_la_cv.notify_all();
    }
    if (g_la_thread_running && g_la_thread.joinable()) g_la_thread.join();
    std::lock_guard<std::mutex> lock(g_la_mutex);
    g_la_thread_running = false;
    g_la_queue.clear();
    g_la_inflight.clear();
}

static bool la_inflight(const std::string& device_id, const std::string& printer_id)
{
    std::lock_guard<std::mutex> lock(g_la_mutex);
    return g_la_inflight.count(device_id + "|" + printer_id) > 0;
}

void live_activity_tick(const json& rows)
{
    if (g_stopping) return;
    const long long now = now_ms();
    std::map<std::string, std::pair<LiveActivity::Row, json>> by_id;
    if (rows.is_array())
        for (const json& r : rows) {
            LiveActivity::Row row = LiveActivity::row_from_json(r);
            if (!row.id.empty()) by_id[row.id] = { row, r };
        }

    std::vector<LaJob> jobs;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_la_known_printers.clear();
        for (const auto& kv : by_id) g_la_known_printers.insert(kv.first);
        if (!g_enabled || !g_la_enabled) return;
        const bool hosted = hosted_mode_locked();
        for (Device& d : g_devices) {
            if (!d.la.enabled || (d.platform != "apns" && d.platform != "fcm")) continue;
            const bool fcm = d.platform == "fcm";
            std::set<std::string> ids;
            for (const auto& kv : by_id) ids.insert(kv.first);
            for (const auto& kv : d.la_tracks) ids.insert(kv.first);
            for (const std::string& id : ids) {
                auto found = by_id.find(id);
                const LiveActivity::Row* row = found == by_id.end() ? nullptr : &found->second.first;
                // Only printers worth a track: one with a print under way, or one already tracked.
                if (!d.la_tracks.count(id)) {
                    if (!row) continue;
                    if (!LiveActivity::is_active(LiveActivity::classify(row->state, row->printing, !row->error_code.empty()))) continue;
                    if (d.la_tracks.size() >= 32) continue;
                }
                LiveActivity::Track& t = d.la_tracks[id];
                t.virtual_activity     = fcm;
                // How long this print has been seen under way (the push-to-start grace).
                const bool active = row && LiveActivity::is_active(LiveActivity::classify(row->state, row->printing, !row->error_code.empty()));
                if (!active) t.active_since_ms = 0;
                else if (t.active_since_ms == 0) t.active_since_ms = now;
                if (la_inflight(d.id, id)) continue;
                const bool can_start = !fcm && !d.la.start_token.empty();
                const LiveActivity::Decision dec = LiveActivity::decide(t, row, can_start, now);
                if (dec.action == LiveActivity::Action::None) continue;

                LaJob job;
                job.dev        = d;
                job.dev.la_tracks.clear(); // the copy needs the row's keys, not every track
                job.printer_id = id;
                job.decision   = dec;
                job.has_row    = row != nullptr;
                if (row) job.row = *row;
                if (found != by_id.end()) job.raw_row = found->second.second;
                job.hosted     = hosted;
                job.cfg        = fcm ? g_fcm_cfg : g_apns_cfg;
                job.made_ms    = now;
                if (!fcm) {
                    const LiveActivity::Rules rules;
                    const long long now_s = now / 1000;
                    const int ttl = dec.priority >= 10 ? rules.ttl_p10_s : rules.ttl_p5_s;
                    switch (dec.action) {
                    case LiveActivity::Action::Update:
                        job.reqs.push_back(la_apns_request(d, t.activity_token, LiveActivity::aps_update(dec.content, now_s), dec.priority, ttl));
                        job.what.push_back("update");
                        break;
                    case LiveActivity::Action::End:
                        job.reqs.push_back(la_apns_request(d, t.activity_token, LiveActivity::aps_end(dec.content, now_s, dec.dismissal_s), 10, rules.ttl_p10_s));
                        job.what.push_back("end");
                        break;
                    case LiveActivity::Action::EndAndStart:
                        if (!t.activity_token.empty()) {
                            job.reqs.push_back(la_apns_request(d, t.activity_token, LiveActivity::aps_end(t.last, now_s, now_s), 10, rules.ttl_p10_s));
                            job.what.push_back("end");
                        }
                        [[fallthrough]];
                    case LiveActivity::Action::Start:
                        job.reqs.push_back(la_apns_request(d, d.la.start_token, LiveActivity::aps_start(dec.content, push_id(id), now_s), 10, rules.ttl_p10_s));
                        job.what.push_back("start");
                        break;
                    case LiveActivity::Action::None: break;
                    }
                    if (job.reqs.empty()) continue;
                }
                jobs.push_back(std::move(job));
            }
            // Tracks with nothing left in them: no token, no printer, nothing remembered.
            for (auto it = d.la_tracks.begin(); it != d.la_tracks.end();) {
                const LiveActivity::Track& t = it->second;
                const bool listed = by_id.count(it->first) > 0;
                if (!listed && t.activity_token.empty() && !t.has_sent && t.dismissed_job.empty()) it = d.la_tracks.erase(it);
                else ++it;
            }
        }
    }
    for (LaJob& job : jobs) la_enqueue(std::move(job));
}

// ------------------------------------------------------------------ the debug route ----

std::pair<int, std::string> debug_op(const std::string& body)
{
    if (!detail::debug_routes_on()) return { 404, json({ { "error", "not found" } }).dump() };
    json in;
    try {
        in = json::parse(body);
    } catch (...) {
        return { 400, json({ { "error", "the body must be JSON" } }).dump() };
    }
    const std::string op = in.value("op", "");
    if (op == "collapse") {
        return { 200, json({ { "collapse_id", collapse_for(in.value("printer", ""), in.value("kind", "")) } }).dump() };
    }
    if (op == "thread") {
        // What aps.thread-id / the hosted "thread" carries for this printer on this hub.
        return { 200, json({ { "thread_id", thread_for(in.value("printer", "")) } }).dump() };
    }
    if (op == "plaintext") {
        return { 200, json({ { "plaintext", plaintext_for(in.value("event", json::object())) } }).dump() };
    }
    if (op == "providers") {
        return { 200, providers_json().dump() };
    }
    if (op == "live_activity") {
        // What a Live Activity push for this printer row would carry right now: the gate checks it
        // names nothing (LiveActivityPush.hpp) without a mock APNs receiving one.
        const json      row = in.value("row", json::object());
        const long long now = in.value("now_ms", now_ms());
        const LiveActivity::Content c = LiveActivity::content_for(LiveActivity::row_from_json(row), now);
        return { 200, json({ { "update", LiveActivity::aps_update(c, now / 1000) },
                             { "start", LiveActivity::aps_start(c, push_id(row.value("id", "")), now / 1000) },
                             { "push_id", push_id(row.value("id", "")) },
                             { "progress", LiveActivity::progress_plaintext(row, now) } }).dump() };
    }
    return { 400, json({ { "error", "op must be collapse, thread, plaintext, providers or live_activity" } }).dump() };
}

// ------------------------------------------------------------------- lifecycle ----

void start(const json& saved)
{
    g_stopping = false;
    if (!g_apns) g_apns = make_apns_provider();
    if (!g_fcm) g_fcm = make_fcm_provider();
    if (!g_hosted) {
        g_hosted = Hosted::make_hosted_provider();
        // A queued notification's final outcome lands on its device row like any other result.
        g_hosted->set_result_sink([](const std::string& id, const PushResult& r) { record_by_id(id, r); });
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_devices.clear();
    g_apns_cfg = json::object();
    g_fcm_cfg  = json::object();
    g_mode.clear();
    g_hosted_cfg = json::object();
    g_id_key.clear();
    try {
        if (saved.is_object()) {
            g_id_key = saved.value("id_key", std::string());
            g_enabled      = saved.value("enabled", true);
            g_la_enabled   = saved.value("live_activity", true);
            g_min_severity = saved.value("min_severity", std::string("info"));
            // A stale kind in a hand-edited settings.json is dropped, never fatal.
            g_kinds.clear();
            if (saved.contains("kinds") && saved["kinds"].is_array())
                for (const auto& k : saved["kinds"])
                    if (k.is_string() && RemoteEvents::is_kind(k.get<std::string>())) g_kinds.push_back(k.get<std::string>());
            if (saved.contains("apns") && saved["apns"].is_object()) g_apns_cfg = saved["apns"];
            if (saved.contains("fcm") && saved["fcm"].is_object()) g_fcm_cfg = saved["fcm"];
            {
                const std::string m = saved.value("mode", std::string());
                g_mode = (m == "hosted" || m == "own") ? m : std::string();
            }
            if (saved.contains("hosted") && saved["hosted"].is_object()) {
                const std::string url = saved["hosted"].value("url", std::string());
                std::string       why;
                // A hand-edited URL that would not be accepted from the page is not accepted here.
                if (!url.empty() && Hosted::url_allowed(url, why)) g_hosted_cfg["url"] = url;
            }
            if (saved.contains("devices") && saved["devices"].is_array())
                for (const auto& e : saved["devices"]) {
                    Device d;
                    d.id       = e.value("id", "");
                    d.platform = e.value("platform", "");
                    d.env      = e.value("env", "production");
                    d.token    = e.value("token", "");
                    d.bundle   = e.value("bundle", "");
                    d.p256dh   = e.value("p256dh", "");
                    d.auth     = e.value("auth", "");
                    d.label    = e.value("label", "");
                    d.app      = e.value("app", "");
                    d.os       = e.value("os", "");
                    d.added    = e.value("added", 0LL);
                    d.last_sent   = e.value("last_sent", 0LL);
                    d.last_status = e.value("last_status", 0);
                    d.last_error  = e.value("last_error", "");
                    d.last_host   = e.value("last_host", "");
                    d.failures    = e.value("failures", 0);
                    if (e.contains("levels")) d.prefs = policy::read_prefs(e["levels"]);
                    if (e.contains("live_activity") && e["live_activity"].is_object()) {
                        const json& l = e["live_activity"];
                        d.la.enabled  = l.value("enabled", false);
                        d.la.frequent = l.value("frequent", false);
                        if (l.contains("start_token") && l["start_token"].is_string() && la_token_ok(l["start_token"].get<std::string>()))
                            d.la.start_token = l["start_token"].get<std::string>();
                        if (d.la.enabled && l.contains("activities") && l["activities"].is_array())
                            for (const auto& a : l["activities"]) {
                                const std::string printer = a.value("printer", "");
                                const std::string tok     = a.value("token", "");
                                if (printer.empty() || !la_token_ok(tok)) continue;
                                LiveActivity::Track& t = d.la_tracks[printer];
                                t.activity_token = tok;
                                t.job_key        = a.value("job", "");
                                t.started_ms     = a.value("started", 0LL);
                            }
                    }
                    if (d.id.empty()) d.id = random_id();
                    if ((d.platform == "apns" || d.platform == "fcm") && !d.token.empty() &&
                        !d.p256dh.empty() && !d.auth.empty())
                        g_devices.push_back(d);
                }
        }
    } catch (...) {} // a settings.json somebody hand-edited must not stop the hub starting
    // The opaque-id secret: minted the first time (or if a hand edit broke it) and written back with
    // the next settings.json. A new key only regroups notifications already on a phone's lock
    // screen; it never stops one being delivered.
    if (!PushIds::valid_key_hex(g_id_key)) {
        g_id_key = PushIds::new_key_hex();
        g_dirty  = true;
    }
    PushIds::set_key(g_id_key);
    g_hosted->set_identity(g_identity);
    reconfigure_locked();
    BOOST_LOG_TRIVIAL(info) << "AppPush: " << g_devices.size() << " registered device(s), push via "
                            << (hosted_mode_locked() ? "the hosted service (" + hosted_url_locked() + ")" : std::string("own keys"));
}

static void la_stop_worker();

void stop()
{
    g_stopping = true;
    la_stop_worker();
    // Stops the retry worker and forgets the queue: it holds device tokens and ciphertext, and a
    // hub that is quitting keeps neither. Outside g_mutex: the worker records a queued
    // notification's outcome under that lock, and this joins it.
    if (g_hosted) g_hosted->stop();
    std::lock_guard<std::mutex> lock(g_mutex);
    // Drops the cached provider credentials - the parsed .p8 and the OAuth2 access token - so
    // nothing sensitive outlives a hub that has been told to quit.
    if (g_apns) g_apns->configure("{}");
    if (g_fcm) g_fcm->configure("{}");
}

} // namespace AppPush
} // namespace GUI
} // namespace Slic3r
