#include "PrinterWebView.hpp"

#include "I18N.hpp"
#include "slic3r/GUI/PrinterWebView.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "common_func/common_func.hpp"

#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/toolbar.h>
#include <wx/textdlg.h>
#include <wx/choice.h>
#include <wx/stattext.h>

#include "libslic3r/PresetBundle.hpp"

#include <algorithm>

#include <slic3r/GUI/Widgets/WebView.hpp>
#include <wx/webview.h>
#include <boost/algorithm/string.hpp>
#include "slic3r/GUI/SSWCP.hpp"
#include "sentry_wrapper/SentryWrapper.hpp"

namespace pt = boost::property_tree;

namespace Slic3r {
namespace GUI {

PrinterWebView::PrinterWebView(wxWindow *parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
 {

    wxBoxSizer* topsizer = new wxBoxSizer(wxVERTICAL);

    wxString url      = wxString::FromUTF8(LOCALHOST_URL + std::to_string(wxGetApp().get_page_http_port()) + "/web/flutter_web/index.html?path=2");
    auto     real_url = wxGetApp().get_international_url(url);
      // Create the webview
    m_browser = WebView::CreateWebView(this, real_url);
    if (m_browser == nullptr) {
        wxLogError("Could not init m_browser");
        return;
    }

    m_browser->Bind(wxEVT_WEBVIEW_ERROR, &PrinterWebView::OnError, this);
    m_browser->Bind(wxEVT_WEBVIEW_LOADED, &PrinterWebView::OnLoaded, this);
    m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &PrinterWebView::OnScriptMessage, this, m_browser->GetId());

    // The device picker, above the page. Created empty and hidden; Sidebar::update_all_preset_
    // comboboxes fills it in for a print-host printer whose model has devices.
    m_device_bar   = new wxPanel(this, wxID_ANY);
    auto* bar_sizer = new wxBoxSizer(wxHORIZONTAL);
    bar_sizer->Add(new wxStaticText(m_device_bar, wxID_ANY, _L("Printer") + ":"), 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(8));
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
    SetEvtHandlerEnabled(false);
    SSWCP::on_webview_delete(m_browser);

    wxGetApp().fltviews().remove_printer_view(this);

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " End";
}


void PrinterWebView::load_url(wxString& url, wxString apikey)
{
    if (m_browser == nullptr)
        return;
    m_apikey = apikey;

    if (url.find("path=2") != std::string::npos) {
        wxGetApp().fltviews().add_printer_view(this, url, apikey);
    } else {
        wxGetApp().fltviews().remove_printer_view(this);
    }

    m_browser->Show();
    m_browser->LoadURL(url);

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
        if (m_device_bar->IsShown()) {
            m_device_bar->Hide();
            Layout();
        }
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
    if (!m_device_bar->IsShown()) {
        m_device_bar->Show();
        Layout();
    }
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
}

void PrinterWebView::reload()
{
    m_browser->Reload();
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
    auto e = "unknown error";
    switch (evt.GetInt()) {
      case wxWEBVIEW_NAV_ERR_CONNECTION:
        e = "wxWEBVIEW_NAV_ERR_CONNECTION";
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
        break;
      case wxWEBVIEW_NAV_ERR_OTHER:
        e = "wxWEBVIEW_NAV_ERR_OTHER";
        break;
      }
    BOOST_LOG_TRIVIAL(fatal) << __FUNCTION__<< boost::format(":PrinterWebView error loading page %1% %2% %3% %4%") %evt.GetURL() %evt.GetTarget() %e %evt.GetString();
}

void PrinterWebView::OnLoaded(wxWebViewEvent &evt)
{
    if (evt.GetURL().IsEmpty())
        return;
    if (evt.GetURL() != m_browser->GetCurrentURL())
        return;
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
