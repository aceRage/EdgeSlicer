// EDGESLICER-E: STATUS_HEAP_CORRUPTION on the UI thread while a Snapmaker U1 was being
// disconnected: Moonraker_Mqtt::disconnect -> ~MqttClient -> ~async_client -> MQTTAsync_destroy ->
// MQTTProtocol_freeClient -> MQTTProtocol_removePublication -> free().
//
// Root cause (vendored Paho C, MQTTAsyncUtils.c, MQTTAsync_processCommand): a QoS 1 publish whose
// socket write failed (SOCKET_ERROR - the printer dropped off the network mid-send) had its topic
// and payload freed with the command, while the stored outbound record (cleansession=0 keeps it
// for a retry) still pointed at the same buffers. MQTTAsync_destroy freed them a second time.
//
// These tests run the real MqttClient (and so the real Paho C library) against a minimal MQTT
// 3.1.1 broker in this process on 127.0.0.1 - nothing leaves the machine and no printer is
// involved. They cover the lifecycle (connect / disconnect / double disconnect / destroy with
// QoS 1 messages still in flight), the broker resetting the socket while QoS 1 publishes are being
// written (the EDGESLICER-E path; a stress loop, since the exact interleaving cannot be forced),
// and MqttClient's rule that the last reference dropped on a callback thread is destroyed on a
// thread of its own.

#include <catch2/catch.hpp>

#include "slic3r/Utils/MQTT.hpp"

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace asio = boost::asio;
using tcp      = asio::ip::tcp;
using namespace std::chrono_literals;

// What the fake broker does with a client's QoS 1 publishes.
enum class BrokerMode {
    AckAll,      // PUBACK every QoS 1 publish
    NeverAck,    // keep them all in flight (the client's outbound list never drains)
    ResetOnPublish // RST the connection as soon as the first PUBLISH arrives
};

// A minimal MQTT 3.1.1 broker: CONNECT -> CONNACK, SUBSCRIBE -> SUBACK, PINGREQ -> PINGRESP,
// PUBLISH -> PUBACK (or not, per mode). One thread per connection; nothing is routed anywhere.
class FakeBroker
{
public:
    explicit FakeBroker(BrokerMode mode) : m_mode(mode), m_acceptor(m_io, tcp::endpoint(asio::ip::address_v4::loopback(), 0))
    {
        m_acceptor.non_blocking(true);
        m_port          = m_acceptor.local_endpoint().port();
        m_accept_thread = std::thread([this]() { accept_loop(); });
    }

    ~FakeBroker()
    {
        m_stop.store(true);
        if (m_accept_thread.joinable())
            m_accept_thread.join();
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            for (auto& s : m_sockets) {
                boost::system::error_code ec;
                s->shutdown(tcp::socket::shutdown_both, ec);
                s->close(ec);
            }
        }
        for (auto& t : m_conn_threads)
            if (t.joinable())
                t.join();
    }

    std::string url() const { return "tcp://127.0.0.1:" + std::to_string(m_port); }
    int         connects() const { return m_connects.load(); }

