// The macOS / Linux halves of the remote hub (slic3r/Utils/HubPlatform.hpp): which go2rtc and
// ffmpeg get run, what the OS firewalls said, who holds a port, the default route, and what
// `tailscale serve` printed while it waited for Serve to be switched on. The parsers are driven with
// captured text, so none of it needs the OS it describes; the POSIX process/socket helpers are
// exercised for real, on POSIX only.

#include <catch2/catch.hpp>

#include "slic3r/Utils/HubPlatform.hpp"

#include <set>
#include <string>
#include <vector>

using namespace Slic3r::HubPlatform;
using Slic3r::TailscaleCli::Platform;

TEST_CASE("go2rtc and ffmpeg are looked for in the bundle first, then the package manager's places", "[HubPlatform]")
{
    REQUIRE(go2rtc_candidates(Platform::Windows, "C:/App/resources") == std::vector<std::string>{ "C:/App/resources/tools/go2rtc/go2rtc.exe" });
    REQUIRE(ffmpeg_candidates(Platform::Windows, "C:/App/resources") == std::vector<std::string>{ "C:/App/resources/tools/go2rtc/ffmpeg.exe" });
    REQUIRE(go2rtc_candidates(Platform::MacOS, "/Applications/EdgeSlicer.app/Contents/Resources") ==
            std::vector<std::string>{ "/Applications/EdgeSlicer.app/Contents/Resources/tools/go2rtc/go2rtc", "/opt/homebrew/bin/go2rtc",
                                      "/usr/local/bin/go2rtc", "/usr/bin/go2rtc" });
    REQUIRE(ffmpeg_candidates(Platform::Linux, "/opt/edge/resources") ==
            std::vector<std::string>{ "/opt/edge/resources/tools/go2rtc/ffmpeg", "/usr/local/bin/ffmpeg", "/usr/bin/ffmpeg" });

    const auto only = [](std::set<std::string> have) { return [have](const std::string& p) { return have.count(p) > 0; }; };
    REQUIRE(pick_first_existing(ffmpeg_candidates(Platform::MacOS, "/R"), only({ "/usr/local/bin/ffmpeg", "/opt/homebrew/bin/ffmpeg" })) ==
            "/opt/homebrew/bin/ffmpeg");
    REQUIRE(pick_first_existing(ffmpeg_candidates(Platform::MacOS, "/R"), only({ "/R/tools/go2rtc/ffmpeg", "/opt/homebrew/bin/ffmpeg" })) ==
            "/R/tools/go2rtc/ffmpeg");
    REQUIRE(pick_first_existing(ffmpeg_candidates(Platform::Linux, "/R"), only({})).empty());
}

