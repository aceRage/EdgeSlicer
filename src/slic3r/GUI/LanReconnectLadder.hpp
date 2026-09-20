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
// The numbers live in this header, not on DeviceManager, for two reasons: DeviceManager.hpp needs
// wx and half the GUI, so a unit test cannot reach it, and this header is small enough that
// including it costs nothing.

namespace Slic3r {
namespace LanReconnectLadder {

constexpr long long GRACE_MS = 15000; // !is_connected() this long before the first retry
constexpr long long MIN_MS   = 15000; // first backoff step
constexpr long long MAX_MS   = 60000; // cap

// The delay to wait after `attempts` retries have already been made.
inline constexpr long long backoff_ms(int attempts)
{
    long long d = MIN_MS;
    for (int i = 1; i < attempts && d < MAX_MS; ++i) d *= 2;
    return d > MAX_MS ? MAX_MS : d;
}

} // namespace LanReconnectLadder
} // namespace Slic3r
