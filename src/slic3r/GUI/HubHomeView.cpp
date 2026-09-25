#include "HubHomeView.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "RemoteAccess.hpp"
#include "RemoteHub.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "Widgets/WebView.hpp"
#include "slic3r/Utils/HubHomeLogic.hpp"

#include <boost/log/trivial.hpp>

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/time.h>
#include <wx/utils.h>
#include <wx/webview.h>

#include <memory>
#include <thread>

namespace Slic3r {
namespace GUI {

namespace {

// A re-check waits this long for /hub/info. The hub answers from a request thread that can be
// slow for a moment on a busy PC; a miss is retried (HubHome::Watch), but a longer wait makes one
// less likely. The call runs on a worker thread, so the wait never blocks the window.
constexpr long HUB_PROBE_TIMEOUT_S = 6;

const char* state_name(int state)
{
    static const char* names[] = { "idle", "starting", "page", "did-not-start", "not-running", "not-responding", "page-error" };
    return state >= 0 && state < int(sizeof(names) / sizeof(names[0])) ? names[state] : "?";
}

bool hidden_instance()
{
    // A hub-managed instance that was never shown: nobody looks at it, so nothing is loaded.
    return wxGetApp().is_hub_managed() && RemoteAccess::get().hidden();
}

} // namespace

HubHomeView::HubHomeView(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize), m_timer(this)
{
    // Shared with the worker threads as a weak_ptr: a hub call that returns after the view is
    // gone is dropped on the GUI thread.
    m_life = std::make_shared<int>(0);

    m_sizer  = new wxBoxSizer(wxVERTICAL);
    m_status = new wxPanel(this, wxID_ANY);

    m_title = new wxStaticText(m_status, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
    m_title->SetFont(Label::Head_16);
    m_detail = new wxStaticText(m_status, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
    m_detail->SetFont(Label::Body_13);

    m_btn_retry = new Button(m_status, _L("Try again"));
    m_btn_retry->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    m_btn_start_page = new Button(m_status, _L("Open start page"));
    m_btn_start_page->SetStyle(ButtonStyle::Regular, ButtonType::Choice);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->Add(m_btn_retry, 0, wxRIGHT, FromDIP(ButtonProps::ChoiceButtonGap()));
    buttons->Add(m_btn_start_page, 0);

    auto* status_sizer = new wxBoxSizer(wxVERTICAL);
    status_sizer->AddStretchSpacer(1);
    status_sizer->Add(m_title, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(24));
    status_sizer->AddSpacer(FromDIP(8));
    status_sizer->Add(m_detail, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(24));
    status_sizer->AddSpacer(FromDIP(18));
    status_sizer->Add(buttons, 0, wxALIGN_CENTER_HORIZONTAL);
    status_sizer->AddStretchSpacer(2);
    m_status->SetSizer(status_sizer);

    m_sizer->Add(m_status, 1, wxEXPAND);
    SetSizer(m_sizer);

    m_btn_retry->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        // A hub that is running but not answering is only asked again: starting another one next
        // to it would just put a second hub on the next port.
        if (m_state == State::Unreachable)
            recheck();
        else
            begin(true);
    });
    m_btn_start_page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (on_open_start_page)
            on_open_start_page();
    });

    apply_colours();
    show_status(State::Idle);

    // One tick a second: notices the view coming on screen (first load, and "welcome back"
    // re-checks) and drives the periodic re-check while it stays there.
    Bind(wxEVT_TIMER, &HubHomeView::tick, this, m_timer.GetId());
    m_timer.Start(1000);
}

HubHomeView::~HubHomeView()
{
    m_timer.Stop();
    m_life.reset();
}

void HubHomeView::on_activated()
{
    poll();
}

void HubHomeView::on_deactivated()
{
    if (m_visible)
        visibility_changed(false);
}

void HubHomeView::tick(wxTimerEvent&) { poll(); }