TEST_CASE("the H.264 encoder is read from ffmpeg -encoders", "[HubPlatform]")
{
    const std::string bundled =
        "Encoders:\n V..... = Video\n A..... = Audio\n ------\n"
        " V....D libopenh264          OpenH264 H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10 (codec h264)\n"
        " V....D h264_amf             AMD AMF H.264 Encoder (codec h264)\n"
        " A....D aac                  AAC (Advanced Audio Coding)\n";
    const std::string homebrew =
        "Encoders:\n ------\n"
        " V....D libx264              libx264 H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10 (codec h264)\n"
        " V....D h264_videotoolbox    VideoToolbox H.264 Encoder (codec h264)\n";
    const std::string both = bundled + " V....D libx264              libx264 H.264\n";
    const std::string vt_only = "Encoders:\n ------\n V....D mpeg4               MPEG-4 part 2\n V....D h264_videotoolbox    VideoToolbox H.264 Encoder (codec h264)\n";
    for (Platform p : { Platform::Windows, Platform::Linux, Platform::MacOS }) {
        REQUIRE(choose_h264_encoder(bundled, p) == H264Encoder::OpenH264);
        REQUIRE(choose_h264_encoder(both, p) == H264Encoder::OpenH264);
        REQUIRE(choose_h264_encoder("", p) == H264Encoder::None);
        // A description that merely mentions libx264 is not an encoder row.
        REQUIRE(choose_h264_encoder("Encoders:\n ------\n V....D mpeg4   an alternative to libx264\n", p) == H264Encoder::None);
    }
    // Homebrew's ffmpeg: libx264 + h264_videotoolbox. The Mac takes the hardware encoder; elsewhere
    // hardware encoders are left alone and libx264 is used.
    REQUIRE(choose_h264_encoder(homebrew, Platform::MacOS) == H264Encoder::VideoToolbox);
    REQUIRE(choose_h264_encoder(homebrew, Platform::Linux) == H264Encoder::X264);
    REQUIRE(choose_h264_encoder(homebrew, Platform::Windows) == H264Encoder::X264);
    // VideoToolbox alone (no software encoder at all): usable on a Mac, nothing elsewhere.
    REQUIRE(choose_h264_encoder(vt_only, Platform::MacOS) == H264Encoder::VideoToolbox);
    REQUIRE(choose_h264_encoder(vt_only, Platform::Linux) == H264Encoder::None);
    // VideoToolbox beats openh264 on a Mac, openh264 wins everywhere else.
    const std::string vt_and_openh264 = bundled + " V....D h264_videotoolbox    VideoToolbox H.264 Encoder (codec h264)\n";
    REQUIRE(choose_h264_encoder(vt_and_openh264, Platform::MacOS) == H264Encoder::VideoToolbox);
    REQUIRE(choose_h264_encoder(vt_and_openh264, Platform::Linux) == H264Encoder::OpenH264);
    // No encoder, hardware or otherwise.
    REQUIRE(choose_h264_encoder("Encoders:\n ------\n V....D mpeg4               MPEG-4 part 2\n", Platform::MacOS) == H264Encoder::None);
    // The template only differs for VideoToolbox; libx264 means "leave go2rtc's own".
    REQUIRE(h264_template_override(H264Encoder::VideoToolbox) == "-codec:v h264_videotoolbox -profile:v main -realtime 1 -bf 0");
    REQUIRE(h264_template_override(H264Encoder::X264).empty());
    REQUIRE(h264_template_override(H264Encoder::OpenH264).empty());
    REQUIRE(h264_template_override(H264Encoder::None).empty());
}

TEST_CASE("macOS firewall output is parsed", "[HubPlatform]")
{
    REQUIRE(parse_mac_fw_enabled("Firewall is enabled. (State = 1)") == Tri::Yes);
    REQUIRE(parse_mac_fw_enabled("Firewall is disabled. (State = 0)\n") == Tri::No);
    REQUIRE(parse_mac_fw_enabled("Firewall is enabled. (State = 2)") == Tri::Yes);
    REQUIRE(parse_mac_fw_enabled("") == Tri::Unknown);
    REQUIRE(parse_mac_fw_enabled("socketfilterfw: permission denied") == Tri::Unknown);

    // Block-all comes from State = 2 when the global state said so, else from --getblockall.
    REQUIRE(parse_mac_fw_block_all("", "Firewall is enabled. (State = 2)") == Tri::Yes);
    REQUIRE(parse_mac_fw_block_all("", "Firewall is enabled. (State = 1)") == Tri::No);
    REQUIRE(parse_mac_fw_block_all("Block all DISABLED! \n", "") == Tri::No);
    REQUIRE(parse_mac_fw_block_all("Block all ENABLED! \n", "") == Tri::Yes);
    REQUIRE(parse_mac_fw_block_all("", "") == Tri::Unknown);

    REQUIRE(parse_mac_fw_app_blocked("The application at path ( /Applications/EdgeSlicer.app ) is not blocked from accepting incoming connections.") == Tri::No);
    REQUIRE(parse_mac_fw_app_blocked("Application at path ( /Applications/EdgeSlicer.app ) is blocked from accepting incoming connections.") == Tri::Yes);
    REQUIRE(parse_mac_fw_app_blocked("The application at path ( /x ) is not part of the firewall list") == Tri::Unknown);
    REQUIRE(parse_mac_fw_app_blocked("") == Tri::Unknown);
}

