// Sending G-code to a print host: the cases where a send hung, reported the wrong outcome, or went to
// the wrong address.
//
// No printer is contacted: every listener here is the test's own, on 127.0.0.1 and an ephemeral
// port, and the rest are the pure functions the send paths call.

#include <catch2/catch.hpp>

#include "slic3r/Utils/ElegooLink.hpp"
#include "slic3r/Utils/MoonRaker.hpp"
#include "slic3r/Utils/PrintHost.hpp"
#include "slic3r/Utils/WebSocketClient.hpp"
#include "slic3r/GUI/SnapmakerLan.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

namespace beast     = boost::beast;
namespace http      = beast::http;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
using tcp           = net::ip::tcp;

// A printer that takes the connection and then says nothing until released: a websocket that
// completes the upgrade, or an HTTP server that reads the request head and never the body.
enum class Quiet { WebSocket, HttpAfterHead };

class QuietPrinter
{
public:
    explicit QuietPrinter(Quiet kind) : m_kind(kind)
    {
        m_accept_thread = std::thread([this] {
            for (;;) {
                auto              ioc = std::make_unique<net::io_context>();
                tcp::socket       socket{*ioc};
                beast::error_code ec;
                m_acceptor.accept(socket, ec);
                if (ec || m_stopping)
                    return;
                m_connections.emplace_back([this, ioc = std::move(ioc), socket = std::move(socket)]() mutable { serve(std::move(socket)); });
            }
        });
    }
    ~QuietPrinter()
    {
        release();
        m_stopping = true;
        net::io_context   wake_ioc;
        tcp::socket       wake{wake_ioc};
        beast::error_code ec;
        wake.connect(m_acceptor.local_endpoint(), ec);
        if (m_accept_thread.joinable())
            m_accept_thread.join();
        for (std::thread& t : m_connections)
            if (t.joinable())
                t.join();
    }
    unsigned short port() const { return m_acceptor.local_endpoint().port(); }
    void           release()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_released = true;
        m_changed.notify_all();
    }

private:
    void serve(tcp::socket socket)
    {
        beast::flat_buffer buffer;
        beast::error_code  ec;
        if (m_kind == Quiet::WebSocket) {
            websocket::stream<tcp::socket> ws{std::move(socket)};
            ws.accept(ec);
            if (!ec)
                wait();
            ws.next_layer().close(ec);
        } else {
            http::request_parser<http::string_body> head;
            head.body_limit(boost::none);
            http::read_header(socket, buffer, head, ec);
            if (!ec)
                wait();
            socket.close(ec);
        }
    }
    void wait()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_changed.wait(lock, [this] { return m_released; });
    }

    const Quiet              m_kind;
    net::io_context          m_accept_ioc;
    tcp::acceptor            m_acceptor{m_accept_ioc, {net::ip::make_address("127.0.0.1"), 0}};
    std::atomic<bool>        m_stopping{false};
    std::thread              m_accept_thread;
    std::vector<std::thread> m_connections;
    std::mutex               m_mutex;
    std::condition_variable  m_changed;
    bool                     m_released = false;
};

// Runs `call`, giving it `limit` to return before the printer lets go of it.
template<class Call> bool finishes_within(QuietPrinter& printer, std::chrono::milliseconds limit, Call&& call)
{
    auto       done    = std::async(std::launch::async, std::forward<Call>(call));
    const bool in_time = done.wait_for(limit) == std::future_status::ready;
    printer.release();
    if (done.wait_for(std::chrono::seconds(30)) != std::future_status::ready) {
        std::cerr << "a send was still running 30 s after the printer let go of it" << std::endl;
        std::_Exit(EXIT_FAILURE);
    }
    return in_time;
}

// Answers every request with one fixed reply, and keeps the target and X-Api-Key of each.
class FixedReplyServer
{
public:
    explicit FixedReplyServer(std::string body) : m_body(std::move(body))
    {
        m_thread = std::thread([this] {
            for (;;) {
                tcp::socket       socket{m_ioc};
                beast::error_code ec;
                m_acceptor.accept(socket, ec);
                if (ec || m_stopping)
                    return;
                beast::flat_buffer               buffer;
                http::request<http::string_body> request;
                http::read(socket, buffer, request, ec);
                if (ec)
                    continue;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_targets.emplace_back(request.target());
                    m_keys.emplace_back(request["X-Api-Key"]);
                }
                http::response<http::string_body> response{http::status::ok, request.version()};
                response.set(http::field::content_type, "application/json");
                response.body() = m_body;
                response.prepare_payload();
                http::write(socket, response, ec);
                socket.shutdown(tcp::socket::shutdown_both, ec);
            }
        });
    }
    ~FixedReplyServer()
    {
        m_stopping = true;
        net::io_context   wake_ioc;
        tcp::socket       wake{wake_ioc};
        beast::error_code ec;
        wake.connect(m_acceptor.local_endpoint(), ec);
        if (m_thread.joinable())
            m_thread.join();
    }
    unsigned short           port() const { return m_acceptor.local_endpoint().port(); }
    std::vector<std::string> targets() const { std::lock_guard<std::mutex> lock(m_mutex); return m_targets; }
    std::vector<std::string> keys() const { std::lock_guard<std::mutex> lock(m_mutex); return m_keys; }