void HubHomeView::poll()
{
    const bool visible = IsShownOnScreen() && !hidden_instance();
    if (visible != m_visible)
        visibility_changed(visible);
    if (!visible || m_busy)
        return;
    // The page is re-checked on a schedule (20 s, sooner after a miss); so are the "not running" and
    // "not responding" screens, which is how they notice a hub another window started, or one that
    // answers again, without anybody clicking.
    const bool watched = m_state == State::Loaded || m_state == State::Stopped || m_state == State::Unreachable;
    if (watched && --m_ticks_to_check <= 0)
        recheck();
}

void HubHomeView::visibility_changed(bool visible)
{
    m_visible = visible;
    if (!visible) {
        set_page_active(false);
        return;
    }
    if (m_state == State::Idle) {
        begin(true);
        return;
    }
    set_page_active(true);
    // Coming back on screen: the port or the link may have changed while nobody was looking. A
    // stopped hub is only looked for, never restarted from here - that is the Start button's job.
    if (m_state == State::Loaded || m_state == State::Stopped || m_state == State::Unreachable)
        recheck();
}

static void run_hub_call(HubHomeView* view, std::weak_ptr<int> life, bool spawn, std::string seed,
                         std::function<void(HubHomeView*, const RemoteHub::Info&)> done)
{
    std::thread([view, life, spawn, seed, done]() {
        RemoteHub::Info info = spawn ? RemoteHub::ensure_running(seed, false) : RemoteHub::query(HUB_PROBE_TIMEOUT_S);
        // Gated (RemoteAccess::post_to_app): an ensure_running() can take ~12 s and must not post
        // to an app that is being torn down meanwhile. `life` covers the view itself.
        RemoteAccess::post_to_app([view, life, info, done]() {
            if (life.expired())
                return;
            done(view, info);
        });
    }).detach();
}

void HubHomeView::set_state(State to, const std::string& reason)
{
    // Warning level on purpose: release builds log warnings and up, and these lines are what tells
    // a false "not running" from a real one after the fact. The token never appears here.
    if (to != m_state)
        BOOST_LOG_TRIVIAL(warning) << "HubHomeView: " << state_name(int(m_state)) << " -> " << state_name(int(to)) << " (" << reason << ")";
    if (to == State::Loaded) {
        m_state = State::Loaded;
        return;
    }
    show_status(to);
}

void HubHomeView::begin(bool spawn)
{
    if (m_busy)
        return;
    m_busy = true;
    set_state(State::Starting, spawn ? "start" : "look");
    // The phone switch stays as it is (false = never turn LAN access on from here); the saved
    // link only seeds a data folder whose hub has never had one, as everywhere else.
    const std::string seed = spawn ? wxGetApp().app_config->get("stream_phone_token") : std::string();
    run_hub_call(this, m_life, spawn, seed, [spawn](HubHomeView* self, const RemoteHub::Info& info) {
        self->m_busy = false;
        const auto r = HubHome::recheck(info.alive, info.port, info.token, self->m_port, self->m_token);
        if (r == HubHome::Recheck::HubDown) {
            self->set_state(spawn ? State::Failed : State::Stopped, spawn ? "no hub came up" : "no hub is running");
            return;
        }
        self->m_watch.reset();
        self->m_ticks_to_check = self->m_watch.next_check_s();
        self->load_hub(info.port, info.token);
    });
}

void HubHomeView::recheck()
{
    if (m_busy)
        return;
    m_busy = true;
    std::weak_ptr<int> life = m_life;
    std::thread([this, life]() {
        HubHome::Probe        probe;
        const RemoteHub::Info info = RemoteHub::query(HUB_PROBE_TIMEOUT_S);
        if (info.alive) {
            probe.answered = true;
            probe.port     = info.port;
            probe.token    = info.token;
        } else {
            // No answer: ask the hub's record (no network) whether it is actually gone.
            const RemoteHub::Record rec = RemoteHub::record();
            probe.presence              = rec.presence;
            probe.exit_reason           = rec.exit_reason;
        }
        RemoteAccess::post_to_app([this, life, probe]() {
            if (life.expired())
                return;
            m_busy = false;
            on_probe(probe);
        });
    }).detach();
}

