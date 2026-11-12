// Printer credentials kept out of other people's reach and out of the log.
//
//  - <datadir>/hub/print_host_devices.json holds every saved printer's API key and password. It is
//    written readable by its owner alone (POSIX; on Windows the profile folder's ACL already is).
//  - An address typed as https://user:password@host must not reach the log with its password, and
//    an Elegoo printer's mainboard id must not reach it with every websocket frame.
//
// Every request here goes to a listener of the test's own on 127.0.0.1.

#include <catch2/catch.hpp>

#include "slic3r/Utils/ElegooCanvas.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/PrintHost.hpp"
#include "slic3r/Utils/PrintHostDevices.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/core.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/make_shared.hpp>
#include <boost/nowide/fstream.hpp>

#include <atomic>
#include <ctime>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
using tcp       = net::ip::tcp;

// Answers every request with one fixed 200 reply, until it is destroyed.
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
    unsigned short port() const { return m_acceptor.local_endpoint().port(); }

private:
    std::string       m_body;
    net::io_context   m_ioc;
    tcp::acceptor     m_acceptor{m_ioc, {net::ip::make_address("127.0.0.1"), 0}};
    std::atomic<bool> m_stopping{false};
    std::thread       m_thread;
};

// Everything logged while it lives, at every severity.
class LogCapture
{
    using Sink = boost::log::sinks::synchronous_sink<boost::log::sinks::text_ostream_backend>;

public:
    LogCapture()
    {
        m_sink = boost::make_shared<Sink>();
        m_sink->locked_backend()->add_stream(boost::shared_ptr<std::ostream>(&m_text, [](std::ostream*) {}));
        m_sink->locked_backend()->auto_flush(true);
        boost::log::core::get()->add_sink(m_sink);
        // The suite never sets a severity filter; make sure no earlier case left one behind.
        boost::log::core::get()->reset_filter();
        m_enabled = boost::log::core::get()->get_logging_enabled();
        boost::log::core::get()->set_logging_enabled(true);
    }
    ~LogCapture()
    {
        boost::log::core::get()->remove_sink(m_sink);
        boost::log::core::get()->set_logging_enabled(m_enabled);
    }
    std::string text() const { return m_text.str(); }

private:
    std::ostringstream     m_text;
    boost::shared_ptr<Sink> m_sink;
    bool                   m_enabled = true;
};

DynamicPrintConfig octoprint_config(const std::string& url)
{
    DynamicPrintConfig cfg;
    cfg.set_deserialize_strict({{"printer_technology", "FFF"}, {"host_type", "octoprint"}, {"print_host", url},
                                {"printhost_apikey", "k"}, {"printhost_cafile", ""}, {"printhost_ssl_ignore_revoke", "1"},
                                {"printhost_authorization_type", "key"}, {"printhost_user", ""}, {"printhost_password", ""}});
    return cfg;
}

} // namespace

#ifndef _WIN32
TEST_CASE("The saved printers file is readable by its owner alone", "[PrintHostDevices][secrets]")
{
    const fs::path path = fs::temp_directory_path() /
                          ("print_host_devices_mode_" + std::to_string((long long) ::time(nullptr)) + ".json");
    boost::system::error_code ec;
    fs::remove(path, ec);
    PrintHostDevices::set_store_path(path.string());

    SECTION("a new file")
    {
        PrintHostDevices::Device d;
        d.alias     = "Left bay";
        d.address   = "192.0.2.10";
        d.host_type = "octoprint";
        d.auth_type = "key";
        d.apikey    = "secret-key";
        std::string error;
        REQUIRE(PrintHostDevices::add("Test Model", d, error));
        REQUIRE(fs::exists(path));
        const fs::perms mode = fs::status(path).permissions();
        INFO("mode " << std::oct << int(mode));
        CHECK((mode & (fs::group_all | fs::others_all)) == 0);
        CHECK((mode & fs::owner_read) != 0);
        CHECK((mode & fs::owner_write) != 0);
    }
    SECTION("a file left readable by everyone is tightened on the next save")
    {
        {
            boost::nowide::ofstream out(path.string(), std::ios::binary | std::ios::trunc);
            out << "{}";
        }
        fs::permissions(path, fs::owner_read | fs::owner_write | fs::group_read | fs::others_read, ec);
        PrintHostDevices::Device d;
        d.address   = "192.0.2.11";
        d.host_type = "octoprint";
        d.password  = "secret-password";
        std::string error;
        REQUIRE(PrintHostDevices::add("Test Model", d, error));
        const fs::perms mode = fs::status(path).permissions();
        INFO("mode " << std::oct << int(mode));
        CHECK((mode & (fs::group_all | fs::others_all)) == 0);
    }

    PrintHostDevices::set_store_path("");
    fs::remove(path, ec);
}
#endif

