// Personal-data scrubber for crash reports. See SentryScrub.hpp for what it removes and why
// it has no dependencies beyond the standard library.

#include "SentryScrub.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <string>

namespace Slic3r {
namespace {

using Re = std::regex;
const auto kFlags  = std::regex::ECMAScript | std::regex::optimize;
const auto kFlagsI = std::regex::ECMAScript | std::regex::optimize | std::regex::icase;

bool is_word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

std::string to_lower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ends_with(const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// An already-inserted marker such as <ip> or <redacted>: never scrub it a second time.
bool is_marker(const std::string& v) { return v.size() >= 3 && v.front() == '<' && v.back() == '>'; }

// Replaces every match of `re` in `s` by what `fn(match, s)` returns.
template<class Fn> std::string replace_each(const std::string& s, const Re& re, Fn&& fn)
{
    std::string out;
    out.reserve(s.size());
    size_t last = 0;
    for (auto it = std::sregex_iterator(s.begin(), s.end(), re); it != std::sregex_iterator(); ++it) {
        const std::smatch& m   = *it;
        const size_t       pos = static_cast<size_t>(m.position(0));
        out.append(s, last, pos - last);
        out += fn(m, s);
        last = pos + static_cast<size_t>(m.length(0));
    }
    out.append(s, last, std::string::npos);
    return out;
}

// ---------------------------------------------------------------------------------------------
// URLs

// Hosts that identify a public service, not the user's own network. Anything else in a URL
// (a printer's IP or name, a NAS, a Tailscale machine) is replaced by <host>.
bool is_public_host(std::string host)
{
    host = to_lower(host);
    if (host.size() > 1 && host.front() == '[' && host.back() == ']')
        host = host.substr(1, host.size() - 2);
    if (host == "localhost" || host == "127.0.0.1" || host == "::1" || host == "0.0.0.0")
        return true;
    static const char* const kPublic[] = {
        "github.com", "githubusercontent.com", "sentry.io", "bambulab.com", "bambulab.cn", "bblmw.com",
        "snapmaker.com", "snapmaker.cn", "orcaslicer.com", "flashforge.com", "prusa3d.com", "printables.com",
        "makerworld.com", "makerworld.com.cn", "thingiverse.com", "octoprint.org", "creality.com", "elegoo.com",
        "anycubic.com", "qidi3d.com", "microsoft.com", "apple.com", "google.com", "googleapis.com",
        "amazonaws.com", "aliyuncs.com", "cloudfront.net", "cloudflare.com", "tailscale.com",
    };
    for (const char* d : kPublic) {
        const std::string dom(d);
        if (host == dom || ends_with(host, "." + dom))
            return true;
    }
    return false;
}

// "?a=1&b=2" -> "?a=<redacted>&b=<redacted>". Query strings carry tokens, one-shot keys and
// device ids too often for an allow-list to be worth it.
std::string scrub_query(const std::string& q)
{
    std::string out;
    out.reserve(q.size());
    size_t i = 0;
    while (i < q.size()) {
        const size_t amp  = q.find('&', i + 1);
        const size_t stop = amp == std::string::npos ? q.size() : amp;
        const std::string part = q.substr(i, stop - i); // starts with '?' or '&'
        const size_t eq = part.find('=');
        if (eq == std::string::npos || eq + 1 >= part.size())
            out += part;
        else
            out += part.substr(0, eq + 1) + "<redacted>";
        i = stop;
    }
    return out;
}

std::string scrub_urls(const std::string& s)
{
    //                      1 scheme                       2 user:pass@           3 host
    static const Re re(R"(\b([A-Za-z][A-Za-z0-9+.\-]{1,15}://)([^\s/?#@"'<>\\]*@)?(\[[0-9A-Fa-f:.]+\]|[^\s/?#:@"'<>\[\]\\]+))"
                       //  4 port        5 path            6 query           7 fragment
                       R"((:[0-9]{1,5})?([^\s?#"'<>]*)(\?[^\s#"'<>]*)?(#[^\s"'<>]*)?)",
                       kFlags);
    return replace_each(s, re, [](const std::smatch& m, const std::string&) {
        std::string out = m[1].str();
        if (m[2].matched)
            out += "<redacted>@";
        const std::string host = m[3].str();
        out += (is_public_host(host) || is_marker(host)) ? host : std::string("<host>");
        out += m[4].str();
        out += m[5].str(); // the later passes still see the path (user names, serials, e-mail)
        if (m[6].matched)
            out += scrub_query(m[6].str());
        if (m[7].matched && m[7].length() > 1)
            out += "#<redacted>";
        return out;
    });
}

