#ifndef slic3r_DevicePageLoad_hpp_
#define slic3r_DevicePageLoad_hpp_

#include <cstdint>
#include <string>

#include "WebLoadRetry.hpp"

// When the Device tab's web view (PrinterWebView) navigates. Kept free of wx so it can be tested
// (tests/slic3rutils/device_page_load_tests.cpp).
//
// 1. Startup. The view used to load the Snapmaker Device page (flutter_web, path=2) from its
//    constructor. WebView2 brings the page up about eight seconds into startup, and the page
//    subscribes to its page-state and foreground events with a 3 s timeout. That is the moment
//    the GUI thread runs post_init, the project reset and the config-wizard step back to back in
//    one pass of wx's pending-event queue (3 to 11 s in the owner's logs, 2026-10-10/11). Those
//    two requests reach SSWCP only after that pass, so the page logs two "request timeout 3s"
//    at every start. Now every load asked for before the app says startup is over is held, the
//    last one wins, and it is loaded once when startup is over or the tab is opened.
//
// 2. Load once. Several places ask for the Device page: the printer preset row
//    (Sidebar::update_all_preset_comboboxes), a connect from another page (sw_mqtt_set_engine
//    with need_reload) and a phone connect (RemoteSnapmaker). One connect could ask twice, and a
//    preset switch reloaded the page even when the view already showed it, so the Flutter app
//    started over. A request is now one of
//      - Show:   "the tab should show this page": nothing to do when the view already shows our
//                own page at this URL, or is loading it;
//      - Reload: "load it again": skipped only when the very same URL is loading right now (a
//                second request for one connect).
namespace Slic3r {
namespace DevicePageLoad {

enum class Kind { Show, Reload };

enum class Action {
    Load,        // navigate now, then call started()
    Hold,        // startup is not over: kept, loaded by release()
    SkipLoading, // the same URL is loading right now
    SkipShowing  // Show for our own page, which the view already shows at this URL
};

inline const char* to_string(Action a)
{
    switch (a) {
    case Action::Load: return "load";
    case Action::Hold: return "hold";
    case Action::SkipLoading: return "skip (already loading)";
    case Action::SkipShowing: return "skip (already showing)";
    }
    return "?";
}

inline const char* to_string(Kind k) { return k == Kind::Show ? "show" : "reload"; }

// The Snapmaker Flutter pages this slicer serves itself; their state lives in the page, so loading
// one again starts it over. Anybody else's page (a print host's web UI) keeps reloading on request.
inline bool own_page(const std::string& url) { return url.find("/web/flutter_web/") != std::string::npos; }

struct Request
{
    std::string url;
    std::string apikey;
};

class Gate
{
public:
    // A loading state older than this no longer blocks a second request (a load that never
    // reported back must not wedge the tab).
    static constexpr int64_t LOADING_WINDOW_MS = 10000;

    explicit Gate(bool released) : m_released(released) {}

    bool released() const { return m_released; }
    bool holding() const { return !m_released && m_has_held; }
    const std::string& held_url() const { return m_held.url; }

    Action request(const std::string& url, const std::string& apikey, Kind kind, int64_t now_ms)
    {
        if (!m_released) {
            m_held     = Request{url, apikey};
            m_has_held = true;
            return Action::Hold;
        }
        if (url == m_target && m_state == State::Loading && now_ms - m_started_ms < LOADING_WINDOW_MS)
            return Action::SkipLoading;
        if (kind == Kind::Show && own_page(url) && url == m_target && m_state == State::Loaded)
            return Action::SkipShowing;
        return Action::Load;
    }

    // A navigation to `url` has been started (by request(), a retry or anything else).
    void started(const std::string& url, int64_t now_ms)
    {
        m_target     = url;
        m_state      = State::Loading;
        m_started_ms = now_ms;
    }

    // The view reports a finished navigation. A report for another page (one this target
    // replaced) changes nothing; the query is ignored as in WebLoadRetry (the page server may add
    // to it on the way).
    void finished(const std::string& url, bool ok)
    {
        if (m_target.empty() || WebLoadRetry::page_of(url) != WebLoadRetry::page_of(m_target))
            return;
        if (ok)
            m_state = State::Loaded;
        else if (m_state == State::Loading)
            m_state = State::Failed;
    }

    // Startup is over (or the tab was opened). Returns true, with the request to load now, when
    // one was held. Later calls return false.
    bool release(Request& out)
    {
        if (m_released)
            return false;
        m_released = true;
        if (!m_has_held)
            return false;
        out        = m_held;
        m_held     = Request{};
        m_has_held = false;
        return true;
    }

private:
    enum class State { None, Loading, Loaded, Failed };

    bool        m_released { false };
    bool        m_has_held { false };
    Request     m_held;
    std::string m_target;
    State       m_state { State::None };
    int64_t     m_started_ms { 0 };
};

} // namespace DevicePageLoad
} // namespace Slic3r

#endif // slic3r_DevicePageLoad_hpp_
