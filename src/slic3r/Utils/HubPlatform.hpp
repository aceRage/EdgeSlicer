#pragma once

// The pieces of the remote hub (RemoteHub.cpp) that used to exist on Windows only, in a form that
// can be driven from tests on any machine: which go2rtc / ffmpeg to run, what the macOS and Linux
// firewalls say, who holds a port, which interface owns the default route, what `tailscale serve`
// printed when it was waiting for the tailnet admin to switch Serve on.
//
// Everything above the "POSIX process and socket helpers" line is pure text-in, text-out (or
// platform-in, answer-out) so tests/slic3rutils/hub_platform_tests.cpp can feed it captured output.
// The helpers below that line touch the OS and exist off Windows only; Windows keeps its own
// implementations in RemoteHub.cpp and none of this changes what they do.

#include "HubAddresses.hpp"
#include "TailscaleCli.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#  include <cerrno>
#  include <chrono>
#  include <csignal>
#  include <cstring>
#  include <dirent.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <thread>
#  include <unistd.h>
#  ifdef __APPLE__
#    include <libproc.h>
#  endif
#endif

namespace Slic3r {
namespace HubPlatform {

using TailscaleCli::Platform;

namespace detail {
inline std::string lower(std::string s)
{
    for (char& c : s) c = (char) std::tolower((unsigned char) c);
    return s;
}
inline std::string trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char) s[b])) ++b;
    while (e > b && std::isspace((unsigned char) s[e - 1])) --e;
    return s.substr(b, e - b);
}
inline std::vector<std::string> split_lines(const std::string& text)
{
    std::vector<std::string> v;
    std::istringstream       is(text);
    std::string              line;
    while (std::getline(is, line)) {
        while (!line.empty() && (line.back() == '\r')) line.pop_back();
        v.push_back(line);
    }
    return v;
}
inline std::vector<std::string> split_ws(const std::string& line)
{
    std::vector<std::string> v;
    std::istringstream       is(line);
    std::string              w;
    while (is >> w) v.push_back(w);
    return v;
}
} // namespace detail

// ---------------------------------------------------------------------------------------------
// go2rtc and ffmpeg: where they are looked for
// ---------------------------------------------------------------------------------------------

// The bundled copy first (CMake / build_release_macos.sh install it under <resources>/tools/go2rtc),
// then the places a package manager puts them, then - done by the caller - PATH. A GUI app on
// macOS gets a minimal PATH, so Homebrew's prefix is named outright. Windows has only the bundled
// .exe and no search, exactly as before.
inline std::vector<std::string> tool_candidates(Platform p, const std::string& resources_dir, const std::string& name)
{
    if (p == Platform::Windows) return { resources_dir + "/tools/go2rtc/" + name + ".exe" };
    std::vector<std::string> v = { resources_dir + "/tools/go2rtc/" + name };
    if (p == Platform::MacOS) v.push_back("/opt/homebrew/bin/" + name);
    v.push_back("/usr/local/bin/" + name);
    v.push_back("/usr/bin/" + name);
    return v;
}
inline std::vector<std::string> go2rtc_candidates(Platform p, const std::string& resources_dir) { return tool_candidates(p, resources_dir, "go2rtc"); }
inline std::vector<std::string> ffmpeg_candidates(Platform p, const std::string& resources_dir) { return tool_candidates(p, resources_dir, "ffmpeg"); }

inline std::string pick_first_existing(const std::vector<std::string>& candidates, const std::function<bool(const std::string&)>& exists)
{
    for (const std::string& c : candidates)
        if (exists && exists(c)) return c;
    return std::string();
}

