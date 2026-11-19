#include "LoopbackAcceptLoop.hpp"

#include <boost/asio/io_context.hpp>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace Slic3r {

using tcp = boost::asio::ip::tcp;

bool LoopbackAcceptLoop::run(tcp::acceptor&                            acceptor,
                             const std::function<void(tcp::socket*)>& on_connection,
                             int                                       max_failures,
                             std::chrono::milliseconds                 retry_delay)
{
    int failures = 0;
    while (!m_stop) {
        std::unique_ptr<tcp::socket> sock(new tcp::socket(acceptor.get_executor()));
        boost::system::error_code    ec;
        acceptor.accept(*sock, ec);
        if (m_stop)
            break; // stop()'s wake-up connection, or a request that raced the quit: drop it
        if (ec) {
            if (++failures >= max_failures)
                break;
            std::this_thread::sleep_for(retry_delay);
            continue;
        }
        failures = 0;
        on_connection(sock.release());
    }
    const bool stopped = m_stop;
    boost::system::error_code ig;
    acceptor.close(ig); // this thread, and only this thread, ever closes the acceptor
    m_finished = true;
    return stopped;
}

bool LoopbackAcceptLoop::stop(std::chrono::milliseconds timeout)
{
    m_stop = true;
    if (m_finished || m_port == 0)
        return true;

    // The wake-up connection is a plain blocking connect on a short-lived thread of its own (with
    // its own io_context: nothing here may touch the listener's), and this thread waits for it at
    // most `timeout` - or until run() has returned, whichever comes first. A loopback connect to a
    // listening port completes from the backlog at once. It does not when run() saw the flag
    // before it ever blocked in accept() and has already closed the listener: Windows then retries
    // the refused connect for about two seconds, which nobody needs to sit out. A connect that
    // hangs ends with the OS connect timeout (or the process) without anything here waiting on it.
    struct Wake
    {
        std::mutex              mutex;
        std::condition_variable cv;
        bool                    done { false };
        bool                    connected { false };
    };
    auto                 wake = std::make_shared<Wake>();
    const unsigned short port = m_port;
    try {
        std::thread([wake, port]() {
            bool connected = false;
            try {
                boost::asio::io_context   io;
                tcp::socket               s(io);
                boost::system::error_code ec;
                s.connect(tcp::endpoint(boost::asio::ip::address_v4::loopback(), port), ec);
                connected = !ec;
                s.close(ec);
            } catch (...) {
            }
            {
                std::lock_guard<std::mutex> lock(wake->mutex);
                wake->done      = true;
                wake->connected = connected;
            }
            wake->cv.notify_all();
        }).detach();
    } catch (const std::exception&) {
        return m_finished; // no thread to connect from
    }
    const auto                   deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock<std::mutex> lock(wake->mutex);
    while (!wake->done && !m_finished && std::chrono::steady_clock::now() < deadline)
        wake->cv.wait_for(lock, std::chrono::milliseconds(5)); // run() does not signal: look again soon
    return (wake->done && wake->connected) || m_finished;
}

} // namespace Slic3r
