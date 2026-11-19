#ifndef slic3r_GUI_ModelBrowserPanel_hpp_
#define slic3r_GUI_ModelBrowserPanel_hpp_

#include <functional>
#include <memory>

#include <wx/panel.h>

namespace Slic3r {
namespace GUI {

// Whether this system can run the model browser: Windows (WebView2), and macOS 12 or later (WKWebView:
// WKDownload, the media-capture permission decision; ModelBrowserPanelMac.mm). Never on Linux.
bool model_browser_supported();

// Whether Home > Models is offered: model_browser_supported(), and Preferences > "Model browser
// (beta)" (app_config "model_browser_beta", on by default).
bool model_browser_enabled();

// Home > Models: a small web browser for the model sites (Printables, MakerWorld, Snapmaker Space).
//
// One interface, one Impl per system: WebView2 on Windows (ModelBrowserPanel.cpp), WKWebView on
// macOS (ModelBrowserPanelMac.mm, which also defines this class's members there). Both carry out the
// same security model with the same rules.
//
// Security model (see libslic3r/ModelBrowserRules.hpp for the rules, which are unit tested):
//  - Its own WebView2 environment and user data folder (%LOCALAPPDATA%\<app>\ModelBrowser): its
//    cookies, logins and cache never mix with the app's own web views (page server, hub, sign-in
//    dialogs), and those views' cookies are never visible to the sites. Logins persist there.
//    macOS 14+: its own persistent WKWebsiteDataStore (dataStoreForIdentifier:); macOS 12-13: a
//    non-persistent store (isolated, but sign-ins last until EdgeSlicer quits) - never the default
//    store the app's own web views share.
//  - It is never made with WebView::CreateWebView and carries no script bridge: web messages and
//    host objects are off, and the panel refuses to show a view in which they are not (asserted
//    after the settings are applied).
//  - Nothing on this computer or the LAN is loaded, top-level, in frames or as a sub-resource
//    (our page server and the phone hub live there). No file:, and no other program is ever
//    started: the sites' "Open in" buttons (Printables' prusaslicer:// / orcaslicer://, MakerWorld's
//    bambustudioopen:// / bambustudio://) are read and handed to the app's downloader, the same path
//    those links take from the system browser, with its domain checks.
//  - Downloads: model files go through the safe download path (sanitized name, exclusive marker,
//    no-replace placement, size and magic-byte checks) and then the import queue; programs and
//    scripts are refused; anything else may be saved to Downloads after asking.
//  - Permissions (camera, microphone, location, notifications, clipboard, ...) are denied,
//    password saving and autofill are off, DevTools only in developer mode, an honest User-Agent.
//  - Popups only for sign-in (or a popup the user clicked for), in the same isolated browser.
class ModelBrowserPanel : public wxPanel
{
public:
    // on_leave: the "Home" button (back to the Home sections).
    ModelBrowserPanel(wxWindow* parent, std::function<void()> on_leave);
    ~ModelBrowserPanel() override;

    // The panel was shown (true) or hidden (false). The browser is created the first time it is
    // shown, never before.
    void set_active(bool active);
    void sys_color_changed();

    struct Impl;

private:
    std::unique_ptr<Impl> p;
};

} // namespace GUI
} // namespace Slic3r

#endif
