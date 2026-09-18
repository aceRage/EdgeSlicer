// A POST with an empty body used to crash the process.
//
// Http installs form_file_read_cb as CURLOPT_READFUNCTION on every request, but sets
// CURLOPT_READDATA only for a PUT. http_perform() then handed curl CURLOPT_POSTFIELDS only when
// the body was non-empty, so an empty-bodied POST reached curl_easy_perform() with CURLOPT_POST
// set, no form, no mime and no POSTFIELDS. curl has to get the body from somewhere, so it called
// the read callback, which reinterpret_cast its unset read-data to a form_file* and dereferenced
// it: an access violation inside std::basic_istream::tellg, on whichever thread made the call.
//
// RemoteHub's control plane is all empty-bodied POSTs (/hub/phone, /hub/newlink, /hub/quit), so
// the slicer's start-up POST /hub/phone killed every instance that made it. The request below is
// the same shape, against a throwaway loopback server, with no hub and no slicer involved.

#include <catch2/catch.hpp>

#include "slic3r/Utils/Http.hpp"

#include <boost/asio.hpp>

#include <atomic>
#include <string>
#include <thread>

namespace {

namespace asio = boost::asio;
using asio::ip::tcp;

// Accepts exactly one connection, reads the request head, answers 200 with a fixed body and
// records the request line and the Content-Length it was given.
struct OneShotServer
{
    asio::io_context ioc;
    tcp::acceptor    acceptor { ioc };
    std::thread      thread;
    std::string      request;
    unsigned short   port { 0 };

    OneShotServer()
    {
        acceptor.open(tcp::v4());
        acceptor.bind(tcp::endpoint(asio::ip::make_address_v4("127.0.0.1"), 0));
        acceptor.listen();
        port = acceptor.local_endpoint().port();
        thread = std::thread([this]() {
            boost::system::error_code ec;
            tcp::socket socket(ioc);
            acceptor.accept(socket, ec);
            if (ec) return;
            // Read until the end of the head; an empty-bodied POST sends nothing after it.
            char buf[4096];
            while (request.find("\r\n\r\n") == std::string::npos) {
                const size_t n = socket.read_some(asio::buffer(buf), ec);
                if (ec || n == 0) break;
                request.append(buf, n);
            }
            const std::string body = "{\"ok\":true}";
            const std::string head = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                     std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
            asio::write(socket, asio::buffer(head + body), ec);
            socket.shutdown(tcp::socket::shutdown_both, ec);
        });
    }

    ~OneShotServer()
    {
        boost::system::error_code ig;
        acceptor.close(ig);
        if (thread.joinable()) thread.join();
    }

    std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port) + path; }
};

} // namespace

TEST_CASE("an empty-bodied POST completes instead of faulting in the read callback", "[Http][socket]")
{
    // The shape RemoteHub::hub_call() sends for /hub/phone: POST, body "", a text/plain type.
    OneShotServer server;

    std::string reply;
    unsigned    status = 0;
    std::string error;

    Slic3r::Http::post(server.url("/hub/phone?on=1"))
        .set_post_body(std::string())
        .header("Content-Type", "text/plain")
        .timeout_connect(2)
        .timeout_max(10)
        .on_complete([&](std::string body, unsigned s) { reply = std::move(body); status = s; })
        .on_error([&](std::string, std::string err, unsigned s) { error = err; status = s; })
        .perform_sync();

    INFO("curl error: " << error);
    REQUIRE(status == 200);
    REQUIRE(reply == "{\"ok\":true}");

    // curl was given a body of length zero rather than being left to ask the read callback for one.
    server.thread.join();
    REQUIRE(server.request.find("POST /hub/phone?on=1") == 0);
    REQUIRE(server.request.find("Content-Length: 0\r\n") != std::string::npos);
    // A fall-through to the read callback would have made curl announce a chunked body instead.
    REQUIRE(server.request.find("Transfer-Encoding: chunked") == std::string::npos);
}

TEST_CASE("a POST with a body still sends it", "[Http][socket]")
{
    OneShotServer server;

    unsigned    status = 0;
    std::string error;
    const std::string body = "{\"state\":\"idle\"}";

    Slic3r::Http::post(server.url("/hub/state"))
        .set_post_body(body)
        .header("Content-Type", "application/json")
        .timeout_connect(2)
        .timeout_max(10)
        .on_complete([&](std::string, unsigned s) { status = s; })
        .on_error([&](std::string, std::string err, unsigned s) { error = err; status = s; })
        .perform_sync();

    INFO("curl error: " << error);
    REQUIRE(status == 200);

    server.thread.join();
    REQUIRE(server.request.find("Content-Length: " + std::to_string(body.size()) + "\r\n") != std::string::npos);
    REQUIRE(server.request.find(body) != std::string::npos);
}
