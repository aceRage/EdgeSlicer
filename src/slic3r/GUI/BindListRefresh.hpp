#pragma once

// When the Bambu device list re-asks the account for its bound printers, and how often.
//
// The account's bind list is fetched at sign-in and whenever a printer picker opens. A printer
// that changes its mode (LAN-only <-> cloud) or its binding (bound <-> free) while the app runs
// leaves that list, and the connection types derived from it, stale until the next fetch. Two
// things re-fetch it: the "Refresh printers" action in the printer lists (manual), and a change
// seen in a known printer's SSDP announcements (automatic). Both go through one Gate so a
// flapping printer, or a user hammering the button, cannot flood the cloud API.
//
// Wx-free and clock-free (times are the caller's milliseconds) so it is unit-tested; see
// tests/slic3rutils/bind_list_refresh_tests.cpp.

#include <algorithm>
#include <string>

namespace Slic3r {
namespace BindListRefresh {

// A printer's state as the device list sees it, after the SSDP rules (SsdpConnectType.hpp).
struct PrinterState
{
    std::string connect_type; // dev_connection_type: "lan", "cloud", ""
    std::string bind_state;   // "free", "occupied", ""
};

inline const std::string& normalized_bind(const std::string& bind)
{
    static const std::string free_state = "free";
    return bind.empty() ? free_state : bind;
}

// Whether the move from `before` to `after` is one the account's list can be stale about: the
// printer's mode changed, or it was bound or unbound. `known_before`: this printer had a state
// recorded already (the first sighting of a printer is not a change; sign-in fetches the list).
inline bool is_state_change(bool known_before, const PrinterState& before, const PrinterState& after)
{
    if (!known_before)
        return false;
    if (before.connect_type != after.connect_type)
        return true;
    return normalized_bind(before.bind_state) != normalized_bind(after.bind_state);
}

// Rate limit and debounce for the two ways of refreshing. One per DeviceManager.
class Gate
{
public:
    // A change is acted on this long after the last change (a printer's announcements flap for a
    // few seconds while it switches mode), but never later than MAX_WAIT after the first.
    static constexpr long long AUTO_DEBOUNCE_MS = 4000;
    static constexpr long long AUTO_MAX_WAIT_MS = 15000;
    // Two automatic fetches are at least this far apart.
    static constexpr long long AUTO_MIN_GAP_MS = 30000;
    // The Refresh button: at most one per this long.
    static constexpr long long MANUAL_MIN_GAP_MS = 5000;

    // A printer changed state at `now`.
    void note_change(long long now)
    {
        const long long t = stamp(now);
        if (m_pending_first == 0)
            m_pending_first = t;
        m_pending_last = t;
    }

    bool auto_pending() const { return m_pending_first != 0; }

    // Milliseconds until an automatic fetch is due; -1 when none is pending. 0 = due now.
    long long auto_delay(long long now) const
    {
        if (m_pending_first == 0)
            return -1;
        long long due = std::min(m_pending_last + AUTO_DEBOUNCE_MS, m_pending_first + AUTO_MAX_WAIT_MS);
        if (m_last_fetch != 0)
            due = std::max(due, m_last_fetch + AUTO_MIN_GAP_MS);
        return std::max(0LL, due - now);
    }

    // True once if an automatic fetch is due: the caller fetches, and the pending change is spent.
    bool take_auto(long long now)
    {
        if (auto_delay(now) != 0)
            return false;
        m_pending_first = m_pending_last = 0;
        m_last_fetch = stamp(now);
        return true;
    }

    // The Refresh button was pressed. True if it may run (and a fetch is now counted); false if it
    // came too soon after the last press. A manual fetch also spends any pending automatic one.
    bool take_manual(long long now)
    {
        const long long t = stamp(now);
        if (m_last_manual != 0 && t - m_last_manual < MANUAL_MIN_GAP_MS)
            return false;
        m_last_manual   = t;
        m_last_fetch    = t;
        m_pending_first = m_pending_last = 0;
        return true;
    }

    // Some other path fetched the list (sign-in, a picker opening): a pending automatic fetch is
    // redundant and the minimum gap restarts.
    void note_fetched(long long now)
    {
        m_last_fetch    = stamp(now);
        m_pending_first = m_pending_last = 0;
    }

private:
    static long long stamp(long long now) { return now > 0 ? now : 1; }

    long long m_pending_first { 0 };
    long long m_pending_last { 0 };
    long long m_last_fetch { 0 };
    long long m_last_manual { 0 };
};

} // namespace BindListRefresh
} // namespace Slic3r
