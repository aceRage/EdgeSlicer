// A device bound by IP whose MQTT port does not answer (2026-10-27: an emulated U1 with its pairing
// broker off; a real U1 in another link mode or a printer that refuses the connection is the same).
// Paho C++ reports a failed connect by throwing mqtt::exception from token::wait_for(); that escaped
// MqttClient::Connect() on SSWCP's worker thread and std::terminate took the whole slicer down.
//
// Everything here is on 127.0.0.1 in this process: a port nothing listens on, and a broker that
// answers CONNECT with a refusal. No printer is involved.

#include <catch2/catch.hpp>

#include "slic3r/Utils/MQTT.hpp"

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace asio = boost::asio;
using tcp      = asio::ip::tcp;
using namespace std::chrono_literals;

std::string unique_client_id(const char* tag)
{
    static std::atomic<int> n {0};
    return std::string("edgeslicer-refused-test-") + tag + "-" + std::to_string(++n);
}

// A loopback port that was free a moment ago and has nothing listening now.
unsigned short closed_port()
{
    asio::io_context io;
    tcp::acceptor    acceptor(io, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    const unsigned short port = acceptor.local_endpoint().port();
    acceptor.close();
    return port;
}

// Accepts one connection, reads the CONNECT and answers CONNACK with return code `rc`
// (5 = not authorised), then closes.
class RefusingBroker
{
public:
    explicit RefusingBroker(uint8_t rc) : m_rc(rc), m_acceptor(m_io, tcp::endpoint(asio::ip::address_v4::loopback(), 0))
    {
        m_port   = m_acceptor.local_endpoint().port();
        m_thread = std::thread([this]() { serve(); });
    }
    ~RefusingBroker()
    {
        boost::system::error_code ec;
        m_acceptor.close(ec);
        if (m_thread.joinable())
            m_thread.join();
    }
    std::string url() const { return "tcp://127.0.0.1:" + std::to_string(m_port); }

private:
    void serve()
    {
        // Paho may retry with an older protocol version after a refusal: answer every attempt.
        for (int attempt = 0; attempt < 4; ++attempt) {
            tcp::socket               sock(m_io);
            boost::system::error_code ec;
            m_acceptor.accept(sock, ec);
            if (ec)
                return;
            uint8_t header[2];
            if (asio::read(sock, asio::buffer(header, 2), ec) != 2 || ec)
                continue;
            // Remaining length (one byte is enough for a CONNECT this small, but read it properly).
            size_t len = header[1] & 0x7f, mul = 128;
            uint8_t b  = header[1];
            while ((b & 0x80) && !ec) {
                asio::read(sock, asio::buffer(&b, 1), ec);
                len += size_t(b & 0x7f) * mul;
                mul *= 128;
            }
            std::vector<uint8_t> body(len);
            asio::read(sock, asio::buffer(body), ec);
            const std::vector<uint8_t> connack {0x20, 0x02, 0x00, m_rc};
            asio::write(sock, asio::buffer(connack), ec);
            sock.shutdown(tcp::socket::shutdown_both, ec);
            sock.close(ec);
        }
    }

    uint8_t           m_rc;
    asio::io_context  m_io;
    tcp::acceptor     m_acceptor;
    unsigned short    m_port {0};
    std::thread       m_thread;
};

} // namespace

TEST_CASE("MqttConnect: a port nothing listens on is a failed connect, not a throw", "[MqttConnect]")
{
    std::shared_ptr<MqttClient> client = MqttClient::create("tcp://127.0.0.1:" + std::to_string(closed_port()), unique_client_id("closed"));
    REQUIRE(client);
    std::string msg;
    bool        ok = true;
    const auto  t0 = std::chrono::steady_clock::now();
    REQUIRE_NOTHROW(ok = client->Connect(msg));
    CHECK_FALSE(ok);
    CHECK_FALSE(msg.empty());
    CHECK_FALSE(client->CheckConnected());
    // Refused at once: well inside the 20 s connect wait.
    CHECK(std::chrono::steady_clock::now() - t0 < 15s);

    // What SSWCP does next on a failure (its connection-failure callback / the page): disconnect.
    REQUIRE_NOTHROW(client->Disconnect(msg));
    client.reset();
}

TEST_CASE("MqttConnect: a broker that refuses the CONNECT is a failed connect, not a throw", "[MqttConnect]")
{
    RefusingBroker              broker(0x05);
    std::shared_ptr<MqttClient> client = MqttClient::create(broker.url(), unique_client_id("refused"));
    REQUIRE(client);
    std::string msg;
    bool        ok = true;
    REQUIRE_NOTHROW(ok = client->Connect(msg));
    CHECK_FALSE(ok);
    CHECK_FALSE(client->CheckConnected());
    std::string sub_msg;
    bool        sub_ok = true;
    REQUIRE_NOTHROW(sub_ok = client->Subscribe("sn/response", 1, sub_msg));
    CHECK_FALSE(sub_ok);
    REQUIRE_NOTHROW(client->Disconnect(msg));
    client.reset();
}