// ---------------------------------------------------------------------------------------------
// Authorisation values: "Bearer eyJ...", "Basic dXNlcjpwYXNz". A bare English word after
// "Basic" ("Basic settings") is left alone: a credential has a digit or a symbol in it.

std::string scrub_auth_schemes(const std::string& s)
{
    static const Re re(R"(\b(Bearer|Basic|Digest)(\s+)([A-Za-z0-9._~+/=:\-]{6,}))", kFlagsI);
    return replace_each(s, re, [](const std::smatch& m, const std::string&) {
        const std::string v = m[3].str();
        const bool looks_secret = std::any_of(v.begin(), v.end(), [](char c) {
            return std::isdigit(static_cast<unsigned char>(c)) || c == '=' || c == '+' || c == '/' || c == '.' ||
                   c == '_' || c == '-' || c == ':' || c == '~';
        });
        return looks_secret ? m[1].str() + m[2].str() + "<redacted>" : m.str(0);
    });
}

// ---------------------------------------------------------------------------------------------
// key = value, "key": "value", key: value, Header: value, --flag=value

// Key names whose values are secrets or identify a person, a device or a network location.
#define SCRUB_SECRET_KEYS                                                                                  \
    "(?:dev[-_]?)?access[-_ ]?code|check[-_ ]?code|pin[-_]?code"                                           \
    "|pass(?:word|wd|phrase)?|pwd"                                                                         \
    "|[A-Za-z0-9]*[-_]?(?:token|secret|passwd|password)"                                                  \
    "|private[-_]?key"                                                                                     \
    "|x[-_]api[-_]key|api[-_]?key|app[-_]?key"                                                             \
    "|authorization|set[-_]cookie|cookie|session[-_]?id"                                                   \
    "|dev(?:ice)?[-_]?(?:id|sn|serial|name|ip)|printer[-_]?(?:sn|serial)"                                  \
    "|serial(?:[-_]?(?:number|no|num))?|sn"                                                                \
    "|user(?:[-_]?(?:id|name))?|uid|account|nick[-_]?name|e[-_]?mail"                                      \
    "|wifi[-_]?(?:ssid|pwd|password)|ssid"                                                                 \
    "|host(?:[-_]?name)?|print[-_]?host|ip(?:[-_]?addr(?:ess)?)?|mac(?:[-_]?addr(?:ess)?)?"

bool value_runs_to_end_of_line(const std::string& key_lower)
{
    // These carry a scheme word and a credential ("Bearer x", "a=1; b=2"): take the lot.
    return key_lower.find("authorization") != std::string::npos || key_lower.find("cookie") != std::string::npos;
}

