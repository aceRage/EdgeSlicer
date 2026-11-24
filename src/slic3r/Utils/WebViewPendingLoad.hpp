#ifndef slic3r_Utils_WebViewPendingLoad_hpp_
#define slic3r_Utils_WebViewPendingLoad_hpp_

#include <cstring>
#include <string>

// macOS WKWebView pending-load gate (GUI/Widgets/WebView.cpp WebViewWebKit), kept free of wx so
// tests/slic3rutils/webview_pending_load_tests.cpp can pin it without starting a webview.
//
// Create() used to pass "about:blank", which WKBackForwardList records, so a later back gesture
// lands on a white page. Create now passes an empty URL; wxWebViewWebKit::Create() then LoadURL("")
// before the script-message handler is installed. That empty load, and about:blank, must be ignored
// so they cannot overwrite the constructor's pending real URL or enter history. AttachNavigationGate
// is unchanged: it still binds the in-flight Flutter load queue after Create().
namespace Slic3r {
namespace WebViewPendingLoad {

inline bool is_blank_url(const char *url)
{
    if (url == nullptr || *url == '\0')
        return true;
    return std::strcmp(url, "about:blank") == 0;
}

inline bool is_blank_url(const std::string &url)
{
    return is_blank_url(url.c_str());
}

enum class LoadAction {
    Ignore, // drop this LoadURL; leave pending as-is
    Stash,  // keep as pending until the script handler is installed
    Load    // handler is in; LoadURL proceeds (RequestLoad still accepts about:blank dialogs)
};

inline LoadAction load_action(bool handler_installed, const std::string &url)
{
    if (handler_installed)
        return LoadAction::Load;
    if (is_blank_url(url))
        return LoadAction::Ignore;
    return LoadAction::Stash;
}

// Stand-in for WebViewWebKit::LoadURL / SetScriptMessageHandlerInstalled.
struct Gate {
    bool        handler_installed { false };
    std::string pending;

    LoadAction load(const std::string &url)
    {
        const LoadAction action = load_action(handler_installed, url);
        if (action == LoadAction::Stash)
            pending = url;
        return action;
    }

    std::string flush()
    {
        handler_installed = true;
        std::string url   = pending;
        pending.clear();
        if (is_blank_url(url))
            return std::string();
        return url;
    }
};

} // namespace WebViewPendingLoad
} // namespace Slic3r

#endif // slic3r_Utils_WebViewPendingLoad_hpp_
