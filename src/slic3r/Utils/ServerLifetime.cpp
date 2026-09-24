#include "ServerLifetime.hpp"

#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>

#ifdef _WIN32
#  include <windows.h>
#endif

namespace Slic3r {

using tcp = boost::asio::ip::tcp;

struct ServerLifetime::State
{
    boost::asio::io_context  io;
    mutable std::mutex       mutex;
    std::condition_variable  cv;
    bool                     stopping { false };
    int                      running { 0 };
    std::set<tcp::socket*>   sockets;

    // Ends blocking reads and writes on the socket without touching asio's own bookkeeping for it:
    // the thread that owns the socket still closes and destroys it. mutex held.
    static void shut(tcp::socket* s)
    {
        boost::system::error_code ig;
        s->shutdown(tcp::socket::shutdown_both, ig);
#ifdef _WIN32
        // On Windows shutdown() alone does not wake a recv() that is already blocked in another
        // thread (it only sends the FIN; the unit test caught this). Cancelling every I/O request
        // outstanding on the handle does: the blocked recv returns WSAEINTR. Closing the handle
        // would too, but asio would then close it a second time when the owner destroys it.
        ::CancelIoEx(reinterpret_cast<HANDLE>(static_cast<SOCKET>(s->native_handle())), nullptr);
#endif
    }
};

ServerLifetime::ServerLifetime() : m_state(std::make_shared<State>()) {}

ServerLifetime::~ServerLifetime() = default;

boost::asio::io_context& ServerLifetime::io() { return m_state->io; }

bool ServerLifetime::spawn(std::function<void()> fn)
{
    std::shared_ptr<State> st = m_state;
    {
        std::lock_guard<std::mutex> lock(st->mutex);
        if (st->stopping)
            return false;
        ++st->running;
    }
    try {
        std::thread([st, fn = std::move(fn)]() mutable {
            {
                // Moved into this scope so that everything fn captured (a socket, an acceptor)
                // is destroyed here, while `st` still keeps the io_context alive.
                std::function<void()> f = std::move(fn);
                try { f(); } catch (...) {}
            }
            {
                std::lock_guard<std::mutex> lock(st->mutex);
                --st->running;
            }
            st->cv.notify_all();
        }).detach();
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(st->mutex);
            --st->running;
        }
        st->cv.notify_all();
        return false;
    }
    return true;
}

ServerLifetime::Tracked::Tracked(ServerLifetime& life, tcp::socket& socket) : m_state(life.m_state), m_socket(&socket)
{
    State* st = static_cast<State*>(m_state.get());
    std::lock_guard<std::mutex> lock(st->mutex);
    st->sockets.insert(m_socket);
    if (st->stopping)
        State::shut(m_socket);
}

ServerLifetime::Tracked::~Tracked()
{
    State* st = static_cast<State*>(m_state.get());
    std::lock_guard<std::mutex> lock(st->mutex); // stop() cannot be mid-shutdown on this socket
    st->sockets.erase(m_socket);
}

bool ServerLifetime::stopping() const
{
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->stopping;
}

int ServerLifetime::running() const
{
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return m_state->running;
}

bool ServerLifetime::stop(std::chrono::milliseconds timeout)
{
    State*                       st = m_state.get();
    std::unique_lock<std::mutex> lock(st->mutex);
    st->stopping = true;
    for (tcp::socket* s : st->sockets)
        State::shut(s);
    return st->cv.wait_for(lock, timeout, [st]() { return st->running == 0; });
}

} // namespace Slic3r
