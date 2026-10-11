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

#include <string>
#include <vector>

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

    // Whether this selection's session is still being dialled at `now` (and not for longer than
    // HOLD_MAX_MS): nothing can be published to the printer yet.
    bool dialling(long long now) const
    {
        return selected_at != 0 && !session_up && !failed && now - selected_at < HOLD_MAX_MS;
    }

    // Whether a status request made at `now` is to be held back until the session is up. Counts
    // the requests it holds.
    bool hold(long long now)
    {
        if (!dialling(now)) return false;
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
    // over this selection's live session (the caller logs it), -1 otherwise. A report that lands
    // before the session is up is not that: it was already on its way over the printer's watch
    // session (UltraNet reports every LAN printer, not only the selected one) when the selection
    // replaced it.
    long long on_status(long long now)
    {
        if (selected_at == 0 || status_logged || !session_up) return -1;
        status_logged = true;
        return now - selected_at;
    }
};

// Commands published while a selection's session is still being dialled. They are kept, not
// refused, and sent in order once the session is up. A request for information (`info`: a status
// or info query) keeps only its latest copy, since a panel that polls one asks again and again
// while it waits. If the session does not come up, every held command is reported as not sent.
struct HeldCommand
{
    std::string key;     // "<block>/<command>", e.g. "print/extrusion_cali_get"
    std::string payload; // the JSON as it would have been published
    int         qos { 0 };
    int         flag { 0 };
    bool        info { false };
};

class HeldQueue
{
public:
    // Returns true when the command replaced an earlier held copy of the same request.
    bool hold(HeldCommand c)
    {
        bool replaced = false;
        if (c.info) {
            for (auto it = m_items.begin(); it != m_items.end(); ++it)
                if (it->info && it->key == c.key) {
                    m_items.erase(it);
                    replaced = true;
                    break;
                }
        }
        m_items.push_back(std::move(c));
        return replaced;
    }
    std::vector<HeldCommand> take()
    {
        std::vector<HeldCommand> out;
        out.swap(m_items);
        return out;
    }
    bool   empty() const { return m_items.empty(); }
    size_t size() const { return m_items.size(); }

private:
    std::vector<HeldCommand> m_items;
};

} // namespace FirstRequestGate
} // namespace Slic3r