std::string scrub_key_values(const std::string& s)
{
    //                    boundary        1 key                            closing quote, separator
    static const Re re("(?:^|[^A-Za-z0-9_])-{0,2}(" SCRUB_SECRET_KEYS ")(?:\\\\?[\"'])?\\s*[:=]\\s*", kFlagsI);

    std::string out;
    out.reserve(s.size());
    size_t      pos = 0;
    std::smatch m;
    auto        begin = s.cbegin();
    // match_prev_avail: a search that resumes mid-line must still see the character before it,
    // or "^" would let a key glued to a previous word ("xuser=") count as a key.
    while (pos < s.size() &&
           std::regex_search(begin + pos, s.cend(), m, re,
                             pos > 0 ? std::regex_constants::match_prev_avail : std::regex_constants::match_default)) {
        const size_t match_end = pos + static_cast<size_t>(m.position(0) + m.length(0));
        const std::string key  = to_lower(m[1].str());

        // Find the value that follows the separator.
        size_t      v_begin = match_end, v_end = match_end;
        std::string open, close;
        if (v_begin + 1 < s.size() && s[v_begin] == '\\' && s[v_begin + 1] == '"') {
            open = close = "\\\"";
        } else if (v_begin < s.size() && (s[v_begin] == '"' || s[v_begin] == '\'')) {
            open = close = std::string(1, s[v_begin]);
        }
        if (!open.empty()) {
            v_begin += open.size();
            const size_t c = s.find(close, v_begin);
            v_end          = c == std::string::npos ? s.size() : c;
        } else if (value_runs_to_end_of_line(key)) {
            v_end = s.find_first_of("\r\n\"", v_begin);
            if (v_end == std::string::npos)
                v_end = s.size();
        } else {
            v_end = v_begin;
            while (v_end < s.size()) {
                const char c = s[v_end];
                if (std::isspace(static_cast<unsigned char>(c)) || c == '"' || c == '\'' || c == ',' || c == ';' ||
                    c == '&' || c == '}' || c == ']' || c == ')' || c == '<' || c == '>' || c == '\\')
                    break;
                ++v_end;
            }
        }

        const std::string value = s.substr(v_begin, v_end - v_begin);
        const bool skip = value.empty() || is_marker(value) || (open.empty() && (value[0] == '=' || value[0] == ':'));
        out.append(s, pos, v_begin - pos);
        if (skip) {
            pos = v_begin; // always past the key, so the loop makes progress
        } else {
            out += "<redacted>";
            pos = v_end;
        }
    }
    out.append(s, std::min(pos, s.size()), std::string::npos);
    return out;
}

// "--hub-token 3f9a...", "password 12345678", "access code 12345678": a secret after a space.
// Flags are always redacted; a bare word is redacted only when it looks like a secret (has a
// digit), so "token expired" stays readable.
std::string scrub_space_separated_secrets(const std::string& s)
{
    static const Re re(R"((^|[^A-Za-z0-9_\-])(--)?)"
                       R"((pass(?:word|wd)?|access[-_ ]?code|check[-_ ]?code|(?:hub|api|access|auth|upload)?[-_]?token|api[-_ ]?key|serial(?:[-_ ]?number)?|dev[-_]?id))"
                       R"((\s+)([^\s"',;]+))",
                       kFlagsI);
    return replace_each(s, re, [](const std::smatch& m, const std::string&) {
        const std::string value   = m[5].str();
        const bool        is_flag = m[2].matched && m[2].length() == 2;
        const bool        has_digit =
            std::any_of(value.begin(), value.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
        if (is_marker(value) || value[0] == '-' || (!is_flag && (!has_digit || value.size() < 4)))
            return m.str(0);
        return m[1].str() + m[2].str() + m[3].str() + m[4].str() + "<redacted>";
    });
}

// ---------------------------------------------------------------------------------------------
// Standalone identifiers

std::string scrub_jwts(const std::string& s)
{
    static const Re re(R"(\beyJ[A-Za-z0-9_\-]{5,}\.[A-Za-z0-9_\-]{5,}\.[A-Za-z0-9_\-]*)", kFlags);
    return std::regex_replace(s, re, "<token>");
}

std::string scrub_emails(const std::string& s)
{
    static const Re re(R"([A-Za-z0-9._%+\-]+@[A-Za-z0-9\-]+(?:\.[A-Za-z0-9\-]+)*\.[A-Za-z]{2,})", kFlags);
    return std::regex_replace(s, re, "<email>");
}

bool is_generic_profile_dir(const std::string& name)
{
    const std::string n = to_lower(name);
    return n == "public" || n == "default" || n == "default user" || n == "all users" || n == "shared" || is_marker(name);
}

// C:\Users\<name>\..., C:\\Users\\<name>\\... (JSON-escaped), /Users/<name>/..., /home/<name>/...
std::string scrub_user_paths(const std::string& s)
{
    // A name followed by another separator may contain spaces ("John Smith").
    static const Re with_sep(R"(([\\/]{1,2})(Users|home|Documents and Settings)([\\/]{1,2})([^\\/:*?"<>|\r\n\t]{1,64}?)(?=[\\/]))",
                             kFlagsI);
    // A name that ends the path stops at white space.
    static const Re at_end(R"(([\\/]{1,2})(Users|home)([\\/]{1,2})([^\\/:*?"<>|\s]{1,64}))", kFlagsI);
    auto fn = [](const std::smatch& m, const std::string&) {
        if (is_generic_profile_dir(m[4].str()))
            return m.str(0);
        return m[1].str() + m[2].str() + m[3].str() + "<user>";
    };
    return replace_each(replace_each(s, with_sep, fn), at_end, fn);
}

