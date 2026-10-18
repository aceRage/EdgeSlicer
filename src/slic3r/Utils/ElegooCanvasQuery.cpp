// The networking half of the CANVAS support declared in ElegooCanvas.hpp: the SDCP websocket the
// conversation in ElegooCanvas.cpp runs over.
//
// Not WebSocketClient.hpp: its reads have no timeout (receive() ignores its argument and blocks in
// a synchronous beast read), so a printer that never answers would hang the caller for good. Every
// step here is an async beast operation run on a private io_context for at most its deadline; a
// step that runs out cancels the socket. Short-lived on purpose: one connection per command, closed
// after the answer, because the CC1 limits concurrent websocket clients and often closes its side
// after replying.

#include "ElegooCanvas.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/log/trivial.hpp>

#include <chrono>
#include <memory>

namespace Slic3r {

namespace {

namespace beast     = boost::beast;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
using tcp           = net::ip::tcp;

// The same endpoint ElegooLink::upload uses to start a print.
const char* const CANVAS_WS_PORT = "3030";
const char* const CANVAS_WS_PATH = "/websocket";
const int         CANVAS_CONNECT_TIMEOUT_MS = 3000;

class BeastSdcpTransport : public SdcpTransport
{
public:
    explicit BeastSdcpTransport(std::string host) : m_host(std::move(host)) {}
    ~BeastSdcpTransport() override { close(); }

    bool open(std::string& error) override
    {
        close();
        m_ws = std::make_unique<websocket::stream<tcp::socket>>(m_ioc);
        m_ws->read_message_max(1024 * 1024);
        // The user agent the fork's ElegooLink client already sends.
        m_ws->set_option(websocket::stream_base::decorator(
            [](websocket::request_type& req) { req.set(beast::http::field::user_agent, "ElegooSlicer"); }));

        boost::system::error_code ec;
        tcp::resolver             resolver(m_ioc);
        tcp::resolver::results_type results;
        bool                      done = false;
        resolver.async_resolve(m_host, CANVAS_WS_PORT, [&](const boost::system::error_code& e, tcp::resolver::results_type r) {
            ec      = e;
            results = std::move(r);
            done    = true;
        });
        if (!run(done, CANVAS_CONNECT_TIMEOUT_MS, [&] { resolver.cancel(); }) || ec)
            return fail(error, "resolve", ec);

        done = false;
        net::async_connect(m_ws->next_layer(), results, [&](const boost::system::error_code& e, const tcp::endpoint&) {
            ec   = e;
            done = true;
        });
        if (!run(done, CANVAS_CONNECT_TIMEOUT_MS, [&] { cancel_socket(); }) || ec)
            return fail(error, "connect", ec);

        done = false;
        m_ws->async_handshake(m_host, CANVAS_WS_PATH, [&](const boost::system::error_code& e) {
            ec   = e;
            done = true;
        });
        if (!run(done, CANVAS_CONNECT_TIMEOUT_MS, [&] { cancel_socket(); }) || ec)
            return fail(error, "handshake", ec);
        m_ws->text(true);
        m_open = true;
        return true;
    }

    bool send(const std::string& text, std::string& error) override
    {
        if (!m_open) {
            error = "not connected";
            return false;
        }
        boost::system::error_code ec;
        bool                      done = false;
        m_ws->async_write(net::buffer(text), [&](const boost::system::error_code& e, std::size_t) {
            ec   = e;
            done = true;
        });
        if (!run(done, CANVAS_CONNECT_TIMEOUT_MS, [&] { cancel_socket(); }) || ec)
            return fail(error, "send", ec);
        return true;
    }

    bool receive(std::string& text, int timeout_ms, std::string& error) override
    {
        if (!m_open) {
            error = "not connected";
            return false;
        }
        beast::flat_buffer        buffer;
        boost::system::error_code ec;
        bool                      done = false;
        // Pings from the printer are answered by beast inside this read.
        m_ws->async_read(buffer, [&](const boost::system::error_code& e, std::size_t) {
            ec   = e;
            done = true;
        });
        if (!run(done, timeout_ms, [&] { cancel_socket(); }) || ec)
            return fail(error, "receive", ec);
        text = beast::buffers_to_string(buffer.data());
        BOOST_LOG_TRIVIAL(trace) << "CANVAS: frame of " << text.size() << " bytes";
        return true;
    }

    void close() override
    {
        if (!m_ws)
            return;
        if (m_open) {
            boost::system::error_code ec;
            bool                      done = false;
            m_ws->async_close(websocket::close_code::normal, [&](const boost::system::error_code& e) {
                ec   = e;
                done = true;
            });
            run(done, 1000, [&] { cancel_socket(); });
        }
        boost::system::error_code ignore;
        m_ws->next_layer().close(ignore);
        m_open      = false;
        m_timed_out = false;
        m_ws.reset();
        m_ioc.restart();
        m_ioc.poll();
    }

private:
    // Runs the io_context until the step completes or the time is up. On a timeout `on_timeout`
    // cancels the operation and the handlers are drained, so nothing refers to the stack frame
    // afterwards. The connection is unusable after a timeout.
    template<class Cancel> bool run(bool& done, int timeout_ms, Cancel on_timeout)
    {
        m_ioc.restart();
        m_ioc.run_for(std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 1));
        if (done)
            return true;
        on_timeout();
        m_ioc.restart();
        m_ioc.run();
        m_open      = false;
        m_timed_out = true;
        return false;
    }

    void cancel_socket()
    {
        if (!m_ws)
            return;
        boost::system::error_code ignore;
        m_ws->next_layer().cancel(ignore);
        m_ws->next_layer().close(ignore);
    }

    bool fail(std::string& error, const char* step, const boost::system::error_code& ec)
    {
        if (m_timed_out)
            error = std::string(step) + ": timed out";
        else
            error = std::string(step) + ": " + ec.message();
        m_timed_out = false;
        m_open      = false;
        return false;
    }

    std::string                                       m_host;
    net::io_context                                   m_ioc;
    std::unique_ptr<websocket::stream<tcp::socket>>   m_ws;
    bool                                              m_open { false };
    bool                                              m_timed_out { false };
};

} // namespace

CanvasQueryResult canvas_query(const std::string& address, const CanvasQueryOptions& opts)
{
    const std::string host = sdcp_host_of(address);
    if (host.empty()) {
        CanvasQueryResult r;
        r.error = "this printer has no address";
        return r;
    }
    CanvasQueryResult r;
    try {
        BeastSdcpTransport transport(host);
        r = canvas_query_over(transport, opts);
    } catch (const std::exception& e) {
        r       = CanvasQueryResult();
        r.error = std::string("CANVAS query failed: ") + e.what();
    }
    // Never the address in the log, only the outcome.
    if (r.ok)
        BOOST_LOG_TRIVIAL(info) << "CANVAS: " << r.info.unit_count << " unit(s), " << r.info.trays.size() << " tray(s), "
                                << r.info.loaded_count() << " with filament, AmsConnectStatus " << r.ams_connect_status;
    else
        BOOST_LOG_TRIVIAL(info) << "CANVAS: not read (" << r.error << ")";
    return r;
}

std::string canvas_capture(const std::string& address, const std::string& context_json, const CanvasQueryOptions& opts)
{
    const std::string host = sdcp_host_of(address);
    try {
        BeastSdcpTransport transport(host);
        return canvas_capture_over(transport, context_json, opts);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "CANVAS capture failed: " << e.what();
        return std::string();
    }
}

} // namespace Slic3r
