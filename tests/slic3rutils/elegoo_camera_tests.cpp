// The Elegoo Centauri Carbon (CC1) camera relay (src/slic3r/GUI/ElegooCamRelay.*).
//
// Owner, 2026-10-18: a CC1 connected through an Elegoo Link printer never appeared in the Stream
// tab. Its camera is MJPEG on port 3031 and the printer lets only 4 viewers in, so the relay keeps
// ONE connection per printer for any number of tiles/phones, says "Camera busy: too many viewers
// (Elegoo allows 4)" when the printer refuses, and only asks over SDCP (Cmd 386) when asking the
// stream directly gave nothing.
//
// The first half is pure (URL building, SDCP request/reply, the busy mapping, the JPEG splitter).
// The second half stands a fake printer up on loopback ports (an MJPEG server and an SDCP
// websocket server) and drives the relay's real serve_client() with several viewers. Nothing here
// touches a real printer or the network beyond 127.0.0.1.

#include <catch2/catch.hpp>

#include "slic3r/GUI/ElegooCamRelay.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace Slic3r::GUI::ElegooCam;
namespace ecam      = Slic3r::GUI::ElegooCam;
namespace asio      = boost::asio;
namespace beast     = boost::beast;
namespace websocket = beast::websocket;
using tcp           = asio::ip::tcp;

// ---------------------------------------------------------------------------------------------
// Pure helpers
// ---------------------------------------------------------------------------------------------

TEST_CASE("Elegoo camera: hosts are taken out of the printer's address field", "[ElegooCam]")
{
    CHECK(host_from_address("192.168.1.50") == "192.168.1.50");
    CHECK(host_from_address("  192.168.1.50  ") == "192.168.1.50");
    CHECK(host_from_address("http://192.168.1.50:80/") == "192.168.1.50");
    CHECK(host_from_address("https://user:pw@printer.local:8443/a/b?c=d") == "printer.local");
    CHECK(host_from_address("").empty());
    CHECK(host_from_address("http://").empty());
    CHECK(host_from_address("bad host").empty());
    CHECK(host_from_address("a;b").empty());

    CHECK(valid_host("printer.local"));
    CHECK(valid_host("10.0.0.2"));
    CHECK_FALSE(valid_host(""));
    CHECK_FALSE(valid_host("10.0.0.2:3031"));
    CHECK_FALSE(valid_host("a/b"));
    CHECK_FALSE(valid_host("a b"));
    CHECK_FALSE(valid_host(std::string(300, 'a')));
}

TEST_CASE("Elegoo camera: the Centauri Carbon 2 is not this relay's printer", "[ElegooCam]")
{
    CHECK_FALSE(is_cc2_name("Elegoo Centauri Carbon 0.4 nozzle"));
    CHECK_FALSE(is_cc2_name("My CC1"));
    CHECK(is_cc2_name("Elegoo Centauri Carbon 2 0.4 nozzle"));
    CHECK(is_cc2_name("Elegoo Centauri 2 0.4 nozzle"));
    CHECK(is_cc2_name("elegoo centauri carbon 2"));
}

TEST_CASE("Elegoo camera: the stream request is HTTP/1.0 for the printer's /video", "[ElegooCam]")
{
    VideoTarget t;
    t.host = "192.168.1.50";
    const std::string req = video_request(t);
    CHECK(req.compare(0, 24, "GET /video HTTP/1.0\r\nHos") == 0);
    CHECK(req.find("Host: 192.168.1.50:3031\r\n") != std::string::npos);
    CHECK(req.size() > 4);
    CHECK(req.compare(req.size() - 4, 4, "\r\n\r\n") == 0);
    CHECK(kVideoPort == 3031);
    CHECK(kControlPort == 3030);
    CHECK(kStreamLimit == 4);
}

