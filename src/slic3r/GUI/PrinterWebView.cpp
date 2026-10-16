#include "PrinterWebView.hpp"

#include "I18N.hpp"
#include "slic3r/GUI/PrinterWebView.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "common_func/common_func.hpp"

#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/toolbar.h>
#include <wx/textdlg.h>
#include <wx/choice.h>
#include <wx/stattext.h>

#include "libslic3r/PresetBundle.hpp"

#include <algorithm>
#include <chrono>

#include <slic3r/GUI/Widgets/WebView.hpp>
#include <wx/webview.h>
#include <boost/algorithm/string.hpp>
#include "slic3r/GUI/SSWCP.hpp"
#include "slic3r/Utils/WebLoadRetry.hpp"
#include "sentry_wrapper/SentryWrapper.hpp"

namespace pt = boost::property_tree;

namespace Slic3r {
namespace GUI {

static int64_t device_page_now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// What the log calls a page: our own pages by their path and route (no host, no token); anybody
// else's page (a print host's web UI, with its address) only by kind.
static std::string device_page_name(const std::string& url)
{
    if (!WebLoadRetry::is_loopback_url(url))
        return url.empty() ? std::string("(blank)") : std::string("a print host page");
    const std::string page = WebLoadRetry::page_of(url);
    const size_t      sep  = page.find("://");
    const size_t      path = sep == std::string::npos ? std::string::npos : page.find('/', sep + 3);
    std::string       name = path == std::string::npos ? page : page.substr(path);
    const size_t      route = url.find("path=");
    if (route != std::string::npos)
        name += "?" + url.substr(route, url.find_first_of("&#", route) - route);
    return name;
}

PrinterWebView::PrinterWebView(wxWindow *parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
 {

    wxBoxSizer* topsizer = new wxBoxSizer(wxVERTICAL);

    wxString url      = wxString::FromUTF8(wxGetApp().page_url("/web/flutter_web/index.html?path=2"));
    auto     real_url = wxGetApp().get_international_url(url);
    // During startup the view is made blank and the Flutter page is held until startup is over
    // (DevicePageLoad.hpp): booted now, its first requests would wait out the GUI thread's
    // post-init work and time out. A view made later (recreate_GUI) loads it straight away.
    // On macOS, WebViewWebKit holds a URL until the script-message handler is installed
    // (SM #857/#865).
    m_load_gate = DevicePageLoad::Gate(wxGetApp().startup_settled());
    const bool load_now = m_load_gate.released();
    m_browser = WebView::CreateWebView(this, load_now ? real_url : wxString());
    if (m_browser == nullptr) {
        wxLogError("Could not init m_browser");
        return;
    }
    const std::string initial_url = real_url.ToUTF8().data();
    if (load_now) {
        m_load_gate.started(initial_url, device_page_now_ms());
    } else {
        m_load_gate.request(initial_url, std::string(), DevicePageLoad::Kind::Show, device_page_now_ms());
        m_held_is_initial = true;
        BOOST_LOG_TRIVIAL(info) << "[DevicePage] holding " << device_page_name(initial_url) << " until startup is over";
    }
    // Opening the tab before startup is over loads it at once.
    Bind(wxEVT_SHOW, [this](wxShowEvent& evt) {
        evt.Skip();
        if (evt.IsShown())
            release_startup_hold("the Device tab was opened");
        // The Storage tab reads the printer only while it is on screen.
        if (m_storage) {
            if (evt.IsShown() && m_storage_shown)
                m_storage->activate();
            else if (!evt.IsShown())
                m_storage->deactivate();
        }
    });

    // Panel bind so WebViewWebKit's navigation gate (also on the webview) still sees LOADED/ERROR.
    Bind(wxEVT_WEBVIEW_ERROR, &PrinterWebView::OnError, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_LOADED, &PrinterWebView::OnLoaded, this, m_browser->GetId());
    m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &PrinterWebView::OnScriptMessage, this, m_browser->GetId());
    m_retry_timer.SetOwner(this);
    Bind(wxEVT_TIMER, &PrinterWebView::OnRetryTimer, this, m_retry_timer.GetId());

    // The bar above the page: the Storage tab's two buttons (the printer's page / its files) and
    // the device picker. Created empty and hidden; Sidebar::update_all_preset_comboboxes fills the
    // picker in for a print-host printer whose model has devices, and sets the Storage tab's printer.
    m_device_bar   = new wxPanel(this, wxID_ANY);
    auto* bar_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_tab_page      = new ::Button(m_device_bar, _L("Printer page"));
    m_tab_page->SetToolTip(_L("The printer's own page"));
    m_tab_page->SetName(_L("Printer page"));
    m_tab_page->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { show_storage(false); });
    m_tab_storage = new ::Button(m_device_bar, _L("Storage"));
    m_tab_storage->SetToolTip(_L("The G-code files on the printer"));
    m_tab_storage->SetName(_L("Storage"));
    m_tab_storage->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { show_storage(true); });
    bar_sizer->Add(m_tab_page, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxTOP | wxBOTTOM, FromDIP(4));
    bar_sizer->Add(m_tab_storage, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxTOP | wxBOTTOM, FromDIP(4));
    m_tab_page->Hide();
    m_tab_storage->Hide();
    m_device_label = new wxStaticText(m_device_bar, wxID_ANY, _L("Printer") + ":");
    bar_sizer->Add(m_device_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(8));
    m_device_choice = new wxChoice(m_device_bar, wxID_ANY);
    bar_sizer->Add(m_device_choice, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, FromDIP(4));
    m_device_bar->SetSizer(bar_sizer);
    m_device_bar->Hide();
    m_device_choice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { load_picked_device(); });

    SetSizer(topsizer);

    topsizer->Add(m_device_bar, 0, wxEXPAND);
    topsizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));

    update_mode();

    //Zoom
    m_zoomFactor = 100;

    //Connect the idle events
    Bind(wxEVT_CLOSE_WINDOW, &PrinterWebView::OnClose, this);

 }