// Bambu MQTT topics: device/<serial>/report, device/<serial>/request
std::string scrub_mqtt_topics(const std::string& s)
{
    static const Re re(R"(\bdevice/([A-Za-z0-9]{6,32})/(report|request))", kFlags);
    return std::regex_replace(s, re, "device/<serial>/$2");
}

std::string scrub_uuids(const std::string& s)
{
    static const Re re(R"(\b[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\b)", kFlags);
    return std::regex_replace(s, re, "<uuid>");
}

std::string scrub_macs(const std::string& s)
{
    static const Re re(R"(\b[0-9A-Fa-f]{2}([:\-])[0-9A-Fa-f]{2}(?:\1[0-9A-Fa-f]{2}){4}\b)", kFlags);
    return std::regex_replace(s, re, "<mac>");
}

// Printer serial numbers: Bambu "01P00A451601234", FlashForge "SNMQRE9400123", and the like -
// 12 to 20 upper-case letters and digits with at least one letter and six digits.
std::string scrub_serials(const std::string& s)
{
    static const Re re(R"(\b[A-Z0-9]{12,20}\b)", kFlags);
    return replace_each(s, re, [](const std::smatch& m, const std::string&) {
        const std::string v = m.str(0);
        const auto digits   = std::count_if(v.begin(), v.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
        const auto letters  = static_cast<long>(v.size()) - digits;
        return (letters >= 1 && digits >= 6) ? std::string("<serial>") : v;
    });
}

std::string scrub_ipv4(const std::string& s)
{
    static const Re re(R"(\b(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)(?:\.(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)){3}\b)", kFlags);
    return replace_each(s, re, [](const std::smatch& m, const std::string& src) {
        const std::string ip  = m.str(0);
        const size_t      pos = static_cast<size_t>(m.position(0));
        const size_t      end = pos + ip.size();
        // Part of a longer dotted number (1.2.3.4.5): not an address.
        if ((pos > 0 && (src[pos - 1] == '.' || std::isdigit(static_cast<unsigned char>(src[pos - 1])))) ||
            (end + 1 < src.size() && src[end] == '.' && std::isdigit(static_cast<unsigned char>(src[end + 1]))))
            return ip;
        if (ip == "127.0.0.1" || ip == "0.0.0.0" || ip == "255.255.255.255" || ip == "255.255.255.0")
            return ip;
        // Version numbers look exactly like addresses: v2.4.0.0, EdgeSlicer/2.4.0.0, "version 1.2.3.4".
        if (pos > 0 && (src[pos - 1] == 'v' || src[pos - 1] == 'V' || src[pos - 1] == '/'))
            return ip;
        const size_t      ctx_from = pos > 24 ? pos - 24 : 0;
        const std::string before   = to_lower(src.substr(ctx_from, pos - ctx_from));
        for (const char* word : {"version", "ver ", "ver:", "firmware", "fw", "edgeslicer", "slicer", "build", "release", "plugin"})
            if (before.find(word) != std::string::npos)
                return ip;
        return std::string("<ip>");
    });
}

std::string scrub_ipv6(const std::string& s)
{
    static const Re re(R"((^|[^0-9A-Za-z_:])((?:[0-9A-Fa-f]{0,4}:){2,7}[0-9A-Fa-f]{0,4})(?![0-9A-Za-z_:]))", kFlags);
    return replace_each(s, re, [](const std::smatch& m, const std::string&) {
        const std::string cand   = m[2].str();
        const auto        colons = std::count(cand.begin(), cand.end(), ':');
        const auto        hex    = std::count_if(cand.begin(), cand.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); });
        const bool        compressed = cand.find("::") != std::string::npos;
        // "::1" is loopback; timestamps (12:34:56) have neither "::" nor seven colons.
        if (hex < 3 || !(compressed || colons == 7))
            return m.str(0);
        return m[1].str() + "<ip>";
    });
}