private:
    std::string              m_body;
    net::io_context          m_ioc;
    tcp::acceptor            m_acceptor{m_ioc, {net::ip::make_address("127.0.0.1"), 0}};
    std::atomic<bool>        m_stopping{false};
    std::thread              m_thread;
    mutable std::mutex       m_mutex;
    std::vector<std::string> m_targets;
    std::vector<std::string> m_keys;
};

DynamicPrintConfig host_config(const std::string& host_type, const std::string& url)
{
    DynamicPrintConfig cfg;
    cfg.set_deserialize_strict({{"printer_technology", "FFF"}, {"host_type", host_type}, {"print_host", url},
                                {"printhost_apikey", "k"}, {"printhost_cafile", ""}, {"printhost_ssl_ignore_revoke", "1"},
                                {"printhost_authorization_type", "key"}, {"printhost_user", ""}, {"printhost_password", ""}});
    return cfg;
}

} // namespace

// ---- 4: the Elegoo SDCP websocket ----

TEST_CASE("WebSocketClient::receive gives up after its timeout", "[ElegooLink][sending][socket]")
{
    QuietPrinter    printer(Quiet::WebSocket);
    WebSocketClient client;
    client.connect("127.0.0.1", std::to_string(printer.port()), "/websocket");
    bool threw = false;
    CHECK(finishes_within(printer, std::chrono::seconds(3), [&] {
        try {
            client.receive(300);
        } catch (const std::exception&) {
            threw = true;
        }
    }));
    CHECK(threw);
}

TEST_CASE("WebSocketClient::receive gives up at the deadline its caller set", "[ElegooLink][sending][socket]")
{
    QuietPrinter    printer(Quiet::WebSocket);
    WebSocketClient client;
    client.connect("127.0.0.1", std::to_string(printer.port()), "/websocket");
    client.set_deadline(std::chrono::steady_clock::now() + std::chrono::milliseconds(300));
    bool timed_out = false;
    CHECK(finishes_within(printer, std::chrono::seconds(3), [&] {
        try {
            client.receive();
        } catch (const WebSocketClient::Timeout&) {
            timed_out = true;
        } catch (const std::exception&) {}
    }));
    CHECK(timed_out);
}

// ---- 6: an answer that came is never a time-out ----

TEST_CASE("An Elegoo start answer that arrives late is not reported as a time-out", "[ElegooLink][sending]")
{
    using std::chrono::seconds;
    CHECK_FALSE(ElegooLink::wait_timed_out(true, seconds(31), seconds(30)));
    CHECK_FALSE(ElegooLink::wait_timed_out(true, seconds(5), seconds(30)));
    CHECK(ElegooLink::wait_timed_out(false, seconds(30), seconds(30)));
    CHECK_FALSE(ElegooLink::wait_timed_out(false, seconds(29), seconds(30)));
}

// ---- 7 and 8: Moonraker's port 8080 retry ----

TEST_CASE("The Moonraker port 8080 address replaces the port, with or without a scheme", "[Moonraker][sending]")
{
    const std::string p = "server/files/upload";
    CHECK(Moonraker::url_on_port_8080("192.168.1.5", p) == "http://192.168.1.5:8080/server/files/upload");
    CHECK(Moonraker::url_on_port_8080("192.168.1.5:7125", p) == "http://192.168.1.5:8080/server/files/upload");
    CHECK(Moonraker::url_on_port_8080("http://192.168.1.5:7125", p) == "http://192.168.1.5:8080/server/files/upload");
    CHECK(Moonraker::url_on_port_8080("http://192.168.1.5:7125/", p) == "http://192.168.1.5:8080/server/files/upload");
    CHECK(Moonraker::url_on_port_8080("https://printer.local:7125", p) == "https://printer.local:8080/server/files/upload");
    CHECK(Moonraker::url_on_port_8080("http://printer.local/", p) == "http://printer.local:8080/server/files/upload");
    CHECK(Moonraker::url_on_port_8080("http://user:pw@192.168.1.5:7125", p) == "http://user:pw@192.168.1.5:8080/server/files/upload");
    CHECK(Moonraker::url_on_port_8080("http://[fe80::1]:7125", p) == "http://[fe80::1]:8080/server/files/upload");
}

