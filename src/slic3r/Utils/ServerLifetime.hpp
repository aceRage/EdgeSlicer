#pragma once

// Lifetime bookkeeping for a blocking boost::asio server that runs its accept loops and its
// connections on detached threads (the hub's listener, RemoteHub.cpp).
//
// Why it exists: the hub kept its io_context as a plain member and started one detached thread per
// accept loop and per connection. On exit the owner was destroyed - and the io_context with it -
// while a long-lived connection (a camera WebSocket tunnel, /api/ws) was still running. When that
// connection ended, its thread destroyed its socket, and asio's socket destructor locked a mutex
// inside the already-freed socket service: an access violation in
// win_iocp_socket_service_base::destroy while the main thread was tearing down windows
// (crash a2375b06, 2026-09-22).
//
// The rules this class enforces:
//   * the io_context is shared by the owner and every thread spawn() started, so it is destroyed
//     by whichever of them lets go last - a socket can never outlive its service;
//   * stop() refuses new threads, shuts every tracked socket down (which ends a blocking read or a
//     tunnel on it) and waits, bounded, for the threads to return. An owner that gets `false` back
//     knows threads are still running and must not destroy anything they use.

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <chrono>
#include <functional>
#include <memory>

namespace Slic3r {

class ServerLifetime
{
public:
    ServerLifetime();
    // Does not wait for anything. The io_context stays alive for as long as any spawned thread is.
    ~ServerLifetime();
    ServerLifetime(const ServerLifetime&)            = delete;
    ServerLifetime& operator=(const ServerLifetime&) = delete;

    boost::asio::io_context& io();

    // Run fn on a detached thread this lifetime counts. Whatever fn captured is destroyed before
    // the thread lets go of the io_context. False, and fn is not run, once stop() has begun.
    bool spawn(std::function<void()> fn);

    // While alive, stop() can shut `socket` down to end whatever blocking call is using it. Create
    // it on the thread that owns the socket and let it die before the socket does. Created after
    // stop() began, it shuts the socket down straight away.
    class Tracked
    {
    public:
        Tracked(ServerLifetime& life, boost::asio::ip::tcp::socket& socket);
        ~Tracked();
        Tracked(const Tracked&)            = delete;
        Tracked& operator=(const Tracked&) = delete;

    private:
        std::shared_ptr<void>          m_state; // the lifetime's State (opaque here)
        boost::asio::ip::tcp::socket*  m_socket;
    };

    bool stopping() const;
    int  running() const; // threads spawn() started that have not returned yet

    // Refuse new threads, shut down every tracked socket and wait up to `timeout` for every
    // spawned thread to return. True when none is left. Never call it from a spawned thread.
    bool stop(std::chrono::milliseconds timeout);

private:
    struct State;
    std::shared_ptr<State> m_state;
};

} // namespace Slic3r
