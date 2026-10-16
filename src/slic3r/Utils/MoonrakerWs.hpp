#pragma once

// A small long-lived client for Moonraker's JSON-RPC WebSocket (ws://<printer>/websocket): one
// connection, one I/O thread, text frames in and out. Used by GUI/U1WsRoute to carry the Device
// page's requests to a U1 over the LAN. Keep-alive pings detect a printer that went away.

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace Slic3r {

class MoonrakerWs : public std::enable_shared_from_this<MoonrakerWs>
{
public:
    using OnMessage = std::function<void(const std::string& text)>;
    using OnClosed  = std::function<void(const std::string& why)>;

    static std::shared_ptr<MoonrakerWs> create();
    ~MoonrakerWs();

    // Set before connect(). Both run on the I/O thread.
    void set_handlers(OnMessage on_message, OnClosed on_closed);
    // Blocking: TCP connect + WebSocket upgrade within `timeout_ms`. On success the I/O thread
    // starts reading. `idle_timeout_ms`: no traffic (pings included) for this long closes it.
    bool connect(const std::string& host, int port, int timeout_ms, int idle_timeout_ms, std::string& error);
    // Queue one text frame; false when the connection is not open.
    bool send(const std::string& text);
    bool is_open() const { return m_open.load(); }
    // Close and wait for the I/O thread (never call from inside a handler: it is detached then).
    void close();

    struct Impl;

private:
    MoonrakerWs();
    void do_read(std::shared_ptr<MoonrakerWs> self);
    void do_write(std::shared_ptr<MoonrakerWs> self);
    std::unique_ptr<Impl> m;
    std::atomic<bool>     m_open { false };
    std::thread           m_thread;
    std::mutex            m_close_mtx;
};

} // namespace Slic3r
