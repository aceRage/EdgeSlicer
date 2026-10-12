#pragma once

// A blocking accept loop on a loopback listener that only its own thread ever touches, and that
// another thread can still stop (RemoteAccess's instance API, RemoteAccess.cpp).
//
// Why it exists: RemoteAccess::stop() used to call acceptor->close() on the GUI thread to unblock
// the accept thread, and the accept thread then deleted the acceptor. close() wakes the blocked
// accept() as soon as it closes the descriptor, but it is not finished with the acceptor yet: it
// still releases the reactor's descriptor state and resets the acceptor's fields. The woken thread
// ran the acceptor's destructor at the same time - a second close() of the same descriptor number,
// a second release of the same reactor state - and freed the acceptor while close() was still
// writing to it. The heap damage surfaced a moment later, in the next burst of frees: macOS caught
// it as a double free inside -[NSWindow orderOut:] when MainFrame::shutdown() hid the frame
// (EDGESLICER-9, "free to empty or invalid chunk detected", only on some quits).
//
// The rule this class enforces: stop() never touches the acceptor. It sets a flag and makes one
// loopback connection to the listener; the blocked accept() returns that connection, the accept
// thread sees the flag, drops it, closes the acceptor and returns, and only then may that thread
// destroy the acceptor.

#include <boost/asio/ip/tcp.hpp>

#include <atomic>
#include <chrono>
#include <functional>

namespace Slic3r {

class LoopbackAcceptLoop
{
public:
    // `port`: the loopback (127.0.0.1) port the acceptor listens on - where stop() connects.
    explicit LoopbackAcceptLoop(unsigned short port) : m_port(port) {}
    LoopbackAcceptLoop(const LoopbackAcceptLoop&)            = delete;
    LoopbackAcceptLoop& operator=(const LoopbackAcceptLoop&) = delete;

    // The accept thread's body. Hands every accepted connection to on_connection, which owns the
    // socket from then on. Returns once stop() has been called (true), or after `max_failures`
    // accept errors in a row (false: the listener is gone for good); a single failed accept - a
    // peer that gave up, a moment without file descriptors - is retried after `retry_delay`.
    // Closes the acceptor before it returns; the calling thread may destroy it afterwards.
    bool run(boost::asio::ip::tcp::acceptor&                            acceptor,
             const std::function<void(boost::asio::ip::tcp::socket*)>& on_connection,
             int                                                        max_failures = 50,
             std::chrono::milliseconds                                  retry_delay  = std::chrono::milliseconds(100));

    // Any thread but the accept thread, any number of times. Flags the stop and, unless run() has
    // already returned, wakes its accept() with one loopback connection - bounded by `timeout`, so
    // a GUI thread never hangs on it. False when that connection could not be made in time: the
    // loop then ends on the next connection it accepts, or with the process.
    bool stop(std::chrono::milliseconds timeout = std::chrono::milliseconds(500));

    bool stop_requested() const { return m_stop.load(); }
    bool finished() const { return m_finished.load(); } // run() has closed the acceptor and returned

private:
    const unsigned short m_port;
    std::atomic<bool>    m_stop { false };
    std::atomic<bool>    m_finished { false };
};

} // namespace Slic3r
