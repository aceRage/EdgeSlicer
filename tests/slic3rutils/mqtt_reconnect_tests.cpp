// The MQTT reconnect policy (MqttReconnectPolicy): how long MqttClient keeps waiting for a dropped
// session before it gives the connection up.
//
// The policy is pure - elapsed time in, verdict out - so the outage the owner reported (PC sleeps
// with a Snapmaker connected, wakes, the Device page never reconnects) can be walked through here
// with a fake clock and no broker. The bug was a single look 20 s after the loss: the network is
// still coming back at that point after a wake, and that one look was final.

#include <catch2/catch.hpp>

#include "slic3r/Utils/MqttReconnectPolicy.hpp"

using namespace Slic3r::MqttReconnectPolicy;

namespace {
constexpr long long S = 1000; // ms per second
}

TEST_CASE("MqttReconnect: the defaults are the ones the fix was designed around", "[MqttReconnect]")
{
    CHECK(CHECK_INTERVAL_MS == 20 * S);
    CHECK(GIVE_UP_AFTER_MS == 180 * S);
    CHECK(RESUME_SETTLE_MS == 5 * S);
    CHECK(RESUME_RETRY_MS == 10 * S);
    // The window must hold several looks, or it is the old single look again.
    CHECK(GIVE_UP_AFTER_MS / CHECK_INTERVAL_MS >= 3);
}

TEST_CASE("MqttReconnect: still waiting inside the window, gives up once it closes", "[MqttReconnect]")
{
    // The old behaviour would have given up here.
    CHECK(decide(20 * S, false) == Verdict::KeepWaiting);
    CHECK(decide(100 * S, false) == Verdict::KeepWaiting);
    CHECK(decide(179 * S + 999, false) == Verdict::KeepWaiting);
    // The deadline itself and anything past it.
    CHECK(decide(180 * S, false) == Verdict::GiveUp);
    CHECK(decide(181 * S, false) == Verdict::GiveUp);
    CHECK(decide(3600 * S, false) == Verdict::GiveUp);
}

TEST_CASE("MqttReconnect: a restored connection wins at any time, even past the deadline", "[MqttReconnect]")
{
    CHECK(decide(0, true) == Verdict::Restored);
    CHECK(decide(100 * S, true) == Verdict::Restored);
    CHECK(decide(181 * S, true) == Verdict::Restored);
}

TEST_CASE("MqttReconnect: a custom window is honoured", "[MqttReconnect]")
{
    CHECK(decide(20 * S, false, 30 * S) == Verdict::KeepWaiting);
    CHECK(decide(30 * S, false, 30 * S) == Verdict::GiveUp);
    // Window 0: give up at once (the old behaviour, minus the 20 s).
    CHECK(decide(0, false, 0) == Verdict::GiveUp);
}

TEST_CASE("MqttReconnect: the waits land the last look on the deadline", "[MqttReconnect]")
{
    CHECK(next_wait_ms(0) == 20 * S);
    CHECK(next_wait_ms(100 * S) == 20 * S);
    CHECK(next_wait_ms(160 * S) == 20 * S);
    // 170 s in: 10 s left, not a full interval.
    CHECK(next_wait_ms(170 * S) == 10 * S);
    CHECK(next_wait_ms(180 * S) == 0);
    CHECK(next_wait_ms(181 * S) == 0);
    // The resume loop's cadence through the same window.
    CHECK(next_wait_ms(0, RESUME_RETRY_MS) == 10 * S);
    CHECK(next_wait_ms(175 * S, RESUME_RETRY_MS) == 5 * S);
}

TEST_CASE("MqttReconnect: walking the watcher through a wake where the network takes 50 s", "[MqttReconnect]")
{
    // Loss noticed at t=1000 s (the keepalive fired after the wake).
    Window w(1000 * S);
    long long now = w.started_ms;
    int       looks = 0;
    Verdict   v     = Verdict::KeepWaiting;
    while (v == Verdict::KeepWaiting) {
        now += w.next_wait_ms(now);
        ++looks;
        const bool network_back = now - w.started_ms >= 50 * S;
        v = w.look(now, network_back);
        REQUIRE(looks < 100);
    }
    CHECK(v == Verdict::Restored);
    // Looks at 20, 40, 60 s: the third one sees it back.
    CHECK(looks == 3);
    CHECK(now - w.started_ms == 60 * S);
}

TEST_CASE("MqttReconnect: walking the watcher through an outage that never ends", "[MqttReconnect]")
{
    Window w(0);
    long long now = 0;
    int       looks = 0;
    Verdict   v     = Verdict::KeepWaiting;
    while (v == Verdict::KeepWaiting) {
        now += w.next_wait_ms(now);
        ++looks;
        v = w.look(now, false);
        REQUIRE(looks < 100);
    }
    CHECK(v == Verdict::GiveUp);
    // 180 / 20 = 9 looks, the ninth exactly on the deadline.
    CHECK(looks == 9);
    CHECK(now == 180 * S);
}

TEST_CASE("MqttReconnect: a session that came back and dropped again gets the full window", "[MqttReconnect]")
{
    Window w(0);
    CHECK(w.look(170 * S, true) == Verdict::Restored);
    // Second loss at 175 s: a window that had not been reopened would give up 5 s later.
    w.reopen(175 * S);
    CHECK(w.look(181 * S, false) == Verdict::KeepWaiting);
    CHECK(w.look(175 * S + 179 * S, false) == Verdict::KeepWaiting);
    CHECK(w.look(175 * S + 180 * S, false) == Verdict::GiveUp);
    CHECK(w.next_wait_ms(175 * S) == 20 * S);
}