// The H.264 software encoder an ffmpeg build offers, from `ffmpeg -hide_banner -encoders`. The
// bundled LGPL build has libopenh264 only; a system build (Homebrew, apt) normally has libx264.
// OpenH264 wins when both are there because the hub's own encoder template is written for it.
//
// On macOS the hardware encoder (VideoToolbox, always present in Homebrew's ffmpeg) is preferred over
// both: it costs next to no CPU. Elsewhere hardware encoders are left alone (they fail in ways
// software encoding does not: no GPU in a headless session, a driver that refuses a second session).
enum class H264Encoder { None, OpenH264, X264, VideoToolbox };

inline H264Encoder choose_h264_encoder(const std::string& ffmpeg_encoders_text, Platform p = TailscaleCli::current_platform())
{
    bool openh264 = false, x264 = false, videotoolbox = false;
    for (const std::string& line : detail::split_lines(ffmpeg_encoders_text)) {
        const std::vector<std::string> w = detail::split_ws(line);
        // A row is "<6 flag characters> <name> <description...>"; the heading lines are not.
        if (w.size() < 2 || w[0].size() != 6 || (w[0][0] != 'V' && w[0][0] != 'A' && w[0][0] != 'S')) continue;
        if (w[1] == "libopenh264") openh264 = true;
        else if (w[1] == "libx264") x264 = true;
        else if (w[1] == "h264_videotoolbox") videotoolbox = true;
    }
    if (videotoolbox && p == Platform::MacOS) return H264Encoder::VideoToolbox;
    return openh264 ? H264Encoder::OpenH264 : x264 ? H264Encoder::X264 : H264Encoder::None;
}

// The `ffmpeg: h264:` template written into go2rtc's config for a system ffmpeg, "" meaning "leave
// go2rtc's own built-in template alone" (it is libx264's: `-codec:v libx264 ... -preset superfast
// -tune zerolatency`). The libopenh264 template is the one the bundled LGPL build needs and lives
// in RemoteHub.cpp beside its long explanation; it is not repeated here.
//
// VideoToolbox: an explicit template rather than go2rtc's `#hardware` selector, so what runs is
// stated in one place: the hardware H.264 encoder, Main profile (every phone decoder takes it),
// `-realtime 1` (VideoToolbox's low-latency mode: frames are emitted as they are encoded) and no
// B-frames. -b:v and -g:v are appended per quality variant by the hub's #raw segments, and
// VideoToolbox honours both.
inline std::string h264_template_override(H264Encoder e)
{
    if (e == H264Encoder::VideoToolbox) return "-codec:v h264_videotoolbox -profile:v main -realtime 1 -bf 0";
    return std::string();
}

// ---------------------------------------------------------------------------------------------
// Firewall: what to tell the user, from what the OS said (read-only, nothing here changes a rule)
// ---------------------------------------------------------------------------------------------

enum class Tri { Unknown, No, Yes };

// `socketfilterfw --getglobalstate`: "Firewall is enabled. (State = 1)", "... disabled. (State = 0)",
// and State = 2 when "Block all incoming connections" is on.
inline Tri parse_mac_fw_enabled(const std::string& text)
{
    const std::string t = detail::lower(text);
    if (t.find("disabled") != std::string::npos) return Tri::No;
    if (t.find("enabled") != std::string::npos) return Tri::Yes;
    return Tri::Unknown;
}

inline Tri parse_mac_fw_block_all(const std::string& getblockall_text, const std::string& getglobalstate_text)
{
    const std::string g = detail::lower(getglobalstate_text);
    const size_t      s = g.find("state = ");
    if (s != std::string::npos && s + 8 < g.size() && std::isdigit((unsigned char) g[s + 8])) return g[s + 8] == '2' ? Tri::Yes : Tri::No;
    const std::string t = detail::lower(getblockall_text);
    if (t.find("block all") == std::string::npos) return Tri::Unknown;
    if (t.find("disabled") != std::string::npos) return Tri::No;
    if (t.find("enabled") != std::string::npos) return Tri::Yes;
    return Tri::Unknown;
}

