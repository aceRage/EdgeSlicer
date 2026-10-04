#include <catch2/catch.hpp>

#include "slic3r/Utils/HubMedia.hpp"

using namespace Slic3r::HubMedia;

namespace {

const std::string ENCODERS_ALL =
    "Encoders:\n"
    " V....D libx264              libx264 H.264 / AVC\n"
    " V....D libopenh264          OpenH264 H.264 / AVC\n"
    " V....D h264_qsv             H.264 (Intel Quick Sync Video)\n"
    " V....D h264_vaapi           H.264/AVC (VAAPI)\n"
    " V....D h264_v4l2m2m         V4L2 mem2mem H.264 encoder wrapper\n"
    " V....D h264_nvenc           NVIDIA NVENC\n"
    " A....D aac                  AAC\n";

const std::string ENCODERS_LGPL =
    "Encoders:\n"
    " V....D libopenh264          OpenH264 H.264 / AVC\n"
    " V....D h264_qsv             H.264 (Intel Quick Sync Video)\n"
    " V....D h264_vaapi           H.264/AVC (VAAPI)\n"
    " V....D h264_v4l2m2m         V4L2 mem2mem H.264 encoder wrapper\n";

Host x86(int cores, bool dri) { Host h; h.os = Os::Linux; h.arch = Arch::X86; h.cores = cores; h.dri_render = dri; return h; }
Host arm(int cores, bool video11) { Host h; h.os = Os::Linux; h.arch = Arch::Arm; h.cores = cores; h.video11 = video11; return h; }
Host windows(int cores) { Host h; h.os = Os::Windows; h.arch = Arch::X86; h.cores = cores; return h; }

std::vector<std::string> order(const Host& h, const std::string& text)
{
    std::vector<std::string> out;
    for (const Candidate& c : candidates(h, encoder_names(text))) out.push_back(encoder_label(c.encoder));
    return out;
}

} // namespace

TEST_CASE("hub media: the encoder list is read from ffmpeg -encoders", "[HubMedia]")
{
    const auto n = encoder_names(ENCODERS_ALL);
    CHECK(n.count("libx264"));
    CHECK(n.count("h264_qsv"));
    CHECK(n.count("h264_v4l2m2m"));
    CHECK_FALSE(n.count("aac")); // audio
    CHECK(encoder_names("").empty());
}

TEST_CASE("hub media: x86 Linux tries QSV, VAAPI, then software", "[HubMedia]")
{
    CHECK(order(x86(8, true), ENCODERS_ALL) == std::vector<std::string>{ "qsv", "vaapi", "libx264", "libopenh264" });
    // no /dev/dri: no hardware candidates at all, whatever ffmpeg lists
    CHECK(order(x86(8, false), ENCODERS_ALL) == std::vector<std::string>{ "libx264", "libopenh264" });
    // the LGPL build has no libx264
    CHECK(order(x86(8, true), ENCODERS_LGPL) == std::vector<std::string>{ "qsv", "vaapi", "libopenh264" });
}

TEST_CASE("hub media: software on x86 needs enough cores", "[HubMedia]")
{
    CHECK(order(x86(2, false), ENCODERS_ALL).empty());
    CHECK(order(x86(3, false), ENCODERS_ALL).empty());
    CHECK(order(x86(4, false), ENCODERS_ALL) == std::vector<std::string>{ "libx264", "libopenh264" });
    // a small host with a GPU still gets the hardware encoder
    CHECK(order(x86(2, true), ENCODERS_LGPL) == std::vector<std::string>{ "qsv", "vaapi" });
}

TEST_CASE("hub media: ARM tries v4l2m2m (Pi 4), then software", "[HubMedia]")
{
    CHECK(order(arm(4, true), ENCODERS_ALL) == std::vector<std::string>{ "v4l2m2m", "libx264", "libopenh264" });
    // a Pi 5 has no hardware H.264 encoder: software is still a candidate (one Low transcode)
    CHECK(order(arm(4, false), ENCODERS_LGPL) == std::vector<std::string>{ "libopenh264" });
    // QSV/VAAPI are x86 things
    CHECK(order(arm(4, true), "Encoders:\n V....D h264_qsv  x\n V....D h264_vaapi x\n").empty());
}