TEST_CASE("macOS firewall verdicts", "[HubPlatform]")
{
    // Off: nothing to say. Never a netsh line.
    auto v = classify_mac_firewall(Tri::No, Tri::No, Tri::Unknown, "EdgeSlicer.exe");
    REQUIRE(v.state == "allowed");
    REQUIRE(v.note.empty());
    REQUIRE(v.command.empty());
    // On and block-all: blocked, with the System Settings path, label without ".exe".
    v = classify_mac_firewall(Tri::Yes, Tri::Yes, Tri::Unknown, "EdgeSlicer.exe");
    REQUIRE(v.state == "blocked");
    REQUIRE_THAT(v.note, Catch::Matchers::Contains("block all incoming connections"));
    REQUIRE_THAT(v.note, Catch::Matchers::Contains("System Settings > Network > Firewall"));
    REQUIRE_THAT(v.note, Catch::Matchers::Contains("EdgeSlicer"));
    REQUIRE_THAT(v.note, !Catch::Matchers::Contains(".exe"));
    REQUIRE(v.command.empty());
    // The app on the list as blocked (the user pressed Deny once).
    v = classify_mac_firewall(Tri::Yes, Tri::No, Tri::Yes, "go2rtc.exe");
    REQUIRE(v.state == "blocked");
    REQUIRE_THAT(v.note, Catch::Matchers::Contains("go2rtc"));
    // On, app allowed / not listed yet / could not be asked.
    REQUIRE(classify_mac_firewall(Tri::Yes, Tri::No, Tri::No, "EdgeSlicer.exe").state == "allowed");
    REQUIRE(classify_mac_firewall(Tri::Yes, Tri::No, Tri::Unknown, "EdgeSlicer.exe").state == "unknown");
    REQUIRE(classify_mac_firewall(Tri::Yes, Tri::No, Tri::Unknown, "EdgeSlicer.exe").note.empty());
    // Could not read the firewall at all.
    v = classify_mac_firewall(Tri::Unknown, Tri::Unknown, Tri::Unknown, "EdgeSlicer.exe");
    REQUIRE(v.state == "unknown");
    REQUIRE_THAT(v.note, Catch::Matchers::Contains("could not be checked"));
}

TEST_CASE("Linux firewall: ufw and firewalld are read without root", "[HubPlatform]")
{
    REQUIRE(ufw_conf_enabled("# /etc/ufw/ufw.conf\n#\n\n# Set to yes to start on boot.\nENABLED=yes\n\nLOGLEVEL=low\n"));
    REQUIRE_FALSE(ufw_conf_enabled("ENABLED=no\n"));
    REQUIRE_FALSE(ufw_conf_enabled("#ENABLED=yes\n"));
    REQUIRE_FALSE(ufw_conf_enabled(""));
    REQUIRE(ufw_conf_enabled("  enabled = YES  \r\n"));
    REQUIRE(firewalld_running("running\n"));
    REQUIRE_FALSE(firewalld_running("not running\n"));
    REQUIRE_FALSE(firewalld_running(""));

    // No firewall we know of: silent unknown.
    auto v = classify_linux_firewall(false, false, "EdgeSlicer.exe", 13640, 13659, false);
    REQUIRE(v.state == "unknown");
    REQUIRE(v.note.empty());
    // ufw on, the phone/LAN range (TCP only).
    v = classify_linux_firewall(true, false, "EdgeSlicer.exe", 13640, 13659, false);
    REQUIRE(v.state == "active");
    REQUIRE(v.command == "sudo ufw allow 13640:13659/tcp");
    REQUIRE_THAT(v.note, Catch::Matchers::Contains("13640-13659"));
    // ufw on, go2rtc's one WebRTC port on both protocols.
    v = classify_linux_firewall(true, false, "go2rtc.exe", 8555, 8555, true);
    REQUIRE(v.command == "sudo ufw allow 8555");
    REQUIRE_THAT(v.note, Catch::Matchers::Contains("TCP and UDP"));
    // firewalld wins when both are reported (it is the one that actually filters).
    v = classify_linux_firewall(true, true, "EdgeSlicer.exe", 13640, 13659, false);
    REQUIRE(v.command == "sudo firewall-cmd --permanent --add-port=13640-13659/tcp && sudo firewall-cmd --reload");
    v = classify_linux_firewall(false, true, "go2rtc.exe", 8555, 8555, true);
    REQUIRE(v.command == "sudo firewall-cmd --permanent --add-port=8555/tcp --add-port=8555/udp && sudo firewall-cmd --reload");
}

