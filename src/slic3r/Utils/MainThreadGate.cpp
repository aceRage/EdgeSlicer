#include "MainThreadGate.hpp"

#include <algorithm>
#include <condition_variable>
#include <memory>

namespace Slic3r {

bool MainThreadGate::post(const Poster& poster, std::function<void()> task)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_closed.load() || !poster)
        return false;
    try {
        // The wrapper re-checks on the GUI thread: work queued just before close() must not run
        // after it, because by then the Plater it would reach for may already be gone.
        poster([this, t = std::move(task)]() {
            if (m_closed.load())
                return;
            try { t(); } catch (...) {}
        });
    } catch (...) {
        return false;
    }
    return true;
}

void MainThreadGate::close()
{
    std::lock_guard<std::mutex> lock(m_mutex); // waits out a post() in progress on another thread
    m_closed = true;
}

void MainThreadGate::reopen()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_closed = false;
}

const char* main_call_result_name(MainCallResult r)
{
    switch (r) {
    case MainCallResult::Done:    return "done";
    case MainCallResult::Closing: return "closing";
    case MainCallResult::Timeout: return "timeout";
    }
    return "?";
}

namespace {

struct CallState
{
    std::mutex              mutex;
    std::condition_variable cv;
    bool                    started { false };
    bool                    finished { false };
    bool                    dropped { false }; // the task was destroyed without having started
};

// Owned only by the queued task: when the task object goes away without having run - the gate's
// wrapper skipped it, or the event loop was torn down with it still pending - the waiter hears
// about it at once instead of at its timeout.
struct DropNote
{
    std::shared_ptr<CallState> state;
    ~DropNote()
    {
        if (!state)
            return;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->started)
                state->dropped = true;
        }
        state->cv.notify_all();
    }
};

} // namespace

MainCallResult call_and_wait(MainThreadGate& gate, const MainThreadGate::Poster& poster, std::function<void()> fn,
                             std::chrono::milliseconds timeout)
{
    auto state = std::make_shared<CallState>();
    auto note  = std::make_shared<DropNote>(); // built in place: a temporary's destructor would report a drop
    note->state = state;
    const bool queued = gate.post(poster, [state, note, fn = std::move(fn)]() {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->started = true;
        }
        try { fn(); } catch (...) {}
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->finished = true;
        }
        state->cv.notify_all();
    });
    note.reset(); // from here the queued task holds the only reference
    if (!queued)
        return MainCallResult::Closing;

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock<std::mutex> lock(state->mutex);
    for (;;) {
        if (state->finished)
            return MainCallResult::Done;
        if (state->dropped || (!state->started && gate.closed()))
            return MainCallResult::Closing;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return MainCallResult::Timeout;
        // The gate has no way to wake us when it closes, so look again every 100 ms: a request
        // must not sit out a five-minute timeout because the app is quitting underneath it.
        state->cv.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds(100)));
    }
}

} // namespace Slic3r
