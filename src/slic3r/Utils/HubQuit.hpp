#pragma once

// How the phone hub (RemoteHub.cpp, `--hub`) turns a request from outside to terminate into the
// same clean stop as "Quit hub" in its tray menu: go2rtc (and the ffmpeg it started) stopped by
// pid, the listeners closed, hub.json removed and "RemoteHub: shutting down" in hub.log.
//
// Why it exists: `kill -TERM <hub>` - what macOS sends at logout and restart - ended the hub with
// the default action. No log line, hub.json left behind, and go2rtc orphaned (PPID 1) with
// go2rtc.pid still naming it; the next hub had to clean it up (2026-10-09, #418).
//
// The pieces, all usable from tests:
//   * Requests  - which terminate request this is: the first starts the clean stop, a later one
//                 means nobody is waiting for it any more (lock-free: a signal handler uses it);
//   * Watchdog  - the safety net: once the hub has begun to quit it is gone within
//                 SHUTDOWN_DEADLINE_MS, clean or not;
//   * POSIX signals - SIGTERM/SIGINT/SIGHUP through a self-pipe to a normal thread, which calls
//                 the hub's request_quit(). The handler itself only writes one byte (or, for a
//                 second signal, kills the registered child group and _exit()s): nothing that is
//                 not async-signal-safe ever runs in signal context.
//
// Windows has no signals to handle here: the hub is a GUI-subsystem process with no console, so
// it gets WM_QUERYENDSESSION / WM_ENDSESSION (wxEVT_QUERY_END_SESSION / wxEVT_END_SESSION) and
// WM_CLOSE instead, and those are handled in RemoteHub.cpp's HubApp. A console control handler
// does not apply: CTRL_LOGOFF_EVENT / CTRL_SHUTDOWN_EVENT only reach console processes that never
// loaded user32.dll.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace Slic3r {
namespace HubQuit {

// Once the hub has begun to quit - for any reason - it exits within this long. shutdown() itself
// is bounded well inside it (loop wake-up, a 3 s connection drain, a 2 s go2rtc grace, the push
// workers' joins), so only a stop that is really stuck ever reaches it.
constexpr int SHUTDOWN_DEADLINE_MS = 20000;

// Which terminate request this is. Lock-free, so a POSIX signal handler may call it.
class Requests
{
public:
    enum class Step { BeginQuit, ForceExit };
    // One more request: the first starts the clean stop; any later one means the person or the OS
    // has stopped waiting for it, so the process must go now.
    Step on_request() { return m_count.fetch_add(1) == 0 ? Step::BeginQuit : Step::ForceExit; }
    int  count() const { return m_count.load(); }
    void reset() { m_count.store(0); }

private:
    std::atomic<int> m_count { 0 };
};
static_assert(std::atomic<int>::is_always_lock_free, "a signal handler uses Requests");

// A one-shot countdown on its own thread: arm() starts it, disarm() ends it, and if neither the
// process nor disarm() got there first, `on_expire` runs on the watchdog's thread. In the hub
// on_expire logs, flushes and exits; tests pass something harmless.
class Watchdog
{
public:
    Watchdog() = default;
    ~Watchdog() { disarm(); }
    Watchdog(const Watchdog&)            = delete;
    Watchdog& operator=(const Watchdog&) = delete;

    // True the first time; false (and nothing changes) after that, or once disarmed - so every
    // place that sees the quit begin can call it and the earliest one sets the deadline.
    bool arm(std::chrono::milliseconds after, std::function<void()> on_expire)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_armed || m_disarmed) return false;
        m_armed  = true;
        m_thread = std::thread([this, after, fn = std::move(on_expire)]() {
            {
                std::unique_lock<std::mutex> lk(m_mutex);
                if (m_cv.wait_for(lk, after, [this] { return m_disarmed; })) return;
                m_fired = true;
            }
            if (fn) fn();
        });
        return true;
    }

    // The stop finished: end the countdown and wait for its thread. Safe without arm(), twice,
    // and from on_expire itself (the thread is then left to finish on its own).
    void disarm()
    {
        std::thread t;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_disarmed = true;
            t          = std::move(m_thread);
        }
        m_cv.notify_all();
        if (!t.joinable()) return;
        if (t.get_id() == std::this_thread::get_id()) t.detach();
        else t.join();
    }

    bool armed() const { std::lock_guard<std::mutex> lock(m_mutex); return m_armed; }
    bool fired() const { std::lock_guard<std::mutex> lock(m_mutex); return m_fired; }

private:
    mutable std::mutex      m_mutex;
    std::condition_variable m_cv;
    std::thread             m_thread;
    bool                    m_armed { false };
    bool                    m_disarmed { false };
    bool                    m_fired { false };
};

#ifndef _WIN32