PrinterWebView::~PrinterWebView()
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Start";
    m_retry_timer.Stop();
    SetEvtHandlerEnabled(false);
    SSWCP::on_webview_delete(m_browser);

    wxGetApp().fltviews().remove_printer_view(this);

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " End";
}


void PrinterWebView::load_url(wxString& url, wxString apikey, DevicePageLoad::Kind kind)
{
    if (m_browser == nullptr)
        return;
    const std::string            u = url.ToUTF8().data();
    const DevicePageLoad::Action a = m_load_gate.request(u, apikey.ToUTF8().data(), kind, device_page_now_ms());
    if (a != DevicePageLoad::Action::Load) {
        if (a == DevicePageLoad::Action::Hold)
            m_held_is_initial = false;
        BOOST_LOG_TRIVIAL(info) << "[DevicePage] " << DevicePageLoad::to_string(kind) << " " << device_page_name(u) << ": "
                                << DevicePageLoad::to_string(a);
        return;
    }
    navigate(url, apikey, true, kind);
}

void PrinterWebView::release_startup_hold(const char* why)
{
    if (m_browser == nullptr)
        return;
    DevicePageLoad::Request r;
    if (!m_load_gate.release(r))
        return;
    BOOST_LOG_TRIVIAL(info) << "[DevicePage] " << why << ": loading " << device_page_name(r.url) << ", held since startup";
    navigate(wxString::FromUTF8(r.url), wxString::FromUTF8(r.apikey), !m_held_is_initial, DevicePageLoad::Kind::Show);
    m_held_is_initial = false;
}

void PrinterWebView::navigate(const wxString& url, const wxString& apikey, bool register_view, DevicePageLoad::Kind kind)
{
    m_apikey = apikey;
    // A page asked for on purpose starts with a clean slate: no retry of whatever failed before.
    m_retry_timer.Stop();
    m_retry_url.clear();
    m_retry_page.clear();
    m_retry_count = 0;

    if (register_view) {
        if (url.find("path=2") != std::string::npos) {
            wxGetApp().fltviews().add_printer_view(this, url, apikey);
        } else {
            wxGetApp().fltviews().remove_printer_view(this);
        }
    }

    // The page loads behind the Storage tab when that is the one on screen.
    if (!m_storage_shown)
        m_browser->Show();
    m_browser->LoadURL(url);
    const std::string u = url.ToUTF8().data();
    m_load_gate.started(u, device_page_now_ms());
    BOOST_LOG_TRIVIAL(info) << "[DevicePage] " << DevicePageLoad::to_string(kind) << " " << device_page_name(u) << ": load";

    UpdateState();
}

void PrinterWebView::set_devices(const std::string& model_key, const std::vector<PrintHostDevices::Device>& devices, const std::string& select_id)
{
    if (!m_device_bar || !m_device_choice)
        return;
    const bool same = m_model_key == model_key && m_devices.size() == devices.size() &&
                      std::equal(m_devices.begin(), m_devices.end(), devices.begin(),
                                 [](const PrintHostDevices::Device& a, const PrintHostDevices::Device& b) {
                                     return a.id == b.id && a.alias == b.alias && a.address == b.address;
                                 });
    m_model_key = model_key;
    m_devices   = devices;
    if (devices.empty()) {
        m_device_choice->Clear();
        update_bar();
        return;
    }
    if (!same) {
        m_device_choice->Clear();
        for (const PrintHostDevices::Device& d : devices) {
            wxString label = from_u8(d.display_name());
            if (!d.alias.empty() && d.alias != d.address)
                label += "  " + from_u8(d.address);
            m_device_choice->Append(label);
        }
    }
    int sel = 0;
    for (size_t i = 0; i < devices.size(); ++i)
        if (!select_id.empty() && devices[i].id == select_id)
            sel = (int) i;
    m_device_choice->SetSelection(sel);
    update_bar();
}