TEST_CASE("hub media: Windows keeps software only, in the old order", "[HubMedia]")
{
    CHECK(order(windows(2), ENCODERS_LGPL) == std::vector<std::string>{ "libopenh264" }); // the bundled build, even on 2 cores
    CHECK(order(windows(8), ENCODERS_ALL) == std::vector<std::string>{ "libx264", "libopenh264" });
}

TEST_CASE("hub media: the first candidate that passes a test encode wins, the rest are not tried", "[HubMedia]")
{
    std::vector<std::string> tried;
    ProbeFn probe = [&](const Candidate& c) { tried.push_back(encoder_label(c.encoder)); return c.encoder == Encoder::Vaapi; };
    const Choice c = choose(x86(8, true), encoder_names(ENCODERS_ALL), probe);
    CHECK(c.encoder.encoder == Encoder::Vaapi);
    CHECK(tried == std::vector<std::string>{ "qsv", "vaapi" });
    REQUIRE(c.probes.size() == 2);
    CHECK_FALSE(c.probes[0].ok);
    CHECK(c.probes[1].ok);
    CHECK(c.quality == std::vector<std::string>{ "med", "low" });
    CHECK(c.default_cap == 6);
    CHECK(c.reason.find("hardware") != std::string::npos);
}

TEST_CASE("hub media: listed but failing encoders fall through to software", "[HubMedia]")
{
    ProbeFn probe = [](const Candidate& c) { return c.software; };
    const Choice c = choose(x86(8, true), encoder_names(ENCODERS_ALL), probe);
    CHECK(c.encoder.encoder == Encoder::X264);
    CHECK(c.probes.size() == 3);
    CHECK(c.default_cap == 4); // 8 cores / 2
}

TEST_CASE("hub media: nothing passes -> passthrough only, nothing offered", "[HubMedia]")
{
    ProbeFn never = [](const Candidate&) { return false; };
    const Choice c = choose(x86(8, true), encoder_names(ENCODERS_ALL), never);
    CHECK(c.encoder.encoder == Encoder::None);
    CHECK(c.quality.empty());
    CHECK(c.default_cap == 0);
    CHECK(c.reason.find("passthrough") != std::string::npos);

    bool called = false;
    ProbeFn spy = [&](const Candidate&) { called = true; return true; };
    const Choice none = choose(x86(2, false), encoder_names(ENCODERS_ALL), spy); // 2 cores, no GPU: no candidates
    CHECK_FALSE(called);
    CHECK(none.quality.empty());
}

TEST_CASE("hub media: a Pi-class host in software gets one transcode at Low", "[HubMedia]")
{
    ProbeFn ok = [](const Candidate&) { return true; };
    const Choice sw = choose(arm(4, false), encoder_names(ENCODERS_LGPL), ok);
    CHECK(sw.encoder.encoder == Encoder::OpenH264);
    CHECK(sw.quality == std::vector<std::string>{ "low" });
    CHECK(sw.default_cap == 1);

    const Choice hw = choose(arm(4, true), encoder_names(ENCODERS_LGPL), ok);
    CHECK(hw.encoder.encoder == Encoder::V4l2m2m);
    CHECK(hw.quality == std::vector<std::string>{ "med", "low" });
    CHECK(hw.default_cap == 2);
}

TEST_CASE("hub media: default caps per host", "[HubMedia]")
{
    ProbeFn ok = [](const Candidate&) { return true; };
    CHECK(choose(x86(4, false), encoder_names(ENCODERS_ALL), ok).default_cap == 2);
    CHECK(choose(x86(16, false), encoder_names(ENCODERS_ALL), ok).default_cap == 4);
    CHECK(choose(x86(16, true), encoder_names(ENCODERS_ALL), ok).default_cap == 6);
    CHECK(choose(windows(8), encoder_names(ENCODERS_LGPL), ok).default_cap == 0); // Windows: no cap, as before
    CHECK(choose(windows(8), encoder_names(ENCODERS_LGPL), ok).quality == std::vector<std::string>{ "med", "low" });
}