// `socketfilterfw --getappblocked <path>`. Yes = the app is on the list as blocked, No = it is
// allowed, Unknown = not on the list / not understood (macOS asks the user the first time a
// signed app listens, so "not listed" is not a problem by itself).
inline Tri parse_mac_fw_app_blocked(const std::string& text)
{
    const std::string t = detail::lower(text);
    if (t.find("not part of") != std::string::npos) return Tri::Unknown;
    if (t.find("not blocked") != std::string::npos || t.find("is permitted") != std::string::npos || t.find("allowed") != std::string::npos) return Tri::No;
    if (t.find("blocked") != std::string::npos) return Tri::Yes;
    return Tri::Unknown;
}

// What the hub page and the phone are told. `state` uses the Windows vocabulary (allowed | partial |
// missing | blocked | unknown) plus "active": a firewall is on and we cannot see whether it lets
// this program through, so the note says what to allow and `command` (if any) is the line to run.
struct FirewallVerdict
{
    std::string state { "unknown" };
    std::string note;
    std::string command;
};

// "go2rtc.exe" -> "go2rtc" for the sentences shown on a Mac or Linux box.
inline std::string program_label(const std::string& label)
{
    if (label.size() > 4 && detail::lower(label.substr(label.size() - 4)) == ".exe") return label.substr(0, label.size() - 4);
    return label;
}

inline FirewallVerdict classify_mac_firewall(Tri enabled, Tri block_all, Tri app_blocked, const std::string& label)
{
    FirewallVerdict v;
    const std::string name = program_label(label);
    if (enabled == Tri::Unknown) {
        v.note = "The macOS firewall could not be checked for " + name + ".";
        return v;
    }
    if (enabled == Tri::No) { v.state = "allowed"; return v; }
    if (block_all == Tri::Yes) {
        v.state = "blocked";
        v.note  = "The macOS firewall is set to block all incoming connections, so a phone cannot reach " + name +
                  ". Turn that off in System Settings > Network > Firewall > Options.";
        return v;
    }
    if (app_blocked == Tri::Yes) {
        v.state = "blocked";
        v.note  = "The macOS firewall blocks incoming connections to " + name +
                  ". Set it to Allow in System Settings > Network > Firewall > Options.";
        return v;
    }
    // On, and either allowed or not on the list yet (macOS asks once, and signed apps are let in
    // by default): nothing to warn about that we can be sure of.
    v.state = app_blocked == Tri::No ? "allowed" : "unknown";
    return v;
}

// ufw: `ENABLED=yes` in /etc/ufw/ufw.conf (world-readable, unlike `ufw status`). firewalld:
// `firewall-cmd --state` prints "running" and needs no root. Rules themselves cannot be read
// without root, so an active firewall is reported as "active" with the line that would open the port.
inline bool ufw_conf_enabled(const std::string& ufw_conf_text)
{
    for (const std::string& raw : detail::split_lines(ufw_conf_text)) {
        const std::string line = detail::trim(raw);
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (detail::lower(detail::trim(line.substr(0, eq))) == "enabled") return detail::lower(detail::trim(line.substr(eq + 1))) == "yes";
    }
    return false;
}

inline bool firewalld_running(const std::string& firewall_cmd_state_text) { return detail::lower(detail::trim(firewall_cmd_state_text)) == "running"; }

inline FirewallVerdict classify_linux_firewall(bool ufw_on, bool firewalld_on, const std::string& label, int port_lo, int port_hi, bool udp)
{
    FirewallVerdict v;
    const std::string name = program_label(label);
    if (!ufw_on && !firewalld_on) return v;
    const bool        range = port_hi > port_lo;
    const std::string shown = range ? std::to_string(port_lo) + "-" + std::to_string(port_hi) : std::to_string(port_lo);
    v.state = "active";
    if (firewalld_on) {
        const std::string r = range ? shown : std::to_string(port_lo);
        v.note    = "firewalld is running on this computer. If your phone cannot reach " + name + ", allow port " + shown + (udp ? " (TCP and UDP)." : " (TCP).");
        v.command = "sudo firewall-cmd --permanent --add-port=" + r + "/tcp" + (udp ? " --add-port=" + r + "/udp" : std::string()) + " && sudo firewall-cmd --reload";
    } else {
        const std::string r = range ? std::to_string(port_lo) + ":" + std::to_string(port_hi) : std::to_string(port_lo);
        v.note    = "ufw is enabled on this computer. If your phone cannot reach " + name + ", allow port " + shown + (udp ? " (TCP and UDP)." : " (TCP).");
        v.command = udp ? "sudo ufw allow " + r : "sudo ufw allow " + r + "/tcp";
    }
    return v;
}

