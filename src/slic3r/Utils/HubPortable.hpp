#ifndef slic3r_HubPortable_hpp_
#define slic3r_HubPortable_hpp_

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

// The small, pure rules the hub needs to run somewhere other than a Windows desktop, kept free of
// wx, sockets and the file system so tests/slic3rutils/hub_portable_tests.cpp can drive them with
// captured text:
//   * the advertised LAN address (EDGESLICER_HUB_PUBLIC_HOST), for a container whose own address
//     is not the one a phone can reach;
//   * where a tool lives on PATH (ffmpeg on Linux), and what its file is called per platform;
//   * which H.264 encoder a system ffmpeg offers, and the go2rtc template to use for it.
namespace Slic3r {
namespace HubPortable {

// ---- the advertised host --------------------------------------------------------------------

struct PublicHost
{
    std::string host;  // a name or an address, never with a scheme, path or brackets
    int         port { 0 }; // 0 = "keep the port the hub is bound to"
    bool        valid() const { return !host.empty(); }
};

inline bool host_char_ok(char c)
{
    return std::isalnum((unsigned char) c) || c == '.' || c == '-' || c == '_' || c == ':';
}

// What EDGESLICER_HUB_PUBLIC_HOST (or --hub-public-host) may say: `host`, `host:port`, `[v6]:port`,
// or an http(s) URL whose host and port are taken and whose path is ignored. Anything that is not
// plainly a host (spaces, credentials, a query, an empty name, a port outside 1..65535) is
// refused as a whole and yields an invalid PublicHost: a typo must not turn into a link the phone
// can never open and nobody can see why.
inline PublicHost parse_public_host(const std::string& raw)
{
    PublicHost out;
    std::string s;
    {
        size_t b = 0, e = raw.size();
        while (b < e && std::isspace((unsigned char) raw[b])) ++b;
        while (e > b && std::isspace((unsigned char) raw[e - 1])) --e;
        s = raw.substr(b, e - b);
    }
    auto starts = [&](const char* p) {
        const std::string t(p);
        return s.size() >= t.size() && std::equal(t.begin(), t.end(), s.begin(), [](char a, char b) { return std::tolower((unsigned char) a) == std::tolower((unsigned char) b); });
    };
    if (starts("http://")) s.erase(0, 7);
    else if (starts("https://")) s.erase(0, 8);
    const size_t slash = s.find('/');
    if (slash != std::string::npos) s.erase(slash);
    if (s.empty() || s.size() > 260) return out;
    if (s.find_first_of(" \t\r\n@?#\\") != std::string::npos) return out;

    std::string host, port_text;
    if (s.front() == '[') {
        const size_t close = s.find(']');
        if (close == std::string::npos) return out;
        host = s.substr(1, close - 1);
        if (close + 1 < s.size()) {
            if (s[close + 1] != ':') return out;
            port_text = s.substr(close + 2);
            if (port_text.empty()) return out;
        }
        if (host.find(':') == std::string::npos) return out; // brackets are for IPv6 literals
    } else {
        const size_t first = s.find(':');
        if (first == std::string::npos) host = s;
        else if (s.find(':', first + 1) == std::string::npos) { host = s.substr(0, first); port_text = s.substr(first + 1); if (port_text.empty()) return out; }
        else host = s; // several colons without brackets: an IPv6 literal with no port
    }
    if (host.empty() || host.size() > 253) return out;
    for (char c : host)
        if (!host_char_ok(c)) return out;
    if (host.front() == '.' || host.front() == '-' || host.back() == '.' || host.back() == '-') return out;
    int port = 0;
    if (!port_text.empty()) {
        for (char c : port_text)
            if (!std::isdigit((unsigned char) c)) return out;
        if (port_text.size() > 5) return out;
        port = std::atoi(port_text.c_str());
        if (port < 1 || port > 65535) return out;
    }
    out.host = host;
    out.port = port;
    return out;
}

// The host as it goes between "http://" and ":port": an IPv6 literal needs its brackets.
inline std::string url_host(const std::string& host)
{
    return host.find(':') != std::string::npos ? "[" + host + "]" : host;
}

// The addresses to offer, best first: the operator's choice (when there is one), then whatever the
// hub found on its own interfaces, with duplicates dropped.
inline std::vector<std::string> advertised_hosts(const PublicHost& override_host, std::vector<std::string> detected)
{
    std::vector<std::string> out;
    if (override_host.valid()) out.push_back(override_host.host);
    for (std::string& d : detected)
        if (std::find(out.begin(), out.end(), d) == out.end()) out.push_back(std::move(d));
    return out;
}

// The port a phone should use: the operator's (a published container port) or the bound one.
inline int advertised_port(const PublicHost& override_host, int bound_port)
{
    return override_host.valid() && override_host.port > 0 ? override_host.port : bound_port;
}

inline std::string lan_url(const std::string& host, int port, const std::string& token)
{
    return "http://" + url_host(host) + ":" + std::to_string(port) + "/r/" + token + "/";
}

// ---- finding a tool ---------------------------------------------------------------------------

// The first directory of `path_env` (split on `sep`) that holds an executable called `name`,
// joined as "<dir>/<name>"; "" when none does. `is_executable` is the file system, injected.
inline std::string find_in_path(const std::string& name, const std::string& path_env, char sep, const std::function<bool(const std::string&)>& is_executable)
{
    if (name.empty() || name.find_first_of("/\\") != std::string::npos) return "";
    size_t pos = 0;
    while (pos <= path_env.size()) {
        size_t end = path_env.find(sep, pos);
        if (end == std::string::npos) end = path_env.size();
        std::string dir = path_env.substr(pos, end - pos);
        pos             = end + 1;
        if (dir.empty() || dir.front() != '/') continue; // a relative or empty entry is not a place to run things from
        while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
        const std::string candidate = (dir == "/" ? "" : dir) + "/" + name;
        if (is_executable(candidate)) return candidate;
    }
    return "";
}

// "go2rtc" -> "go2rtc.exe" on Windows, "go2rtc" elsewhere.
inline std::string tool_file_name(const std::string& base, bool windows)
{
    return windows ? base + ".exe" : base;
}

// ---- which H.264 encoder a system ffmpeg has ----------------------------------------------------

enum class H264Encoder { None, LibX264, LibOpenH264 };

// `ffmpeg -hide_banner -encoders` prints one line per encoder: " V....D libx264   libx264 H.264 ...".
// The encoder name is the second token of a line whose first token is six flag characters starting
// with V (video). libx264 wins when both exist (it is what go2rtc's own template is written for);
// the bundled LGPL build has only libopenh264.
inline H264Encoder pick_h264_encoder(const std::string& encoders_text)
{
    bool x264 = false, openh264 = false;
    size_t pos = 0;
    while (pos < encoders_text.size()) {
        size_t nl = encoders_text.find('\n', pos);
        if (nl == std::string::npos) nl = encoders_text.size();
        std::string line = encoders_text.substr(pos, nl - pos);
        pos              = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t i = 0;
        while (i < line.size() && line[i] == ' ') ++i;
        const size_t flags_at = i;
        while (i < line.size() && line[i] != ' ') ++i;
        if (i - flags_at != 6 || line[flags_at] != 'V') continue;
        while (i < line.size() && line[i] == ' ') ++i;
        const size_t name_at = i;
        while (i < line.size() && line[i] != ' ') ++i;
        const std::string name = line.substr(name_at, i - name_at);
        if (name == "libx264") x264 = true;
        else if (name == "libopenh264") openh264 = true;
    }
    return x264 ? H264Encoder::LibX264 : openh264 ? H264Encoder::LibOpenH264 : H264Encoder::None;
}

// go2rtc's `ffmpeg: h264:` template for an encoder. The openh264 one is the template the bundled
// LGPL build has always used (RemoteHub.cpp explains each flag); the x264 one is go2rtc's built-in.
inline std::string h264_template(H264Encoder e)
{
    switch (e) {
    case H264Encoder::LibX264:
        return "-codec:v libx264 -g:v 30 -preset:v superfast -tune:v zerolatency -profile:v main -level:v 4.1";
    case H264Encoder::LibOpenH264:
        return "-codec:v libopenh264 -profile:v constrained_baseline -rc_mode bitrate -bf 0";
    default:
        return "";
    }
}

} // namespace HubPortable
} // namespace Slic3r

#endif // slic3r_HubPortable_hpp_
