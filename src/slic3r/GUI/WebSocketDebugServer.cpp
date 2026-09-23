// WebSocket Debug Server implementation
#include "WebSocketDebugServer.hpp"
#include <boost/beast/http.hpp>
#include <boost/log/trivial.hpp>
#include <iostream>

namespace Slic3r { namespace GUI {

namespace {

// The debug client is a flutter dev build served from localhost (or a CLI tool with no Origin).
// WebSockets are not covered by CORS, so without this check any website open in a browser could
// connect and drive the app through SSWCP while debug mode is on.
bool is_loopback_origin(const std::string& origin)
{
    if (origin.empty())
        return true;
    std::string rest;
    if (origin.compare(0, 7, "http://") == 0)
        rest = origin.substr(7);
    else if (origin.compare(0, 8, "https://") == 0)
        rest = origin.substr(8);
    else
        return false;
    std::string host;
    if (!rest.empty() && rest[0] == '[') {
        const size_t close = rest.find(']');
        if (close == std::string::npos)
            return false;
        host = rest.substr(0, close + 1);
        rest = rest.substr(close + 1);
    } else {
        const size_t colon = rest.find(':');
        host = rest.substr(0, colon);
        rest = colon == std::string::npos ? std::string() : rest.substr(colon);
    }
    if (!rest.empty() && (rest[0] != ':' || rest.size() < 2 || rest.find_first_not_of("0123456789", 1) != std::string::npos))
        return false;
    return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
}

} // namespace

WebSocketDebugServer::WebSocketDebugServer(unsigned short port)
    : m_port(port)
    , m_running(false)
    , m_has_client(false)
{
    BOOST_LOG_TRIVIAL(info) << "WebSocketDebugServer created on port " << m_port;
}

WebSocketDebugServer::~WebSocketDebugServer()
{
    stop();
}

bool WebSocketDebugServer::start()
{
    if (m_running.load()) {
        BOOST_LOG_TRIVIAL(warning) << "WebSocket Debug Server already running";
        return true;
    }

    m_io_context = std::make_unique<net::io_context>();

    // Loopback only: this is a developer bridge straight into SSWCP with no authentication.
    tcp::endpoint endpoint(net::ip::make_address_v4("127.0.0.1"), m_port);
    m_acceptor = std::make_unique<tcp::acceptor>(*m_io_context, endpoint);

    m_running.store(true);

    // Start accept thread
    m_accept_thread = std::thread(&WebSocketDebugServer::accept_loop, this);

    // Start send worker thread
    m_send_thread = std::thread(&WebSocketDebugServer::send_worker, this);

    BOOST_LOG_TRIVIAL(info) << " WebSocket Debug Server started on ws://localhost:" << m_port;
    BOOST_LOG_TRIVIAL(info) << "   Waiting for Flutter Web client to connect...";
    return true;

}

void WebSocketDebugServer::stop()
{
    if (!m_running.load()) 
        return;

    BOOST_LOG_TRIVIAL(info) << "Stopping WebSocket Debug Server...";
    m_running.store(false);
    m_send_cv.notify_all(); 

    if (m_acceptor && m_acceptor->is_open()) 
    {
        boost::system::error_code ec;
        m_acceptor->close(ec);

        if (ec)
            BOOST_LOG_TRIVIAL(warning) << "Error closing acceptor: " << ec.message();        
    }

    // Close WebSocket connection
    if (m_ws_stream) 
    {
        boost::system::error_code ec;
        m_ws_stream->close(websocket::close_code::normal, ec);

        if (ec)         
            BOOST_LOG_TRIVIAL(warning) << "Error closing WebSocket: " << ec.message();        
    }

    // Stop io_context
    if (m_io_context)
        m_io_context->stop();
    
    // Join threads
    if (m_accept_thread.joinable())    
        m_accept_thread.join();
    
    if (m_send_thread.joinable()) 
        m_send_thread.join();

    for (auto& t : m_session_threads) 
        if (t.joinable()) t.join();
    
    m_session_threads.clear();
    m_has_client.store(false);

    BOOST_LOG_TRIVIAL(info) << "WebSocket Debug Server stopped";
}

void WebSocketDebugServer::accept_loop()
{
    while (m_running.load())
    {       
        tcp::socket socket(*m_io_context);

        // Accept connection (blocking)
        boost::system::error_code ec;
        m_acceptor->accept(socket, ec);

        if (ec)
        {

            if (m_running.load()) 
                BOOST_LOG_TRIVIAL(error) << "Accept error: " << ec.message(); 

            continue;
        }

        BOOST_LOG_TRIVIAL(info) << "Flutter Web client connected from "
                                << socket.remote_endpoint().address().to_string();

        // Spawn a thread per session so accept_loop stays unblocked
        m_session_threads.emplace_back(&WebSocketDebugServer::session_loop, this, std::move(socket));        
    }
}

void WebSocketDebugServer::session_loop(tcp::socket socket)
{
    // Create WebSocket stream
    auto ws = std::make_shared<websocket::stream<tcp::socket>>(std::move(socket));

    // Set WebSocket options
    ws->set_option(websocket::stream_base::decorator(
        [](websocket::response_type& res) {
            res.set(beast::http::field::server, "OrcaSlicer-Debug-Server");
        }
    ));

    // Read the upgrade request ourselves so its Origin can be checked before the handshake.
    beast::flat_buffer                              hs_buffer;
    beast::http::request<beast::http::string_body> hs_request;
    boost::system::error_code                       hs_ec;
    beast::http::read(ws->next_layer(), hs_buffer, hs_request, hs_ec);
    const std::string origin(hs_request[beast::http::field::origin]);
    if (hs_ec || !websocket::is_upgrade(hs_request) || !is_loopback_origin(origin)) {
        BOOST_LOG_TRIVIAL(warning) << "WebSocket Debug Server: refused a connection (origin '" << origin << "')";
        boost::system::error_code ignored;
        ws->next_layer().close(ignored);
        return;
    }

    // Accept WebSocket handshake
    ws->accept(hs_request, hs_ec);
    if (hs_ec) {
        BOOST_LOG_TRIVIAL(warning) << "WebSocket Debug Server: handshake failed: " << hs_ec.message();
        return;
    }

    // Swap in the new stream under the lock, then close old streams outside
    // the lock so send_worker is never blocked waiting for TCP teardown.
    std::vector<std::shared_ptr<websocket::stream<tcp::socket>>> to_close;
    {
        std::lock_guard<std::mutex> lock(m_client_mutex);
        to_close = std::move(m_old_streams);
        if (m_ws_stream) {
            to_close.push_back(m_ws_stream);
        }
        m_ws_stream = ws;
        m_has_client.store(true);
    }

    for (auto& old : to_close) {
        if (old && old->is_open()) {
            boost::system::error_code ec;
            old->close(websocket::close_code::going_away, ec);
        }
    }

    BOOST_LOG_TRIVIAL(info) << " WebSocket handshake completed, client ready";

    // Message receive loop
    while (m_running.load()) {
        beast::flat_buffer buffer;

        boost::system::error_code ec;
        ws->read(buffer, ec);

        if (ec == websocket::error::closed) {
            BOOST_LOG_TRIVIAL(info) << "Client closed connection";
            break;
        }

        if (ec) {
            BOOST_LOG_TRIVIAL(error) << "Read error: " << ec.message();
            break;
        }

        std::string message = beast::buffers_to_string(buffer.data());

        BOOST_LOG_TRIVIAL(debug) << "Received from Flutter: " << message.substr(0, 200)
                                    << (message.length() > 200 ? "..." : "");

        // Call message callback
        if (m_message_callback)
                m_message_callback(message);       
    }

    // Clean up
    {
        std::lock_guard<std::mutex> lock(m_client_mutex);
        m_ws_stream.reset();
        m_has_client.store(false);
    }

    BOOST_LOG_TRIVIAL(info) << " Flutter Web client disconnected";
}

void WebSocketDebugServer::send_worker()
{
    while (m_running.load()) {
        std::string message;

        {
            std::unique_lock<std::mutex> lock(m_send_mutex);
            m_send_cv.wait(lock, [this] {
                return !m_send_queue.empty() || !m_running.load();
            });

            if (!m_running.load()) break;

            message = m_send_queue.front();
            m_send_queue.pop();
        }

        if (!message.empty()) {
            std::lock_guard<std::mutex> lock(m_client_mutex);
            if (m_ws_stream && m_has_client.load()) 
            {

                    boost::system::error_code ec;
                    m_ws_stream->write(net::buffer(message), ec);

                    if (ec) 
                        BOOST_LOG_TRIVIAL(error) << "Send error: " << ec.message();
                     else 
                        BOOST_LOG_TRIVIAL(debug) << "Sent to Flutter: " << message.substr(0, 200)
                                                 << (message.length() > 200 ? "..." : "");                    

            }
        }
    }
}

void WebSocketDebugServer::send_message(const std::string& message)
{
    if (!m_running.load()) {
        BOOST_LOG_TRIVIAL(warning) << "Cannot send message: server not running";
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_send_mutex);
        m_send_queue.push(message);
    }
    m_send_cv.notify_one();
}

void WebSocketDebugServer::set_message_callback(MessageCallback callback)
{
    m_message_callback = callback;
}

}} // namespace Slic3r::GUI