TEST_CASE("Elegoo camera: the VideoUrl the printer returns", "[ElegooCam]")
{
    VideoTarget t;
    // Firmware V1.4.49 returns no scheme.
    REQUIRE(video_target_from_reply("192.168.1.50:3031/video", "192.168.1.50", t));
    CHECK(t.host == "192.168.1.50");
    CHECK(t.port == 3031);
    CHECK(t.path == "/video");

    REQUIRE(video_target_from_reply("http://192.168.1.50:3031/video", "192.168.1.50", t));
    CHECK(t.port == 3031);

    // "this host" spellings mean the address we reached it on.
    REQUIRE(video_target_from_reply("http://0.0.0.0:3031/video", "192.168.1.50", t));
    CHECK(t.host == "192.168.1.50");
    REQUIRE(video_target_from_reply("localhost:3031/video", "192.168.1.50", t));
    CHECK(t.host == "192.168.1.50");

    // A different host, a different scheme, rubbish: refused, so the relay can't be pointed elsewhere.
    CHECK_FALSE(video_target_from_reply("http://10.9.9.9:3031/video", "192.168.1.50", t));
    CHECK_FALSE(video_target_from_reply("rtsp://192.168.1.50:554/live", "192.168.1.50", t));
    CHECK_FALSE(video_target_from_reply("", "192.168.1.50", t));
    CHECK_FALSE(video_target_from_reply("192.168.1.50:99999/video", "192.168.1.50", t));
    CHECK_FALSE(video_target_from_reply("192.168.1.50:3031/vi deo", "192.168.1.50", t));
    CHECK_FALSE(video_target_from_reply("192.168.1.50:3031/video\r\nX: y", "192.168.1.50", t));
}

TEST_CASE("Elegoo camera: the SDCP Cmd 386 request", "[ElegooCam]")
{
    const nlohmann::json j = nlohmann::json::parse(enable_request(true, "abc123", 1700000000));
    CHECK(j["Data"]["Cmd"] == 386);
    CHECK(j["Data"]["Data"]["Enable"] == 1);
    CHECK(j["Data"]["RequestID"] == "abc123");
    CHECK(j["Data"]["MainboardID"] == "");
    CHECK(j["Data"]["TimeStamp"] == 1700000000);
    CHECK(nlohmann::json::parse(enable_request(false, "x", 1))["Data"]["Data"]["Enable"] == 0);
}

TEST_CASE("Elegoo camera: a Cmd 386 reply is told from a status push, and Ack 1 is busy", "[ElegooCam]")
{
    const EnableReply ok = parse_enable_reply(
        R"({"Id":"x","Data":{"Cmd":386,"Data":{"Ack":0,"VideoUrl":"10.0.0.2:3031/video"},"RequestID":"r","MainboardID":"m","TimeStamp":1},"Topic":"sdcp/response/m"})");
    CHECK(ok.valid);
    CHECK(ok.ack == 0);
    CHECK(ok.video_url == "10.0.0.2:3031/video");

    const EnableReply busy = parse_enable_reply(R"({"Data":{"Cmd":386,"Data":{"Ack":1}}})");
    CHECK(busy.valid);
    CHECK(busy.ack == 1);

    // A status push, another command's answer, junk: not ours.
    CHECK_FALSE(parse_enable_reply(R"({"Topic":"sdcp/status/m","Status":{"CurrentStatus":[0]}})").valid);
    CHECK_FALSE(parse_enable_reply(R"({"Data":{"Cmd":0,"Data":{"Ack":0}}})").valid);
    CHECK_FALSE(parse_enable_reply("pong").valid);
    CHECK_FALSE(parse_enable_reply("").valid);

    CHECK(state_from_ack(0) == State::Live);
    CHECK(state_from_ack(1) == State::Busy);
    CHECK(state_from_ack(2) == State::NoCamera);
    CHECK(state_from_ack(3) == State::Failed);
    CHECK(state_from_ack(-1) == State::Failed);
    CHECK(state_from_ack(77) == State::Failed);
}

