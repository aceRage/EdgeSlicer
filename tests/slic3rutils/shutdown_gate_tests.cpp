// The two shutdown-time crashes of 2026-09-22, as rules that run without a GUI:
//
//  * c2a7d4de - a phone API request queued work on the GUI thread (RemoteAccess's run_on_main)
//    while the main window was closing; the GUI thread ran it after the Plater had been freed.
//    MainThreadGate: once closed, nothing new is queued, queued work is dropped, and the waiter
//    hears "closing" at once instead of sitting out its timeout.
//
//  * a2375b06 - the hub destroyed its io_context (a plain member) while a detached connection
//    thread (a camera WebSocket tunnel) still owned a socket on it; destroying that socket later
//    crashed in the socket service. ServerLifetime: stop() ends the connections and waits for
//    them, and the io_context lives as long as any of their threads does.
//
// The GUI thread is a plain queue here; the servers are real loopback sockets.

#include <catch2/catch.hpp>

#include "slic3r/Utils/MainThreadGate.hpp"
#include "slic3r/Utils/ServerLifetime.hpp"

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace Slic3r;
using namespace std::chrono_literals;
using tcp   = boost::asio::ip::tcp;
using Clock = std::chrono::steady_clock;

namespace {

// Stands in for wxApp::CallAfter and the GUI thread's event loop.
struct FakeLoop
{
    std::mutex                        mutex;
    std::deque<std::function<void()>> queue;

    MainThreadGate::Poster poster()
    {
        return [this](std::function<void()> f) {
            std::lock_guard<std::mutex> lock(mutex);
            queue.push_back(std::move(f));
        };
    }
    size_t size()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return queue.size();
    }
    // One pass of the event loop: run what is pending, then destroy it (as wx deletes the event).
    void run_all()
    {
        std::deque<std::function<void()>> take;
        {
            std::lock_guard<std::mutex> lock(mutex);
            take.swap(queue);
        }
        for (auto& f : take)
            f();
    }
    // The event loop went away with work still pending: destroyed without being run.
    void discard()
    {
        std::deque<std::function<void()>> take;
        {
            std::lock_guard<std::mutex> lock(mutex);
            take.swap(queue);
        }
    }
};

// A GUI thread that keeps pumping until told to stop.
struct Pump
{
    FakeLoop&         loop;
    std::atomic<bool> stop { false };
    std::thread       thread;
    explicit Pump(FakeLoop& l) : loop(l), thread([this]() {
        while (!stop) {
            loop.run_all();
            std::this_thread::sleep_for(2ms);
        }
    }) {}
    ~Pump()
    {
        stop = true;
        thread.join();
    }
};