void HubHomeView::on_probe(const HubHome::Probe& probe)
{
    using HubHome::Verdict;
    const Verdict v  = m_watch.judge(probe, m_port, m_token);
    m_ticks_to_check = m_watch.next_check_s();
    switch (v) {
    case Verdict::Keep:
        if (m_page_failed && m_state == State::Loaded) {
            // The hub answers at the same address, yet the page reported a load error: reload it
            // once (a hiccup while the hub was busy), then say so instead of reloading in a loop.
            if (!m_error_reload_used) {
                m_error_reload_used = true;
                BOOST_LOG_TRIVIAL(warning) << "HubHomeView: the page failed to load but the hub answers; reloading it once";
                load_hub(probe.port, probe.token);
            } else {
                set_state(State::PageError, "the hub answers but its page failed to load again");
            }
        } else if (m_state != State::Loaded) {
            // A stopped or unresponsive hub is back (restarted by another window, or it was only
            // slow): show its page again.
            BOOST_LOG_TRIVIAL(warning) << "HubHomeView: the hub answers again";
            load_hub(probe.port, probe.token);
        }
        break;
    case Verdict::Reload:
        BOOST_LOG_TRIVIAL(warning) << "HubHomeView: the hub moved (port " << m_port << " -> " << probe.port
                                   << (probe.token != m_token ? ", new link" : "") << "), reloading";
        load_hub(probe.port, probe.token);
        break;
    case Verdict::Retry:
        // Keep whatever is on screen - above all a working page - and look again soon.
        if (m_state == State::Loaded)
            BOOST_LOG_TRIVIAL(warning) << "HubHomeView: the hub did not answer (miss " << m_watch.failures() << ", "
                                       << HubHome::presence_name(probe.presence) << "); keeping the page, next look in "
                                       << m_ticks_to_check << " s";
        break;
    case Verdict::Unreachable:
        if (m_state != State::Unreachable)
            set_state(State::Unreachable, std::to_string(m_watch.failures()) + " looks in a row unanswered, " +
                                              HubHome::presence_name(probe.presence));
        break;
    case Verdict::Gone:
        if (m_state != State::Stopped || m_gone_presence != probe.presence || m_gone_reason != probe.exit_reason) {
            m_gone_presence = probe.presence;
            m_gone_reason   = probe.exit_reason;
            std::string why = HubHome::presence_name(probe.presence);
            if (probe.presence == HubHome::Presence::NoRecord)
                why += std::string(", exit reason ") + HubHome::exit_reason_name(probe.exit_reason);
            if (m_state == State::Stopped)
                BOOST_LOG_TRIVIAL(warning) << "HubHomeView: still not running (" << why << ")";
            set_state(State::Stopped, why);
        }
        break;
    }
}

void HubHomeView::load_hub(int port, const std::string& token)
{
    const std::string url = HubHome::hub_page_url(port, token, wxGetApp().dark_mode());
    if (url.empty()) {
        set_state(State::Failed, "the hub gave no usable port or link");
        return;
    }
    ensure_browser();
    if (m_browser == nullptr) {
        set_state(State::Failed, "no embedded browser");
        return;
    }
    // Set before LoadURL: on_navigating allows exactly this port and token.
    if (port != m_port || token != m_token)
        m_error_reload_used = false;
    m_port        = port;
    m_token       = token;
    m_page_failed = false;
    // The port is logged, the token never is.
    BOOST_LOG_TRIVIAL(info) << "HubHomeView: loading the hub page from port " << port;
    m_browser->LoadURL(wxString::FromUTF8(url));
    set_state(State::Loaded, "hub page on port " + std::to_string(port));
    m_status->Hide();
    m_browser->Show();
    Layout();
}