TEST_CASE("Elegoo camera: busy-state mapping and the words on the tile", "[ElegooCam]")
{
    CHECK(state_from_http_status(200) == State::Live);
    CHECK(state_from_http_status(503) == State::Busy);
    CHECK(state_from_http_status(429) == State::Busy);
    CHECK(state_from_http_status(404) == State::Failed);
    CHECK(state_from_http_status(0) == State::Failed);

    CHECK(parse_http_status("HTTP/1.0 200 OK\r\nA: b\r\n\r\n") == 200);
    CHECK(parse_http_status("HTTP/1.1 503 Service Unavailable\r\n\r\n") == 503);
    CHECK(parse_http_status("garbage") == 0);
    CHECK(parse_http_status("") == 0);

    CHECK(message_for(State::Busy) == "Camera busy: too many viewers (Elegoo allows 4).");
    CHECK(message_for(State::NoCamera).find("no camera") != std::string::npos);
    CHECK(message_for(State::Failed).find("only 4") != std::string::npos);
    CHECK(message_for(State::Live).empty());

    const nlohmann::json j = nlohmann::json::parse(status_json(State::Busy));
    CHECK(j["state"] == "busy");
    CHECK(j["message"] == message_for(State::Busy));
    CHECK(nlohmann::json::parse(status_json(State::NoCamera))["state"] == "no_camera");
    CHECK(std::string(state_name(State::Idle)) == "idle");
}

TEST_CASE("Elegoo camera: JPEG frames are cut out of the byte stream", "[ElegooCam]")
{
    auto jpeg = [](char fill, int n) {
        std::string s = "\xFF\xD8";
        s.append((size_t) n, fill);
        s += "\xFF\xD9";
        return s;
    };
    const std::string a = jpeg('a', 50), b = jpeg('b', 70);
    const std::string stream = "--frame\r\nContent-Type: image/jpeg\r\n\r\n" + a + "\r\n--frame\r\nContent-Type: image/jpeg\r\n\r\n" + b + "\r\n";

    SECTION("whole")
    {
        JpegSplitter s;
        s.feed(stream.data(), stream.size());
        std::string f;
        REQUIRE(s.next(f));
        CHECK(f == a);
        REQUIRE(s.next(f));
        CHECK(f == b);
        CHECK_FALSE(s.next(f));
    }
    SECTION("a byte at a time")
    {
        JpegSplitter s;
        std::vector<std::string> got;
        std::string f;
        for (char c : stream) {
            s.feed(&c, 1);
            while (s.next(f)) got.push_back(f);
        }
        REQUIRE(got.size() == 2);
        CHECK(got[0] == a);
        CHECK(got[1] == b);
    }
    SECTION("a marker split between reads")
    {
        JpegSplitter s;
        std::string f;
        const std::string head = "junk\xFF";
        s.feed(head.data(), head.size());
        CHECK_FALSE(s.next(f));
        const std::string rest = a.substr(1);
        s.feed(rest.data(), rest.size());
        REQUIRE(s.next(f));
        CHECK(f == a);
    }
    SECTION("nothing but noise")
    {
        JpegSplitter s;
        std::string f;
        const std::string noise(1000, 'x');
        s.feed(noise.data(), noise.size());
        CHECK_FALSE(s.next(f));
    }
}

TEST_CASE("Elegoo camera: which relay requests are ours", "[ElegooCam]")
{
    CHECK(is_elegoo_target("/elegoo?ip=1.2.3.4"));
    CHECK(is_elegoo_target("/elegoo/status?ip=1.2.3.4"));
    CHECK(is_elegoo_target("/elegoo"));
    CHECK_FALSE(is_elegoo_target("/bambu?ip=1.2.3.4&code=1"));
    CHECK_FALSE(is_elegoo_target("/elegooX"));
}

// ---------------------------------------------------------------------------------------------
// A fake printer and the relay's real serve_client()
// ---------------------------------------------------------------------------------------------

namespace {

constexpr const char* kHost = "127.0.0.1";

std::string fake_jpeg()
{
    std::string s = "\xFF\xD8";
    s.append(100, '\x11');
    s += "\xFF\xD9";
    return s;
}

struct FakePrinter
{
    enum VideoMode { Stream = 0, CloseAtOnce = 1, Http503 = 2 };