bool wait_until(const std::function<bool()>& cond, std::chrono::milliseconds limit = 5000ms)
{
    const auto end = Clock::now() + limit;
    while (!cond()) {
        if (Clock::now() > end)
            return false;
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

long long ms_since(Clock::time_point t) { return (long long) std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count(); }

// A safety net so a failing test cannot hang the suite on a blocking read.
void recv_timeout(tcp::socket& s, int ms)
{
#ifdef _WIN32
    DWORD v = (DWORD) ms;
    ::setsockopt(s.native_handle(), SOL_SOCKET, SO_RCVTIMEO, (const char*) &v, sizeof(v));
#else
    struct timeval tv { ms / 1000, (ms % 1000) * 1000 };
    ::setsockopt(s.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

} // namespace

// ------------------------------------------------------------------------ MainThreadGate ----

TEST_CASE("MainThreadGate: an open gate runs the task and the caller sees Done", "[ShutdownGate]")
{
    MainThreadGate gate;
    FakeLoop       loop;
    Pump           pump(loop);
    bool           ran = false;
    REQUIRE(call_and_wait(gate, loop.poster(), [&ran]() { ran = true; }, 5000ms) == MainCallResult::Done);
    CHECK(ran);
}

TEST_CASE("MainThreadGate: a closed gate queues nothing and answers Closing at once", "[ShutdownGate]")
{
    MainThreadGate gate;
    FakeLoop       loop;
    gate.close();
    CHECK(gate.closed());
    bool ran = false;
    CHECK_FALSE(gate.post(loop.poster(), [&ran]() { ran = true; }));
    const auto t0 = Clock::now();
    CHECK(call_and_wait(gate, loop.poster(), [&ran]() { ran = true; }, 30000ms) == MainCallResult::Closing);
    CHECK(ms_since(t0) < 1000);
    CHECK(loop.size() == 0);
    loop.run_all();
    CHECK_FALSE(ran);
}

// The c2a7d4de sequence: a request thread queues its GUI step, the main window starts closing,
// the event loop then reaches the queued step - which must not run, because the Plater is gone.
TEST_CASE("MainThreadGate: work queued before the close is dropped and its waiter hears Closing", "[ShutdownGate]")
{
    MainThreadGate    gate;
    FakeLoop          loop;
    auto              plater_alive = std::make_shared<std::atomic<bool>>(true);
    std::atomic<bool> touched_dead_plater { false }, ran { false };
    MainCallResult    result = MainCallResult::Done;
    const auto        t0     = Clock::now();
    std::thread       request([&]() {
        result = call_and_wait(gate, loop.poster(), [&, plater_alive]() {
            ran = true;
            if (!*plater_alive) touched_dead_plater = true;
        }, 30000ms);
    });
    REQUIRE(wait_until([&]() { return loop.size() == 1; }));
    gate.close();           // MainFrame::shutdown
    *plater_alive = false;  // the frame and its Plater are destroyed
    loop.run_all();         // ...and the event loop still processes what was pending
    request.join();
    CHECK(result == MainCallResult::Closing);
    CHECK_FALSE(ran);
    CHECK_FALSE(touched_dead_plater);
    CHECK(ms_since(t0) < 5000); // not the 30 s timeout
}

TEST_CASE("MainThreadGate: a queued task the event loop never runs does not leave its waiter hanging", "[ShutdownGate]")
{
    MainThreadGate gate;
    FakeLoop       loop;
    MainCallResult result = MainCallResult::Done;
    const auto     t0     = Clock::now();

    SECTION("the gate closed and the loop ended with the task still queued") {
        std::thread request([&]() { result = call_and_wait(gate, loop.poster(), []() {}, 30000ms); });
        REQUIRE(wait_until([&]() { return loop.size() == 1; }));
        gate.close();
        request.join(); // noticed by polling the gate, before the loop is even gone
        loop.discard();
    }
    SECTION("the loop dropped the task while the gate was still open") {
        std::thread request([&]() { result = call_and_wait(gate, loop.poster(), []() {}, 30000ms); });
        REQUIRE(wait_until([&]() { return loop.size() == 1; }));
        loop.discard();
        request.join();
    }
    CHECK(result == MainCallResult::Closing);
    CHECK(ms_since(t0) < 5000);
}

TEST_CASE("MainThreadGate: a task that has started finishes and is reported Done even if the gate closes meanwhile", "[ShutdownGate]")
{
    MainThreadGate gate;
    FakeLoop       loop;
    Pump           pump(loop);
    bool           finished = false;
    const MainCallResult r  = call_and_wait(gate, loop.poster(), [&]() {
        gate.close(); // the close begins while this request's GUI step is running
        std::this_thread::sleep_for(300ms);
        finished = true;
    }, 5000ms);
    CHECK(r == MainCallResult::Done);
    CHECK(finished);
}

TEST_CASE("MainThreadGate: an open gate whose GUI thread is stuck times out", "[ShutdownGate]")
{
    MainThreadGate gate;
    FakeLoop       loop; // nobody pumps it
    bool           ran = false;
    const auto     t0  = Clock::now();
    CHECK(call_and_wait(gate, loop.poster(), [&ran]() { ran = true; }, 200ms) == MainCallResult::Timeout);
    CHECK(ms_since(t0) >= 190);
    // The GUI thread gets there after the window started closing: still not run.
    gate.close();
    loop.run_all();
    CHECK_FALSE(ran);
}

TEST_CASE("MainThreadGate: fire-and-forget posts queued before the close are skipped; reopen lets work through", "[ShutdownGate]")
{
    MainThreadGate gate;
    FakeLoop       loop;
    int            count = 0;
    for (int i = 0; i < 3; ++i)
        CHECK(gate.post(loop.poster(), [&count]() { ++count; }));
    gate.close();
    loop.run_all();
    CHECK(count == 0);
    // A language switch rebuilds the window and reopens the gate.
    gate.reopen();
    CHECK(gate.post(loop.poster(), [&count]() { ++count; }));
    loop.run_all();
    CHECK(count == 1);
}

TEST_CASE("MainThreadGate: close() waits for a post in progress on another thread", "[ShutdownGate]")
{
    MainThreadGate    gate;
    FakeLoop          loop;
    std::atomic<bool> entered { false }, pushed { false };
    bool              ran = false;
    MainThreadGate::Poster slow = [&](std::function<void()> f) {
        entered = true;
        std::this_thread::sleep_for(300ms); // wxApp::CallAfter, slowed down
        loop.poster()(std::move(f));
        pushed = true;
    };
    std::thread poster_thread([&]() { gate.post(slow, [&ran]() { ran = true; }); });
    REQUIRE(wait_until([&]() { return entered.load(); }));
    gate.close();
    // Had close() not waited, the app object CallAfter needs could be destroyed under the push.
    CHECK(pushed);
    poster_thread.join();
    loop.run_all();
    CHECK_FALSE(ran);
}

// ------------------------------------------------------------------------ ServerLifetime ----

TEST_CASE("ServerLifetime: stop() ends blocked connections and waits for every thread", "[ShutdownGate]")
{
    ServerLifetime life;
    auto           acceptor = std::make_shared<tcp::acceptor>(life.io(), tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
    const unsigned short port = acceptor->local_endpoint().port();
    std::atomic<int> ended { 0 };

    // The hub's shape: an accept loop and one detached thread per connection, each blocked in a read.
    REQUIRE(life.spawn([&life, acceptor, &ended]() {
        for (;;) {
            auto sock = std::make_shared<tcp::socket>(life.io());
            boost::system::error_code ec;
            acceptor->accept(*sock, ec);
            if (ec) break;
            recv_timeout(*sock, 10000); // safety net only: stop() must end it long before this
            if (!life.spawn([&life, sock, &ended]() {
                    ServerLifetime::Tracked tracked(life, *sock);
                    char                    buf[16];
                    boost::system::error_code e;
                    sock->read_some(boost::asio::buffer(buf), e); // the peer never sends
                    ++ended;
                }))
                break;
        }
    }));

    boost::asio::io_context client_io;
    std::vector<std::unique_ptr<tcp::socket>> clients;
    for (int i = 0; i < 3; ++i) {
        clients.push_back(std::make_unique<tcp::socket>(client_io));
        clients.back()->connect(tcp::endpoint(boost::asio::ip::address_v4::loopback(), port));
    }
    REQUIRE(wait_until([&]() { return life.running() == 4; })); // accept loop + 3 connections

    const auto t0 = Clock::now();
    boost::system::error_code ig;
    acceptor->close(ig); // what HubServer::shutdown() does first
    CHECK(life.stop(3000ms));
    CHECK(ms_since(t0) < 3000);
    CHECK(ended == 3);
    CHECK(life.running() == 0);
    CHECK(life.stopping());
    CHECK_FALSE(life.spawn([]() {}));
}

// The a2375b06 sequence: a connection thread that stop() cannot end in time still owns its
// socket when the owner - and, before the fix, the io_context - is destroyed. It destroys the
// socket afterwards; that used to be an access violation in the socket service.
TEST_CASE("ServerLifetime: a connection that outlives its server destroys its socket safely", "[ShutdownGate]")
{
    auto life     = std::make_unique<ServerLifetime>();
    auto acceptor = std::make_unique<tcp::acceptor>(life->io(), tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
    boost::asio::io_context client_io;
    tcp::socket             client(client_io);
    client.connect(acceptor->local_endpoint()); // completes from the listen backlog
    auto server_side = std::make_shared<tcp::socket>(life->io());
    acceptor->accept(*server_side);

    std::atomic<bool> release { false }, done { false };
    REQUIRE(life->spawn([server_side, &release, &done]() mutable {
        // Not tracked, and not reading: nothing stop() does can end it early (a stream stuck on
        // its camera for the 30 s read timeout, say).
        while (!release) std::this_thread::sleep_for(5ms);
        boost::system::error_code ig;
        server_side->shutdown(tcp::socket::shutdown_both, ig);
        server_side.reset(); // destroyed here, on this thread, after the owner is gone
        done = true;
    }));
    server_side.reset(); // only the connection thread holds it now

    CHECK_FALSE(life->stop(100ms)); // it is still running: the owner must not tear anything down...
    acceptor.reset();
    life.reset(); // ...but the owner itself goes away regardless (the hub process is exiting)

    release = true;
    REQUIRE(wait_until([&]() { return done.load(); }));
}

TEST_CASE("ServerLifetime: a socket tracked after stop() is shut straight away", "[ShutdownGate]")
{
    ServerLifetime life;
    tcp::acceptor  acceptor(life.io(), tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
    boost::asio::io_context client_io;
    tcp::socket             client(client_io);
    client.connect(acceptor.local_endpoint());
    tcp::socket server_side(life.io());
    acceptor.accept(server_side);
    recv_timeout(server_side, 10000);

    REQUIRE(life.stop(0ms)); // nothing running
    bool ran = false;
    CHECK_FALSE(life.spawn([&ran]() { ran = true; }));
    CHECK_FALSE(ran);

    // A connection accepted in the instant before the listener closed: its read must not block.
    const auto t0 = Clock::now();
    {
        ServerLifetime::Tracked tracked(life, server_side);
        char                      buf[16];
        boost::system::error_code ec;
        server_side.read_some(boost::asio::buffer(buf), ec);
        CHECK(ec);
    }
    CHECK(ms_since(t0) < 2000);
}