private:
    void accept_loop()
    {
        while (!m_stop.load()) {
            auto                      sock = std::make_shared<tcp::socket>(m_io);
            boost::system::error_code ec;
            m_acceptor.accept(*sock, ec);
            if (ec) {
                std::this_thread::sleep_for(5ms);
                continue;
            }
            sock->non_blocking(false, ec);
            std::lock_guard<std::mutex> lk(m_mtx);
            m_sockets.push_back(sock);
            m_conn_threads.emplace_back([this, sock]() { serve(sock); });
        }
    }

    static bool read_exact(tcp::socket& s, uint8_t* buf, size_t n)
    {
        boost::system::error_code ec;
        asio::read(s, asio::buffer(buf, n), ec);
        return !ec;
    }

    static void write_all(tcp::socket& s, const std::vector<uint8_t>& bytes)
    {
        boost::system::error_code ec;
        asio::write(s, asio::buffer(bytes), ec);
    }

    static void reset(tcp::socket& s)
    {
        boost::system::error_code ec;
        s.set_option(asio::socket_base::linger(true, 0), ec); // close() sends RST
        s.close(ec);
    }

    void serve(std::shared_ptr<tcp::socket> sock)
    {
        tcp::socket& s = *sock;
        for (;;) {
            uint8_t header = 0;
            if (!read_exact(s, &header, 1))
                return;
            // Remaining length: MQTT variable-byte integer.
            size_t   len = 0, mult = 1;
            uint8_t  b   = 0;
            for (int i = 0; i < 4; ++i) {
                if (!read_exact(s, &b, 1))
                    return;
                len += size_t(b & 0x7F) * mult;
                mult *= 128;
                if ((b & 0x80) == 0)
                    break;
            }
            std::vector<uint8_t> body(len);
            if (len > 0 && !read_exact(s, body.data(), len))
                return;

            switch (header >> 4) {
            case 1: // CONNECT
                ++m_connects;
                write_all(s, {0x20, 0x02, 0x00, 0x00}); // CONNACK, no session present, accepted
                break;
            case 3: { // PUBLISH
                if (m_mode == BrokerMode::ResetOnPublish) {
                    reset(s);
                    return;
                }
                const int qos = (header >> 1) & 0x03;
                if (qos > 0 && m_mode == BrokerMode::AckAll && body.size() >= 2) {
                    const size_t topic_len = (size_t(body[0]) << 8) | body[1];
                    if (body.size() >= 2 + topic_len + 2) {
                        const uint8_t msb = body[2 + topic_len], lsb = body[3 + topic_len];
                        write_all(s, {0x40, 0x02, msb, lsb}); // PUBACK
                    }
                }
                break;
            }
            case 8: // SUBSCRIBE
                if (body.size() >= 2)
                    write_all(s, {0x90, 0x03, body[0], body[1], 0x01}); // SUBACK, granted QoS 1
                break;
            case 12: // PINGREQ
                write_all(s, {0xD0, 0x00});
                break;
            case 14: // DISCONNECT
            {
                boost::system::error_code ec;
                s.close(ec);
                return;
            }
            default: break;
            }
        }
    }

    BrokerMode                                 m_mode;
    asio::io_context                           m_io;
    tcp::acceptor                              m_acceptor;
    unsigned short                             m_port {0};
    std::atomic<bool>                          m_stop {false};
    std::atomic<int>                           m_connects {0};
    std::thread                                m_accept_thread;
    std::mutex                                 m_mtx;
    std::vector<std::shared_ptr<tcp::socket>>  m_sockets;
    std::vector<std::thread>                   m_conn_threads;
};

std::string unique_client_id(const char* tag)
{
    static std::atomic<int> n {0};
    return std::string("edgeslicer-teardown-test-") + tag + "-" + std::to_string(++n);
}

} // namespace

TEST_CASE("MqttTeardown: connect, publish, disconnect and destroy, several times over", "[MqttTeardown]")
{
    FakeBroker broker(BrokerMode::AckAll);
    const size_t live_before = MqttClient::live_client_count();

    for (int round = 0; round < 3; ++round) {
        DYNAMIC_SECTION("round " << round)
        {
            std::shared_ptr<MqttClient> client = MqttClient::create(broker.url(), unique_client_id("cycle"));
            std::string                 msg;
            REQUIRE(client->Connect(msg));
            CHECK(client->Subscribe("sn/response", 1, msg));
            for (int i = 0; i < 10; ++i)
                CHECK(client->Publish("sn/request", "{\"id\":" + std::to_string(i) + "}", 1, msg));

            CHECK(client->Disconnect(msg));
            // A second disconnect (the owner's after a give-up, or a reconnect plus a user
            // disconnect) is a harmless no-op.
            CHECK(client->Disconnect(msg));
            CHECK(msg == "success");
            client.reset();
            CHECK(MqttClient::live_client_count() == live_before);
        }
    }
}