    asio::io_context ioc;
    tcp::acceptor    video { ioc, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0) };
    tcp::acceptor    control { ioc, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0) };

    std::atomic<int>  video_total { 0 };
    std::atomic<int>  video_current { 0 };
    std::atomic<int>  sdcp_requests { 0 };
    std::atomic<int>  video_mode { Stream };
    std::atomic<int>  refuse_first { 0 };    // close this many video connections at once, then stream
    std::atomic<int>  ack { 0 };
    std::atomic<bool> stop { false };
    std::mutex        mtx;
    std::string       last_enable;

    int video_port() { return video.local_endpoint().port(); }
    int control_port() { return control.local_endpoint().port(); }

    void start()
    {
        std::thread([this]() { accept_video(); }).detach();
        std::thread([this]() { accept_control(); }).detach();
    }
    ~FakePrinter()
    {
        stop = true;
        beast::error_code ig;
        video.close(ig);
        control.close(ig);
        // Detached threads may still be unwinding; give them a moment to leave before ioc goes.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    void accept_video()
    {
        while (!stop) {
            auto sock = std::make_shared<tcp::socket>(ioc);
            beast::error_code ec;
            video.accept(*sock, ec);
            if (ec)
                return;
            std::thread([this, sock]() { serve_video(sock); }).detach();
        }
    }
    void serve_video(std::shared_ptr<tcp::socket> sock)
    {
        beast::error_code ec;
        asio::streambuf  buf;
        asio::read_until(*sock, buf, "\r\n\r\n", ec);
        const int n = ++video_total;
        if (ec)
            return;
        if (n <= refuse_first || video_mode == CloseAtOnce) {
            sock->close(ec);
            return;
        }
        if (video_mode == Http503) {
            const std::string r = "HTTP/1.0 503 Service Unavailable\r\n\r\n";
            asio::write(*sock, asio::buffer(r), ec);
            sock->close(ec);
            return;
        }
        ++video_current;
        const std::string head = "HTTP/1.0 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=f\r\n\r\n";
        asio::write(*sock, asio::buffer(head), ec);
        const std::string frame = "--f\r\nContent-Type: image/jpeg\r\n\r\n" + fake_jpeg() + "\r\n";
        while (!ec && !stop) {
            asio::write(*sock, asio::buffer(frame), ec);
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        --video_current;
    }

    void accept_control()
    {
        while (!stop) {
            auto sock = std::make_shared<tcp::socket>(ioc);
            beast::error_code ec;
            control.accept(*sock, ec);
            if (ec)
                return;
            std::thread([this, sock]() { serve_control(sock); }).detach();
        }
    }
    void serve_control(std::shared_ptr<tcp::socket> sock)
    {
        try {
            websocket::stream<tcp::socket> ws(std::move(*sock));
            ws.accept();
            beast::flat_buffer buf;
            ws.text(true);
            // The printer pushes status on its own; the relay has to look past it.
            ws.write(asio::buffer(std::string(R"({"Topic":"sdcp/status/m","Status":{"CurrentStatus":[0]}})")));
            ws.read(buf);
            {
                std::lock_guard<std::mutex> lk(mtx);
                last_enable = beast::buffers_to_string(buf.data());
            }
            ++sdcp_requests;
            // A real printer takes a moment; it also lets a second viewer arrive while the first waits.
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            nlohmann::json r;
            r["Id"]                     = "x";
            r["Data"]["Cmd"]            = 386;
            r["Data"]["Data"]["Ack"]    = ack.load();
            if (ack == 0)
                r["Data"]["Data"]["VideoUrl"] = std::string(kHost) + ":" + std::to_string(video_port()) + "/video"; // no scheme, as on V1.4.49
            r["Data"]["RequestID"]      = "r";
            r["Data"]["MainboardID"]    = "m";
            r["Data"]["TimeStamp"]      = 1;
            r["Topic"]                  = "sdcp/response/m";
            ws.write(asio::buffer(r.dump()));
            for (;;) { // until the relay closes
                buf.consume(buf.size());
                ws.read(buf);
            }
        } catch (...) {
        }
    }
};

// One viewer of the relay: the relay's serve_client() on one end of a loopback pair, a reader on the other.
struct Viewer
{
    asio::io_context ioc;
    tcp::socket      server_side { ioc };
    tcp::socket      client_side { ioc };
    std::mutex       mtx;
    std::string      data;
    std::atomic<bool> done { false };
    std::atomic<bool> served { false };

    std::string text()
    {
        std::lock_guard<std::mutex> lk(mtx);
        return data;
    }
    int frames()
    {
        const std::string t = text();
        int               n = 0;
        for (size_t p = t.find("--elegooframe"); p != std::string::npos; p = t.find("--elegooframe", p + 1))
            ++n;
        return n;
    }
    void close()
    {
        // shutdown wakes the reader thread out of its read before the socket is closed under it.
        beast::error_code ig;
        client_side.shutdown(tcp::socket::shutdown_both, ig);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        client_side.close(ig);
    }
};

std::shared_ptr<Viewer> open_viewer(const std::string& target)
{
    auto v = std::make_shared<Viewer>();
    tcp::acceptor acc(v->ioc, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    v->client_side.connect(acc.local_endpoint());
    acc.accept(v->server_side);
    std::thread([v, target]() {
        try {
            serve_client(v->server_side, target);
        } catch (...) {
        }
        beast::error_code ig;
        v->server_side.close(ig);
        v->served = true;
    }).detach();
    std::thread([v]() {
        char buf[8192];
        for (;;) {
            beast::error_code ec;
            const size_t n = v->client_side.read_some(asio::buffer(buf), ec);
            if (ec)
                break;
            std::lock_guard<std::mutex> lk(v->mtx);
            v->data.append(buf, n);
        }
        v->done = true;
    }).detach();
    return v;
}

template<class Pred>
bool wait_until(Pred p, int ms)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (p())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return p();
}

// The session lets go a moment after the last viewer; every case waits for that so the next one
// starts from nothing.
struct Fixture
{
    FakePrinter printer;
    Fixture()
    {
        printer.start();
        ecam::Testing::set_ports(printer.video_port(), printer.control_port());
    }
    ~Fixture()
    {
        wait_until([] { return ecam::Testing::session_count() == 0; }, 8000);
        ecam::Testing::set_ports(0, 0);
    }
};

const std::string kStream = std::string("/elegoo?ip=") + kHost;
const std::string kStatus = std::string("/elegoo/status?ip=") + kHost;

} // namespace

