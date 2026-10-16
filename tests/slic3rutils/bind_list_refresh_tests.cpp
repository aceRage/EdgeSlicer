// When the Bambu device list re-asks the account for its bound printers (BindListRefresh.hpp).
//
// A printer that switches between LAN-only and cloud mode, or is bound or unbound, while the app
// runs leaves the account's bind list stale. The state-change test says when a printer's
// announcements mean that; the gate debounces the resulting fetch and rate-limits the Refresh
// button. No wx, no network.

#include <catch2/catch.hpp>

#include "slic3r/GUI/BindListRefresh.hpp"

using namespace Slic3r::BindListRefresh;

namespace {
constexpr long long S = 1000; // ms per second
const PrinterState LAN_FREE { "lan", "free" };
const PrinterState CLOUD_BOUND { "cloud", "occupied" };
const PrinterState CLOUD_FREE { "cloud", "free" };
} // namespace

TEST_CASE("The first sighting of a printer is not a state change", "[BindListRefresh]")
{
    CHECK_FALSE(is_state_change(false, PrinterState{}, LAN_FREE));
    CHECK_FALSE(is_state_change(false, PrinterState{}, CLOUD_BOUND));
}

TEST_CASE("LAN-only <-> cloud and bound <-> free are state changes", "[BindListRefresh]")
{
    CHECK(is_state_change(true, LAN_FREE, CLOUD_BOUND)); // the owner's X1C, for Bambu Connect
    CHECK(is_state_change(true, CLOUD_BOUND, LAN_FREE));
    CHECK(is_state_change(true, CLOUD_FREE, CLOUD_BOUND)); // bound to an account
    CHECK(is_state_change(true, CLOUD_BOUND, CLOUD_FREE)); // unbound
    // A LAN printer (kept "lan") whose announcement now says "occupied": the account's list may
    // not know it yet.
    CHECK(is_state_change(true, LAN_FREE, PrinterState { "lan", "occupied" }));
}

TEST_CASE("The same state again is not a change, and an empty bind state is free", "[BindListRefresh]")
{
    CHECK_FALSE(is_state_change(true, LAN_FREE, LAN_FREE));
    CHECK_FALSE(is_state_change(true, CLOUD_BOUND, CLOUD_BOUND));
    // U0 firmware: no bind state at all.
    CHECK_FALSE(is_state_change(true, PrinterState { "lan", "" }, LAN_FREE));
    CHECK_FALSE(is_state_change(true, LAN_FREE, PrinterState { "lan", "" }));
    CHECK(is_state_change(true, PrinterState { "lan", "" }, PrinterState { "lan", "occupied" }));
}

TEST_CASE("Nothing pending: no automatic fetch", "[BindListRefresh]")
{
    Gate g;
    CHECK(g.auto_delay(1000 * S) == -1);
    CHECK_FALSE(g.take_auto(1000 * S));
    CHECK_FALSE(g.auto_pending());
}

TEST_CASE("An automatic fetch waits out the debounce, then runs once", "[BindListRefresh]")
{
    Gate g;
    g.note_change(1000 * S);
    CHECK(g.auto_pending());
    CHECK(g.auto_delay(1000 * S) == Gate::AUTO_DEBOUNCE_MS);
    CHECK_FALSE(g.take_auto(1000 * S + Gate::AUTO_DEBOUNCE_MS - 1));
    CHECK(g.take_auto(1000 * S + Gate::AUTO_DEBOUNCE_MS));
    // Spent: nothing is pending any more.
    CHECK_FALSE(g.auto_pending());
    CHECK_FALSE(g.take_auto(1000 * S + 60 * S));
}

TEST_CASE("A printer that keeps changing is debounced, but not starved", "[BindListRefresh]")
{
    Gate g;
    long long t = 1000 * S;
    g.note_change(t);
    // A change every 3 s keeps pushing the fetch back...
    for (int i = 0; i < 3; ++i) {
        t += 3 * S;
        g.note_change(t);
        CHECK_FALSE(g.take_auto(t));
    }
    CHECK(g.auto_delay(t) == Gate::AUTO_DEBOUNCE_MS);
    // ...but never past AUTO_MAX_WAIT_MS after the first.
    for (int i = 0; i < 20; ++i) {
        t += 3 * S;
        g.note_change(t);
    }
    CHECK(g.auto_delay(t) == 0);
    CHECK(g.take_auto(t));
}

TEST_CASE("Automatic fetches are at least AUTO_MIN_GAP_MS apart", "[BindListRefresh]")
{
    Gate g;
    g.note_change(1000 * S);
    REQUIRE(g.take_auto(1000 * S + Gate::AUTO_DEBOUNCE_MS));
    const long long first = 1000 * S + Gate::AUTO_DEBOUNCE_MS;
    // Another change right away: debounced, but held to the gap.
    g.note_change(first + S);
    CHECK(g.auto_delay(first + S) == Gate::AUTO_MIN_GAP_MS - S);
    CHECK_FALSE(g.take_auto(first + Gate::AUTO_MIN_GAP_MS - 1));
    CHECK(g.take_auto(first + Gate::AUTO_MIN_GAP_MS));
}

TEST_CASE("The Refresh button is rate-limited, and spends a pending automatic fetch", "[BindListRefresh]")
{
    Gate g;
    CHECK(g.take_manual(1000 * S)); // the first press always runs
    CHECK_FALSE(g.take_manual(1000 * S + Gate::MANUAL_MIN_GAP_MS - 1));
    CHECK(g.take_manual(1000 * S + Gate::MANUAL_MIN_GAP_MS));

    Gate h;
    h.note_change(2000 * S);
    REQUIRE(h.auto_pending());
    CHECK(h.take_manual(2000 * S + S));
    CHECK_FALSE(h.auto_pending()); // the manual fetch already covers it
    // And an automatic one right after it waits for the gap.
    h.note_change(2000 * S + 2 * S);
    CHECK(h.auto_delay(2000 * S + 2 * S) == Gate::AUTO_MIN_GAP_MS - S);
}

TEST_CASE("A fetch from another path (sign-in, a picker opening) covers a pending one", "[BindListRefresh]")
{
    Gate g;
    g.note_change(1000 * S);
    g.note_fetched(1000 * S + S);
    CHECK_FALSE(g.auto_pending());
    CHECK_FALSE(g.take_auto(1000 * S + 60 * S));
    // A Refresh press is not held back by an automatic fetch, only by another press.
    CHECK(g.take_manual(1000 * S + 2 * S));
}

TEST_CASE("A zero clock does not read as 'never'", "[BindListRefresh]")
{
    Gate g;
    g.note_change(0);
    CHECK(g.auto_pending());
    CHECK(g.take_manual(0));
    CHECK_FALSE(g.take_manual(1));
}