void HubHomeView::ensure_browser()
{
    if (m_browser != nullptr)
        return;
    // No "wx" script bridge: the page served by the hub process gets no channel into the app.
    m_browser = WebView::CreateWebView(this, "about:blank", "SM-Slicer", false);
    if (m_browser == nullptr)
        return;
    // A theme switch is applied live (sys_color_changed); a reload would restart every stream.
    WebView::SetReloadOnThemeChange(m_browser, false);
    m_browser->Bind(wxEVT_WEBVIEW_NAVIGATING, &HubHomeView::on_navigating, this);
    m_browser->Bind(wxEVT_WEBVIEW_NEWWINDOW, &HubHomeView::on_new_window, this);
    m_browser->Bind(wxEVT_WEBVIEW_ERROR, &HubHomeView::on_error, this);
    m_browser->Bind(wxEVT_WEBVIEW_NAVIGATED, &HubHomeView::on_navigated, this);
    m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &HubHomeView::on_script_message, this);
    m_browser->Hide();
    m_sizer->Add(m_browser, 1, wxEXPAND);
}

void HubHomeView::show_status(State state)
{
    m_state = state;
    wxString title, detail, action;
    switch (state) {
    case State::Idle: break;
    case State::Starting:
        title  = _L("Starting the phone hub...");
        detail = _L("The Home tab shows the same page your phone opens: printers, cameras and the slicers running on this PC.");
        break;
    case State::Failed:
        title  = _L("The phone hub did not start");
        detail = _L("EdgeSlicer could not start its hub helper, or the hub did not answer on any port from 13640 to 13659. "
                    "Try again, or check whether a firewall or another program is holding those ports.");
        action = _L("Try again");
        break;
    case State::Stopped:
        // Only what the hub's own record says: "closed from its tray icon" only when it was.
        if (m_gone_presence == HubHome::Presence::Crashed) {
            title  = _L("The phone hub stopped unexpectedly");
            detail = _L("Its process is no longer running. Start it again to see your printers and cameras here.");
        } else if (m_gone_reason == HubHome::ExitReason::Tray) {
            title  = _L("The phone hub was closed");
            detail = _L("It was quit from its tray icon. Start it to see your printers and cameras here.");
        } else if (m_gone_reason == HubHome::ExitReason::Idle) {
            title  = _L("The phone hub is not running");
            detail = _L("It closed by itself while phone access was off and no slicer window was open. Start it to see your printers and cameras here.");
        } else if (m_gone_reason == HubHome::ExitReason::Requested) {
            title  = _L("The phone hub is not running");
            detail = _L("It was shut down by EdgeSlicer. Start it to see your printers and cameras here.");
        } else {
            title  = _L("The phone hub is not running");
            detail = _L("Start it to see your printers and cameras here.");
        }
        action = _L("Start");
        break;
    case State::Unreachable:
        title  = _L("The phone hub is not responding");
        detail = _L("It is running but has not answered for a while. EdgeSlicer keeps checking and shows the page again as soon as it answers.");
        action = _L("Check again");
        break;
    case State::PageError:
        title  = _L("The hub page did not load");
        detail = _L("The phone hub is running, but its page could not be opened.");
        action = _L("Try again");
        break;
    case State::Loaded: return;
    }
    m_title->SetLabel(title);
    m_detail->SetLabel(detail);
    m_detail->Wrap(FromDIP(480));
    m_btn_retry->SetLabel(action);
    m_btn_retry->Show(!action.IsEmpty());
    m_btn_start_page->Show(state == State::Failed || state == State::Stopped || state == State::Unreachable ||
                           state == State::PageError);
    if (m_browser)
        m_browser->Hide();
    m_status->Show();
    m_status->Layout();
    Layout();
}

