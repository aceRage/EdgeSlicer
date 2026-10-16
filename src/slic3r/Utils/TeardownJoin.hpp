#pragma once

// Joining a worker thread from a GUI destructor without re-entering the event loop.
//
// EDGESLICER-A (2.4.5.4, Windows): ~MediaPlayCtrl waited for its camera worker with
// `while (!try_join) wxEventLoopBase::GetActive()->Yield();`. The destructor runs while the main
// window's tree is being torn down (app close, or the old frame after a language / theme switch),
// and Yield() dispatches everything: a pending wxTimer ran Plater's background-process update,
// which repainted the half-destroyed MainFrame's slice/print buttons and crashed. Yield() also
// runs idle processing, i.e. a nested DeletePendingObjects().
//
// The worker may really be blocked on the GUI thread (a WMP/DirectShow call marshalled to the UI
// apartment, a cross-thread SendMessage), so the destructor cannot simply block either. The rule
// here: wait in short slices and, between them, serve only that kind of traffic
// (TeardownPump::Service). Only if the worker is still not done after `widen_after` does the
// caller widen to TeardownPump::Wide - still never timers, user input, wx pending events or idle
// processing - so a worker that needs some other window message cannot hang the close forever.
//
// Plain C++ (no wx): the policy is unit-tested in tests/slic3rutils/teardown_join_tests.cpp; the
// platform pump lives with its caller.

#include <chrono>

namespace Slic3r {

enum class TeardownPump {
    Service, // only what a worker blocked on the GUI thread needs (COM / sent messages)
    Wide,    // fallback: native UI messages too; still no timers, input, pending events or idle
};

struct TeardownJoinResult
{
    bool widened = false; // the Service-only pump was not enough within widen_after
    int  rounds  = 0;     // pump calls made (0: the worker was already done)
};

// try_join(): waits one short slice for the worker; true once it has been joined.
// pump(TeardownPump): serves the GUI-thread work the worker may be waiting for.
template<class TryJoin, class Pump, class Clock = std::chrono::steady_clock>
TeardownJoinResult join_without_reentry(TryJoin &&try_join, Pump &&pump, typename Clock::duration widen_after)
{
    TeardownJoinResult res;
    const auto         start = Clock::now();
    while (!try_join()) {
        if (!res.widened && Clock::now() - start >= widen_after)
            res.widened = true;
        ++res.rounds;
        pump(res.widened ? TeardownPump::Wide : TeardownPump::Service);
    }
    return res;
}

} // namespace Slic3r
