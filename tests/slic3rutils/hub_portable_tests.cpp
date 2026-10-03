#include <catch2/catch.hpp>

#include "slic3r/Utils/HubPortable.hpp"

using namespace Slic3r::HubPortable;

TEST_CASE("hub portable: the advertised host accepts a host, host:port, an IPv6 literal or a URL", "[HubPortable]")
{
    auto ok = [](const std::string& in, const std::string& host, int port) {
        const PublicHost p = parse_public_host(in);
        INFO(in);
        CHECK(p.valid());
        CHECK(p.host == host);
        CHECK(p.port == port);
    };
    ok("192.168.1.50", "192.168.1.50", 0);
    ok("  192.168.1.50  ", "192.168.1.50", 0);
    ok("hub.lan", "hub.lan", 0);
    ok("hub.lan:13650", "hub.lan", 13650);
    ok("http://nas.local:13650/anything", "nas.local", 13650);
    ok("HTTPS://nas.local/", "nas.local", 0);
    ok("[fd00::5]:13650", "fd00::5", 13650);
    ok("[fd00::5]", "fd00::5", 0);
    ok("fd00::5", "fd00::5", 0);
}

TEST_CASE("hub portable: anything that is not plainly a host is refused", "[HubPortable]")
{
    for (const char* bad : { "", "   ", "http://", "host with space", "user@host", "host?x=1", "host#frag", "host:", "host:0", "host:70000",
                             "host:12a", "host:123456", "-host", "host-", ".host", "host.", "[host]", "[fd00::5", "[fd00::5]x", "[fd00::5]:",
                             "ho st:1" }) {
        INFO(bad);
        CHECK_FALSE(parse_public_host(bad).valid());
    }
}

TEST_CASE("hub portable: the advertised list puts the override first without duplicates", "[HubPortable]")
{
    const PublicHost p = parse_public_host("192.168.1.50:13650");
    CHECK(advertised_hosts(p, { "172.17.0.2", "192.168.1.50" }) == std::vector<std::string>{ "192.168.1.50", "172.17.0.2" });
    CHECK(advertised_hosts(PublicHost(), { "172.17.0.2" }) == std::vector<std::string>{ "172.17.0.2" });
    CHECK(advertised_port(p, 13640) == 13650);
    CHECK(advertised_port(parse_public_host("192.168.1.50"), 13640) == 13640);
    CHECK(advertised_port(PublicHost(), 13641) == 13641);
}

TEST_CASE("hub portable: the LAN link brackets an IPv6 host", "[HubPortable]")
{
    CHECK(lan_url("192.168.1.50", 13650, "tok") == "http://192.168.1.50:13650/r/tok/");
    CHECK(lan_url("fd00::5", 13640, "tok") == "http://[fd00::5]:13640/r/tok/");
}

TEST_CASE("hub portable: find_in_path takes the first executable and ignores relative entries", "[HubPortable]")
{
    auto exec = [](const std::string& p) { return p == "/usr/local/bin/ffmpeg" || p == "/usr/bin/ffmpeg" || p == "/ffmpeg"; };
    CHECK(find_in_path("ffmpeg", "/opt/x:/usr/local/bin:/usr/bin", ':', exec) == "/usr/local/bin/ffmpeg");
    CHECK(find_in_path("ffmpeg", "/usr/bin//:/usr/local/bin", ':', exec) == "/usr/bin/ffmpeg");
    CHECK(find_in_path("ffmpeg", "bin:.::/usr/bin", ':', exec) == "/usr/bin/ffmpeg");
    CHECK(find_in_path("ffmpeg", "/", ':', exec) == "/ffmpeg");
    CHECK(find_in_path("ffmpeg", "/nowhere", ':', exec).empty());
    CHECK(find_in_path("ffmpeg", "", ':', exec).empty());
    CHECK(find_in_path("../ffmpeg", "/usr/bin", ':', exec).empty());
    CHECK(find_in_path("", "/usr/bin", ':', exec).empty());
}

TEST_CASE("hub portable: tool names per platform", "[HubPortable]")
{
    CHECK(tool_file_name("go2rtc", true) == "go2rtc.exe");
    CHECK(tool_file_name("go2rtc", false) == "go2rtc");
    CHECK(tool_file_name("ffmpeg", false) == "ffmpeg");
}

TEST_CASE("hub portable: the H.264 encoder is read off ffmpeg -encoders", "[HubPortable]")
{
    const std::string gpl =
        "Encoders:\n"
        " V..... = Video\n"
        " ------\n"
        " V....D libx264              libx264 H.264 / AVC / MPEG-4 AVC (codec h264)\n"
        " V....D libopenh264          OpenH264 H.264 / AVC / MPEG-4 AVC (codec h264)\n"
        " A....D aac                  AAC (Advanced Audio Coding)\n";
    const std::string lgpl =
        "Encoders:\r\n"
        " V....D libopenh264          OpenH264 H.264 / AVC / MPEG-4 AVC (codec h264)\r\n"
        " V....D mpeg4                MPEG-4 part 2\r\n";
    const std::string none =
        "Encoders:\n"
        " V....D mpeg4                MPEG-4 part 2\n"
        " A....D libx264              an audio line that merely mentions it\n"
        " libx264 on a line with no flags\n";
    CHECK(pick_h264_encoder(gpl) == H264Encoder::LibX264);
    CHECK(pick_h264_encoder(lgpl) == H264Encoder::LibOpenH264);
    CHECK(pick_h264_encoder(none) == H264Encoder::None);
    CHECK(pick_h264_encoder("") == H264Encoder::None);
}

TEST_CASE("hub portable: the templates name only flags their encoder has", "[HubPortable]")
{
    const std::string x = h264_template(H264Encoder::LibX264);
    const std::string o = h264_template(H264Encoder::LibOpenH264);
    CHECK(x.find("libx264") != std::string::npos);
    CHECK(x.find("-tune:v zerolatency") != std::string::npos);
    CHECK(o == "-codec:v libopenh264 -profile:v constrained_baseline -rc_mode bitrate -bf 0"); // what the bundled build has always used
    CHECK(o.find("-preset") == std::string::npos);
    CHECK(h264_template(H264Encoder::None).empty());
}
