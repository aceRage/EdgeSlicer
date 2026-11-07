#pragma once

// Tells a Bambu LAN printer that refuses its saved access code from one that is simply off.
//
// A LAN printer whose access code was changed on its screen keeps refusing the slicer's MQTT
// login. Before this, the LAN reconnect ladder (LanReconnectLadder.hpp) redialled it every 60 s
// forever, every publish failed with ret=-1, and nothing told the user the code was wrong: the
// only way out was to log the printer out on the Device page and connect it again.
//
// The network plug-in reports a LAN dial through on_local_connect only as up / failed / lost with
// a free-text message; neither the Bambu plug-in nor UltraNet hands over a reliable "bad password"
// code (paho's MQTT 3.1.1 CONNACK refusal comes back as a plain failure). What does tell the two
// apart is timing:
//
//   * a printer that refuses the login answers: the TLS handshake and the CONNACK come back
//     within about a second of the dial (a good dial is up in 0.6-1.7 s), or the broker accepts
//     the session and closes it again a few seconds later;
//   * a printer that is off, asleep or gone answers nothing: the dial runs into the plug-in's
//     connect timeout (8 s for UltraNet's LAN session) or a socket error ("timed out", "no route",
//     "unreachable").
//
// So a dial counts as REFUSED when the plug-in reports it failed within QUICK_REFUSAL_MS of the
// dial with no socket-error wording, when its session dropped within SHORT_SESSION_MS of coming
// up, or when the message carries the MQTT "not authorized" code outright (is_auth_refusal).
// REFUSALS_FOR_NOTICE refused dials in a row with the same code, spread over at least MIN_RUN_MS
// (a printer still booting may turn a dial or two away before its broker is up), raise the notice
// once; a slow failure (unreachable) breaks the run and never counts; a session that stays up for
// SHORT_SESSION_MS proves the code and clears the notice. A new code starts afresh.
//
// No wx, no clock, no printer: the caller passes steady-clock milliseconds, the tests drive it.
// One instance per printer (DeviceManager keys them by serial), GUI thread only.

#include <cstddef>
#include <functional>
#include <string>

