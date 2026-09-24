#ifndef slic3r_GUI_HubHomeView_hpp_
#define slic3r_GUI_HubHomeView_hpp_

#include <functional>
#include <memory>
#include <string>

#include <wx/panel.h>
#include <wx/timer.h>

#include "slic3r/Utils/HubHomeLogic.hpp"

class wxWebView;
class wxWebViewEvent;
class wxStaticText;
class wxBoxSizer;
class Button;

namespace Slic3r {
namespace GUI {

// The Home tab's main view: the phone hub page (stream_center.html, served by the separate --hub
// process at http://127.0.0.1:<port>/r/<token>/) in an embedded browser.
//
// * Contained: the view is created WITHOUT the app's "wx" script bridge, so the page cannot post
//   anything to the slicer. Navigation is limited to the hub page it loaded; http(s) links anywhere
//   else open in the system browser, everything else is dropped.
// * Lazy: nothing is created or loaded until the view is actually on screen (never in a hidden,
//   hub-managed instance, whose window is never shown).
// * The port and the link token come only from RemoteHub::ensure_running()/query(), i.e. the hub's
//   own /hub/info behind the hub secret; the address is re-checked when the view comes back on
//   screen, every 20 s while it is visible and after a load error, so a moved port (phone on/off
//   rebinds, fallback ports), a restarted hub or a "New link" are followed.
// * A re-check that gets no answer never takes a working page down by itself (HubHome::Watch):
//   the page stays until the hub's record says it quit or crashed (seen twice) or several probes
//   in a row went unanswered, and the status screen then says which of those it was.
// * Theme: ?theme=dark|light on load and window.__edgeTheme() on a theme switch; the view is
//   excluded from WebView::RecreateAll()'s reload so the camera streams keep running.
class HubHomeView : public wxPanel
{
public:
    explicit HubHomeView(wxWindow* parent);
    ~HubHomeView() override;

    // The Home tab (with this view in front) was selected / left. Only a hint for a faster reaction:
    // the view also notices by itself when it comes on screen or goes off it.
    void on_activated();
    void on_deactivated();
    void sys_color_changed();

    // "Open start page" on the status screens; HomePanel wires it.
    std::function<void()> on_open_start_page;

private:
    enum class State {
        Idle,       // never been on screen yet
        Starting,   // a thread is asking the hub (and spawning it on the first load / Start)
        Loaded,     // the hub page is (being) shown
        Failed,     // the hub did not come up
        Stopped,    // a re-check found the hub gone: offer Start, do not respawn it behind the user's back
        Unreachable,// the hub's process is there but several re-checks in a row got no answer
        PageError   // the hub answers but its page did not load
    };

    void tick(wxTimerEvent&);
    void poll();
    void visibility_changed(bool visible);
    void begin(bool spawn);                       // off-thread ensure_running (spawn) or query
    void recheck();                               // off-thread query, compare with what is loaded
    void on_probe(const HubHome::Probe& probe);   // GUI thread: what a re-check found
    void set_state(State to, const std::string& reason); // logs every transition with its reason
    void load_hub(int port, const std::string& token);
    void show_status(State state);
    void ensure_browser();
    void set_page_active(bool active);
    void apply_colours();

    void on_navigating(wxWebViewEvent& evt);
    void on_new_window(wxWebViewEvent& evt);
    void on_error(wxWebViewEvent& evt);
    void on_navigated(wxWebViewEvent& evt);
    void on_script_message(wxWebViewEvent& evt);

    wxWebView*    m_browser { nullptr };
    wxPanel*      m_status { nullptr };
    wxStaticText* m_title { nullptr };
    wxStaticText* m_detail { nullptr };
    Button*       m_btn_retry { nullptr };
    Button*       m_btn_start_page { nullptr };
    wxBoxSizer*   m_sizer { nullptr };
    wxTimer       m_timer;

    State       m_state { State::Idle };
    bool        m_visible { false };   // on screen as of the last tick
    bool        m_busy { false };      // a hub call is in flight
    int         m_ticks_to_check { HubHome::Watch::RECHECK_S };
    HubHome::Watch      m_watch;       // consecutive unanswered re-checks
    HubHome::Presence   m_gone_presence { HubHome::Presence::Unknown };      // why Stopped: what the record said
    HubHome::ExitReason m_gone_reason { HubHome::ExitReason::Unknown };
    bool        m_page_failed { false };   // the loaded page reported a main-frame load error
    bool        m_error_reload_used { false }; // one automatic reload after a load error, then the error screen
    int         m_port { 0 };          // what the loaded page was built from
    std::string m_token;
    std::shared_ptr<int> m_life;
};

} // namespace GUI
} // namespace Slic3r

#endif