std::string PrinterWebView::picked_device() const
{
    if (!m_device_choice)
        return {};
    const int sel = m_device_choice->GetSelection();
    if (sel < 0 || sel >= (int) m_devices.size())
        return {};
    return m_devices[sel].id;
}

void PrinterWebView::load_picked_device()
{
    const int sel = m_device_choice ? m_device_choice->GetSelection() : -1;
    if (sel < 0 || sel >= (int) m_devices.size())
        return;
    const PrintHostDevices::Device& d = m_devices[sel];
    PresetBundle*                   bundle = wxGetApp().preset_bundle;
    if (!bundle)
        return;
    const DynamicPrintConfig& cfg = bundle->printers.get_edited_preset().config;
    std::string               url = d.address;
    // print_host_webui is an override for the preset's own address, so it only applies to whichever
    // device carries that address; every other device is reached at its own.
    const std::string webui = cfg.opt_string("print_host_webui");
    if (!webui.empty() &&
        PrintHostDevices::normalize_address(cfg.opt_string("print_host")) == PrintHostDevices::normalize_address(d.address))
        url = webui;
    if (url.empty())
        return;
    if (url.find("http://") != 0 && url.find("https://") != 0)
        url = "http://" + url;
    // Ultra: remember which device the tab is on, so the next Print preselects the same one.
    try {
        if (!m_model_key.empty())
            PrintHostDevices::set_last_used(m_model_key, d.id);
    } catch (...) {}
    wxString wurl   = from_u8(url);
    wxString apikey = from_u8(d.apikey);
    load_url(wurl, apikey);
    // The Storage tab follows the picker.
    set_storage_device(d, "ph:" + d.id);
}

void PrinterWebView::set_storage_device(const PrintHostDevices::Device& device, const std::string& key)
{
    const PrinterStorage::Target t = PrinterStorage::target_for_host(device.host_type, device.address, device.apikey,
                                                                     device.display_name(), key);
    set_storage_source(t.kind == PrinterStorage::Target::Kind::None ? PrinterStoragePanel::Source() :
                                                                     PrinterStoragePanel::fixed_source(t));
}

void PrinterWebView::set_storage_source(PrinterStoragePanel::Source source)
{
    const bool has       = (bool) source;
    m_storage_has_source = has;
    if (has && !m_storage) {
        m_storage = new PrinterStoragePanel(this);
        m_storage->Hide();
        GetSizer()->Add(m_storage, wxSizerFlags().Expand().Proportion(1));
    }
    if (m_storage)
        m_storage->set_source(has ? std::move(source) : PrinterStoragePanel::Source());
    if (!has && m_storage_shown)
        show_storage(false);
    else
        update_bar();
}

void PrinterWebView::show_storage(bool show)
{
    if (show && (!m_storage || !m_storage_has_source))
        show = false;
    m_storage_shown = show;
    if (m_storage) {
        m_storage->Show(show);
        if (show && IsShownOnScreen())
            m_storage->activate();
        else if (!show)
            m_storage->deactivate();
    }
    if (m_browser)
        m_browser->Show(!show);
    update_bar();
    if (show && m_storage)
        m_storage->SetFocus();
}

void PrinterWebView::update_bar()
{
    if (!m_device_bar)
        return;
    const bool has_devices = !m_devices.empty();
    const bool storage_tab = m_storage != nullptr && m_storage_has_source;
    // The preset comboboxes call in here often: lay out only when something changed.
    const int state = (has_devices ? 1 : 0) | (storage_tab ? 2 : 0) | (m_storage_shown ? 4 : 0);
    if (state == m_bar_state)
        return;
    m_bar_state = state;
    m_device_label->Show(has_devices);
    m_device_choice->Show(has_devices);
    m_tab_page->Show(storage_tab);
    m_tab_storage->Show(storage_tab);
    // The tab on screen is the highlighted one.
    m_tab_page->SetStyle(m_storage_shown ? ButtonStyle::Regular : ButtonStyle::Confirm, ButtonType::Window);
    m_tab_storage->SetStyle(m_storage_shown ? ButtonStyle::Confirm : ButtonStyle::Regular, ButtonType::Window);
    m_device_bar->Show(has_devices || storage_tab);
    m_device_bar->Layout();
    Layout();
}