TEST_CASE("Elegoo camera relay: any number of viewers share ONE connection to the printer", "[ElegooCam][relay]")
{
    Fixture f;
    auto a = open_viewer(kStream);
    auto b = open_viewer(kStream);
    auto c = open_viewer(kStream);
    auto d = open_viewer(kStream);
    auto e = open_viewer(kStream);
    REQUIRE(wait_until([&] { return a->frames() >= 3 && b->frames() >= 3 && c->frames() >= 3 && d->frames() >= 3 && e->frames() >= 3; }, 10000));

    // Five viewers - more than the printer would admit - and the printer saw one.
    CHECK(f.printer.video_total == 1);
    CHECK(f.printer.video_current == 1);
    // The stream answered directly, so nothing was sent over SDCP.
    CHECK(f.printer.sdcp_requests == 0);
    CHECK(a->text().find("200 OK") != std::string::npos);
    CHECK(a->text().find("multipart/x-mixed-replace") != std::string::npos);
    CHECK(a->text().find("image/jpeg") != std::string::npos);

    // The status endpoint says it is live.
    auto st = open_viewer(kStatus);
    REQUIRE(wait_until([&] { return st->done.load(); }, 5000));
    CHECK(st->text().find("\"state\":\"live\"") != std::string::npos);
    CHECK(st->text().find("Access-Control-Allow-Origin: *") != std::string::npos);

    // They leave one by one: the connection stays until the last one has gone, then it is closed.
    a->close();
    b->close();
    c->close();
    d->close();
    CHECK(f.printer.video_total == 1);
    REQUIRE(wait_until([&] { return d->served.load(); }, 5000));
    CHECK(f.printer.video_current == 1);
    e->close();
    CHECK(wait_until([&] { return f.printer.video_current == 0; }, 8000));
}

