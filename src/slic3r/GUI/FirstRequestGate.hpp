#pragma once

// The first request after a Bambu printer is selected.
//
// Selecting a printer starts its session; it does not finish it. A LAN printer is dialled
// (MachineObject::connect -> the network plug-in's connect_printer, which returns at once and
// reports back through on_local_connect), and a cloud printer's session is opened by the plug-in
// on the first set_user_selected_machine when none is up yet (it reports through
// on_server_connected). Meanwhile the code that runs right after the selection asks the printer
// for its full status: MonitorPanel::on_select_printer -> update_all -> check_pushing ->
// keep_alive sees m_push_count == 0 (reset() just cleared it) and sends a pushall, synchronously,
// a few milliseconds into a TLS dial that takes hundreds. The plug-in has no session to publish on
// and refuses (-1), which logged
//     send_message_to_printer error ... ret=-1 ... "pushall"   /   publish_json ... code: -1
// on every first selection - while nothing was actually wrong, because the connected callback
// asks for the same report again a moment later (GUI_App's on_local_connect(Ok) handler sends
// pushall + get_version; for the cloud the plug-in replays a pushall to every subscribed printer
// on connect, and on_server_connected re-selects the printer).
//
// So an early status request is held back, not sent: the connected callback is the one that
// asks. Held requests are not queued and replayed - the callback already sends exactly the
// request that was held, so replaying would only send it twice. A hold lasts at most
// HOLD_MAX_MS: a session that has not come up by then is not "connecting" any more, and requests
// go out (and fail, and are logged) as before.
//
// ConnectWatch also produces the one-line-per-connect log the Device tab lacked: when the session
// came up, and when the first status report landed, each in milliseconds from the selection.
//
// Kept in this small header, apart from DeviceManager (which needs wx and half the GUI), so the
// rule can be unit-tested; see tests/slic3rutils/first_request_gate_tests.cpp.

namespace Slic3r {
namespace FirstRequestGate {

// Longest a status request is held for a session that has not come up. UltraNet's LAN dial gives
// up after 8 s (connect timeout) and reports the failure; its cloud dial after 10 s.
constexpr long long HOLD_MAX_MS = 15000;

// Per-printer bookkeeping. Times are the caller's clock, in milliseconds; 0 means "never".
struct ConnectWatch
{
    long long selected_at { 0 };   // when the current selection / dial started; 0 = not watching
    bool      session_up { false }; // the plug-in reported this selection's session up
    bool      failed { false };     // ... or reported it failed / lost
    bool      up_logged { false };
    bool      status_logged { false };
    int       held { 0 };           // status requests held back since the selection

    // A selection (or a re-dial) starts. `already_up`: the session it needs is up already (a cloud
    // printer while the cloud session is connected) - nothing to wait for then.
    void on_selected(long long now, bool already_up)
    {
        selected_at   = now > 0 ? now : 1;
        session_up    = already_up;
        failed        = false;
        up_logged     = false;
        status_logged = false;
        held          = 0;
    }

    // Whether a status request made at `now` is to be held back until the session is up. Counts
    // the requests it holds.
    bool hold(long long now)
    {
        if (selected_at == 0 || session_up || failed) return false;
        if (now - selected_at >= HOLD_MAX_MS) return false;
        ++held;
        return true;
    }

    // The plug-in reported the session up. Returns the milliseconds since the selection the first
    // time this happens for the current selection (the caller logs it), -1 otherwise.
    long long on_session_up(long long now)
    {
        session_up = true;
        failed     = false;
        if (selected_at == 0 || up_logged) return -1;
        up_logged = true;
        return now - selected_at;
    }

    // The plug-in reported the dial failed or the session lost: stop holding, so requests fail
    // (and are logged) the way they always have.
    void on_session_down() { session_up = false; failed = true; }

    // A status report arrived. Returns the milliseconds since the selection for the first report
    // after it (the caller logs it), -1 otherwise.
    long long on_status(long long now)
    {
        if (selected_at == 0 || status_logged) return -1;
        status_logged = true;
        return now - selected_at;
    }
};

} // namespace FirstRequestGate
} // namespace Slic3r
