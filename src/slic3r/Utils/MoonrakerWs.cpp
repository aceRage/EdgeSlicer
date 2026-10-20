#include "MoonrakerWs.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/log/trivial.hpp>

#include <chrono>
#include <deque>

namespace Slic3r {

namespace beast     = boost::beast;
namespace websocket = boost::beast::websocket;
namespace net       = boost::asio;
using tcp           = boost::asio::ip::tcp;

struct MoonrakerWs::Impl
{
    net::io_context                      ioc;
    websocket::stream<beast::tcp_stream> ws { ioc };
    beast::flat_buffer                   buf;
    std::deque<std::string>              outq;
    OnMessage                            on_message;
    OnClosed                             on_closed;
    bool                                 closing { false };
};

std::shared_ptr<MoonrakerWs> MoonrakerWs::create() { return std::shared_ptr<MoonrakerWs>(new MoonrakerWs()); }

MoonrakerWs::MoonrakerWs() : m(new Impl()) {}

MoonrakerWs::~MoonrakerWs()
{
    close();
}

void MoonrakerWs::set_handlers(OnMessage on_message, OnClosed on_closed)
{
    m->on_message = std::move(on_message);
    m->on_closed  = std::move(on_closed);
}

// One async operation run to completion on the calling thread (before the I/O thread exists).
template<class Start> static beast::error_code run_now(net::io_context& ioc, Start&& start)
{
    beast::error_code ec = net::error::would_block;
    start([&ec](beast::error_code e, auto&&...) { ec = e; });
    ioc.restart();
    ioc.run();
    return ec;
}

bool MoonrakerWs::connect(const std::string& host, int port, int timeout_ms, int idle_timeout_ms, std::string& error)
{
    try {
        tcp::resolver     resolver(m->ioc);
        beast::error_code ec;
        const auto        results = resolver.resolve(host, std::to_string(port), ec);
        if (ec) {
            error = "resolve: " + ec.message();
            return false;
        }
        auto& tcp_layer = beast::get_lowest_layer(m->ws);
        tcp_layer.expires_after(std::chrono::milliseconds(timeout_ms));
        ec = run_now(m->ioc, [&](auto&& h) { tcp_layer.async_connect(results, h); });
        if (ec) {
            error = "connect: " + ec.message();
            return false;
        }
        tcp_layer.expires_after(std::chrono::milliseconds(timeout_ms));
        const std::string host_header = port == 80 ? host : host + ":" + std::to_string(port);
        websocket::response_type res;
        ec = run_now(m->ioc, [&](auto&& h) { m->ws.async_handshake(res, host_header, "/websocket", h); });
        if (ec) {
            error = "handshake: " + ec.message() + (res.result_int() ? " (HTTP " + std::to_string(res.result_int()) + ")" : "");
            return false;
        }
        // From here the WebSocket's own timeouts: pings when idle, closed when nothing answers.
        tcp_layer.expires_never();
        websocket::stream_base::timeout opt {};
        opt.handshake_timeout = std::chrono::milliseconds(timeout_ms);
        opt.idle_timeout      = std::chrono::milliseconds(idle_timeout_ms);
        opt.keep_alive_pings  = true;
        m->ws.set_option(opt);
        m->ws.text(true);
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    m_open.store(true);

    auto self = shared_from_this();
    m->ioc.restart();
    m_thread = std::thread([this, self]() {
        do_read(self);
        try {
            m->ioc.run();
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "[MoonrakerWs] I/O thread: " << e.what();
        }
        m_open.store(false);
    });
    return true;
}

// The read loop: every frame to on_message; the first error ends the connection.
void MoonrakerWs::do_read(std::shared_ptr<MoonrakerWs> self)
{
    m->ws.async_read(m->buf, [this, self](beast::error_code ec, std::size_t) {
        if (ec) {
            const bool was_open = m_open.exchange(false);
            if (was_open && !m->closing && m->on_closed)
                m->on_closed(ec.message());
            return;
        }
        const std::string text = beast::buffers_to_string(m->buf.data());
        m->buf.consume(m->buf.size());
        if (m->on_message)
            m->on_message(text);
        do_read(self);
    });
}

// One write at a time (Beast allows one outstanding write); the queue holds the rest.
void MoonrakerWs::do_write(std::shared_ptr<MoonrakerWs> self)
{
    m->ws.async_write(net::buffer(m->outq.front()), [this, self](beast::error_code ec, std::size_t) {
        m->outq.pop_front();
        if (ec) {
            m->outq.clear();
            return; // the read loop reports the end of the connection
        }
        if (!m->outq.empty())
            do_write(self);
    });
}

bool MoonrakerWs::send(const std::string& text)
{
    if (!m_open.load())
        return false;
    auto self = shared_from_this();
    net::post(m->ioc, [this, self, text]() {
        if (!m_open.load() || m->closing)
            return;
        m->outq.push_back(text);
        if (m->outq.size() == 1)
            do_write(self);
    });
    return true;
}

void MoonrakerWs::close()
{
    std::lock_guard<std::mutex> lk(m_close_mtx);
    if (m_thread.joinable()) {
        net::post(m->ioc, [this]() {
            m->closing = true;
            beast::get_lowest_layer(m->ws).expires_after(std::chrono::seconds(2));
            m->ws.async_close(websocket::close_code::normal, [this](beast::error_code) {
                beast::error_code e;
                beast::get_lowest_layer(m->ws).socket().close(e);
                m->ioc.stop();
            });
        });
        if (m_thread.get_id() == std::this_thread::get_id()) {
            m_thread.detach();
        } else {
            // A connection that never answers the close: stop the loop after the close timeout.
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!m->ioc.stopped() && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            m->ioc.stop();
            m_thread.join();
        }
    }
    m_open.store(false);
}

} // namespace Slic3r