TEST_CASE("MqttTeardown: destroying a connected client with QoS 1 publishes still in flight", "[MqttTeardown]")
{
    // The broker never acknowledges, so MQTTAsync_destroy has to free the outbound list itself -
    // the very list EDGESLICER-E crashed in.
    FakeBroker broker(BrokerMode::NeverAck);
    for (int round = 0; round < 3; ++round) {
        DYNAMIC_SECTION("round " << round)
        {
            std::shared_ptr<MqttClient> client = MqttClient::create(broker.url(), unique_client_id("inflight"));
            std::string                 msg;
            REQUIRE(client->Connect(msg));
            for (int i = 0; i < 20; ++i)
                client->Publish("sn/request", std::string(256, char('a' + i % 26)), 1, msg);
            std::this_thread::sleep_for(50ms); // let the send thread write them
            // No explicit Disconnect: ~MqttClient disconnects, detaches the callbacks and destroys.
            client.reset();
            SUCCEED("destroyed with QoS 1 messages unacknowledged");
        }
    }
}

TEST_CASE("MqttTeardown: the broker resets the socket while QoS 1 publishes are being written", "[MqttTeardown]")
{
    // EDGESLICER-E: a publish whose write fails with SOCKET_ERROR used to leave the outbound
    // record pointing at freed buffers, freed again by MQTTAsync_destroy (or by the PUBACK after a
    // reconnect). The broker resets the connection on the first PUBLISH; the client keeps
    // publishing as fast as it can, so some writes land on the dead socket. The interleaving is
    // up to the scheduler, hence the rounds; a heap corruption ends the test process.
    FakeBroker broker(BrokerMode::ResetOnPublish);
    int        connected_rounds = 0;
    for (int round = 0; round < 8; ++round) {
        std::shared_ptr<MqttClient> client = MqttClient::create(broker.url(), unique_client_id("reset"));
        std::string                 msg;
        if (!client->Connect(msg))
            continue;
        ++connected_rounds;
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - t0 < 300ms) {
            try {
                if (!client->Publish("sn/request", std::string(512, 'x'), 1, msg))
                    std::this_thread::sleep_for(1ms);
            } catch (const std::exception&) {
                // Paho throws MQTTASYNC_DISCONNECTED once it has noticed the reset.
                std::this_thread::sleep_for(1ms);
            }
        }
        client->Disconnect(msg);
        client.reset();
    }
    CHECK(connected_rounds > 0);
    CHECK(broker.connects() >= connected_rounds);
}

TEST_CASE("MqttTeardown: the last reference dropped on a callback thread is destroyed elsewhere", "[MqttTeardown]")
{
    // No broker needed: an unconnected client tears down without network traffic.
    const size_t before = MqttClient::deferred_destroy_count();
    {
        std::shared_ptr<MqttClient> client = MqttClient::create("tcp://127.0.0.1:1", unique_client_id("deferred"));
        MqttClient::CallbackThreadScope scope;
        CHECK(MqttClient::on_callback_thread());
        client.reset();
    }
    CHECK_FALSE(MqttClient::on_callback_thread());
    CHECK(MqttClient::deferred_destroy_count() == before + 1);

    // Outside a callback scope the destruction happens in place.
    {
        std::shared_ptr<MqttClient> client = MqttClient::create("tcp://127.0.0.1:1", unique_client_id("inplace"));
        client.reset();
    }
    CHECK(MqttClient::deferred_destroy_count() == before + 1);

    // Give the detached teardown thread time to finish before the process moves on.
    std::this_thread::sleep_for(500ms);
}

TEST_CASE("MqttTeardown: callback scopes nest", "[MqttTeardown]")
{
    CHECK_FALSE(MqttClient::on_callback_thread());
    {
        MqttClient::CallbackThreadScope outer;
        {
            MqttClient::CallbackThreadScope inner;
            CHECK(MqttClient::on_callback_thread());
        }
        CHECK(MqttClient::on_callback_thread());
    }
    CHECK_FALSE(MqttClient::on_callback_thread());
}
