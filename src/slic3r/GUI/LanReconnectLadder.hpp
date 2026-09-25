#pragma once

// The LAN reconnect ladder: the cadence DeviceManager::lan_reconnect_tick walks when a LAN-mode
// Bambu printer's MQTT session has dropped.
//
// A LAN printer's session is the slicer's only link to it, and when it dropped nothing
// re-established it: MonitorPanel::update retried only inside `if (is_user_login())`, so with no
// Bambu cloud account a dropped session stayed dropped until the user left the Device tab and came
// back. The retry now runs off the GUI heartbeat instead, at this cadence: wait out the grace
// window, then 15 s, 30 s, 60 s, 60 s... until a push lands, which resets the ladder.
//
// What counts as "dropped" (lan_tick_step below). The first version of the tick re-dialled as soon
// as no report had landed for DISCONNECT_TIMEOUT (30 s) plus the grace, whatever the network
// plug-in said about the session. That tore down sessions that were only QUIET, and - worse - it
// was one half of the Device-tab flap: two EdgeSlicer processes dialling one printer with the same
// MQTT ClientId took the session off each other, and each one's tick, seeing the silence, took it
// back 45-50 s later (the plug-in half, the fixed ClientId, is fixed in UltraNet). So the tick now
// follows Bambu Studio's own rule for a silent printer - ask for a report (its check_pushing sends
// a "pushing" request after 20 s of silence, TIMEOUT_FOR_STRAT) - and re-dials only on a REAL drop:
//   * the plug-in reported the session lost or failed (on_local_connect with a non-Ok status), or
//     a probe could not even be published (the plug-in has no session to put it on);
//   * or the session is mute for STALE_MS although the plug-in still calls it up (a half-open
//     socket, or a plug-in that never reports a loss) - the probes have gone unanswered for 100 s.
//
// The numbers live in this header, not on DeviceManager, for two reasons: DeviceManager.hpp needs
// wx and half the GUI, so a unit test cannot reach it, and this header is small enough that
// including it costs nothing.

namespace Slic3r {
namespace LanReconnectLadder {

constexpr long long GRACE_MS = 15000; // a real drop this long before the first retry
constexpr long long MIN_MS   = 15000; // first backoff step
constexpr long long MAX_MS   = 60000; // cap

// Silence (no report) after which the tick asks the printer for one: Bambu Studio's
// TIMEOUT_FOR_STRAT, and 10 s inside DISCONNECT_TIMEOUT, so a quiet but healthy printer is answered
// before the Device tab would show it as disconnected.
constexpr long long PROBE_AFTER_MS = 20000;
// While the silence lasts, one probe per this interval (a pushall is cheap, but not free).
constexpr long long PROBE_EVERY_MS = 10000;
// A session the plug-in calls up that has not produced a report for this long (five unanswered
// probes) is treated as dropped.
constexpr long long STALE_MS = 120000;

// The delay to wait after `attempts` retries have already been made.
inline constexpr long long backoff_ms(int attempts)
{
    long long d = MIN_MS;
    for (int i = 1; i < attempts && d < MAX_MS; ++i) d *= 2;
    return d > MAX_MS ? MAX_MS : d;
}

// Per-printer bookkeeping for lan_tick_step. All times are the caller's clock, in milliseconds.
struct LinkState
{
    long long down_since { 0 }; // first tick of the current silence past PROBE_AFTER_MS; 0 = healthy
    long long probe_at { 0 };   // when the last probe went out
    long long last_try { 0 };   // when the last re-dial was made
    int       attempts { 0 };   // re-dials since the session was last healthy
};

enum class TickAction
{
    None,      // nothing to do this tick
    Probe,     // ask the printer for a report (pushall); if that cannot be published, the
               // session is gone - tell the next tick with session_up = false
    Reconnect  // re-dial: disconnect_printer, reset, connect_printer
};

// One tick of the reconnect rule for one watched LAN printer.
//   silence_ms  how long since the last report (MachineObject::last_update_time; a re-dial's
//               reset() restarts it)
//   session_up  what the network plug-in last said about the session: true after
//               on_local_connect(Ok), false after any other status, after a re-dial is started
//               (until its Ok arrives) and after a probe that could not be published
inline TickAction lan_tick_step(LinkState& s, long long now, long long silence_ms, bool session_up)
{
    if (silence_ms < PROBE_AFTER_MS) {
        // Reports are flowing. Only a session the plug-in calls up clears the ladder: right after
        // a re-dial the report clock was reset but nothing has arrived yet, and that must not
        // wipe the backoff of a printer that is still away.
        if (session_up) s = LinkState();
        return TickAction::None;
    }
    if (s.down_since == 0) s.down_since = now;

    const bool dropped = !session_up || silence_ms >= STALE_MS;
    if (!dropped) {
        // Quiet, not gone: ask for a report instead of tearing the session down.
        if (s.probe_at == 0 || now - s.probe_at >= PROBE_EVERY_MS) {
            s.probe_at = now;
            return TickAction::Probe;
        }
        return TickAction::None;
    }
    // A real drop. The grace window lets the plug-in's own re-dial (UltraNet re-dials an
    // established session by itself within seconds) win before the host steps in.
    if (now - s.down_since < GRACE_MS) return TickAction::None;
    if (s.last_try != 0 && now - s.last_try < backoff_ms(s.attempts)) return TickAction::None;
    s.last_try = now;
    ++s.attempts;
    return TickAction::Reconnect;
}

} // namespace LanReconnectLadder
} // namespace Slic3r