TEST_CASE("the macOS firewall lists apps by bundle", "[HubPlatform]")
{
    REQUIRE(mac_app_bundle_of("/Applications/EdgeSlicer.app/Contents/MacOS/EdgeSlicer") == "/Applications/EdgeSlicer.app");
    REQUIRE(mac_app_bundle_of("/Users/me/build/EdgeSlicer.app/Contents/Resources/tools/go2rtc/go2rtc") == "/Users/me/build/EdgeSlicer.app");
    REQUIRE(mac_app_bundle_of("/opt/homebrew/bin/go2rtc") == "/opt/homebrew/bin/go2rtc");
}

TEST_CASE("lsof -F output gives the program holding a port", "[HubPlatform]")
{
    const auto l = parse_lsof_listener("p4242\ncEdgeSlicer\np777\ncpython\n");
    REQUIRE(l.pid == 4242);
    REQUIRE(l.command == "EdgeSlicer");
    REQUIRE(parse_lsof_listener("").pid == 0);
    REQUIRE(parse_lsof_listener("pNaN\ncx\n").pid == 0);
    REQUIRE(parse_lsof_listener("p12\n").command.empty());
}

TEST_CASE("/proc/net/tcp gives the socket inode listening on a port", "[HubPlatform]")
{
    const std::string tcp =
        "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"
        "   0: 0100007F:0035 00000000:0000 0A 00000000:00000000 00:00000000 00000000   101        0 11111 1 ffff 100 0 0 10 0\n"
        "   1: 00000000:3548 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 22222 1 ffff 100 0 0 10 0\n"
        "   2: 0F01A8C0:3548 0201A8C0:C350 01 00000000:00000000 00:00000000 00000000  1000        0 33333 1 ffff 100 0 0 10 0\n";
    REQUIRE(parse_proc_net_tcp_listen_inode(tcp, 13640) == 22222); // 0x3548
    REQUIRE(parse_proc_net_tcp_listen_inode(tcp, 53) == 11111);
    REQUIRE(parse_proc_net_tcp_listen_inode(tcp, 80) == 0);
    // An established connection on that port is not a listener.
    REQUIRE(parse_proc_net_tcp_listen_inode(tcp.substr(tcp.find("   2:")), 13640) == 0);
    // IPv6 rows carry a long address.
    const std::string tcp6 = "   0: 00000000000000000000000000000000:3548 00000000000000000000000000000000:0000 0A 00000000:00000000 00:00000000 00000000  1000 0 44444 1 ffff 100 0 0 10 0\n";
    REQUIRE(parse_proc_net_tcp_listen_inode(tcp6, 13640) == 44444);
}

TEST_CASE("the default route's interface, on Linux and on macOS", "[HubPlatform]")
{
    const std::string route =
        "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
        "tailscale0\t00000000\t00000000\t0001\t0\t0\t5000\t00000000\t0\t0\t0\n"
        "wlan0\t00000000\t0101A8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n"
        "eth0\t00000000\t0101A8C0\t0003\t0\t0\t100\t00000000\t0\t0\t0\n"
        "eth0\t0001A8C0\t00000000\t0001\t0\t0\t100\t00FFFFFF\t0\t0\t0\n";
    REQUIRE(default_iface_from_proc_net_route(route) == "eth0"); // lowest metric among UP+GATEWAY default routes
    REQUIRE(default_iface_from_proc_net_route("Iface\tDestination\n").empty());
    REQUIRE(default_iface_from_proc_net_route("").empty());

    const std::string mac =
        "   route to: default\ndestination: default\n       mask: default\n    gateway: 192.168.1.1\n  interface: en0\n      flags: <UP,GATEWAY,DONE,STATIC,PRIVATE>\n";
    REQUIRE(default_iface_from_route_get(mac) == "en0");
    REQUIRE(default_iface_from_route_get("route: writing to routing socket: not in table\n").empty());

    Slic3r::HubAddresses::Adapter en0, lo;
    en0.name = "en0"; en0.ipv4 = { "192.168.1.5" };
    lo.name  = "lo0"; lo.ipv4 = { "127.0.0.1" };
    REQUIRE(preferred_ipv4_for_iface({ lo, en0 }, "en0") == "192.168.1.5");
    REQUIRE(preferred_ipv4_for_iface({ lo, en0 }, "").empty());
    REQUIRE(preferred_ipv4_for_iface({ lo }, "en0").empty());
}

