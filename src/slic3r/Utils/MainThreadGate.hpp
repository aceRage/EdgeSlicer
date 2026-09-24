#pragma once

// Work that request and worker threads hand to the GUI thread, and the one switch that stops it
// once the main window is going away.
//
// The phone API (RemoteAccess), the send and control jobs (RemoteSend, RemoteControl) and the
// Snapmaker connect calls (RemoteSnapmaker) all run on threads of their own and queue anything that
// touches the Plater, the plates, the presets or the devices onto the GUI thread (wxApp::CallAfter),
// then wait for it. Nothing stopped that queue when the app closed: a request that arrived while the
// main window was being torn down ran on the GUI thread after the Plater had been freed and crashed
// in RemoteSend::export_name_for -> Plater::get_partplate_list (crash c2a7d4de, 2026-09-22).
//
// The gate closes when the main window starts shutting down (MainFrame::shutdown). From then on:
//   * nothing new is queued (post() returns false without calling the poster),
//   * anything already queued but not yet started is skipped when the GUI thread reaches it,
//   * a caller waiting in call_and_wait() is told Closing instead of waiting out its timeout,
//   * and close() does not return while another thread is still inside post(), so the app object
//     that CallAfter needs cannot be destroyed underneath a thread that is posting to it.
//
// This file is wx-free on purpose: the poster is a parameter (wxApp::CallAfter in the app, a plain
// queue in the tests), so the rule can be exercised without a GUI.

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>

namespace Slic3r {

class MainThreadGate
{
public:
    // Hands one task to the GUI thread, e.g. [](std::function<void()> f) { wxGetApp().CallAfter(f); }.
    using Poster = std::function<void(std::function<void()>)>;

    // Queue `task` through `poster` unless the gate is closed. The queued wrapper checks the gate
    // again when it runs and drops `task` without calling it if the gate closed in the meantime.
    // False when closed (the poster is not called) or when the poster threw.
    bool post(const Poster& poster, std::function<void()> task);

    // Main thread, as the first step of shutting the main window down. Blocks only while another
    // thread is inside post() (a queue push), never on queued work.
    void close();
    // The window was rebuilt (language switch): let work through again.
    void reopen();
    bool closed() const { return m_closed.load(); }

private:
    mutable std::mutex m_mutex; // held across a post, so close() cannot slip between check and push
    std::atomic<bool>  m_closed { false };
};

enum class MainCallResult {
    Done,    // the task ran to the end on the GUI thread
    Closing, // the gate was closed: the task was never started (or never will be)
    Timeout  // the gate is open but the GUI thread did not get to the task in time
};

const char* main_call_result_name(MainCallResult r);

// Post `fn` through the gate and wait for it. A task that has started always runs to the end and
// is waited for (up to `timeout`); one that has not started when the gate closes is abandoned at
// once rather than after `timeout`, which is up to five minutes for a send.
MainCallResult call_and_wait(MainThreadGate& gate, const MainThreadGate::Poster& poster, std::function<void()> fn,
                             std::chrono::milliseconds timeout);

} // namespace Slic3r