TEST_CASE("Moonraker retries an upload on port 8080 only when nothing answered as Moonraker", "[Moonraker][sending]")
{
    CHECK(Moonraker::retry_on_port_8080(0));   // no HTTP answer at all: refused, timed out, unreachable
    CHECK(Moonraker::retry_on_port_8080(404)); // something answered, but has no Moonraker API there
    CHECK_FALSE(Moonraker::retry_on_port_8080(400));
    CHECK_FALSE(Moonraker::retry_on_port_8080(401));
    CHECK_FALSE(Moonraker::retry_on_port_8080(403));
    CHECK_FALSE(Moonraker::retry_on_port_8080(409));
    CHECK_FALSE(Moonraker::retry_on_port_8080(413));
    CHECK_FALSE(Moonraker::retry_on_port_8080(500));
}

// ---- 9: OctoPrint's test with an answer that is not OctoPrint's ----

TEST_CASE("OctoPrint's test says why when the answer has no API version", "[PrintHost][sending][socket]")
{
    FixedReplyServer   server(R"({"hello":"world"})");
    DynamicPrintConfig cfg = host_config("octoprint", "http://127.0.0.1:" + std::to_string(server.port()));
    std::unique_ptr<PrintHost> host(PrintHost::get_print_host(&cfg));
    REQUIRE(host != nullptr);
    wxString msg;
    CHECK_FALSE(host->test(msg));
    INFO("message: " << msg.ToUTF8().data());
    CHECK_FALSE(msg.IsEmpty());
}

// ---- 10: an address and a port ----

TEST_CASE("A print host address joins host and port with a colon", "[PrintHost][sending]")
{
    CHECK(print_host_address("192.168.1.5", 7125) == "192.168.1.5:7125");
    CHECK(print_host_address("192.168.1.5", -1) == "192.168.1.5");
    CHECK(print_host_address("printer.local", 80) == "printer.local:80");
}

// ---- 11: a script sent to the print host the job uploads to ----

TEST_CASE("A Klipper script goes to the print host's own scheme and port with its API key", "[SnapmakerLan][sending][socket]")
{
    FixedReplyServer server(R"({"result":"ok"})");
    std::string      error;
    const std::string base = "http://127.0.0.1:" + std::to_string(server.port());
    REQUIRE(GUI::SnapmakerLan::run_script(base, "the-key", "G28", error));
    REQUIRE(server.targets().size() == 1);
    CHECK(server.targets().front() == "/printer/gcode/script?script=G28");
    CHECK(server.keys().front() == "the-key");

    SECTION("no key, no header")
    {
        REQUIRE(GUI::SnapmakerLan::run_script(base, "", "G28", error));
        REQUIRE(server.keys().size() == 2);
        CHECK(server.keys().back().empty());
    }
}

// ---- 12: a U1 upload the printer stops taking ----

TEST_CASE("A U1 upload the printer stops taking gives up instead of hanging", "[SnapmakerLan][sending][socket]")
{
    // Bigger than what the loopback socket buffers hold, so the sender really stalls.
    const fs::path file = fs::temp_directory_path() / ("u1_stall_" + std::to_string((long long) ::time(nullptr)) + ".gcode");
    {
        boost::nowide::ofstream out(file.string(), std::ios::binary | std::ios::trunc);
        const std::string       line(1023, ';');
        for (int i = 0; i < 64 * 1024; ++i)
            out << line << '\n';
    }
    QuietPrinter               printer(Quiet::HttpAfterHead);
    GUI::SnapmakerLan::Device  d;
    d.ip   = "127.0.0.1";
    d.port = printer.port();
    std::string error;
    bool        ok = true;
    CHECK(finishes_within(printer, std::chrono::seconds(15), [&] {
        ok = GUI::SnapmakerLan::upload(d, file.string(), "stall.gcode", nullptr, error, 2);
    }));
    CHECK_FALSE(ok);
    INFO("error: " << error);
    CHECK(error.find("stopped") != std::string::npos);
    boost::system::error_code ec;
    fs::remove(file, ec);
}