TEST_CASE("a CLI that did not answer is not a CLI that is not installed", "[HubPlatform]")
{
    REQUIRE(classify_cli_run(false, false) == CliRun::NotInstalled);
    REQUIRE(classify_cli_run(true, false) == CliRun::Exited);
    REQUIRE(classify_cli_run(true, true) == CliRun::TimedOut);
    REQUIRE(classify_cli_run(false, true) == CliRun::TimedOut);
    REQUIRE_THAT(cli_no_answer_message(15), Catch::Matchers::Contains("did not answer within 15 seconds"));
}

TEST_CASE("tailscale serve waiting for the tailnet admin is recognised from its output", "[HubPlatform]")
{
    const std::string prompt =
        "\nServe is not enabled on your tailnet.\nTo enable, visit:\n\n         https://login.tailscale.com/f/serve?node=n1234CNTRL\n";
    const ServeEnable e = detect_serve_needs_enabling(prompt);
    REQUIRE(e.needed);
    REQUIRE(e.url == "https://login.tailscale.com/f/serve?node=n1234CNTRL");
    REQUIRE_THAT(serve_enable_message(e.url), Catch::Matchers::Contains("Tailscale admin console"));
    REQUIRE_THAT(serve_enable_message(e.url), Catch::Matchers::Contains(e.url));
    // Any admin-console link while it is still waiting means the same thing, whatever the wording.
    REQUIRE(detect_serve_needs_enabling("Enable HTTPS here: https://login.tailscale.com/admin/dns\n").needed);
    // The phrase without a link still counts.
    const ServeEnable bare = detect_serve_needs_enabling("Funnel is not enabled on your tailnet.");
    REQUIRE(bare.needed);
    REQUIRE(bare.url.empty());
    // Ordinary output does not.
    REQUIRE_FALSE(detect_serve_needs_enabling("Available within your tailnet:\n\nhttps://pc.tail1234.ts.net/\n|-- / proxy http://127.0.0.1:13640\n").needed);
    REQUIRE_FALSE(detect_serve_needs_enabling("").needed);
}

TEST_CASE("the Funnel line names the CLI that was found", "[HubPlatform]")
{
    const std::string app = "/Applications/Tailscale.app/Contents/MacOS/Tailscale";
    REQUIRE(cli_for_terminal(Platform::MacOS, app) == app);
    REQUIRE(cli_for_terminal(Platform::MacOS, "tailscale") == "tailscale"); // found on PATH
    REQUIRE(cli_for_terminal(Platform::Linux, "/usr/bin/tailscale") == "/usr/bin/tailscale");
    REQUIRE(cli_for_terminal(Platform::MacOS, "/Users/me/My Tools/tailscale") == "'/Users/me/My Tools/tailscale'");
    REQUIRE(cli_for_terminal(Platform::MacOS, "") == "tailscale");
    // Windows text is untouched, whatever was resolved.
    REQUIRE(cli_for_terminal(Platform::Windows, "C:\\Program Files\\Tailscale\\tailscale.exe") == "tailscale");
    REQUIRE(shell_word("it's") == "'it'\\''s'");
}

#ifndef _WIN32