// ".../EdgeSlicer.app/Contents/MacOS/EdgeSlicer" -> ".../EdgeSlicer.app": the macOS firewall lists
// applications by bundle. Anything not inside a bundle is returned unchanged.
inline std::string mac_app_bundle_of(const std::string& exe_path)
{
    const std::string marker = ".app/Contents/";
    const size_t      at     = exe_path.find(marker);
    return at == std::string::npos ? exe_path : exe_path.substr(0, at + 4);
}

// ---------------------------------------------------------------------------------------------
// Who holds a port, and which interface is the default route
// ---------------------------------------------------------------------------------------------

struct Listener
{
    long        pid { 0 };
    std::string command;
};

// `lsof -nP -iTCP:<port> -sTCP:LISTEN -Fpc +c0`: "p<pid>" then "c<command>" per process.
inline Listener parse_lsof_listener(const std::string& text)
{
    Listener l;
    for (const std::string& line : detail::split_lines(text)) {
        if (line.size() < 2) continue;
        if (line[0] == 'p') {
            if (l.pid > 0) break; // the first process only
            try { l.pid = std::stol(line.substr(1)); } catch (...) { l.pid = 0; }
        } else if (line[0] == 'c' && l.pid > 0 && l.command.empty()) {
            l.command = line.substr(1);
        }
    }
    return l;
}

// /proc/net/tcp (and tcp6): the socket inode of whatever is LISTENing (state 0A) on `port`, or 0.
inline unsigned long parse_proc_net_tcp_listen_inode(const std::string& text, int port)
{
    char want[16];
    std::snprintf(want, sizeof(want), ":%04X", port & 0xFFFF);
    for (const std::string& line : detail::split_lines(text)) {
        const std::vector<std::string> w = detail::split_ws(line);
        // sl local rem st tx:rx tr:when retrnsmt uid timeout inode
        if (w.size() < 10 || w[3] != "0A") continue;
        const std::string& local = w[1];
        if (local.size() < 5 || local.compare(local.size() - 5, 5, want) != 0) continue;
        try { return std::stoul(w[9]); } catch (...) { continue; }
    }
    return 0;
}

// /proc/net/route: the interface of the default route (destination and mask 0, UP and GATEWAY),
// the lowest metric winning; "" if there is none. Fields are tab- or space-separated.
inline std::string default_iface_from_proc_net_route(const std::string& text)
{
    std::string best;
    long        best_metric = -1;
    for (const std::string& line : detail::split_lines(text)) {
        const std::vector<std::string> w = detail::split_ws(line);
        if (w.size() < 8 || w[0] == "Iface") continue;
        try {
            const unsigned long flags  = std::stoul(w[3], nullptr, 16);
            const long          metric = std::stol(w[6]);
            if (w[1] != "00000000" || w[7] != "00000000" || (flags & 1) == 0 || (flags & 2) == 0) continue;
            if (best_metric < 0 || metric < best_metric) { best = w[0]; best_metric = metric; }
        } catch (...) { continue; }
    }
    return best;
}

// macOS `route -n get default`: the "interface: en0" line.
inline std::string default_iface_from_route_get(const std::string& text)
{
    for (const std::string& line : detail::split_lines(text)) {
        const std::string t = detail::trim(line);
        if (t.compare(0, 10, "interface:") == 0) return detail::trim(t.substr(10));
    }
    return std::string();
}