TEST_CASE("hub media: the cap setting", "[HubMedia]")
{
    CHECK(parse_cap("", 3) == 3);
    CHECK(parse_cap("2", 3) == 2);
    CHECK(parse_cap("0", 3) == 0);
    CHECK(parse_cap("32", 3) == 32);
    CHECK(parse_cap("33", 3) == 3);
    CHECK(parse_cap("-1", 3) == 3);
    CHECK(parse_cap("two", 3) == 3);
    CHECK(parse_cap("1000", 3) == 3);
}

TEST_CASE("hub media: the test encode runs the candidate's real chain", "[HubMedia]")
{
    const auto vaapi = test_encode_args("ffmpeg", make_candidate(Encoder::Vaapi));
    auto has = [&](const std::string& s) { return std::find(vaapi.begin(), vaapi.end(), s) != vaapi.end(); };
    CHECK(has("-vaapi_device"));
    CHECK(has("h264_vaapi"));
    CHECK(has("scale=320:-2,format=nv12,hwupload"));
    CHECK(vaapi.back() == "-");
    CHECK(vaapi[vaapi.size() - 2] == "null");
    const auto sw = test_encode_args("ffmpeg", make_candidate(Encoder::OpenH264));
    CHECK(std::find(sw.begin(), sw.end(), "scale=320:-2") != sw.end());
    CHECK(std::find(sw.begin(), sw.end(), "libopenh264") != sw.end());
}

TEST_CASE("hub media gate: a variant takes a slot, a second viewer of it shares it", "[HubMedia]")
{
    TranscodeGate g(1);
    auto a = g.admit("cam_low", true, "cam");
    CHECK(a.transcode);
    CHECK(a.served == "cam_low");
    auto b = g.admit("cam_low", true, "cam");
    CHECK(b.transcode);
    CHECK(g.active() == 1);
    g.release(a);
    CHECK(g.active() == 1); // still one viewer
    g.release(b);
    CHECK(g.active() == 0);
}

TEST_CASE("hub media gate: past the cap a viewer gets the source stream, not an error", "[HubMedia]")
{
    TranscodeGate g(1);
    auto printer_a = g.admit("a_low", true, "a");
    auto printer_b = g.admit("b_low", true, "b");
    CHECK(printer_a.transcode);
    CHECK_FALSE(printer_b.transcode);
    CHECK(printer_b.downgraded);
    CHECK(printer_b.served == "b");
    CHECK(g.downgraded_total() == 1);
    CHECK(g.active() == 1);
    // releasing the downgraded ticket changes nothing
    g.release(printer_b);
    CHECK(g.active() == 1);
}

TEST_CASE("hub media gate: leaving a printer frees the slot for the next one", "[HubMedia]")
{
    TranscodeGate g(1);
    auto a = g.admit("a_med", true, "a");
    g.release(a); // the last viewer of printer A left
    auto b = g.admit("b_med", true, "b");
    CHECK(b.transcode);
    CHECK(b.served == "b_med");
}

TEST_CASE("hub media gate: the source stream is never counted, and 0 means no cap", "[HubMedia]")
{
    TranscodeGate g(1);
    CHECK_FALSE(g.admit("a", false, "a").transcode);
    CHECK(g.active() == 0);
    TranscodeGate open(0);
    for (int i = 0; i < 10; ++i) CHECK(open.admit("c" + std::to_string(i) + "_low", true, "c" + std::to_string(i)).transcode);
    CHECK(open.active() == 10);
    g.set_cap(2);
    CHECK(g.cap() == 2);
}

TEST_CASE("hub media: variant names", "[HubMedia]")
{
    const std::vector<std::string> steps = { "med", "low" };
    auto v = split_variant("u1_cam_low", steps);
    CHECK(v.is_variant);
    CHECK(v.base == "u1_cam");
    CHECK(v.step == "low");
    CHECK_FALSE(split_variant("u1_cam", steps).is_variant);
    CHECK_FALSE(split_variant("_low", steps).is_variant);          // nothing before the suffix
    CHECK_FALSE(split_variant("cam_high", steps).is_variant);
    CHECK_FALSE(split_variant("cam_med", { "low" }).is_variant);   // a step this host does not offer
}