namespace detail {
struct SignalState
{
    int                                       pipe_r { -1 };
    int                                       pipe_w { -1 };
    Requests                                  requests;
    std::atomic<long>                         child_group { 0 }; // killed by forced_exit(); 0 = none
    std::vector<std::pair<int, struct sigaction>> previous;      // what install() replaced
    std::function<void(int)>                  on_first;
    std::thread                               watcher;
    bool                                      installed { false };
};
// Created by install() (or set_forced_exit_group()) before any handler can run, so a signal
// handler never reaches the first-use initialisation of this static.
inline SignalState& state()
{
    static SignalState s;
    return s;
}
static_assert(std::atomic<long>::is_always_lock_free, "a signal handler reads child_group");
} // namespace detail

// The process group a forced exit takes down first (go2rtc leads its own group, and the ffmpeg it
// starts is in it - HubPlatform::spawn_child_posix). 0 = none. Any thread.
inline void set_forced_exit_group(long pid) { detail::state().child_group.store(pid > 1 ? pid : 0); }

// Exit now, without the clean stop, but never leaving the registered child group behind.
// Async-signal-safe: only kill() and _exit().
[[noreturn]] inline void forced_exit(int code)
{
    const long g = detail::state().child_group.load();
    if (g > 1) {
        ::kill(-(pid_t) g, SIGKILL);
        ::kill((pid_t) g, SIGKILL);
    }
    ::_exit(code);
}

inline void signal_handler(int sig)
{
    const int saved = errno;
    detail::SignalState& s = detail::state();
    if (s.requests.on_request() == Requests::Step::ForceExit)
        forced_exit(128 + sig); // a second signal while the first one's stop is running
    const unsigned char b = (unsigned char) sig;
    if (s.pipe_w >= 0) (void) !::write(s.pipe_w, &b, 1);
    errno = saved;
}

// Route `sigs` to `on_first(sig)`, which runs on a thread of its own (never in signal context) for
// the first signal; a second one exits at once through forced_exit(128 + sig). A signal that was
// ignored when this is called (nohup's SIGHUP) stays ignored, except SIGTERM. False when already
// installed or the pipe could not be made (the default actions then stay as they were).
inline bool install_signal_handlers(const std::vector<int>& sigs, std::function<void(int)> on_first)
{
    detail::SignalState& s = detail::state();
    if (s.installed) return false;
    int p[2] = { -1, -1 };
    if (::pipe(p) != 0) return false;
    for (int fd : p) {
        const int fl = ::fcntl(fd, F_GETFD);
        if (fl >= 0) ::fcntl(fd, F_SETFD, fl | FD_CLOEXEC); // no slicer or go2rtc inherits it
    }
    const int wfl = ::fcntl(p[1], F_GETFL);
    if (wfl >= 0) ::fcntl(p[1], F_SETFL, wfl | O_NONBLOCK); // a handler must never block
    s.requests.reset();
    s.pipe_r   = p[0];
    s.pipe_w   = p[1];
    s.on_first = std::move(on_first);
    s.watcher  = std::thread([&s]() {
        for (;;) {
            unsigned char b = 0;
            const ssize_t n = ::read(s.pipe_r, &b, 1);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0 || b == 0) return; // closed, or uninstall()'s wake-up
            if (s.on_first) s.on_first((int) b);
        }
    });
    for (int sig : sigs) {
        struct sigaction old {};
        if (::sigaction(sig, nullptr, &old) != 0) continue;
        if (old.sa_handler == SIG_IGN && sig != SIGTERM) continue;
        struct sigaction sa {};
        sa.sa_handler = &signal_handler;
        sigemptyset(&sa.sa_mask); // no ::, macOS defines sigemptyset as a macro
        sa.sa_flags = SA_RESTART;
        if (::sigaction(sig, &sa, &old) == 0) s.previous.emplace_back(sig, old);
    }
    s.installed = true;
    return true;
}

// Put the replaced handlers back and end the watcher thread. Call it once nothing a signal would
// reach (the hub's server) is left; a signal after it gets the old (default) action.
inline void uninstall_signal_handlers()
{
    detail::SignalState& s = detail::state();
    if (!s.installed) return;
    for (const auto& prev : s.previous) ::sigaction(prev.first, &prev.second, nullptr);
    s.previous.clear();
    const unsigned char zero = 0;
    (void) !::write(s.pipe_w, &zero, 1);
    if (s.watcher.joinable()) {
        if (s.watcher.get_id() == std::this_thread::get_id()) s.watcher.detach();
        else s.watcher.join();
    }
    ::close(s.pipe_r);
    ::close(s.pipe_w);
    s.pipe_r = s.pipe_w = -1;
    s.on_first          = nullptr;
    s.installed         = false;
}

// Signals received since install_signal_handlers().
inline int signals_received() { return detail::state().requests.count(); }

inline const char* signal_name(int sig)
{
    switch (sig) {
    case SIGTERM: return "SIGTERM";
    case SIGINT: return "SIGINT";
    case SIGHUP: return "SIGHUP";
    default: return "a signal";
    }
}

#else // _WIN32

inline void set_forced_exit_group(long) {} // go2rtc is in a kill-on-close job: it goes with the process
[[noreturn]] inline void forced_exit(int code) { std::_Exit(code); }

#endif

} // namespace HubQuit
} // namespace Slic3r