// LAN and overlay-network host names outside URLs.
std::string scrub_lan_hostnames(const std::string& s)
{
    static const Re re(R"(\b[A-Za-z0-9][A-Za-z0-9\-]{0,62}(?:\.[A-Za-z0-9\-]{1,63})*\.(?:local|lan|home|localdomain|internal|intranet|home\.arpa|ts\.net|fritz\.box)\b)",
                       kFlagsI);
    return std::regex_replace(s, re, "<host>");
}

// Long random-looking tokens (hex keys, base64 chunks) that no rule above named.
std::string scrub_long_tokens(const std::string& s)
{
    static const Re re(R"([A-Za-z0-9_+\-]{32,}={0,2})", kFlags);
    return replace_each(s, re, [](const std::smatch& m, const std::string& src) {
        const std::string v   = m.str(0);
        const size_t      pos = static_cast<size_t>(m.position(0));
        // Part of a longer word, or an MSVC lambda name (<lambda_1c5a...>) in a __FUNCTION__.
        if ((pos > 0 && is_word_char(src[pos - 1])) || v.rfind("lambda_", 0) == 0)
            return v;
        const auto digits  = std::count_if(v.begin(), v.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
        const auto letters = std::count_if(v.begin(), v.end(), [](char c) { return std::isalpha(static_cast<unsigned char>(c)); });
        const auto seps    = std::count_if(v.begin(), v.end(), [](char c) { return c == '_' || c == '-'; });
        return (digits >= 3 && letters >= 3 && seps <= 4) ? std::string("<token>") : v;
    });
}

std::string truncate_utf8(const std::string& s, size_t max_len)
{
    if (max_len == 0 || s.size() <= max_len)
        return s;
    size_t cut = max_len;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
        --cut; // do not split a UTF-8 sequence
    return s.substr(0, cut) + "...[truncated]";
}

} // namespace

std::string scrub_for_crash_report(const std::string& text, size_t max_len)
{
    std::string s = truncate_utf8(text, max_len);
    try {
        s = scrub_urls(s);
        s = scrub_auth_schemes(s);
        s = scrub_key_values(s);
        s = scrub_space_separated_secrets(s);
        s = scrub_jwts(s);
        s = scrub_emails(s);
        s = scrub_user_paths(s);
        s = scrub_mqtt_topics(s);
        s = scrub_uuids(s);
        s = scrub_macs(s);
        s = scrub_serials(s);
        s = scrub_ipv4(s);
        s = scrub_ipv6(s);
        s = scrub_lan_hostnames(s);
        s = scrub_long_tokens(s);
    } catch (const std::exception&) {
        // std::regex can throw (error_complexity / error_stack) on pathological input. Sending
        // nothing is always safe; sending a half-scrubbed line is not.
        return "<unscrubbable line removed>";
    }
    return s;
}

} // namespace Slic3r