TEST_CASE("Elegoo camera relay: a printer that is full says so", "[ElegooCam][relay]")
{
    Fixture f;
    f.printer.video_mode = FakePrinter::CloseAtOnce; // the stream is refused ...
    f.printer.ack        = 1;                        // ... and SDCP says why: exceeded the viewer limit

    auto a = open_viewer(kStream);
    auto b = open_viewer(kStream);
    REQUIRE(wait_until([&] { return a->done.load() && b->done.load(); }, 20000));

    for (auto& v : { a, b }) {
        const std::string t = v->text();
        CHECK(t.find("503") != std::string::npos);
        CHECK(t.find("X-Elegoo-Camera: busy") != std::string::npos);
        CHECK(t.find("Camera busy: too many viewers (Elegoo allows 4).") != std::string::npos);
    }
    // One question to the printer for both viewers, and it was the stream-enable command.
    CHECK(f.printer.sdcp_requests == 1);
    {
        std::lock_guard<std::mutex> lk(f.printer.mtx);
        const nlohmann::json j = nlohmann::json::parse(f.printer.last_enable);
        CHECK(j["Data"]["Cmd"] == 386);
        CHECK(j["Data"]["Data"]["Enable"] == 1);
    }

    // The tile asks why it is blank: the refusal is remembered for a while.
    auto st = open_viewer(kStatus);
    REQUIRE(wait_until([&] { return st->done.load(); }, 5000));
    CHECK(st->text().find("\"state\":\"busy\"") != std::string::npos);
    CHECK(st->text().find("Camera busy: too many viewers (Elegoo allows 4).") != std::string::npos);
}

TEST_CASE("Elegoo camera relay: an HTTP 503 from the stream is busy without bothering SDCP", "[ElegooCam][relay]")
{
    Fixture f;
    f.printer.video_mode = FakePrinter::Http503;
    auto a = open_viewer(kStream);
    REQUIRE(wait_until([&] { return a->done.load(); }, 20000));
    CHECK(a->text().find("X-Elegoo-Camera: busy") != std::string::npos);
    CHECK(f.printer.sdcp_requests == 0);
}

TEST_CASE("Elegoo camera relay: a printer with no camera says so", "[ElegooCam][relay]")
{
    Fixture f;
    f.printer.video_mode = FakePrinter::CloseAtOnce;
    f.printer.ack        = 2;
    auto a = open_viewer(kStream);
    REQUIRE(wait_until([&] { return a->done.load(); }, 20000));
    CHECK(a->text().find("X-Elegoo-Camera: no_camera") != std::string::npos);
    CHECK(a->text().find("no camera") != std::string::npos);
}

TEST_CASE("Elegoo camera relay: when the stream needs enabling, SDCP does it once and the picture follows", "[ElegooCam][relay]")
{
    Fixture f;
    f.printer.refuse_first = 1; // the first ask is turned away, as by a camera that is switched off
    f.printer.ack          = 0;

    auto a = open_viewer(kStream);
    auto b = open_viewer(kStream);
    REQUIRE(wait_until([&] { return a->frames() >= 2 && b->frames() >= 2; }, 20000));
    CHECK(f.printer.sdcp_requests == 1);
    CHECK(f.printer.video_total == 2); // the refused ask and the one that played
    CHECK(f.printer.video_current == 1);
    a->close();
    b->close();
}

TEST_CASE("Elegoo camera relay: a printer nobody can reach gives a clear failure, not a hang", "[ElegooCam][relay]")
{
    Fixture f;
    // Both of the fake printer's ports go away.
    f.printer.stop = true;
    beast::error_code ig;
    f.printer.video.close(ig);
    f.printer.control.close(ig);
    auto a = open_viewer(kStream);
    REQUIRE(wait_until([&] { return a->done.load(); }, 30000));
    CHECK(a->text().find("503") != std::string::npos);
    CHECK(a->text().find("X-Elegoo-Camera: failed") != std::string::npos);
}
