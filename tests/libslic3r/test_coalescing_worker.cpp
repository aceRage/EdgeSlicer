#include <catch2/catch.hpp>

#include "libslic3r/CoalescingWorker.hpp"

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace Slic3r;

// The ZIP work-folder clean-up keeps one of these as a file-static object. Before it did, a
// finished pass left a joinable std::thread behind, and an exit that skipped the explicit
// shutdown (an unhandled exception ending the main loop, an end of session) aborted in that
// static's destructor. A failure of the first two cases is a std::terminate() of the test run.

TEST_CASE("CoalescingWorker: a finished pass that was never shut down is joined by the destructor", "[CoalescingWorker]")
{
    auto worker = std::make_unique<CoalescingWorker>();
    std::promise<void> done;
    std::future<void>  done_f = done.get_future();
    REQUIRE(worker->submit(0, [&done]() { done.set_value(); }));
    done_f.wait();
    // The task has ended (the loop may still be finishing); the thread object is held until joined.
    REQUIRE(worker->holds_thread());
    worker.reset(); // a plain joinable std::thread here would call std::terminate()
    SUCCEED();
}

TEST_CASE("CoalescingWorker: the destructor cancels a running task and joins it", "[CoalescingWorker]")
{
    std::atomic<bool>  saw_cancel{false};
    std::promise<void> started;
    std::future<void>  started_f = started.get_future();
    {
        CoalescingWorker worker;
        REQUIRE(worker.submit(0, [&]() {
            started.set_value();
            while (!worker.cancelled())
                std::this_thread::yield();
            saw_cancel = true;
        }));
        started_f.wait();
    } // the destructor sets the cancel flag, the task returns, the join completes
    REQUIRE(saw_cancel.load());
}

TEST_CASE("CoalescingWorker: shutdown joins, refuses later work and stops hand-overs", "[CoalescingWorker]")
{
    CoalescingWorker worker;
    std::promise<void> done;
    std::future<void>  done_f = done.get_future();
    REQUIRE(worker.submit(0, [&done]() { done.set_value(); }));
    done_f.wait();
    REQUIRE(worker.run_unless_stopped([]() {}));

    worker.shutdown();
    REQUIRE_FALSE(worker.holds_thread());
    REQUIRE(worker.cancelled());
    bool ran = false;
    REQUIRE_FALSE(worker.submit(2, [&ran]() { ran = true; }));
    REQUIRE_FALSE(worker.run_unless_stopped([&ran]() { ran = true; }));
    REQUIRE_FALSE(ran);
    REQUIRE_FALSE(worker.holds_thread());
    worker.shutdown(); // idempotent
}

TEST_CASE("CoalescingWorker: a pending task is replaced by an equal or stronger one only", "[CoalescingWorker]")
{
    std::mutex       log_mutex;
    std::vector<int> log;
    auto note = [&](int id) {
        std::lock_guard<std::mutex> lock(log_mutex);
        log.push_back(id);
    };

    CoalescingWorker worker;
    std::promise<void> gate;
    std::shared_future<void> gate_f = gate.get_future().share();
    std::promise<void> first_started;
    std::future<void>  first_started_f = first_started.get_future();
    std::promise<void> last_done;
    std::future<void>  last_done_f = last_done.get_future();

    // Keep the thread busy so the next submits only change what is pending.
    REQUIRE(worker.submit(0, [&, gate_f]() {
        first_started.set_value();
        gate_f.wait();
        note(1);
    }));
    first_started_f.wait();

    REQUIRE(worker.submit(0, [&]() { note(2); }));                    // pending
    REQUIRE(worker.submit(1, [&]() { note(3); }));                    // stronger: replaces 2
    REQUIRE(worker.submit(0, [&]() { note(4); }));                    // weaker: dropped, 3 stays
    REQUIRE(worker.submit(1, [&]() { note(5); last_done.set_value(); })); // equal: replaces 3

    gate.set_value();
    last_done_f.wait();
    worker.shutdown();

    REQUIRE(log == std::vector<int>{1, 5});
}

TEST_CASE("CoalescingWorker: several passes in a row, then a shutdown", "[CoalescingWorker]")
{
    CoalescingWorker worker;
    for (int i = 0; i < 3; ++i) {
        std::promise<void> done;
        std::future<void>  done_f = done.get_future();
        REQUIRE(worker.submit(0, [&done]() { done.set_value(); }));
        done_f.wait();
    }
    REQUIRE(worker.holds_thread());
    worker.shutdown();
    REQUIRE_FALSE(worker.holds_thread());
}

TEST_CASE("CoalescingWorker: the thread-start hook runs on the worker thread", "[CoalescingWorker]")
{
    std::thread::id hook_thread;
    std::thread::id task_thread;
    {
        CoalescingWorker worker([&hook_thread]() { hook_thread = std::this_thread::get_id(); });
        std::promise<void> done;
        std::future<void>  done_f = done.get_future();
        REQUIRE(worker.submit(0, [&]() {
            task_thread = std::this_thread::get_id();
            done.set_value();
        }));
        done_f.wait();
    }
    REQUIRE(hook_thread == task_thread);
    REQUIRE(hook_thread != std::this_thread::get_id());
}
