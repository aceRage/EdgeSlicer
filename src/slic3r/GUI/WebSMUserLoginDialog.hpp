#pragma once
#ifndef slic3r_SMWebUserLogin_HEAD_
#define slic3r_SMWebUserLogin_HEAD_

#include "wx/artprov.h"
#include "wx/cmdline.h"
#include "wx/notifmsg.h"
#include "wx/settings.h"
#include "wx/webview.h"

#if wxUSE_WEBVIEW_IE
#include "wx/msw/webview_ie.h"
#endif
#if wxUSE_WEBVIEW_EDGE
#include "wx/msw/webview_edge.h"
#endif

#include "wx/webviewarchivehandler.h"
#include "wx/webviewfshandler.h"
#include "wx/numdlg.h"
#include "wx/infobar.h"
#include "wx/filesys.h"
#include "wx/fs_arc.h"
#include "wx/fs_mem.h"
#include "wx/stdpaths.h"
#include <wx/frame.h>
#include "wx/timer.h"
#include <wx/tbarbase.h>
#include "wx/textctrl.h"

#include <chrono>
#include <functional>
#include <string>

namespace Slic3r { namespace GUI {

class SMUserLogin : public wxDialog
{
public:
    // Silent mode (startup sign-in, GUI_App::sm_start_silent_login): the dialog is never shown;
    // it loads the sign-in page once in its hidden web view and reports what happened through
    // the callback exactly once: a token (the saved session was valid), or an empty token with the
    // reason it gave up ("no session", "timed out", a page error). The owner tears it down after.
    struct SilentResult
    {
        std::string token;       // non-empty: signed in
        std::string outcome;     // when token is empty: "no session" / "timed out" / "failed"
        std::string detail;      // optional, for the log line; never the token
        std::string user_info_url;
    };
    using SilentCallback = std::function<void(const SilentResult &)>;

    SMUserLogin(bool isLogout = false, bool silent = false);
    virtual ~SMUserLogin();

    // Silent mode only: start the watchdog. The callback runs on the UI thread, at most once.
    void start_silent(SilentCallback cb);
    // Silent mode only: stop the watchdog and drop the callback (teardown / cancel).
    void stop_silent();
    bool is_silent() const { return m_silent; }

    // The account lookup both paths share: fills GUI_App::SMUserInfo from the
    // /api/common/accounts/current answer (HTTP 200) and marks the user signed in. Must run on the
    // UI thread. false (and not signed in) when the answer is not readable JSON.
    static bool apply_account_info(const std::string &body, const std::string &token);

    void load_url(wxString &url);

    std::string w2s(wxString sSrc);

    void UpdateState();
    void OnIdle(wxIdleEvent &evt);
    // void OnClose(wxCloseEvent &evt);

    void OnNavigationRequest(wxWebViewEvent &evt);
    void OnNavigationComplete(wxWebViewEvent &evt);
    void OnDocumentLoaded(wxWebViewEvent &evt);
    void OnNewWindow(wxWebViewEvent &evt);
    void OnError(wxWebViewEvent &evt);
    void OnTitleChanged(wxWebViewEvent &evt);
    void OnFullScreenChanged(wxWebViewEvent &evt);
    void OnScriptMessage(wxWebViewEvent &evt);

    void OnScriptResponseMessage(wxCommandEvent &evt);
    void RunScript(const wxString &javascript);

    bool m_networkOk { false };
    bool ShowErrorPage();

    bool run();

    static int web_sequence_id;
private:
    wxTimer *m_timer { nullptr };
    void     OnTimer(wxTimerEvent &event);

    // silent mode
    bool           m_silent { false };
    bool           m_silent_done { false };
    SilentCallback m_silent_cb;
    wxTimer        m_silent_timer;
    std::chrono::steady_clock::time_point m_silent_started;
    std::chrono::steady_clock::time_point m_silent_last_load;
    bool           m_silent_page_loaded { false };
    void           OnSilentTimer(wxTimerEvent &event);
    void           finish_silent(const std::string &token, const std::string &outcome, const std::string &detail);

private:

    wxString   TargetUrl     = "https://id.snapmaker.cn?from=orca";
    wxString   LogoutUrl     = "https://id.snapmaker.cn/logout?from=orca";
    wxString   m_hostUrl     = "https://id.snapmaker.cn";
    wxString   m_accountUrl  = "https://api.snapmaker.cn";
    wxString   m_userInfoUrl = "https://api.snapmaker.cn/api/common/accounts/current";
    wxString   m_home_url    = "https://www.snapmaker.cn/";
    wxWebView *m_browser;

    std::string m_AutotestToken;

#if wxUSE_WEBVIEW_IE
    wxMenuItem *m_script_object_el;
    wxMenuItem *m_script_date_el;
    wxMenuItem *m_script_array_el;
#endif
    // Last executed JavaScript snippet, for convenience.
    wxString m_javascript;
    wxString m_response_js;

    wxString m_sm_user_agent;

    DECLARE_EVENT_TABLE()
};

}} // namespace Slic3r::GUI

#endif 
