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

// One printer's selection: the watch plus the held commands, and the rules that tie them together.
// MachineObject forwards its events here, so the rules (and their orderings) can be unit-tested.
//
//  * The session coming up hands back every held command to send, whichever event reports it
//    first, and only once. (A first version flushed them from the LAN connect handler AFTER the
//    handler's own pushall, and that pushall's publish took the still-full queue for an expired
//    hold and dropped it as "did not come up in time".)
//  * A session that fails, a hold that runs out and a new selection hand back what was held as
//    not sent, each with its own reason.
//  * held() counts every request held for the selection: status requests (pushall/get_version,
//    which the connected callback asks for again) and held commands alike.
class SelectionGate
{
public:
    struct Dropped
    {
        std::vector<HeldCommand> commands;
        const char*              why { "" };
        bool                     send { false }; // settle() only: send these, do not report them
    };

    // A selection (or a re-dial) starts. Returns what the previous selection still held.
    Dropped select(long long now, bool session_already_up)
    {
        Dropped d = drop("a new selection replaced the session they were waiting for");
        m_watch.on_selected(now, session_already_up);
        m_held    = 0;
        m_retried = false;
        return d;
    }

    // A LAN dial starts (MachineObject::connect - every dial goes through it, whatever started it:
    // a Device-tab selection, the printer restored from the last session when the Device tab
    // opens, the send dialog, a re-dial). Arms the gate unless this selection is already dialling,
    // so a dial that no selection announced holds its early requests too.
    Dropped dial_started(long long now)
    {
        if (m_watch.dialling(now)) return {};
        return select(now, false);
    }

    // The plug-in reported this selection's dial failed. The first time within the hold, returns
    // true: keep holding, the caller dials once more (a printer's broker can refuse a connection
    // made the moment its previous one - the watch session the selection just closed - is still
    // being torn down). Otherwise false: the caller reports the session down.
    bool dial_failed(long long now)
    {
        if (!m_watch.dialling(now) || m_retried) return false;
        m_retried = true;
        return true;
    }

    bool dialling(long long now) const { return m_watch.dialling(now); }

    // A pushall / get_version while dialling: not kept (the connected callback asks for it).
    bool hold_status(long long now)
    {
        if (!m_watch.hold(now)) return false;
        ++m_held;
        return true;
    }

    // Any other command while dialling: kept until the session is up. `replaced`: an earlier held
    // copy of the same request was dropped for it.
    bool hold_command(long long now, HeldCommand c, bool* replaced = nullptr)
    {
        if (!m_watch.dialling(now)) return false;
        const bool r = m_queue.hold(std::move(c));
        if (replaced) *replaced = r;
        if (!r) ++m_held; // a newer copy of a request already counted is not another request
        return true;
    }

    struct Up
    {
        long long                ms { -1 }; // ms since selection, the first time only (else -1)
        int                      held { 0 }; // requests held for it until now
        std::vector<HeldCommand> send;       // to publish now, in order
    };
    // The plug-in reported this selection's session up.
    Up session_up(long long now)
    {
        Up u;
        u.held = m_held;
        u.ms   = m_watch.on_session_up(now);
        u.send = m_queue.take();
        return u;
    }

    // The plug-in reported the dial failed or the session lost.
    Dropped session_down()
    {
        m_watch.on_session_down();
        return drop("the session did not come up");
    }

    // Before anything is published once the selection is no longer dialling: a hold that ran out
    // without the plug-in reporting either way. (A session that is up was flushed by session_up.)
    Dropped settle(long long now)
    {
        if (m_queue.empty() || m_watch.dialling(now)) return {};
        if (m_watch.session_up) {
            // Not expected - session_up flushed the queue - but never drop what can be sent.
            Dropped d;
            d.commands = m_queue.take();
            d.send     = true;
            return d;
        }
        return drop("the session did not come up within 15 s");
    }

    long long on_status(long long now) { return m_watch.on_status(now); }
    const ConnectWatch& watch() const { return m_watch; }
    int    held() const { return m_held; }
    size_t queued() const { return m_queue.size(); }

private:
    Dropped drop(const char* why)
    {
        Dropped d;
        d.commands = m_queue.take();
        d.why      = why;
        return d;
    }

    ConnectWatch m_watch;
    HeldQueue    m_queue;
    int          m_held { 0 };
    bool         m_retried { false }; // this selection's one re-dial after a refused dial is used
};

// Which session events reach a selection: the printer's own LAN session for a LAN-mode printer,
// the account's cloud session for a cloud printer. A cloud session coming up while a LAN printer
// is selected (the two arrive in either order) says nothing about that printer.
inline bool event_applies(bool selected_is_lan, bool event_is_lan) { return selected_is_lan == event_is_lan; }

} // namespace FirstRequestGate
} // namespace Slic3r