TEST_CASE("a long-running child is started without a shell, stopped and reaped", "[HubPlatform]")
{
    const long pid = spawn_child_posix({ "/bin/sleep", "30" }, "");
    REQUIRE(pid > 0);
    int code = -2;
    REQUIRE_FALSE(reap_child_posix(pid, &code)); // still running
    const auto t0 = std::chrono::steady_clock::now();
    terminate_child_posix(pid, 3000);
    REQUIRE(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() < 3000);
    int status = 0;
    REQUIRE(::waitpid(-1, &status, WNOHANG) == -1); // nothing left to reap
    REQUIRE(errno == ECHILD);
    REQUIRE(::kill((pid_t) pid, 0) != 0);
}

TEST_CASE("a child that ends by itself is reaped with its exit status", "[HubPlatform]")
{
    const long pid = spawn_child_posix({ "/bin/ls", "/definitely/not/a/real/path" }, "");
    REQUIRE(pid > 0);
    int code = 0;
    for (int i = 0; i < 200 && !reap_child_posix(pid, &code); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(code != 0);
}

TEST_CASE("a child that cannot be started returns 0, not a pid that exits 127", "[HubPlatform]")
{
    REQUIRE(spawn_child_posix({ "/definitely/not/installed/go2rtc" }, "") == 0);
    REQUIRE(spawn_child_posix({ "/etc/hosts" }, "") == 0); // present but not executable
    REQUIRE(spawn_child_posix({ "go2rtc" }, "") == 0);      // the caller must name it by path
    REQUIRE(spawn_child_posix({}, "") == 0);
    int status = 0;
    REQUIRE(::waitpid(-1, &status, WNOHANG) == -1);
}

TEST_CASE("a child's output goes to the log file, not to the hub's terminal", "[HubPlatform]")
{
    char path[] = "/tmp/hubplatform_log_XXXXXX";
    const int fd = ::mkstemp(path);
    REQUIRE(fd >= 0);
    ::close(fd);
    const long pid = spawn_child_posix({ "/bin/echo", "to-the-log" }, path);
    REQUIRE(pid > 0);
    int code = -1;
    for (int i = 0; i < 200 && !reap_child_posix(pid, &code); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(code == 0);
    REQUIRE(read_file_posix(path) == "to-the-log\n");
    ::unlink(path);
}

TEST_CASE("process_image_path_posix reads this process's own executable", "[HubPlatform]")
{
#if defined(__APPLE__) || defined(__linux__)
    if (!std::getenv("APPIMAGE")) {
        const std::string p = process_image_path_posix((long) ::getpid());
        REQUIRE_FALSE(p.empty());
        REQUIRE(p[0] == '/');
    }
#endif
    REQUIRE(process_image_path_posix(0).empty());
    REQUIRE(process_image_path_posix(-5).empty());
}

#ifdef __linux__
TEST_CASE("find_listener_via_proc finds this process on a port it listens on", "[HubPlatform]")
{
    const int s = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(s >= 0);
    sockaddr_in a {};
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port        = 0;
    REQUIRE(::bind(s, (sockaddr*) &a, sizeof(a)) == 0);
    REQUIRE(::listen(s, 1) == 0);
    socklen_t len = sizeof(a);
    REQUIRE(::getsockname(s, (sockaddr*) &a, &len) == 0);
    const Listener l = find_listener_via_proc((int) ntohs(a.sin_port));
    REQUIRE(l.pid == (long) ::getpid());
    REQUIRE_FALSE(l.command.empty());
    ::close(s);
    REQUIRE(find_listener_via_proc((int) ntohs(a.sin_port)).pid == 0);
}

TEST_CASE("set_tcp_keepalive_posix sets the idle, interval and count", "[HubPlatform]")
{
    const int s = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(s >= 0);
    set_tcp_keepalive_posix(s, 45, 7, 4);
    int v = 0;
    socklen_t n = sizeof(v);
    REQUIRE(::getsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &v, &n) == 0);
    REQUIRE(v != 0);
    REQUIRE(::getsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, &v, &n) == 0);
    REQUIRE(v == 45);
    REQUIRE(::getsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &v, &n) == 0);
    REQUIRE(v == 7);
    REQUIRE(::getsockopt(s, IPPROTO_TCP, TCP_KEEPCNT, &v, &n) == 0);
    REQUIRE(v == 4);
    ::close(s);
}
#endif

#endif // !_WIN32
