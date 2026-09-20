#pragma once

// The MQTT reconnect policy: how long MqttClient keeps waiting for a dropped session to come back
// before it gives the connection up, and how it behaves after the PC wakes from sleep.
//
// Why a policy at all: when a session dropped, connection_lost() used to start one detached thread
// that slept 20 s and, if the client was still down, disconnected for good and told the owner
// (which, on the Device page, forgets the printer and shows "please reconnect"). A PC that went to
// sleep only notices the dead socket after it wakes, via the keepalive, and Wi-Fi / Ethernet is
// routinely still coming back 20 s later - so the one look landed inside the outage and the page
// stayed disconnected until the slicer was restarted. Paho's own automatic reconnect would have
// recovered on its own; it was the give-up that killed it.
//
// The numbers live in this header, not on MqttClient, because MQTT.hpp needs the Paho headers and
// a unit test should be able to drive the decision with a fake clock and no broker.

namespace Slic3r {
namespace MqttReconnectPolicy {

// The lost-session watcher looks at the connection this often ...
constexpr long long CHECK_INTERVAL_MS = 20 * 1000;
// ... and stops waiting for it once this long has passed without a restore.
constexpr long long GIVE_UP_AFTER_MS = 180 * 1000;
// After a system resume, wait this long before the first connect attempt: the network stack is
// never up the instant the wake event arrives.
constexpr long long RESUME_SETTLE_MS = 5 * 1000;
// Between hand-driven connect attempts after a resume (each attempt itself waits up to the
// client's connect timeout, so the effective cadence is a little longer).
constexpr long long RESUME_RETRY_MS = 10 * 1000;

enum class Verdict {
    Restored,    // the session is back; stop watching
    KeepWaiting, // still down, still inside the window
    GiveUp       // still down and the window has closed
};

// The decision `elapsed_ms` after the loss was noticed.
inline constexpr Verdict decide(long long elapsed_ms, bool connected, long long give_up_after_ms = GIVE_UP_AFTER_MS)
{
    if (connected)
        return Verdict::Restored;
    return elapsed_ms >= give_up_after_ms ? Verdict::GiveUp : Verdict::KeepWaiting;
}

// How long to sleep before the next look, so the last look lands on the deadline rather than a
// whole interval past it. Zero once the deadline is reached (decide() then says GiveUp).
inline constexpr long long next_wait_ms(long long elapsed_ms,
                                        long long interval_ms      = CHECK_INTERVAL_MS,
                                        long long give_up_after_ms = GIVE_UP_AFTER_MS)
{
    const long long remaining = give_up_after_ms - elapsed_ms;
    if (remaining <= 0)
        return 0;
    return remaining < interval_ms ? remaining : interval_ms;
}

// One outage: the window opened when the loss was noticed. Restored / GiveUp both end it; the next
// loss opens a fresh window, so a session that came back and dropped again gets the full time.
struct Window
{
    long long started_ms       = 0;
    long long interval_ms      = CHECK_INTERVAL_MS;
    long long give_up_after_ms = GIVE_UP_AFTER_MS;

    constexpr Window() = default;
    constexpr Window(long long started, long long interval = CHECK_INTERVAL_MS, long long give_up_after = GIVE_UP_AFTER_MS)
        : started_ms(started), interval_ms(interval), give_up_after_ms(give_up_after)
    {}

    constexpr long long elapsed_ms(long long now_ms) const { return now_ms - started_ms; }
    constexpr Verdict   look(long long now_ms, bool connected) const
    {
        return decide(elapsed_ms(now_ms), connected, give_up_after_ms);
    }
    constexpr long long next_wait_ms(long long now_ms) const
    {
        return MqttReconnectPolicy::next_wait_ms(elapsed_ms(now_ms), interval_ms, give_up_after_ms);
    }
    // A restore closes the window; the next loss reopens it from that moment.
    constexpr void reopen(long long now_ms) { started_ms = now_ms; }
};

} // namespace MqttReconnectPolicy
} // namespace Slic3r