// The address the hub should list first: the first IPv4 of the default route's interface.
inline std::string preferred_ipv4_for_iface(const std::vector<HubAddresses::Adapter>& adapters, const std::string& iface)
{
    if (iface.empty()) return std::string();
    for (const HubAddresses::Adapter& a : adapters)
        if (a.name == iface && !a.ipv4.empty()) return a.ipv4.front();
    return std::string();
}

// ---------------------------------------------------------------------------------------------
// `tailscale` that did not simply answer
// ---------------------------------------------------------------------------------------------

enum class CliRun { NotInstalled, TimedOut, Exited };

// run_capture() says "false" for both "could not start it" and "had to kill it"; they mean very
// different things to the person reading the card.
inline CliRun classify_cli_run(bool started, bool timed_out)
{
    if (timed_out) return CliRun::TimedOut;
    return started ? CliRun::Exited : CliRun::NotInstalled;
}

// What `tailscale serve` prints and then waits on when Serve (or HTTPS certificates) has never been
// switched on for the tailnet: "Serve is not enabled on your tailnet. To enable, visit:" and a
// https://login.tailscale.com/f/serve?node=... link. Any admin-console link in its output while it
// is still running means it is waiting for someone to click it.
struct ServeEnable
{
    bool        needed { false };
    std::string url;
};

inline ServeEnable detect_serve_needs_enabling(const std::string& output)
{
    ServeEnable r;
    const std::string low = detail::lower(output);
    const size_t      u   = low.find("https://login.tailscale.com/");
    if (u != std::string::npos) {
        size_t e = u;
        while (e < output.size() && !std::isspace((unsigned char) output[e])) ++e;
        r.url    = output.substr(u, e - u);
        r.needed = true;
    }
    if (low.find("is not enabled on your tailnet") != std::string::npos || low.find("https certificates are not enabled") != std::string::npos)
        r.needed = true;
    return r;
}

inline std::string serve_enable_message(const std::string& url)
{
    return "Tailscale Serve is not turned on for your tailnet. Enable Serve and HTTPS certificates in the Tailscale admin console" +
           (url.empty() ? std::string() : " (" + url + ")") + ", then try again.";
}

inline std::string cli_no_answer_message(int seconds)
{
    return "Tailscale is installed but did not answer within " + std::to_string(seconds) + " seconds. Make sure Tailscale is running, then try again.";
}

// ---------------------------------------------------------------------------------------------
// The Funnel line the hub page shows (documented, never run)
// ---------------------------------------------------------------------------------------------

// One word of a shell command, single-quoted unless it is plainly safe.
inline std::string shell_word(const std::string& w)
{
    if (!w.empty() && std::all_of(w.begin(), w.end(), [](unsigned char c) { return std::isalnum(c) || std::strchr("_@%+=:,./-", c) != nullptr; })) return w;
    std::string q = "'";
    for (char c : w) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return q + "'";
}

// What to type for "tailscale" in a terminal: the bare name on Windows (the installer puts it on
// PATH, and the text stays as it was) or when the resolved CLI is itself found by PATH; otherwise
// the path that was found, which is how a Mac without the app's "Install CLI" shim can run it.
inline std::string cli_for_terminal(Platform p, const std::string& resolved_exe)
{
    if (p == Platform::Windows || resolved_exe.empty() || resolved_exe.find('/') == std::string::npos) return "tailscale";
    return shell_word(resolved_exe);
}

// ---------------------------------------------------------------------------------------------
// POSIX process and socket helpers (nothing here exists on Windows)
// ---------------------------------------------------------------------------------------------
#ifndef _WIN32

