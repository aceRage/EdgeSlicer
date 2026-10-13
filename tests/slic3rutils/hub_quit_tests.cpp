// The phone hub's terminate plumbing (slic3r/Utils/HubQuit.hpp): which request starts the clean
// stop and which one forces the exit, the shutdown watchdog, and - on macOS / Linux - signals
// arriving on a normal thread instead of in signal context. The signal tests use SIGUSR1 / SIGUSR2
// so the test runner's own SIGTERM / SIGINT stay as they were; the forced exit runs in a fork.

#include <catch2/catch.hpp>

#include "slic3r/Utils/HubQuit.hpp"

#include <atomic>
#include <chrono>
#include <thread>

#ifndef _WIN32
#include "slic3r/Utils/HubPlatform.hpp"
#include <cerrno>
#include <cstdio>
#include <sys/wait.h>
#endif

using namespace Slic3r::HubQuit;
using namespace std::chrono_literals;

TEST_CASE("the first terminate request starts the clean stop, any later one forces the exit", "[HubQuit]")
{
    Requests r;
    REQUIRE(r.count() == 0);
    REQUIRE(r.on_request() == Requests::Step::BeginQuit);
    REQUIRE(r.on_request() == Requests::Step::ForceExit);
    REQUIRE(r.on_request() == Requests::Step::ForceExit);
    REQUIRE(r.count() == 3);
    r.reset();
    REQUIRE(r.on_request() == Requests::Step::BeginQuit);
}

TEST_CASE("the shutdown watchdog fires once its deadline passes", "[HubQuit]")
{
    Watchdog          w;
    std::atomic<int>  fired { 0 };
    REQUIRE_FALSE(w.armed());
    REQUIRE(w.arm(50ms, [&fired]() { ++fired; }));
    REQUIRE(w.armed());
    // Later arms (every place that sees the quit begin calls it) neither restart nor add a deadline.
    REQUIRE_FALSE(w.arm(1ms, [&fired]() { fired += 100; }));
    for (int i = 0; i < 200 && fired.load() == 0; ++i) std::this_thread::sleep_for(10ms);
    std::this_thread::sleep_for(50ms);
    REQUIRE(fired.load() == 1);
    REQUIRE(w.fired());
    w.disarm();
    REQUIRE(fired.load() == 1);
}

TEST_CASE("a stop that finishes in time disarms the watchdog", "[HubQuit]")
{
    std::atomic<int> fired { 0 };
    const auto       t0 = std::chrono::steady_clock::now();
    {
        Watchdog w;
        REQUIRE(w.arm(10s, [&fired]() { ++fired; }));
        std::this_thread::sleep_for(20ms);
        w.disarm(); // returns at once, not after 10 s
        REQUIRE_FALSE(w.fired());
        REQUIRE_FALSE(w.arm(1ms, [&fired]() { ++fired; })); // disarmed for good
    }
    const bool quick = std::chrono::steady_clock::now() - t0 < 5s;
    REQUIRE(quick);
    std::this_thread::sleep_for(30ms);
    REQUIRE(fired.load() == 0);

    Watchdog never; // destroyed without arm(): nothing to join
    never.disarm();
    REQUIRE_FALSE(never.armed());
}

#ifndef _WIN32

TEST_CASE("a signal reaches the callback on a normal thread, and uninstall puts the old handler back", "[HubQuit]")
{
    struct sigaction before {};
    REQUIRE(::sigaction(SIGUSR1, nullptr, &before) == 0);

    std::atomic<int>             got { 0 };
    std::atomic<bool>            other_thread { false };
    const std::thread::id        main_id = std::this_thread::get_id();
    REQUIRE(install_signal_handlers({ SIGUSR1 }, [&](int sig) {
        got          = sig;
        other_thread = std::this_thread::get_id() != main_id;
    }));
    REQUIRE_FALSE(install_signal_handlers({ SIGUSR1 }, [](int) {})); // once per process at a time

    ::raise(SIGUSR1);
    for (int i = 0; i < 300 && got.load() == 0; ++i) std::this_thread::sleep_for(10ms);
    REQUIRE(got.load() == SIGUSR1);
    REQUIRE(other_thread.load());
    REQUIRE(signals_received() == 1);

    uninstall_signal_handlers();
    struct sigaction after {};
    REQUIRE(::sigaction(SIGUSR1, nullptr, &after) == 0);
    REQUIRE(after.sa_handler == before.sa_handler);
    uninstall_signal_handlers(); // twice is harmless
}

TEST_CASE("a second signal exits at once and takes the registered child group with it", "[HubQuit]")
{
    // In a fork: the forced exit ends the process it runs in.
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        // A stand-in for go2rtc with an ffmpeg of its own: a process group of two.
        const long g = Slic3r::HubPlatform::spawn_child_posix({ "/bin/sh", "-c", "sleep 60 & exec sleep 60" }, "");
        if (g <= 0) ::_exit(2);
        set_forced_exit_group(g);
        // Tell the parent which group to look for afterwards.
        char path[64];
        std::snprintf(path, sizeof(path), "/tmp/hubquit_group_%d", (int) ::getpid());
        if (FILE* f = std::fopen(path, "w")) { std::fprintf(f, "%ld\n", g); std::fclose(f); }
        // The first signal starts a stop that never finishes (the callback does nothing); the
        // second one must still end the process.
        install_signal_handlers({ SIGUSR2 }, [](int) {});
        ::raise(SIGUSR2);
        std::this_thread::sleep_for(200ms);
        ::raise(SIGUSR2);
        std::this_thread::sleep_for(5s);
        ::_exit(0); // not reached when the forced exit works
    }
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 128 + SIGUSR2);

    char path[64];
    std::snprintf(path, sizeof(path), "/tmp/hubquit_group_%d", (int) child);
    long g = 0;
    if (FILE* f = std::fopen(path, "r")) { if (std::fscanf(f, "%ld", &g) != 1) g = 0; std::fclose(f); }
    std::remove(path);
    REQUIRE(g > 1);
    // SIGKILLed, and with the forked process gone init reaps them: give it a moment.
    bool empty = false;
    for (int i = 0; i < 100 && !empty; ++i) {
        empty = Slic3r::HubPlatform::process_group_empty_posix(g);
        if (!empty) std::this_thread::sleep_for(20ms);
    }
    REQUIRE(empty);
}

TEST_CASE("stopping a child's process group stops what it started too", "[HubQuit][HubPlatform]")
{
    // go2rtc's shape: the leader, plus a process it started (ffmpeg) in the same group.
    const long g = Slic3r::HubPlatform::spawn_child_posix({ "/bin/sh", "-c", "sleep 60 & exec sleep 60" }, "");
    REQUIRE(g > 1);
    std::this_thread::sleep_for(200ms); // let the shell start its background sleep
    REQUIRE_FALSE(Slic3r::HubPlatform::process_group_empty_posix(g));
    Slic3r::HubPlatform::terminate_child_group_posix(g, 2000);
    bool empty = false;
    for (int i = 0; i < 100 && !empty; ++i) {
        empty = Slic3r::HubPlatform::process_group_empty_posix(g);
        if (!empty) std::this_thread::sleep_for(20ms);
    }
    REQUIRE(empty);
}

#endif
