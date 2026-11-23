#ifndef slic3r_CoalescingWorker_hpp_
#define slic3r_CoalescingWorker_hpp_

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

namespace Slic3r {

// One background thread that runs queued tasks one after another. A task queued while another
// one is pending replaces it when its rank is equal or higher, and is dropped otherwise.
//
// The worker owns its thread for its whole life: the destructor cancels and joins it. A
// std::thread that is still joinable when it is destroyed calls std::terminate(), and a worker
// that is a file-static object is destroyed at process exit, after any exit path that skipped
// shutdown() (an unhandled exception that ended the main loop, an end of session).
//
// shutdown() is the orderly stop and is meant to run while the program is still whole: it
// cancels, drops what is pending, joins, and refuses later submits. The destructor is the last
// guard and does less on purpose: it sets the cancel flag and joins, and it never takes the
// lock. On Windows a static object of a DLL is destroyed in DLL process detach, after
// ExitProcess() has already ended every other thread, possibly while one of them held the lock;
// taking it there would turn the old abort into a hang. The join returns at once in that case
// (the thread is gone).
class CoalescingWorker
{
public:
    using Task = std::function<void()>;

    // on_thread_start runs on each new worker thread before its first task (thread priority).
    explicit CoalescingWorker(std::function<void()> on_thread_start = {});
    ~CoalescingWorker();

    CoalescingWorker(const CoalescingWorker &) = delete;
    CoalescingWorker &operator=(const CoalescingWorker &) = delete;

    // Queue a task; starts the thread when none is running. Returns false when the task was not
    // queued: the worker is shut down, or the thread could not be started. Owner thread only.
    bool submit(int rank, Task task);

    // Cancel, drop what is pending and join. Later submits are refused. Idempotent. Owner thread
    // only.
    void shutdown();

    // Set by shutdown() and by the destructor. Long tasks poll it and return early.
    bool cancelled() const { return m_cancel.load(); }

    // Runs fn under the worker's lock unless the worker is shut down or cancelled, and says whether
    // it ran. A task hands its result to another thread through this, so that nothing is handed
    // over once shutdown() has returned.
    bool run_unless_stopped(const std::function<void()> &fn);

    // Whether a thread object is held: a running thread, or a finished one not joined yet. For the
    // tests.
    bool holds_thread() const { return m_thread.joinable(); }

private:
    void loop();

    std::function<void()> m_on_thread_start;
    std::mutex            m_mutex;
    // Guarded by m_mutex.
    Task                  m_pending;
    int                   m_pending_rank = 0;
    bool                  m_running      = false;
    bool                  m_shutdown     = false;
    std::atomic<bool>     m_cancel{false};
    // Owner thread only (submit, shutdown, destructor).
    std::thread           m_thread;
};

} // namespace Slic3r

#endif // slic3r_CoalescingWorker_hpp_