TEST_CASE("hub media: rewriting the src of a WebSocket request head", "[HubMedia]")
{
    const std::string head = "GET /api/ws?src=cam_low&lt=abc HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n\r\n";
    CHECK(replace_query_param(head, "src", "cam") == "GET /api/ws?src=cam&lt=abc HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n\r\n");
    CHECK(replace_query_param(head, "nope", "x") == head);
    CHECK(replace_query_param("GET /api/ws HTTP/1.1\r\nHost: x\r\n\r\n", "src", "cam") == "GET /api/ws HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(replace_query_param("GET /a?src=x HTTP/1.1", "src", "y") == "GET /a?src=y HTTP/1.1");
}

TEST_CASE("hub media ports: parsing and precedence", "[HubMedia]")
{
    CHECK(parse_port("8555") == 8555);
    CHECK(parse_port("1") == 1);
    CHECK(parse_port("65535") == 65535);
    CHECK(parse_port("65536") == 0);
    CHECK(parse_port("0") == 0);
    CHECK(parse_port("") == 0);
    CHECK(parse_port("85a5") == 0);
    CHECK(parse_port("-1") == 0);
    CHECK(configured_port("9000", 8000) == 9000);   // the environment (and --hub-... flags) beat the settings file
    CHECK(configured_port("", 8000) == 8000);
    CHECK(configured_port("junk", 8000) == 8000);
    CHECK(configured_port("", 0) == 0);
    CHECK(configured_port("", 70000) == 0);
}

TEST_CASE("hub media ports: a free requested port is used as asked", "[HubMedia]")
{
    auto r = resolve_port(1984, 0, 0, false, [](int) { return true; });
    CHECK(r.status == PortStatus::Ok);
    CHECK(r.port == 1984);
}

TEST_CASE("hub media ports: a taken requested port moves to the next free one on a desktop hub", "[HubMedia]")
{
    auto r = resolve_port(8555, 0, 0, false, [](int p) { return p != 8555 && p != 8556; });
    CHECK(r.status == PortStatus::Moved);
    CHECK(r.port == 8557);
    CHECK(r.asked == 8555);
}

TEST_CASE("hub media ports: a taken requested port is a failure in service mode", "[HubMedia]")
{
    auto r = resolve_port(8555, 0, 0, true, [](int p) { return p != 8555; });
    CHECK(r.status == PortStatus::Failed);
    CHECK(r.port == 0);
    CHECK(r.asked == 8555);
}

TEST_CASE("hub media ports: no free port nearby is a failure either way", "[HubMedia]")
{
    CHECK(resolve_port(8555, 0, 0, false, [](int) { return false; }).status == PortStatus::Failed);
    CHECK(resolve_port(65535, 0, 0, false, [](int p) { return p != 65535; }).status == PortStatus::Failed); // nothing above 65535
}

TEST_CASE("hub media ports: the automatic range takes the first free port", "[HubMedia]")
{
    auto r = resolve_port(0, 8555, 8574, false, [](int p) { return p >= 8558; });
    CHECK(r.status == PortStatus::Ok);
    CHECK(r.port == 8558);
    CHECK(resolve_port(0, 8555, 8574, true, [](int) { return false; }).status == PortStatus::Failed);
}

TEST_CASE("hub media: the RTSP restream is loopback unless credentials say otherwise", "[HubMedia]")
{
    auto def = plan_rtsp("", "", "");
    CHECK(def.listen_host == "127.0.0.1");
    CHECK_FALSE(def.exposed);
    CHECK(def.warning.empty());
    CHECK_FALSE(plan_rtsp("localhost", "", "").exposed);

    auto open = plan_rtsp("0.0.0.0", "", "");
    CHECK_FALSE(open.exposed);                       // no credentials: refused, stays on loopback
    CHECK_FALSE(open.warning.empty());
    CHECK(open.listen_host == "127.0.0.1");

    auto ok = plan_rtsp("0.0.0.0", "ha", "secret");
    CHECK(ok.exposed);
    CHECK(ok.listen_host == "0.0.0.0");
    CHECK(ok.user == "ha");
    CHECK(plan_rtsp("192.168.1.50", "ha", "secret").exposed);
    CHECK_FALSE(plan_rtsp("nas.local", "ha", "secret").exposed); // a name is not an address here
    CHECK_FALSE(plan_rtsp("not an address", "ha", "secret").warning.empty());
}
