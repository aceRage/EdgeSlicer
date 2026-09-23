#ifndef slic3r_GUI_WebView_hpp_
#define slic3r_GUI_WebView_hpp_

#include <wx/webview.h>

class WebView
{
public:
    // Ultra: brand_tag prefixes the User-Agent ("SM-Slicer" by default). The Bambu account
    // login must use "BBL-Slicer" or bambulab.com's sign-in won't run the slicer login flow
    // (it just redirects to the marketing home and never posts the token back).
    // script_bridge=false skips the app-wide "wx" script channel: the page then has no way to
    // post anything to the app (the Home tab's hub view, which shows a page served by another
    // process, is built that way).
    static wxWebView *CreateWebView(wxWindow *parent, wxString const &url, wxString const &brand_tag = "SM-Slicer",
                                    bool script_bridge = true);
#if wxUSE_WEBVIEW_EDGE
    static bool CheckWebViewRuntime();
    static bool DownloadAndInstallWebViewRuntime();
#endif
    static void LoadUrl(wxWebView * webView, wxString const &url);

    static bool RunScript(wxWebView * webView, wxString const & msg);

    // After a theme change: refresh every view's User-Agent (it carries the theme token) and
    // reload it - except views that opted out with SetReloadOnThemeChange(view, false), which
    // switch theme live instead (a reload would restart the hub view's camera streams).
    static void RecreateAll();
    static void SetReloadOnThemeChange(wxWebView *webView, bool reload);
};

#endif // !slic3r_GUI_WebView_hpp_