void PrinterWebView::sys_color_changed()
{
    if (m_tab_page) {
        m_tab_page->Rescale();
        m_tab_storage->Rescale();
        m_bar_state = -1;
        update_bar();
    }
    if (m_storage)
        m_storage->sys_color_changed();
}

void PrinterWebView::msw_rescale()
{
    if (m_tab_page) {
        m_tab_page->Rescale();
        m_tab_storage->Rescale();
        m_bar_state = -1;
        update_bar();
    }
    if (m_storage)
        m_storage->msw_rescale();
}

void PrinterWebView::reload()
{
    m_browser->Reload();
    m_load_gate.started(m_browser->GetCurrentURL().ToUTF8().data(), device_page_now_ms());
}

bool PrinterWebView::isSnapmakerPage()
{
    if (m_browser == nullptr)
        return false;
    auto url = m_browser->GetCurrentURL();
    return (url.find("flutter_web") != std::string::npos);
}

void PrinterWebView::sendMessage(const std::string& msg) {
    WebView::RunScript(m_browser, msg);
}

void PrinterWebView::update_mode()
{
    // m_browser->EnableAccessToDevTools(wxGetApp().app_config->get_bool("developer_mode"));
    m_browser->EnableAccessToDevTools(true);
}

/**
 * Method that retrieves the current state from the web control and updates the
 * GUI the reflect this current state.
 */
void PrinterWebView::UpdateState() {
  // SetTitle(m_browser->GetCurrentTitle());

}

void PrinterWebView::OnClose(wxCloseEvent& evt)
{
    this->Hide();
}

void PrinterWebView::SendAPIKey()
{
    if (m_apikey.IsEmpty())
        return;

    // Re-inject on every document load (e.g. context-menu Reload). Idempotent
    // JS-level marker avoids stacking fetch/XHR wrappers if LOADED fires more than once.
    wxString script = wxString::Format(R"(
    (function() {
        if (window.__sm_apikey_hooked) return;
        window.__sm_apikey_hooked = true;
        var apiKey = '%s';
        // Override fetch to inject X-API-Key header
        if (window.fetch) {
            var originalFetch = window.fetch;
            window.fetch = function(input, init) {
                init = init || {};
                init.headers = init.headers || {};
                if (!init.headers['X-API-Key']) {
                    init.headers['X-API-Key'] = apiKey;
                }
                return originalFetch(input, init);
            };
        }
        // Override XMLHttpRequest to inject X-API-Key header.
        // Preserves prototype chain and static constants for compatibility
        // with libraries that check instanceof or readyState constants.
        var OrigXHR = window.XMLHttpRequest;
        var newXHR = function() {
            var xhr = new OrigXHR();
            var origOpen = xhr.open;
            var headersSet = false;
            xhr.open = function(method, url) {
                origOpen.apply(xhr, arguments);
                if (!headersSet) {
                    xhr.setRequestHeader('X-API-Key', apiKey);
                    headersSet = true;
                }
            };
            return xhr;
        };
        newXHR.prototype = OrigXHR.prototype;
        newXHR.DONE = OrigXHR.DONE;
        newXHR.UNSENT = OrigXHR.UNSENT;
        newXHR.OPENED = OrigXHR.OPENED;
        newXHR.HEADERS_RECEIVED = OrigXHR.HEADERS_RECEIVED;
        newXHR.LOADING = OrigXHR.LOADING;
        window.XMLHttpRequest = newXHR;
    })();
)",
                                       m_apikey);

    // Inject immediately into the current page on all platforms.
    WebView::RunScript(m_browser, script);

#ifndef __WXMAC__
    // On Windows/Linux: also install a persistent user script so the
    // API key is injected at document start on future navigations.
    // AddUserScript works correctly on these platforms (Edge WebView2, WebKitGTK).
    // Do NOT call Reload() — the current page is already handled by RunScript above.
    m_browser->RemoveAllUserScripts();
    m_browser->AddUserScript(script);
#endif
}

