#pragma once

// Shared UI-thread marshal for plugin host bindings. The function runs with the GIL released
// so a Model / PresetBundle copy on the UI thread cannot deadlock a worker that holds the GIL.
//
// H3: the marshal is also called from threads that do NOT hold the GIL (the plugin-host IPC
// worker answering host.model / host.app_info for the helper, or the UI thread after the
// interpreter released the GIL). pybind11::gil_scoped_release there calls PyEval_SaveThread()
// with no thread state, which is a Py_FatalError in Python 3.12. ScopedGilReleaseIfHeld only
// releases a GIL the calling thread actually holds. The wait for the UI thread is bounded, so a
// UI thread that is itself waiting on the caller (a post-process run started on the UI thread)
// turns into an error, not a deadlock.

#include <slic3r/GUI/GUI_App.hpp>

#include "slic3r/plugin/EdgePythonInclude.hpp"
#include <pybind11/gil.h>
#include <pybind11/pybind11.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <wx/app.h>
#include <wx/thread.h>

namespace Slic3r {
namespace plugin_host {

inline bool app_closing() { return wxTheApp == nullptr || GUI::wxGetApp().is_closing(); }

enum : int { UiCallPending, UiCallClaimed, UiCallAbandoned };

// How long a plugin call waits for the UI thread before it fails.
inline constexpr std::chrono::milliseconds k_ui_call_timeout{30000};

// Releases the GIL for its lifetime only when this thread holds it; otherwise a no-op.
class ScopedGilReleaseIfHeld
{
public:
    ScopedGilReleaseIfHeld()
    {
        if (Py_IsInitialized() && PyGILState_Check())
            m_state = PyEval_SaveThread();
    }
    ~ScopedGilReleaseIfHeld()
    {
        if (m_state != nullptr)
            PyEval_RestoreThread(m_state);
    }
    bool released() const { return m_state != nullptr; }

    ScopedGilReleaseIfHeld(const ScopedGilReleaseIfHeld&)            = delete;
    ScopedGilReleaseIfHeld& operator=(const ScopedGilReleaseIfHeld&) = delete;

private:
    PyThreadState* m_state{nullptr};
};

// The generic marshal: `post` queues a task on the UI thread, `on_ui_thread` says whether the
// caller is the UI thread already, `closing` whether the app is going away. Tests drive it with
// their own "UI thread"; run_on_ui_blocking() below binds it to wx.
template<typename Fn>
auto run_on_ui_blocking_with(Fn&&                                              fn,
                             const std::function<void(std::function<void()>)>& post,
                             const std::function<bool()>&                      on_ui_thread,
                             const std::function<bool()>&                      closing,
                             std::chrono::milliseconds                         timeout = k_ui_call_timeout) -> std::invoke_result_t<Fn&>
{
    using R = std::invoke_result_t<Fn&>;

    if (on_ui_thread()) {
        ScopedGilReleaseIfHeld nogil;
        return fn();
    }

    if (closing())
        throw std::runtime_error("OrcaSlicer is closing");

    struct Call
    {
        std::promise<R>  prom;
        std::atomic<int> state{UiCallPending};
    };
    auto           call = std::make_shared<Call>();
    std::future<R> fut  = call->prom.get_future();

    ScopedGilReleaseIfHeld nogil;
    // The task owns a copy of fn: after a timeout the caller is gone, and a task that runs late
    // must find nothing of the caller's stack (it then also sees UiCallAbandoned and does nothing).
    auto fn_copy = std::make_shared<std::decay_t<Fn>>(std::forward<Fn>(fn));
    post([call, fn_copy]() {
        int expected = UiCallPending;
        if (!call->state.compare_exchange_strong(expected, UiCallClaimed))
            return;
        try {
            if constexpr (std::is_void_v<R>) {
                (*fn_copy)();
                call->prom.set_value();
            } else {
                call->prom.set_value((*fn_copy)());
            }
        } catch (...) {
            call->prom.set_exception(std::current_exception());
        }
    });
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (fut.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready) {
        int expected = UiCallPending;
        if (closing() && call->state.compare_exchange_strong(expected, UiCallAbandoned))
            throw std::runtime_error("OrcaSlicer is closing");
        expected = UiCallPending;
        if (std::chrono::steady_clock::now() >= deadline && call->state.compare_exchange_strong(expected, UiCallAbandoned))
            throw std::runtime_error("OrcaSlicer did not answer the plugin in time (the UI thread is busy)");
    }
    return fut.get();
}

template<typename Fn>
auto run_on_ui_blocking(Fn&& fn) -> std::invoke_result_t<Fn&>
{
    if (wxTheApp == nullptr)
        throw std::runtime_error("OrcaSlicer application is not initialized");
    return run_on_ui_blocking_with(
        std::forward<Fn>(fn), [](std::function<void()> task) { GUI::wxGetApp().CallAfter(std::move(task)); },
        [] { return wxIsMainThread(); }, [] { return app_closing(); });
}

} // namespace plugin_host
} // namespace Slic3r
