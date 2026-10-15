// EDGESLICER-A: ~MediaPlayCtrl waited for its camera worker with a full wxYield(), which ran a
// Plater timer against the half-destroyed MainFrame while the Device tab was torn down. The join
// policy now lives in slic3r/Utils/TeardownJoin.hpp: wait in slices, serve only the traffic a
// worker blocked on the GUI thread needs, widen (still without timers / input / idle) only after a
// deadline. These tests drive it with a real worker thread and a fake "GUI thread" pump.

#include <catch2/catch.hpp>

#include "slic3r/Utils/TeardownJoin.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace Slic3r;
using namespace std::chrono_literals;

namespace {

// A worker thread that finishes once `release` is true, joined in short slices like
// boost::thread::try_join_for(10ms).
struct Worker
{
    std::atomic<bool> release{false};
    std::atomic<bool> done{false};
    std::thread       thread;

    Worker()
    {
        thread = std::thread([this] {
            while (!release.load())
                std::this_thread::sleep_for(1ms);
            done = true;
        });
    }
    ~Worker()
    {
        release = true;
        if (thread.joinable())
            thread.join();
    }
    bool try_join()
    {
        if (done.load()) {
            thread.join();
            return true;
        }
        std::this_thread::sleep_for(2ms);
        return false;
    }
};

} // namespace

TEST_CASE("TeardownJoin: an idle worker is joined without pumping", "[TeardownJoin]")
{
    Worker w;
    w.release = true;
    while (!w.done.load())
        std::this_thread::sleep_for(1ms);

    int        pumps = 0;
    const auto res   = join_without_reentry([&] { return w.try_join(); }, [&](TeardownPump) { ++pumps; }, 10s);
    CHECK(res.rounds == 0);
    CHECK(pumps == 0);
    CHECK_FALSE(res.widened);
}

TEST_CASE("TeardownJoin: a worker blocked on GUI-thread service finishes with the service pump only", "[TeardownJoin]")
{
    // Stands for a WMP / DirectShow call marshalled to the UI apartment: it completes once the
    // GUI thread has served it, which the Service stage does.
    Worker                    w;
    std::vector<TeardownPump> stages;
    const auto res = join_without_reentry([&] { return w.try_join(); },
                                          [&](TeardownPump stage) {
                                              stages.push_back(stage);
                                              if (stage == TeardownPump::Service && stages.size() >= 3)
                                                  w.release = true;
                                          },
                                          10s);
    CHECK_FALSE(res.widened);
    REQUIRE_FALSE(stages.empty());
    for (TeardownPump s : stages)
        CHECK(s == TeardownPump::Service);
    CHECK(res.rounds == int(stages.size()));
}

TEST_CASE("TeardownJoin: widens only after the deadline, and never narrows again", "[TeardownJoin]")
{
    // A worker the Service stage cannot unblock: the join must not hang, and must keep the narrow
    // pump until the deadline has passed.
    Worker                                 w;
    std::vector<TeardownPump>              stages;
    std::chrono::steady_clock::time_point  first_wide{};
    const auto                             start = std::chrono::steady_clock::now();
    const auto                             res   = join_without_reentry([&] { return w.try_join(); },
                                          [&](TeardownPump stage) {
                                              stages.push_back(stage);
                                              if (stage == TeardownPump::Wide) {
                                                  if (first_wide == std::chrono::steady_clock::time_point{})
                                                      first_wide = std::chrono::steady_clock::now();
                                                  w.release = true;
                                              }
                                          },
                                          50ms);
    CHECK(res.widened);
    REQUIRE(stages.size() >= 2);
    CHECK(stages.front() == TeardownPump::Service);
    CHECK(first_wide - start >= 50ms);
    bool seen_wide = false;
    for (TeardownPump s : stages) {
        if (s == TeardownPump::Wide)
            seen_wide = true;
        else
            CHECK_FALSE(seen_wide); // no Service round after the first Wide one
    }
}