// The list is replaced in one step: the old file is no longer removed before the new one is renamed over it.
TEST_CASE("Saving the printers file twice keeps the list and leaves no temporary file", "[PrintHostDevices][secrets]")
{
    const fs::path path = fs::temp_directory_path() /
                          ("print_host_devices_twice_" + std::to_string((long long) ::time(nullptr)) + ".json");
    const fs::path temp = path.parent_path() / (path.filename().string() + ".tmp");
    boost::system::error_code ec;
    fs::remove(path, ec);
    PrintHostDevices::set_store_path(path.string());

    PrintHostDevices::Device a;
    a.alias     = "Left bay";
    a.address   = "192.0.2.20";
    a.host_type = "octoprint";
    a.auth_type = "key";
    a.apikey    = "fake-key-1";
    std::string error;
    REQUIRE(PrintHostDevices::add("Test Model", a, error));
    REQUIRE(PrintHostDevices::devices("Test Model").size() == 1);
    CHECK_FALSE(fs::exists(temp));

    PrintHostDevices::Device b;
    b.alias     = "Right bay";
    b.address   = "192.0.2.21";
    b.host_type = "octoprint";
    b.auth_type = "key";
    b.apikey    = "fake-key-2";
    REQUIRE(PrintHostDevices::add("Test Model", b, error));
    const std::vector<PrintHostDevices::Device> both = PrintHostDevices::devices("Test Model");
    REQUIRE(both.size() == 2);
    CHECK(both[0].address == "192.0.2.20");
    CHECK(both[1].address == "192.0.2.21");
    CHECK(both[0].apikey == "fake-key-1");
    CHECK_FALSE(fs::exists(temp));

    // Still the same JSON object keyed by model as before (format unchanged).
    std::string text;
    {
        boost::nowide::ifstream in(path.string(), std::ios::binary);
        std::stringstream       ss;
        ss << in.rdbuf();
        text = ss.str();
    }
    CHECK(text.rfind("{", 0) == 0);
    CHECK(text.find("Test Model") != std::string::npos);

    PrintHostDevices::set_store_path("");
    fs::remove(path, ec);
}

TEST_CASE("url_without_userinfo drops user:password@ and nothing else", "[Http][secrets]")
{
    CHECK(Http::url_without_userinfo("https://user:secret@host.example:7125/api/version") == "https://host.example:7125/api/version");
    CHECK(Http::url_without_userinfo("http://user@192.168.1.5/api") == "http://192.168.1.5/api");
    CHECK(Http::url_without_userinfo("user:secret@192.168.1.5:7125") == "192.168.1.5:7125");
    CHECK(Http::url_without_userinfo("http://us%40er:p@ss@host/x") == "http://host/x");
    CHECK(Http::url_without_userinfo("http://[fe80::1]:7125/server") == "http://[fe80::1]:7125/server");
    // An '@' after the host is part of the path or query, not a password.
    CHECK(Http::url_without_userinfo("http://host/files/a@b.gcode?x=y@z") == "http://host/files/a@b.gcode?x=y@z");
    CHECK(Http::url_without_userinfo("192.168.1.5") == "192.168.1.5");
    CHECK(Http::url_without_userinfo("") == "");
}

TEST_CASE("OctoPrint's connection test does not log the address's password", "[PrintHost][secrets][socket]")
{
    FixedReplyServer  server(R"({"api":"0.1","server":"1.9.0","text":"OctoPrint 1.9.0"})");
    DynamicPrintConfig cfg = octoprint_config("http://maker:hunter2-secret@127.0.0.1:" + std::to_string(server.port()));
    std::unique_ptr<PrintHost> host(PrintHost::get_print_host(&cfg));
    REQUIRE(host != nullptr);

    LogCapture capture;
    wxString   msg;
    CHECK(host->test(msg));
    const std::string log = capture.text();
    INFO("log: " << log);
    CHECK(log.find("127.0.0.1") != std::string::npos); // the test did log the address
    CHECK(log.find("hunter2-secret") == std::string::npos);
}

// ElegooLink logs every SDCP frame it reads while it starts a print; it logs them through
// canvas_redact_frame. The frame below is the start-print answer quoted in ElegooLink.cpp.
TEST_CASE("An Elegoo start-print answer is logged without the printer's mainboard id", "[ElegooLink][secrets]")
{
    const std::string frame =
        R"({"Id":"979d4C788A4a78bC777A870F1A02867A","Data":{"Cmd":128,"Data":{"Ack":1},"RequestID":"5223de52cc7642ae8d7924f9dd46f6ad",)"
        R"("MainboardID":"1c7319d30105041800009c0000000000","TimeStamp":1735032553},"Topic":"sdcp/response/1c7319d30105041800009c0000000000"})";
    const std::string logged = canvas_redact_frame(frame);
    INFO("logged: " << logged);
    CHECK(logged.find("1c7319d30105041800009c0000000000") == std::string::npos);
    CHECK(logged.find("128") != std::string::npos); // what was answered stays readable
    CHECK(logged.find("Ack") != std::string::npos);
}