void HubHomeView::set_page_active(bool active)
{
    if (m_browser == nullptr || m_state != State::Loaded)
        return;
    // Host -> page only: the page pauses its polling while nobody sees it.
    WebView::RunScript(m_browser, active ? "if (window.__edgeActive) window.__edgeActive(true);"
                                         : "if (window.__edgeActive) window.__edgeActive(false);");
}

void HubHomeView::apply_colours()
{
    const wxColour bg     = StateColor::darkModeColorFor(wxColour("#FFFFFF"));
    const wxColour fg     = StateColor::darkModeColorFor(wxColour("#262E30"));
    const wxColour dimmed = StateColor::darkModeColorFor(wxColour("#6B6B6A"));
    SetBackgroundColour(bg);
    m_status->SetBackgroundColour(bg);
    m_title->SetForegroundColour(fg);
    m_detail->SetForegroundColour(dimmed);
    if (m_browser)
        m_browser->SetBackgroundColour(bg);
    Refresh();
}

void HubHomeView::sys_color_changed()
{
    apply_colours();
    m_btn_retry->Rescale();
    m_btn_start_page->Rescale();
    if (m_browser != nullptr && m_state == State::Loaded)
        WebView::RunScript(m_browser, wxString::FromUTF8(HubHome::theme_script(wxGetApp().dark_mode())));
}

void HubHomeView::on_navigating(wxWebViewEvent& evt)
{
    const std::string url = evt.GetURL().ToStdString(wxConvUTF8);
    if (HubHome::is_allowed_navigation(url, m_port, m_token)) {
        evt.Skip();
        return;
    }
    // Never away from the hub page: a web link goes to the system browser, anything else nowhere.
    evt.Veto();
    if (HubHome::is_external_browser_url(url))
        wxLaunchDefaultBrowser(evt.GetURL());
    else
        BOOST_LOG_TRIVIAL(info) << "HubHomeView: dropped a navigation that is neither the hub page nor a web link";
}

void HubHomeView::on_new_window(wxWebViewEvent& evt)
{
    // Only a link the user clicked, and only a web link: the system browser opens it.
    const std::string url = evt.GetURL().ToStdString(wxConvUTF8);
    if (evt.GetNavigationAction() == wxWEBVIEW_NAV_ACTION_USER && HubHome::is_external_browser_url(url) &&
        !HubHome::is_hub_page_url(url, m_port, m_token))
        wxLaunchDefaultBrowser(evt.GetURL());
}

void HubHomeView::on_error(wxWebViewEvent& evt)
{
    if (m_state != State::Loaded)
        return;
    const std::string url  = evt.GetURL().ToStdString(wxConvUTF8);
    const auto        kind = HubHome::classify_load_error(evt.GetInt() == wxWEBVIEW_NAV_ERR_USER_CANCELLED,
                                                          evt.GetInt() == wxWEBVIEW_NAV_ERR_CONNECTION, url, m_port, m_token);
    if (kind == HubHome::LoadError::Ignore)
        return; // our own vetoes, an interrupted reload, or a page this view has already left
    BOOST_LOG_TRIVIAL(warning) << "HubHomeView: the hub page failed to load ("
                               << (kind == HubHome::LoadError::Connection ? "connection" : "page") << " error " << evt.GetInt() << ")";
    m_page_failed = true;
    // Most likely the hub moved or restarted: ask it where it is now. A look already on its way
    // sees m_page_failed when it lands.
    recheck();
}

void HubHomeView::on_navigated(wxWebViewEvent& evt)
{
    // The hub page itself finished loading: whatever failed before is behind us.
    if (HubHome::is_hub_page_url(evt.GetURL().ToStdString(wxConvUTF8), m_port, m_token)) {
        m_page_failed       = false;
        m_error_reload_used = false;
    }
}

void HubHomeView::on_script_message(wxWebViewEvent& evt)
{
    // The view has no bridge, so nothing should arrive; if anything does, it goes nowhere (and in
    // particular does not bubble up to a parent that handles the app's own pages).
    (void) evt;
}

} // namespace GUI
} // namespace Slic3r
