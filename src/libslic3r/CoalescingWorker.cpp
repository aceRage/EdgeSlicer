#include "CoalescingWorker.hpp"

#include <system_error>
#include <utility>

namespace Slic3r {

CoalescingWorker::CoalescingWorker(std::function<void()> on_thread_start) : m_on_thread_start(std::move(on_thread_start)) {}

CoalescingWorker::~CoalescingWorker()
{
    // No lock here, see the class comment. A task still running sees the flag and returns; the
    // loop then sees it too and ends.
    m_cancel = true;
    if (m_thread.joinable()) {
        try {
            m_thread.join();
        } catch (...) {
            // join() only fails on a thread that cannot be joined from here; never terminate at exit.
            try {
                m_thread.detach();
            } catch (...) {}
        }
    }
}

bool CoalescingWorker::submit(int rank, Task task)
{
    if (!task)
        return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_shutdown || m_cancel.load())
        return false;
    if (!m_pending || rank >= m_pending_rank) {
        m_pending      = std::move(task);
        m_pending_rank = rank;
    }
    if (m_running)
        return true; // the running loop takes the pending task when its current one ends
    if (m_thread.joinable())
        m_thread.join(); // the previous loop has ended (m_running is false), so this is immediate
    m_running = true;
    try {
        m_thread = std::thread([this]() { loop(); });
    } catch (const std::system_error &) {
        m_running = false;
        m_pending = nullptr;
        return false;
    }
    return true;
}

void CoalescingWorker::loop()
{
    if (m_on_thread_start)
        m_on_thread_start();
    for (;;) {
        Task task;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_pending || m_shutdown || m_cancel.load()) {
                m_running = false;
                return;
            }
            task = std::move(m_pending);
            m_pending = nullptr;
        }
        try {
            task();
        } catch (...) {
            // A task reports its own errors; one that throws must not end the program.
        }
    }
}

bool CoalescingWorker::run_unless_stopped(const std::function<void()> &fn)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_shutdown || m_cancel.load())
        return false;
    fn();
    return true;
}

void CoalescingWorker::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_shutdown = true;
        m_pending  = nullptr;
    }
    m_cancel = true;
    if (m_thread.joinable()) {
        try {
            m_thread.join();
        } catch (...) {
            try {
                m_thread.detach();
            } catch (...) {}
        }
    }
}

} // namespace Slic3r