namespace Slic3r {
namespace LanAccessCode {

// Refused dials in a row (same code) before the user is told.
static constexpr int       REFUSALS_FOR_NOTICE = 3;
// ...and the run must have lasted this long, first refusal to last.
static constexpr long long MIN_RUN_MS          = 90000;
// A dial that fails sooner than this was answered by the printer.
static constexpr long long QUICK_REFUSAL_MS    = 4000;
// A session that drops sooner than this after coming up was closed on us, not lost.
static constexpr long long SHORT_SESSION_MS    = 15000;

inline std::string lower_ascii(std::string s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

// The message names the MQTT "not authorized" refusal: the Bambu plug-in's bare "5" (CONNACK 5),
// or an "rc=<n>" with 5 (3.1.1 not authorized), 134 (5.0 bad user name or password) or 135
// (5.0 not authorized).
inline bool is_auth_refusal(const std::string& msg)
{
    if (msg == "5")
        return true;
    const std::string m = lower_ascii(msg);
    for (const char* code : {"rc=5", "rc=134", "rc=135"}) {
        const std::string c(code);
        const size_t      at = m.find(c);
        if (at != std::string::npos) {
            const size_t end = at + c.size();
            if (end == m.size() || m[end] < '0' || m[end] > '9')
                return true;
        }
    }
    return m.find("not authori") != std::string::npos || m.find("bad user name or password") != std::string::npos;
}

// The message reads like the printer did not answer at all (a socket error, not a refusal).
inline bool is_unreachable_message(const std::string& msg)
{
    const std::string m = lower_ascii(msg);
    for (const char* w : {"timed out", "timeout", "unreachable", "no route", "host is down", "network is down"})
        if (m.find(w) != std::string::npos)
            return true;
    return false;
}

// A fingerprint of the code a dial used, so "once per code" holds without keeping the code.
inline size_t code_fingerprint(const std::string& code) { return std::hash<std::string>{}(code); }

enum class Verdict
{
    None,   // nothing for the user
    Notify, // show the "printer refused the access code" notice (once per code)
    Clear,  // the code works again: close the notice
};

// How the plug-in's report of a dial that ended was classified (for the log and the tests).
enum class Outcome
{
    Ignored,     // no dial was open (a duplicate report)
    Refused,     // answered and refused: counts towards the notice
    Unreachable, // nobody answered: breaks the run, never counts
    Dropped,     // a session that had been up for a while dropped: the code was fine
};

class Watch
{
public:
    // The slicer started a LAN dial with this code.
    void dial_started(long long now_ms, size_t code_fp)
    {
        if (!m_have_code || code_fp != m_code_fp) {
            m_have_code = true;
            m_code_fp   = code_fp;
            m_refusals  = 0;
            m_shown     = false;
        }
        m_dial_open = true;
        m_dial_at   = now_ms;
        m_up_at     = -1;
    }

    // The plug-in reported the session up. UltraNet re-dials a dropped session on its own and
    // reports the result as a fresh "up", so an up with no open dial opens one.
    void session_up(long long now_ms)
    {
        if (!m_dial_open) {
            m_dial_open = true;
            m_dial_at   = now_ms;
        }
        m_up_at = now_ms;
    }

    // The plug-in reported the dial failed or the session lost. `msg` is its free-text reason.
    Verdict dial_ended(long long now_ms, const std::string& msg)
    {
        m_last = classify(now_ms, msg);
        m_dial_open = false;
        m_up_at     = -1;
        switch (m_last) {
        case Outcome::Ignored: return Verdict::None;
        case Outcome::Unreachable: m_refusals = 0; return Verdict::None;
        case Outcome::Dropped: return succeeded();
        case Outcome::Refused:
            if (++m_refusals == 1)
                m_first_refusal_at = now_ms;
            if (m_refusals >= REFUSALS_FOR_NOTICE && now_ms - m_first_refusal_at >= MIN_RUN_MS && !m_shown) {
                m_shown = true;
                return Verdict::Notify;
            }
            return Verdict::None;
        }
        return Verdict::None;
    }

    // Once a second: a session that has stayed up for SHORT_SESSION_MS proves the code.
    Verdict tick(long long now_ms)
    {
        if (m_dial_open && m_up_at >= 0 && now_ms - m_up_at >= SHORT_SESSION_MS) {
            m_dial_open = false;
            m_up_at     = -1;
            return succeeded();
        }
        return Verdict::None;
    }

    // The user saved a new code: forget the run and the notice (the next dial_started carries it).
    void forget()
    {
        m_have_code = false;
        m_refusals  = 0;
        m_shown     = false;
        m_dial_open = false;
        m_up_at     = -1;
    }

    int     refusals() const { return m_refusals; }
    bool    notice_shown() const { return m_shown; }
    // The last dial with this code was refused (the Send dialog offers to re-enter it).
    bool    last_dial_refused() const { return m_refusals > 0; }
    Outcome last_outcome() const { return m_last; }

private:
    Outcome classify(long long now_ms, const std::string& msg) const
    {
        if (!m_dial_open)
            return Outcome::Ignored;
        if (is_auth_refusal(msg))
            return Outcome::Refused;
        if (m_up_at >= 0)
            return now_ms - m_up_at < SHORT_SESSION_MS ? Outcome::Refused : Outcome::Dropped;
        if (is_unreachable_message(msg))
            return Outcome::Unreachable;
        return now_ms - m_dial_at < QUICK_REFUSAL_MS ? Outcome::Refused : Outcome::Unreachable;
    }

    Verdict succeeded()
    {
        m_refusals = 0;
        if (m_shown) {
            m_shown = false;
            return Verdict::Clear;
        }
        return Verdict::None;
    }

    bool      m_have_code{false};
    size_t    m_code_fp{0};
    int       m_refusals{0};
    long long m_first_refusal_at{0};
    bool      m_shown{false};
    bool      m_dial_open{false};
    long long m_dial_at{0};
    long long m_up_at{-1};
    Outcome   m_last{Outcome::Ignored};
};

// Whether the reconnect ladder should stop redialling this printer: its code has been refused often
// enough to raise the notice. Another dial with the same code only repeats the refusal, and kept the
// Device page on "Connecting..." for ever. The user's next step dials again: a saved code
// (DeviceManager::lan_access_code_changed), OK on the unchanged code, or picking the printer again.
// The hidden hub-managed instance keeps redialling: nobody there can re-enter the code, and the
// visible slicer may already have saved a new one that its next dial reads.
inline bool redial_paused(const Watch& w, bool hub_managed) { return !hub_managed && w.notice_shown(); }

} // namespace LanAccessCode
} // namespace Slic3r
