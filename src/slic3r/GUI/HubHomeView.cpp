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

// Re-check the hub every this many one-second ticks while the page is on screen.
constexpr int RECHECK_TICKS = 20;
// A load error within this long of the previous error-triggered check shows the error screen
// instead of asking the hub again (no reload loop against a hub that answers but cannot serve).
constexpr long long ERROR_RECHECK_MIN_MS = 3000;

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

    m_btn_retry->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { begin(true); });
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
    if (m_state == State::Loaded && ++m_ticks_since_check >= RECHECK_TICKS) {
        m_ticks_since_check = 0;
        recheck();
    }
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
    if (m_state == State::Loaded || m_state == State::Stopped) {
        m_ticks_since_check = 0;
        recheck();
    }
}

static void run_hub_call(HubHomeView* view, std::weak_ptr<int> life, bool spawn, std::string seed,
                         std::function<void(HubHomeView*, const RemoteHub::Info&)> done)
{
    std::thread([view, life, spawn, seed, done]() {
        RemoteHub::Info info = spawn ? RemoteHub::ensure_running(seed, false) : RemoteHub::query();
        wxGetApp().CallAfter([view, life, info, done]() {
            if (life.expired())
                return;
            done(view, info);
        });
    }).detach();
}

void HubHomeView::begin(bool spawn)
{
    if (m_busy)
        return;
    m_busy = true;
    show_status(State::Starting);
    // The phone switch stays as it is (false = never turn LAN access on from here); the saved
    // link only seeds a data folder whose hub has never had one, as everywhere else.
    const std::string seed = spawn ? wxGetApp().app_config->get("stream_phone_token") : std::string();
    run_hub_call(this, m_life, spawn, seed, [spawn](HubHomeView* self, const RemoteHub::Info& info) {
        self->m_busy = false;
        const auto r = HubHome::recheck(info.alive, info.port, info.token, self->m_port, self->m_token);
        if (r == HubHome::Recheck::HubDown) {
            BOOST_LOG_TRIVIAL(warning) << "HubHomeView: no hub " << (spawn ? "came up" : "is running");
            self->show_status(spawn ? State::Failed : State::Stopped);
            return;
        }
        self->load_hub(info.port, info.token);
    });
}

void HubHomeView::recheck()
{
    if (m_busy)
        return;
    m_busy = true;
    const bool from_error = m_state == State::PageError;
    run_hub_call(this, m_life, false, std::string(), [from_error](HubHomeView* self, const RemoteHub::Info& info) {
        self->m_busy = false;
        const auto r = HubHome::recheck(info.alive, info.port, info.token, self->m_port, self->m_token);
        switch (r) {
        case HubHome::Recheck::HubDown:
            // The hub was quit (tray) or died. Say so and offer Start; do not respawn it silently.
            if (self->m_state != State::Stopped)
                BOOST_LOG_TRIVIAL(info) << "HubHomeView: the hub is gone";
            self->show_status(State::Stopped);
            break;
        case HubHome::Recheck::Reload:
            BOOST_LOG_TRIVIAL(info) << "HubHomeView: the hub moved (port " << self->m_port << " -> " << info.port
                                    << (info.token != self->m_token ? ", new link" : "") << "), reloading";
            self->load_hub(info.port, info.token);
            break;
        case HubHome::Recheck::Keep:
            // Same hub, same address. After a load error that means its page itself failed:
            // show that instead of reloading in a loop. A stopped screen whose hub is back
            // (started by another instance) just shows the page again.
            if (from_error)
                self->show_status(State::PageError);
            else if (self->m_state != State::Loaded)
                self->load_hub(info.port, info.token);
            break;
        }
    });
}

void HubHomeView::load_hub(int port, const std::string& token)
{
    const std::string url = HubHome::hub_page_url(port, token, wxGetApp().dark_mode());
    if (url.empty()) {
        show_status(State::Failed);
        return;
    }
    ensure_browser();
    if (m_browser == nullptr) {
        show_status(State::Failed);
        return;
    }
    // Set before LoadURL: on_navigating allows exactly this port and token.
    m_port              = port;
    m_token             = token;
    m_ticks_since_check = 0;
    // The port is logged, the token never is.
    BOOST_LOG_TRIVIAL(info) << "HubHomeView: loading the hub page from port " << port;
    m_browser->LoadURL(wxString::FromUTF8(url));
    m_state = State::Loaded;
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
        title  = _L("The phone hub is not running");
        detail = _L("It was closed from its tray icon, or it stopped. Start it to see your printers and cameras here.");
        action = _L("Start");
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
    m_btn_start_page->Show(state == State::Failed || state == State::Stopped || state == State::PageError);
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
    // Our own vetoes arrive here as cancelled navigations.
    if (evt.GetInt() == wxWEBVIEW_NAV_ERR_USER_CANCELLED || m_state != State::Loaded)
        return;
    const long long now = wxGetLocalTimeMillis().GetValue();
    const bool      too_soon = now - m_last_error_check_ms < ERROR_RECHECK_MIN_MS;
    m_last_error_check_ms    = now;
    BOOST_LOG_TRIVIAL(warning) << "HubHomeView: the hub page failed to load (" << evt.GetInt() << ")";
    m_state = State::PageError;
    if (too_soon || m_busy) {
        show_status(State::PageError);
        return;
    }
    // Most likely the hub moved or restarted: ask it where it is now.
    recheck();
}

void HubHomeView::on_script_message(wxWebViewEvent& evt)
{
    // The view has no bridge, so nothing should arrive; if anything does, it goes nowhere (and in
    // particular does not bubble up to a parent that handles the app's own pages).
    (void) evt;
}

} // namespace GUI
} // namespace Slic3r
