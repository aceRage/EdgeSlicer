#pragma once

// The hub's per-client access rules - pure, wx-free and clock-free, so RemoteHub and the unit
// tests run exactly the same code.
//
// Before this, one token (the phone link) opened everything the hub can do: read, cameras,
// printer control, uploads and prints, project edits, quitting slicer windows. Now every client
// can get a token of its own that carries a set of scopes:
//
//   status   read: the device list, events, printer rows, archive, job progress, notifications
//   camera   the camera streams and timelapses
//   control  printer controls (pause, stop, lights, temperatures ...), slicer windows, archive
//            deletes and everything else that changes something and is not a print
//   print    upload a file, slice, choose presets, send / reprint a job
//
// The phone link the hub always had stays what it was - every scope - and is called the legacy
// client; it is rotated with "New link" as before. Each other client is revoked on its own.
//
// route_scope() maps a request on the phone listener (the part after /r/<token>, or after /h when
// the token comes in an Authorization header) to the one scope it needs. A route nobody listed
// needs `status` to read and `control` to change anything, so a new route defaults to the safe
// side. SCOPE_ANY marks the page furniture every client needs whatever it may do (the page, its
// manifest and icons, /state and /pair).

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace HubClients {

enum : unsigned {
    SCOPE_NONE    = 0,
    SCOPE_STATUS  = 1,
    SCOPE_CAMERA  = 2,
    SCOPE_CONTROL = 4,
    SCOPE_PRINT   = 8,
    SCOPE_FULL    = 15,
    SCOPE_ANY     = 16, // a route marker only: any client with at least one scope
};

inline const std::vector<std::pair<const char*, unsigned>>& scope_table()
{
    static const std::vector<std::pair<const char*, unsigned>> t = {
        { "status", SCOPE_STATUS }, { "camera", SCOPE_CAMERA }, { "control", SCOPE_CONTROL }, { "print", SCOPE_PRINT }
    };
    return t;
}

// "status,camera" / "full" -> mask. Unknown names are ignored (an empty result means "nothing").
inline unsigned parse_scopes(const std::string& csv)
{
    unsigned    mask = 0;
    std::string cur;
    auto        flush = [&]() {
        if (cur == "full" || cur == "all") mask |= SCOPE_FULL;
        for (const auto& s : scope_table())
            if (cur == s.first) mask |= s.second;
        cur.clear();
    };
    for (char c : csv) {
        if (c == ',' || c == ' ' || c == '+') flush();
        else cur += (char) ((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    }
    flush();
    return mask;
}

inline unsigned parse_scope_list(const nlohmann::json& arr)
{
    unsigned mask = 0;
    if (!arr.is_array()) return 0;
    for (const auto& v : arr)
        if (v.is_string()) mask |= parse_scopes(v.get<std::string>());
    return mask;
}

inline std::vector<std::string> scope_names(unsigned mask)
{
    std::vector<std::string> out;
    for (const auto& s : scope_table())
        if (mask & s.second) out.emplace_back(s.first);
    return out;
}

inline bool allows(unsigned client_scopes, unsigned needed)
{
    if (needed == SCOPE_ANY) return (client_scopes & SCOPE_FULL) != 0;
    if (needed == SCOPE_NONE) return false;
    return (client_scopes & needed) == needed;
}

// The scope a request on the phone listener needs. `rest` starts with '/' (or is empty).
inline unsigned route_scope(const std::string& method, const std::string& rest)
{
    const bool get    = method == "GET" || method == "HEAD";
    auto       starts = [&rest](const char* p) { return rest.compare(0, std::strlen(p), p) == 0; };
    auto       ends   = [](const std::string& s, const char* p) {
        const size_t n = std::strlen(p);
        return s.size() >= n && s.compare(s.size() - n, n, p) == 0;
    };
    // The page and its furniture: every client needs these, whatever it may do.
    if (rest.empty() || rest == "/" || rest == "/index.html" || rest == "/state" || rest == "/pair" || rest == "/pair/" ||
        rest == "/manifest.webmanifest" || rest == "/sw.js" || rest == "/ping.gif" || rest == "/icon-192.png" ||
        rest == "/icon-512.png" || rest == "/icon-512-maskable.png" || rest == "/apple-touch-icon.png")
        return get ? SCOPE_ANY : SCOPE_CONTROL;
    if (rest == "/bambu" || rest == "/ff") return SCOPE_CAMERA;
    // Registering this device for notifications is part of reading the printers' state.
    if (starts("/push/")) return SCOPE_STATUS;
    if (rest == "/api/instances/open") return SCOPE_PRINT;
    if (starts("/i/")) {
        const size_t      slash = rest.find('/', 3);
        const std::string sub   = slash == std::string::npos ? std::string() : rest.substr(slash);
        if (sub == "/open") return SCOPE_PRINT;
        if (sub.compare(0, 4, "/api") == 0) {
            if (get) {
                if (sub.compare(0, 14, "/api/printers/") == 0 && sub.find("/timelapses") != std::string::npos) return SCOPE_CAMERA;
                return SCOPE_STATUS;
            }
            if (sub.compare(0, 14, "/api/printers/") == 0 && ends(sub, "/control")) return SCOPE_CONTROL;
            if (sub.compare(0, 12, "/api/plates/") == 0 && ends(sub, "/send")) return SCOPE_PRINT;
            if (sub.compare(0, 13, "/api/archive/") == 0) {
                if (method == "POST" && (ends(sub, "/send") || ends(sub, "/preview"))) return SCOPE_PRINT;
                return SCOPE_CONTROL; // delete (POST .../delete or DELETE)
            }
            if (sub == "/api/slice" || sub == "/api/project/open" || sub == "/api/objects/transform" ||
                sub.compare(0, 13, "/api/presets/") == 0 || sub.compare(0, 21, "/api/settings/process") == 0)
                return SCOPE_PRINT;
            return SCOPE_CONTROL; // window, quit, attention, snapmaker add/remove/connect, anything new
        }
        return get ? SCOPE_STATUS : SCOPE_CONTROL;
    }
    return get ? SCOPE_STATUS : SCOPE_CONTROL;
}

// A token bucket: `capacity` requests at once, refilled at `per_second`. take() spends one.
struct Bucket
{
    double    tokens { -1 }; // < 0: never used, starts full
    long long last_ms { 0 };
};

inline bool take(Bucket& b, double capacity, double per_second, long long now_ms)
{
    if (b.tokens < 0) { b.tokens = capacity; b.last_ms = now_ms; }
    if (now_ms > b.last_ms) {
        b.tokens  = std::min(capacity, b.tokens + (double) (now_ms - b.last_ms) / 1000.0 * per_second);
        b.last_ms = now_ms;
    }
    if (b.tokens < 1.0) return false;
    b.tokens -= 1.0;
    return true;
}

// Wrong tokens from one source: `limit` failures inside `window_ms` blocks that source for
// `block_ms` (every request from it answers 429 meanwhile, right token or not).
struct FailWindow
{
    int       count { 0 };
    long long window_start_ms { 0 };
    long long blocked_until_ms { 0 };
};

inline bool blocked(const FailWindow& f, long long now_ms) { return now_ms < f.blocked_until_ms; }

inline void note_failure(FailWindow& f, long long now_ms, int limit, long long window_ms, long long block_ms)
{
    if (now_ms - f.window_start_ms > window_ms) { f.count = 0; f.window_start_ms = now_ms; }
    if (++f.count >= limit) { f.blocked_until_ms = now_ms + block_ms; f.count = 0; f.window_start_ms = now_ms; }
}

// One client as settings.json keeps it. The token itself is never stored: only its SHA-256
// (hex), so a copy of the settings file does not hand out working tokens. The legacy phone link
// is not one of these (it is the hub's m_token, shown on the hub page as a QR code).
struct Client
{
    std::string id;     // "c" + 12 hex; what the hub page revokes by
    std::string name;   // what its owner called it ("Workshop PC", "Mum's phone")
    std::string hash;   // hex SHA-256 of the token
    unsigned    scopes { SCOPE_NONE };
    bool        tagged_ok { false }; // may come through Tailscale Serve from a tagged device (no login)
    long long   created_at { 0 }, last_used_at { 0 };
};

inline nlohmann::json to_settings(const Client& c)
{
    return nlohmann::json{ { "id", c.id },
                           { "name", c.name },
                           { "hash", c.hash },
                           { "scopes", scope_names(c.scopes) },
                           { "tagged_ok", c.tagged_ok },
                           { "created_at", c.created_at },
                           { "last_used_at", c.last_used_at } };
}

// What the hub page sees: everything but the hash.
inline nlohmann::json to_public(const Client& c)
{
    nlohmann::json j = to_settings(c);
    j.erase("hash");
    j["legacy"] = false;
    return j;
}

inline bool valid_hash(const std::string& h)
{
    return h.size() == 64 && h.find_first_not_of("0123456789abcdef") == std::string::npos;
}

// A malformed entry (no id, no hash, no scopes) comes back with an empty id: skip it.
inline Client from_settings(const nlohmann::json& j)
{
    Client c;
    if (!j.is_object()) return c;
    c.id           = j.value("id", std::string());
    c.name         = j.value("name", std::string());
    c.hash         = j.value("hash", std::string());
    c.scopes       = parse_scope_list(j.value("scopes", nlohmann::json::array()));
    c.tagged_ok    = j.value("tagged_ok", false);
    c.created_at   = j.value("created_at", 0LL);
    c.last_used_at = j.value("last_used_at", 0LL);
    if (c.id.empty() || c.id.size() > 40 || !valid_hash(c.hash) || c.scopes == SCOPE_NONE) c.id.clear();
    return c;
}

// A client's name as typed on the hub page: printable, trimmed, at most 60 bytes, no markup.
inline std::string clean_name(const std::string& in)
{
    std::string out;
    for (unsigned char ch : in) {
        if (ch < 0x20 || ch == 0x7f || ch == '<' || ch == '>' || ch == '"' || ch == '\\') continue;
        out += (char) ch;
    }
    const size_t b = out.find_first_not_of(' ');
    if (b == std::string::npos) return {};
    out = out.substr(b, out.find_last_not_of(' ') - b + 1);
    if (out.size() > 60) {
        out.resize(60);
        while (!out.empty() && ((unsigned char) out.back() & 0xC0) == 0x80) out.pop_back(); // never split a UTF-8 sequence
        if (!out.empty() && ((unsigned char) out.back() & 0x80)) out.pop_back();
    }
    return out;
}

// Which client a hashed token belongs to, without an early exit: every stored hash is compared
// in full, so the time taken says nothing about which (or whether) one matched. -1 = none.
inline int find_by_hash(const std::vector<Client>& clients, const std::string& hash)
{
    int found = -1;
    if (!valid_hash(hash)) return -1;
    for (size_t i = 0; i < clients.size(); ++i) {
        unsigned char diff = clients[i].hash.size() == hash.size() ? 0 : 1;
        for (size_t k = 0; k < hash.size() && k < clients[i].hash.size(); ++k) diff |= (unsigned char) (clients[i].hash[k] ^ hash[k]);
        if (diff == 0 && found < 0) found = (int) i;
    }
    return found;
}

}}} // namespace Slic3r::GUI::HubClients