// TCP keep-alive so a phone that vanished is noticed in about idle + interval * count seconds, not
// the OS default of two hours. Windows does the same with SIO_KEEPALIVE_VALS (RemoteHub.cpp).
inline void set_tcp_keepalive_posix(int fd, int idle_s, int interval_s = 5, int count = 10)
{
    if (fd < 0) return;
    const int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
    if (idle_s <= 0) return;
#if defined(__APPLE__)
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &idle_s, sizeof(idle_s));
#  ifdef TCP_KEEPINTVL
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval_s, sizeof(interval_s));
#  endif
#  ifdef TCP_KEEPCNT
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
#  endif
#elif defined(__linux__)
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle_s, sizeof(idle_s));
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval_s, sizeof(interval_s));
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
#else
    (void) interval_s; (void) count;
#endif
}

// The executable a running pid was started from ("" when unreadable). macOS: proc_pidpath. Linux:
// /proc/<pid>/exe - but not inside an AppImage, where the image path is the mount and not the file
// a slicer launches (so it would never compare equal).
inline std::string process_image_path_posix(long pid)
{
    if (pid <= 0) return std::string();
#if defined(__APPLE__)
    char      buf[PROC_PIDPATHINFO_MAXSIZE];
    const int n = ::proc_pidpath((int) pid, buf, sizeof(buf));
    return n > 0 ? std::string(buf, (size_t) n) : std::string();
#elif defined(__linux__)
    if (const char* ai = std::getenv("APPIMAGE"); ai && *ai) return std::string();
    char          buf[4096];
    const ssize_t n = ::readlink(("/proc/" + std::to_string(pid) + "/exe").c_str(), buf, sizeof(buf) - 1);
    if (n <= 0) return std::string();
    std::string s(buf, (size_t) n);
    const std::string gone = " (deleted)";
    if (s.size() > gone.size() && s.compare(s.size() - gone.size(), gone.size(), gone) == 0) s.resize(s.size() - gone.size());
    return s;
#else
    return std::string();
#endif
}

inline std::string read_file_posix(const std::string& path, size_t max_bytes = 1 << 20)
{
    std::string out;
    const int   fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return out;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        out.append(buf, (size_t) n);
        if (out.size() >= max_bytes) break;
    }
    ::close(fd);
    return out;
}

// Linux without lsof: the socket inode from /proc/net/tcp{,6}, then the process whose
// /proc/<pid>/fd holds "socket:[inode]" (only processes of this user are readable, same as lsof).
inline Listener find_listener_via_proc(int port)
{
    Listener      l;
    unsigned long inode = parse_proc_net_tcp_listen_inode(read_file_posix("/proc/net/tcp"), port);
    if (!inode) inode = parse_proc_net_tcp_listen_inode(read_file_posix("/proc/net/tcp6"), port);
    if (!inode) return l;
    const std::string want = "socket:[" + std::to_string(inode) + "]";
    DIR*              proc = ::opendir("/proc");
    if (!proc) return l;
    while (const dirent* e = ::readdir(proc)) {
        if (!std::isdigit((unsigned char) e->d_name[0])) continue;
        const std::string fddir = std::string("/proc/") + e->d_name + "/fd";
        DIR*              fds   = ::opendir(fddir.c_str());
        if (!fds) continue;
        bool found = false;
        while (const dirent* f = ::readdir(fds)) {
            char          buf[128];
            const ssize_t n = ::readlink((fddir + "/" + f->d_name).c_str(), buf, sizeof(buf) - 1);
            if (n > 0 && want.compare(0, std::string::npos, buf, (size_t) n) == 0) { found = true; break; }
        }
        ::closedir(fds);
        if (found) {
            l.pid     = std::atol(e->d_name);
            l.command = detail::trim(read_file_posix(std::string("/proc/") + e->d_name + "/comm", 256));
            break;
        }
    }
    ::closedir(proc);
    return l;
}