void PrinterWebView::OnError(wxWebViewEvent &evt)
{
    evt.Skip();
    auto e = "unknown error";
    WebLoadRetry::Failure failure = WebLoadRetry::Failure::Other;
    switch (evt.GetInt()) {
      case wxWEBVIEW_NAV_ERR_CONNECTION:
        e = "wxWEBVIEW_NAV_ERR_CONNECTION";
        failure = WebLoadRetry::Failure::Connection;
        break;
      case wxWEBVIEW_NAV_ERR_CERTIFICATE:
        e = "wxWEBVIEW_NAV_ERR_CERTIFICATE";
        break;
      case wxWEBVIEW_NAV_ERR_AUTH:
        e = "wxWEBVIEW_NAV_ERR_AUTH";
        break;
      case wxWEBVIEW_NAV_ERR_SECURITY:
        e = "wxWEBVIEW_NAV_ERR_SECURITY";
        break;
      case wxWEBVIEW_NAV_ERR_NOT_FOUND:
        e = "wxWEBVIEW_NAV_ERR_NOT_FOUND";
        break;
      case wxWEBVIEW_NAV_ERR_REQUEST:
        e = "wxWEBVIEW_NAV_ERR_REQUEST";
        break;
      case wxWEBVIEW_NAV_ERR_USER_CANCELLED:
        e = "wxWEBVIEW_NAV_ERR_USER_CANCELLED";
        failure = WebLoadRetry::Failure::Cancelled;
        break;
      case wxWEBVIEW_NAV_ERR_OTHER:
        e = "wxWEBVIEW_NAV_ERR_OTHER";
        break;
      }
    const std::string url  = evt.GetURL().ToUTF8().data();
    const std::string page = WebLoadRetry::page_of(url);
    // Another load has replaced this one since: nothing anybody is looking at failed.
    const std::string current = m_browser ? std::string(m_browser->GetCurrentURL().ToUTF8().data()) : std::string();
    if (page != m_retry_page) {
        m_retry_page  = page;
        m_retry_count = 0;
    }
    WebLoadRetry::Facts facts;
    facts.failure        = failure;
    facts.loopback       = WebLoadRetry::is_loopback_url(url);
    facts.superseded     = !current.empty() && WebLoadRetry::page_of(current) != page;
    facts.retries_so_far = m_retry_count;
    const WebLoadRetry::Decision d = WebLoadRetry::decide(facts);
    // A cancelled load was replaced by another one, which is the load that counts.
    if (failure != WebLoadRetry::Failure::Cancelled)
        m_load_gate.finished(url, false);
    // Never fatal: a page that does not load is not the application failing.
    const auto what = boost::format(":PrinterWebView error loading page %1% %2% %3% %4%") % evt.GetURL() % evt.GetTarget() % e % evt.GetString();
    if (d.retry) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << what << "; retrying in " << d.delay_ms << " ms (" << (m_retry_count + 1) << "/"
                                   << WebLoadRetry::MAX_RETRIES << ")";
        ++m_retry_count;
        m_retry_url = url;
        m_retry_timer.StartOnce(d.delay_ms);
    } else if (d.log == WebLoadRetry::Log::Info) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << what << (facts.superseded ? " (replaced by another page)" : "");
    } else {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << what << (m_retry_count > 0 ? " (gave up after retrying)" : "");
    }
}

void PrinterWebView::OnRetryTimer(wxTimerEvent&)
{
    if (m_browser == nullptr || m_retry_url.empty())
        return;
    // Only if the view is still on that page; a page chosen since wins.
    const std::string current = m_browser->GetCurrentURL().ToUTF8().data();
    if (!current.empty() && WebLoadRetry::page_of(current) != m_retry_page)
        return;
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": loading " << WebLoadRetry::page_of(m_retry_url) << " again";
    m_browser->LoadURL(wxString::FromUTF8(m_retry_url));
    m_load_gate.started(m_retry_url, device_page_now_ms());
}

void PrinterWebView::OnLoaded(wxWebViewEvent &evt)
{
    evt.Skip();
    if (evt.GetURL().IsEmpty())
        return;
    if (evt.GetURL() != m_browser->GetCurrentURL())
        return;
    m_load_gate.finished(evt.GetURL().ToUTF8().data(), true);
    if (WebLoadRetry::page_of(evt.GetURL().ToUTF8().data()) == m_retry_page) {
        if (m_retry_count > 0)
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": " << m_retry_page << " loaded after " << m_retry_count << " retr" << (m_retry_count == 1 ? "y" : "ies");
        m_retry_count = 0;
    }
    SendAPIKey();
}

void PrinterWebView::OnScriptMessage(wxWebViewEvent& evt) {
    // BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << ": " << evt.GetString().ToUTF8().data();

    // if (wxGetApp().get_mode() == comDevelop)
    //     wxLogMessage("Script message received; value = %s, handler = %s", evt.GetString(), evt.GetMessageHandler());

    // test
    SSWCP::handle_web_message(evt.GetString().ToUTF8().data(), m_browser);
}


} // GUI
} // Slic3r