// A long-running child (go2rtc): fork + exec, no shell, stdin from /dev/null, stdout and stderr
// appended to `log_path` ("" = /dev/null), every other descriptor closed, its own process group
// (a ^C at the terminal that started the hub does not reach it; the hub stops it itself). Returns
// the pid, or 0 when it could not be started - a missing or non-executable program is reported
// through a close-on-exec pipe, so it is 0 rather than a child that exits 127 a moment later.
// The caller owns the child: reap_child_posix() or terminate_child_posix(), or it becomes a zombie.
inline long spawn_child_posix(const std::vector<std::string>& args, const std::string& log_path)
{
    if (args.empty() || args.front().find('/') == std::string::npos) return 0; // the caller names the program by path
    std::vector<char*> argv;
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    int errp[2] = { -1, -1 };
    if (::pipe(errp) != 0) return 0;
    for (int fd : { errp[0], errp[1] }) {
        const int fl = ::fcntl(fd, F_GETFD);
        if (fl >= 0) ::fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
    }
    int devnull = ::open("/dev/null", O_RDONLY);
    int logfd   = log_path.empty() ? ::open("/dev/null", O_WRONLY) : ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    long maxfd  = ::sysconf(_SC_OPEN_MAX);
    if (maxfd < 0 || maxfd > 4096) maxfd = 4096;

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(errp[0]); ::close(errp[1]);
        if (devnull >= 0) ::close(devnull);
        if (logfd >= 0) ::close(logfd);
        return 0;
    }
    if (pid == 0) {
        ::setpgid(0, 0);
        signal(SIGPIPE, SIG_DFL);
        sigset_t none;
        sigemptyset(&none); // no ::, macOS defines sigemptyset as a macro
        sigprocmask(SIG_SETMASK, &none, nullptr);
        if (devnull >= 0) ::dup2(devnull, 0);
        if (logfd >= 0) { ::dup2(logfd, 1); ::dup2(logfd, 2); }
        for (int fd = 3; fd < maxfd; ++fd)
            if (fd != errp[1]) ::close(fd);
        ::execv(args.front().c_str(), argv.data());
        const int e = errno;
        (void) !::write(errp[1], &e, sizeof(e));
        ::_exit(127);
    }
    ::close(errp[1]);
    if (devnull >= 0) ::close(devnull);
    if (logfd >= 0) ::close(logfd);
    int     e = 0;
    pollfd  p { errp[0], POLLIN, 0 };
    int     r;
    do { r = ::poll(&p, 1, 3000); } while (r < 0 && errno == EINTR);
    ssize_t n = 0;
    if (r > 0) { do { n = ::read(errp[0], &e, sizeof(e)); } while (n < 0 && errno == EINTR); }
    ::close(errp[0]);
    if (n == (ssize_t) sizeof(e)) {
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        return 0;
    }
    return (long) pid;
}

// Non-blocking: true (and `exit_code` set) when the child has ended and has now been reaped.
inline bool reap_child_posix(long pid, int* exit_code = nullptr)
{
    if (pid <= 0) return true;
    int         status = 0;
    const pid_t w      = ::waitpid((pid_t) pid, &status, WNOHANG);
    if (w == 0) return false;
    if (exit_code) *exit_code = (w == (pid_t) pid) ? (WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1) : -1;
    return true; // reaped, or already gone (ECHILD)
}

// SIGTERM, wait up to `grace_ms` for it to go, then SIGKILL; always reaped.
inline void terminate_child_posix(long pid, int grace_ms = 2000)
{
    if (pid <= 0) return;
    if (reap_child_posix(pid)) return;
    ::kill((pid_t) pid, SIGTERM);
    for (int waited = 0; waited < grace_ms; waited += 20) {
        if (reap_child_posix(pid)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ::kill((pid_t) pid, SIGKILL);
    int status = 0;
    while (::waitpid((pid_t) pid, &status, 0) < 0 && errno == EINTR) {}
}

#endif // !_WIN32

} // namespace HubPlatform
} // namespace Slic3r
