// Home > Models on macOS: the model browser on a WKWebView of its own, with the security model of
// the Windows (WebView2) panel in ModelBrowserPanel.cpp and the same rules
// (libslic3r/ModelBrowserRules.hpp, unit tested). This file defines ModelBrowserPanel's members on
// macOS; ModelBrowserPanel.cpp keeps the Windows Impl and model_browser_enabled().
//
// How each point of the security model is carried out here:
//  - Isolated profile: the views use a WKWebsiteDataStore of their own, never the default store the
//    app's own web views (wxWebView: page server, hub, Bambu / Snapmaker sign-in) share. macOS 14+:
//    dataStoreForIdentifier: (persistent: logins survive a restart; the identifier is kept in
//    app_config "model_browser_store_id"). macOS 12-13: nonPersistentDataStore (isolated too, but
//    sign-ins last until EdgeSlicer quits). Popups get the opener's configuration, so its store.
//  - No script bridge: one WKUserContentController that never gets a script message handler or a
//    user script, no URL scheme handlers; checked (verify_isolation) before any view is shown.
//  - Nothing local: decidePolicyForNavigationAction applies decide_navigation() to every navigation
//    (main frame and frames, redirects included); a WKContentRuleList compiled from
//    content_rule_list_json() blocks loads from blocked hosts (sub-resources, fetch, WebSockets);
//    the browser is not created if the rules cannot be compiled. Dropped files are refused.
//  - No other programs: a navigation to any other scheme is cancelled; the "Open in" links go to
//    GUI_App::start_download. Nothing here calls NSWorkspace.
//  - Downloads (WKDownload): decide_download() on WebKit's suggested name and the response; model
//    files go through the claim / marker / content-check / no-replace placement of the "Open in"
//    downloads and then the import queue; programs and scripts are refused; anything else is
//    saved to Downloads only after asking. Saved files are quarantined like a browser download.
//  - Permissions: camera and microphone are denied (other permissions have no WKWebView API on
//    macOS and are not granted). The Web Inspector is off in release builds.
//
// Memory management: this file is compiled without ARC (like the other .mm files of the GUI), so
// every alloc / copy / retain is matched by a release here.

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include "ModelBrowserPanel.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "format.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/ModelBrowserRules.hpp"
#include "libslic3r/UntrustedInput.hpp"
#include "common_func/common_func.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <wx/frame.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/stdpaths.h>
#include <wx/timer.h>
#include <wx/utils.h>
#include <wx/weakref.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace fs = boost::filesystem;
using Slic3r::GUI::ModelBrowserPanel;

// ---------------------------------------------------------------------------------- web view ----

// The model browser's WKWebView. MacDarkMode.mm swizzles WKWebView's drag and drop so that a file
// dropped on any web view is loaded into it (loadFileURL:); this one takes no drops at all, so
// nothing on this computer is ever loaded by dragging it in.
@interface EdgeSlicerModelBrowserWebView : WKWebView
@end

@implementation EdgeSlicerModelBrowserWebView
- (instancetype)initWithFrame:(NSRect)frame configuration:(WKWebViewConfiguration *)configuration
{
    self = [super initWithFrame:frame configuration:configuration];
    if (self != nil)
        [self unregisterDraggedTypes];
    return self;
}
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender { return NSDragOperationNone; }
- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender { return NSDragOperationNone; }
- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)sender { return NO; }
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender { return NO; }
@end

// Navigation, UI (popups, dialogs, permissions) and download delegate of every view of the browser.
// It holds a plain pointer to the panel's Impl, cleared when the panel goes; after that it refuses
// everything (completion handlers are always called, exactly once).
API_AVAILABLE(macos(12.0))
@interface EdgeSlicerModelBrowserDelegate : NSObject <WKNavigationDelegate, WKUIDelegate, WKDownloadDelegate>
{
@public
    ModelBrowserPanel::Impl *impl;
}
@end

namespace Slic3r {
namespace GUI {

using namespace Slic3r::modelbrowser;

namespace {

std::string to_std(NSString *s) { return s == nil ? std::string() : std::string([s UTF8String] ? [s UTF8String] : ""); }
std::string url_string(NSURL *u) { return u == nil ? std::string() : to_std(u.absoluteString); }
NSString   *to_ns(const std::string &s) { return [NSString stringWithUTF8String:s.c_str()]; }

fs::path downloads_folder()
{
    const fs::path dir = into_path(wxStandardPaths::Get().GetUserDir(wxStandardPaths::Dir_Downloads));
    boost::system::error_code ec;
    if (!dir.empty() && fs::is_directory(dir, ec))
        return dir;
    return fs::path();
}

// Where model files go: the same folder as the "Open in" link downloads, else Downloads.
fs::path model_folder()
{
    const fs::path dir(wxGetApp().app_config->get("download_path"));
    boost::system::error_code ec;
    if (!dir.empty() && fs::is_directory(dir, ec))
        return dir;
    return downloads_folder();
}

NSColor *to_ns_colour(const wxColour &c)
{
    return [NSColor colorWithSRGBRed:c.Red() / 255.0 green:c.Green() / 255.0 blue:c.Blue() / 255.0 alpha:1.0];
}

// KVO contexts.
void *const KVO_URL     = (void *) "edgeslicer.modelbrowser.url";
void *const KVO_HISTORY = (void *) "edgeslicer.modelbrowser.history";
void *const KVO_TITLE   = (void *) "edgeslicer.modelbrowser.title";

} // namespace

struct ModelBrowserPanel::Impl
{
    ModelBrowserPanel*    owner;
    std::function<void()> on_leave;
    std::shared_ptr<bool> alive { std::make_shared<bool>(true) };

    // ---- widgets (the same toolbar as on Windows)
    wxPanel*              toolbar { nullptr };
    Button*               btn_leave { nullptr };
    Button*               btn_back { nullptr };
    Button*               btn_forward { nullptr };
    Button*               btn_reload { nullptr };
    Button*               btn_home { nullptr };
    std::vector<Button*>  site_buttons;
    wxStaticText*         address { nullptr };
    wxStaticText*         status { nullptr };
    Button*               btn_external { nullptr };
    wxPanel*              notice { nullptr };
    wxStaticText*         notice_text { nullptr };
    Button*               notice_action { nullptr };
    Button*               notice_close { nullptr };
    std::function<void()> notice_callback;
    wxTimer               notice_timer;
    wxTimer               progress_timer;
    wxPanel*              placeholder { nullptr };
    wxStaticText*         placeholder_text { nullptr };
    Button*               btn_retry { nullptr };
    wxWindow*             host { nullptr };

    // ---- browser (Objective-C objects owned here: retained, released in close_browser())
    WKWebView*               view { nil };
    id                       delegate { nil };   // EdgeSlicerModelBrowserDelegate
    WKWebsiteDataStore*      store { nil };
    WKUserContentController* content { nil };
    WKContentRuleList*       rules { nil };
    bool                     observing { false };
    bool                     persistent_store { false };
    bool                     creating { false };
    bool                     active { false };
    std::string              site_id { "printables" };
    std::string              last_app_link;
    std::chrono::steady_clock::time_point last_app_link_time;

    struct Popup
    {
        wxWeakRef<wxFrame> frame;
        WKWebView*         view { nil };
    };
    std::vector<Popup> popups;

    struct Download
    {
        id          op { nil }; // WKDownload, retained
        fs::path    folder;
        fs::path    marker;
        fs::path    part;
        std::string name;
        std::string source;
        bool        to_import { false };
        bool        started { false }; // a destination was given
    };
    std::map<int, Download> downloads;
    int                     next_download { 1 };
    // The destination handlers of downloads waiting for the user's answer: called exactly once,
    // with nil when the panel goes first.
    std::map<int, void (^)(NSURL *)> pending_destinations;

    std::deque<fs::path> import_queue;
    bool                 importing { false };

    Impl(ModelBrowserPanel* owner, std::function<void()> on_leave) : owner(owner), on_leave(std::move(on_leave)) {}
    ~Impl();

    void build_ui();
    void apply_colours();
    void update_nav_buttons();
    void update_address(const std::string& url);
    void show_placeholder(const wxString& text, bool retry);
    enum class NoticeKind { Info, Warning, Error };
    NoticeKind notice_kind { NoticeKind::Warning };
    void show_notice(const wxString& text, NoticeKind kind, const wxString& action_label = wxString(), std::function<void()> action = nullptr);
    void apply_notice_colours();
    void hide_notice();
    void show_passkey_note();

    void create_browser();
    void create_browser_with_rules() API_AVAILABLE(macos(12.0));
    void fail(const wxString& why);
    void close_browser();
    WKWebViewConfiguration* make_configuration() API_AVAILABLE(macos(12.0));
    bool verify_isolation(WKWebView* v) const;
    void apply_theme_to_browser();
    void apply_theme(WKWebView* v);
    void resize();
    void navigate(const std::string& url);
    void go_site(const std::string& id);
    std::string current_url() const { return view != nil ? url_string(view.URL) : std::string(); }

    // Delegate callbacks.
    WKNavigationActionPolicy decide_action(WKWebView* sender, WKNavigationAction* action) API_AVAILABLE(macos(12.0));
    WKNavigationResponsePolicy decide_response(WKWebView* sender, WKNavigationResponse* response) API_AVAILABLE(macos(12.0));
    void carry_out(const NavDecision& d, const std::string& url, bool user_visible);
    WKWebView* create_popup(WKWebView* sender, WKWebViewConfiguration* configuration, WKNavigationAction* action,
                            WKWindowFeatures* features) API_AVAILABLE(macos(12.0));
    void close_popup_of(WKWebView* v);
    bool close_popup(wxFrame* frame);
    void on_load_failed(WKWebView* sender, NSError* error);
    void on_content_process_ended(WKWebView* sender);
    void on_kvo(WKWebView* sender, void* context);

    void adopt_download(WKDownload* download, WKWebView* sender) API_AVAILABLE(macos(12.0));
    void decide_destination(WKDownload* download, NSURLResponse* response, NSString* suggested, void (^handler)(NSURL*)) API_AVAILABLE(macos(12.0));
    bool begin_download(WKDownload* download, const std::string& name, const fs::path& folder, bool to_import, NSURL** destination) API_AVAILABLE(macos(12.0));
    void on_download_finished(WKDownload* download) API_AVAILABLE(macos(12.0));
    void on_download_failed(WKDownload* download, NSError* error) API_AVAILABLE(macos(12.0));
    int  download_id(id op) const;
    void update_progress();
    void finish_download(Download d);
    void quarantine(const fs::path& file, const std::string& source);

    void enqueue_import(const fs::path& path);
    void process_imports();
};

} // namespace GUI
} // namespace Slic3r

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::modelbrowser;

// ------------------------------------------------------------------------------ the delegate ----

@implementation EdgeSlicerModelBrowserDelegate

// ---- navigation

- (void)webView:(WKWebView *)webView
    decidePolicyForNavigationAction:(WKNavigationAction *)navigationAction
                    decisionHandler:(void (^)(WKNavigationActionPolicy))decisionHandler
{
    decisionHandler(impl != nullptr ? impl->decide_action(webView, navigationAction) : WKNavigationActionPolicyCancel);
}

- (void)webView:(WKWebView *)webView
    decidePolicyForNavigationResponse:(WKNavigationResponse *)navigationResponse
                      decisionHandler:(void (^)(WKNavigationResponsePolicy))decisionHandler
{
    decisionHandler(impl != nullptr ? impl->decide_response(webView, navigationResponse) : WKNavigationResponsePolicyCancel);
}

- (void)webView:(WKWebView *)webView navigationAction:(WKNavigationAction *)navigationAction didBecomeDownload:(WKDownload *)download
{
    if (impl != nullptr)
        impl->adopt_download(download, webView);
    else
        [download cancel:nil];
}

- (void)webView:(WKWebView *)webView navigationResponse:(WKNavigationResponse *)navigationResponse didBecomeDownload:(WKDownload *)download
{
    if (impl != nullptr)
        impl->adopt_download(download, webView);
    else
        [download cancel:nil];
}

- (void)webView:(WKWebView *)webView didFailProvisionalNavigation:(WKNavigation *)navigation withError:(NSError *)error
{
    if (impl != nullptr)
        impl->on_load_failed(webView, error);
}

- (void)webView:(WKWebView *)webView didFailNavigation:(WKNavigation *)navigation withError:(NSError *)error
{
    if (impl != nullptr)
        impl->on_load_failed(webView, error);
}

- (void)webViewWebContentProcessDidTerminate:(WKWebView *)webView
{
    if (impl != nullptr)
        impl->on_content_process_ended(webView);
}

// ---- new windows, dialogs, permissions

- (WKWebView *)webView:(WKWebView *)webView
    createWebViewWithConfiguration:(WKWebViewConfiguration *)configuration
               forNavigationAction:(WKNavigationAction *)navigationAction
                    windowFeatures:(WKWindowFeatures *)windowFeatures
{
    return impl != nullptr ? impl->create_popup(webView, configuration, navigationAction, windowFeatures) : nil;
}

- (void)webViewDidClose:(WKWebView *)webView
{
    if (impl != nullptr)
        impl->close_popup_of(webView);
}

- (void)webView:(WKWebView *)webView
    requestMediaCapturePermissionForOrigin:(WKSecurityOrigin *)origin
                          initiatedByFrame:(WKFrameInfo *)frame
                                      type:(WKMediaCaptureType)type
                           decisionHandler:(void (^)(WKPermissionDecision))decisionHandler
{
    BOOST_LOG_TRIVIAL(info) << "ModelBrowser: denied camera / microphone access to " << to_std(origin.host);
    decisionHandler(WKPermissionDecisionDeny);
}

static NSString *dialog_title(WKFrameInfo *frame)
{
    NSString *host = frame.securityOrigin.host;
    return host.length > 0 ? host : @"EdgeSlicer";
}

- (void)webView:(WKWebView *)webView
    runJavaScriptAlertPanelWithMessage:(NSString *)message
                      initiatedByFrame:(WKFrameInfo *)frame
                     completionHandler:(void (^)(void))completionHandler
{
    if (impl != nullptr) {
        NSAlert *alert = [[NSAlert alloc] init];
        alert.messageText     = dialog_title(frame);
        alert.informativeText = message ?: @"";
        [alert addButtonWithTitle:@"OK"];
        [alert runModal];
        [alert release];
    }
    completionHandler();
}

- (void)webView:(WKWebView *)webView
    runJavaScriptConfirmPanelWithMessage:(NSString *)message
                        initiatedByFrame:(WKFrameInfo *)frame
                       completionHandler:(void (^)(BOOL))completionHandler
{
    BOOL ok = NO;
    if (impl != nullptr) {
        NSAlert *alert = [[NSAlert alloc] init];
        alert.messageText     = dialog_title(frame);
        alert.informativeText = message ?: @"";
        [alert addButtonWithTitle:@"OK"];
        [alert addButtonWithTitle:to_ns(into_u8(_L("Cancel")))];
        ok = [alert runModal] == NSAlertFirstButtonReturn;
        [alert release];
    }
    completionHandler(ok);
}

- (void)webView:(WKWebView *)webView
    runJavaScriptTextInputPanelWithPrompt:(NSString *)prompt
                              defaultText:(NSString *)defaultText
                         initiatedByFrame:(WKFrameInfo *)frame
                        completionHandler:(void (^)(NSString *))completionHandler
{
    NSString *answer = nil;
    if (impl != nullptr) {
        NSAlert *alert = [[NSAlert alloc] init];
        alert.messageText     = dialog_title(frame);
        alert.informativeText = prompt ?: @"";
        [alert addButtonWithTitle:@"OK"];
        [alert addButtonWithTitle:to_ns(into_u8(_L("Cancel")))];
        NSTextField *field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 300, 24)];
        field.stringValue = defaultText ?: @"";
        alert.accessoryView = field;
        if ([alert runModal] == NSAlertFirstButtonReturn)
            answer = [[field.stringValue copy] autorelease];
        [field release];
        [alert release];
    }
    completionHandler(answer);
}

// <input type="file">: the files the user picks (never a folder), as in any browser.
- (void)webView:(WKWebView *)webView
    runOpenPanelWithParameters:(WKOpenPanelParameters *)parameters
              initiatedByFrame:(WKFrameInfo *)frame
             completionHandler:(void (^)(NSArray<NSURL *> *))completionHandler
{
    if (impl == nullptr) {
        completionHandler(nil);
        return;
    }
    NSOpenPanel *panel             = [NSOpenPanel openPanel];
    panel.canChooseFiles           = YES;
    panel.canChooseDirectories     = NO;
    panel.allowsMultipleSelection  = parameters.allowsMultipleSelection;
    panel.resolvesAliases          = YES;
    completionHandler([panel runModal] == NSModalResponseOK ? panel.URLs : nil);
}

// ---- downloads

- (void)download:(WKDownload *)download
    decideDestinationUsingResponse:(NSURLResponse *)response
                 suggestedFilename:(NSString *)suggestedFilename
                 completionHandler:(void (^)(NSURL *))completionHandler
{
    if (impl != nullptr)
        impl->decide_destination(download, response, suggestedFilename, completionHandler);
    else
        completionHandler(nil);
}

- (void)download:(WKDownload *)download
    willPerformHTTPRedirection:(NSHTTPURLResponse *)response
                    newRequest:(NSURLRequest *)request
             decisionHandler:(void (^)(WKDownloadRedirectPolicy))decisionHandler
{
    // A download that redirects to this computer or the LAN stops there.
    const std::string url = url_string(request.URL);
    const NavDecision d   = decide_navigation(url, std::string(), false);
    if (impl == nullptr || d.action != NavAction::Allow) {
        BOOST_LOG_TRIVIAL(warning) << "ModelBrowser: a download redirect was refused (" << d.reason << ")";
        decisionHandler(WKDownloadRedirectPolicyCancel);
        return;
    }
    decisionHandler(WKDownloadRedirectPolicyAllow);
}

- (void)downloadDidFinish:(WKDownload *)download
{
    if (impl != nullptr)
        impl->on_download_finished(download);
}

- (void)download:(WKDownload *)download didFailWithError:(NSError *)error resumeData:(NSData *)resumeData
{
    if (impl != nullptr)
        impl->on_download_failed(download, error);
}

// ---- address, history, popup titles

- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object change:(NSDictionary *)change context:(void *)context
{
    if (context == KVO_URL || context == KVO_HISTORY || context == KVO_TITLE) {
        if (impl != nullptr && [object isKindOfClass:[WKWebView class]])
            impl->on_kvo((WKWebView *) object, context);
        return;
    }
    [super observeValueForKeyPath:keyPath ofObject:object change:change context:context];
}

@end

namespace Slic3r {
namespace GUI {

// ------------------------------------------------------------------------------------- UI ----

void ModelBrowserPanel::Impl::build_ui()
{
    wxWindow* panel = owner;
    auto*     main  = new wxBoxSizer(wxVERTICAL);

    toolbar = new wxPanel(panel, wxID_ANY);
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    auto  compact = [this](const wxString& label, const wxString& tip) {
        auto* b = new Button(toolbar, label);
        b->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
        if (!tip.empty())
            b->SetToolTip(tip);
        return b;
    };
    btn_leave   = compact(_L("Back to Home"), _L("Back to Recent, Library and the other Home sections"));
    btn_back    = compact(_L("Back"), _L("Go back"));
    btn_forward = compact(_L("Forward"), _L("Go forward"));
    btn_reload  = compact(_L("Reload"), _L("Reload the page"));
    btn_home    = compact(_L("Home"), _L("The site's home page"));
    const int gap = owner->FromDIP(4);
    row->Add(btn_leave, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, owner->FromDIP(8));
    row->AddSpacer(owner->FromDIP(12));
    for (Button* b : {btn_back, btn_forward, btn_reload, btn_home})
        row->Add(b, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
    row->AddSpacer(owner->FromDIP(12));
    for (const Site& s : sites()) {
        Button* b = compact(from_u8(s.name), wxString());
        const std::string id = s.id;
        b->Bind(wxEVT_BUTTON, [this, id](wxCommandEvent&) { go_site(id); });
        site_buttons.push_back(b);
        row->Add(b, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
    }
    address = new wxStaticText(toolbar, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
    address->SetFont(Label::Body_12);
    row->Add(address, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, owner->FromDIP(10));
    status = new wxStaticText(toolbar, wxID_ANY, wxEmptyString);
    status->SetFont(Label::Body_12);
    row->Add(status, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, owner->FromDIP(8));
    btn_external = compact(_L("Open in browser"), _L("Open this page in your web browser"));
    row->Add(btn_external, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, owner->FromDIP(8));
    auto* tb_sizer = new wxBoxSizer(wxVERTICAL);
    tb_sizer->Add(row, 0, wxEXPAND | wxTOP | wxBOTTOM, owner->FromDIP(6));
    toolbar->SetSizer(tb_sizer);

    notice      = new wxPanel(panel, wxID_ANY);
    notice_text = new wxStaticText(notice, wxID_ANY, wxEmptyString);
    notice_text->SetFont(Label::Body_13);
    notice_action = new Button(notice, wxEmptyString);
    notice_action->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    notice_close = new Button(notice, _L("Close"));
    notice_close->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    auto* nrow = new wxBoxSizer(wxHORIZONTAL);
    nrow->Add(notice_text, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, owner->FromDIP(12));
    nrow->Add(notice_action, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, owner->FromDIP(8));
    nrow->Add(notice_close, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, owner->FromDIP(8));
    auto* nsizer = new wxBoxSizer(wxVERTICAL);
    nsizer->Add(nrow, 0, wxEXPAND | wxTOP | wxBOTTOM, owner->FromDIP(6));
    notice->SetSizer(nsizer);
    notice->Hide();

    placeholder      = new wxPanel(panel, wxID_ANY);
    placeholder_text = new wxStaticText(placeholder, wxID_ANY, _L("Starting the model browser..."), wxDefaultPosition, wxDefaultSize,
                                        wxALIGN_CENTRE_HORIZONTAL);
    placeholder_text->SetFont(Label::Body_14);
    btn_retry = new Button(placeholder, _L("Retry"));
    btn_retry->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    btn_retry->Hide();
    auto* psizer = new wxBoxSizer(wxVERTICAL);
    psizer->AddStretchSpacer(1);
    psizer->Add(placeholder_text, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, owner->FromDIP(24));
    psizer->Add(btn_retry, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, owner->FromDIP(12));
    psizer->AddStretchSpacer(2);
    placeholder->SetSizer(psizer);

    // The WKWebView is the only subview of this window's NSView; nothing else is drawn in it.
    host = new wxWindow(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    host->Hide();

    main->Add(toolbar, 0, wxEXPAND);
    main->Add(notice, 0, wxEXPAND);
    main->Add(placeholder, 1, wxEXPAND);
    main->Add(host, 1, wxEXPAND);
    panel->SetSizer(main);

    btn_leave->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (on_leave)
            on_leave();
    });
    btn_back->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (view != nil)
            [view goBack];
    });
    btn_forward->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (view != nil)
            [view goForward];
    });
    btn_reload->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (view != nil)
            [view reload];
        else if (!creating)
            create_browser();
    });
    btn_home->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { go_site(site_id); });
    btn_external->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const std::string url = current_url();
        if (untrusted::is_safe_to_open_externally(url) && url.rfind("mailto:", 0) != 0)
            wxLaunchDefaultBrowser(from_u8(url));
    });
    btn_retry->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { create_browser(); });
    notice_close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { hide_notice(); });
    notice_action->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        auto cb = notice_callback;
        hide_notice();
        if (cb)
            cb();
    });
    notice_timer.SetOwner(owner);
    owner->Bind(wxEVT_TIMER, [this](wxTimerEvent&) { hide_notice(); }, notice_timer.GetId());
    progress_timer.SetOwner(owner);
    owner->Bind(wxEVT_TIMER, [this](wxTimerEvent&) { update_progress(); }, progress_timer.GetId());

    host->Bind(wxEVT_SIZE, [this](wxSizeEvent& evt) {
        evt.Skip();
        resize();
    });
    host->Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& evt) {
        evt.Skip();
        if (view != nil && view.window != nil)
            [view.window makeFirstResponder:view];
    });

    apply_colours();
    update_nav_buttons();
}

void ModelBrowserPanel::Impl::apply_colours()
{
    const wxColour bg     = StateColor::darkModeColorFor(wxColour("#FFFFFF"));
    const wxColour bar    = StateColor::darkModeColorFor(wxColour("#F8F8F8"));
    const wxColour text   = StateColor::darkModeColorFor(wxColour("#262E30"));
    const wxColour muted  = StateColor::darkModeColorFor(wxColour("#6B6B6B"));
    owner->SetBackgroundColour(bg);
    toolbar->SetBackgroundColour(bar);
    address->SetForegroundColour(muted);
    status->SetForegroundColour(muted);
    apply_notice_colours();
    placeholder->SetBackgroundColour(bg);
    placeholder_text->SetForegroundColour(text);
    host->SetBackgroundColour(bg);
    for (Button* b : {btn_leave, btn_back, btn_forward, btn_reload, btn_home, btn_external, notice_action, notice_close, btn_retry})
        b->Rescale();
    for (size_t i = 0; i < site_buttons.size(); ++i) {
        const bool current = sites()[i].id == site_id;
        site_buttons[i]->SetStyle(current ? ButtonStyle::Confirm : ButtonStyle::Regular, ButtonType::Compact);
        site_buttons[i]->Rescale();
    }
    toolbar->Layout();
    owner->Layout();
    owner->Refresh();
    apply_theme_to_browser();
}

void ModelBrowserPanel::Impl::update_nav_buttons()
{
    btn_back->Enable(view != nil && view.canGoBack);
    btn_forward->Enable(view != nil && view.canGoForward);
    btn_reload->Enable(view != nil);
    btn_home->Enable(view != nil);
    btn_external->Enable(view != nil);
}

void ModelBrowserPanel::Impl::update_address(const std::string& url)
{
    wxString shown;
    if (url.rfind("https://", 0) == 0)
        shown = from_u8(url.substr(8));
    else if (url.rfind("http://", 0) == 0)
        shown = _L("Not secure") + "  " + from_u8(url.substr(7));
    else
        shown = from_u8(url);
    address->SetLabel(shown);
    address->SetToolTip(from_u8(url));
    // The site button follows the page.
    if (const Site* s = site_for_url(url)) {
        if (s->id != site_id) {
            site_id = s->id;
            apply_colours();
        }
    }
}

void ModelBrowserPanel::Impl::show_placeholder(const wxString& text, bool retry)
{
    placeholder_text->SetLabel(text);
    placeholder_text->Wrap(owner->FromDIP(520));
    btn_retry->Show(retry);
    placeholder->Show();
    host->Hide();
    owner->Layout();
}

void ModelBrowserPanel::Impl::apply_notice_colours()
{
    const char* bg = notice_kind == NoticeKind::Error ? "#FDE8E8" :   // error banner background
                     notice_kind == NoticeKind::Warning ? "#FFF3EB" : // warning banner background
                                                          "#F3F4F6";  // card / divider grey
    notice->SetBackgroundColour(StateColor::darkModeColorFor(wxColour(bg)));
    notice_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#262E30")));
    notice->Refresh();
    notice_text->Refresh();
}

void ModelBrowserPanel::Impl::show_notice(const wxString& text, NoticeKind kind, const wxString& action_label, std::function<void()> action)
{
    notice_kind = kind;
    apply_notice_colours();
    notice_text->SetLabel(text);
    notice_text->Wrap((std::max)(owner->GetClientSize().x - owner->FromDIP(260), owner->FromDIP(300)));
    notice_callback = std::move(action);
    notice_action->SetLabel(action_label);
    notice_action->Show(!action_label.empty() && notice_callback != nullptr);
    notice->Show();
    owner->Layout();
    notice_timer.StartOnce(20000);
}

void ModelBrowserPanel::Impl::hide_notice()
{
    notice_timer.Stop();
    notice_callback = nullptr;
    if (notice->IsShown()) {
        notice->Hide();
        owner->Layout();
    }
}

// --------------------------------------------------------------------------------- browser ----

void ModelBrowserPanel::Impl::fail(const wxString& why)
{
    creating = false;
    close_browser();
    BOOST_LOG_TRIVIAL(error) << "ModelBrowser: " << into_u8(why);
    show_placeholder(why, true);
    update_nav_buttons();
}

void ModelBrowserPanel::Impl::create_browser()
{
    if (creating || view != nil)
        return;
    if (@available(macOS 12.0, *)) {
        creating = true;
        show_placeholder(_L("Starting the model browser..."), false);
        if (rules != nil) {
            create_browser_with_rules();
            return;
        }
        // The rules that keep this computer and the LAN out of reach come first: no rules, no browser.
        auto alive_ = alive;
        [[WKContentRuleListStore defaultStore]
            compileContentRuleListForIdentifier:@"EdgeSlicer.ModelBrowser.BlockLocal"
                         encodedContentRuleList:to_ns(content_rule_list_json())
                              completionHandler:^(WKContentRuleList* list, NSError* error) {
                                  if (!*alive_)
                                      return;
                                  if (list == nil || error != nil) {
                                      BOOST_LOG_TRIVIAL(error) << "ModelBrowser: the blocking rules did not compile: "
                                                               << to_std(error.localizedDescription);
                                      fail(_L("The model browser was not started because its isolation could not be confirmed."));
                                      return;
                                  }
                                  rules = [list retain];
                                  create_browser_with_rules();
                              }];
    } else {
        fail(_L("The model browser needs macOS 12 or later."));
    }
}

// Settings for the main view. Popups get WebKit's copy of it (the opener's configuration), so the
// same store, content controller and rules.
WKWebViewConfiguration* ModelBrowserPanel::Impl::make_configuration()
{
    WKWebViewConfiguration* cfg = [[[WKWebViewConfiguration alloc] init] autorelease];
    cfg.websiteDataStore        = store;
    cfg.userContentController   = content;
    cfg.preferences.javaScriptCanOpenWindowsAutomatically = NO;
    cfg.preferences.fraudulentWebsiteWarningEnabled       = YES;
    cfg.defaultWebpagePreferences.allowsContentJavaScript = YES;
    // "<WebKit's default User-Agent> EdgeSlicer/<version>": honest, nothing that passes for another app.
    cfg.applicationNameForUserAgent = to_ns(browser_user_agent(std::string(), Snapmaker_VERSION));
    return cfg;
}

void ModelBrowserPanel::Impl::create_browser_with_rules()
{
    // The store: never the default one (the app's own web views and their sign-ins live there).
    if (store == nil) {
        if (@available(macOS 14.0, *)) {
            AppConfig* config = wxGetApp().app_config;
            NSUUID*    uuid   = nil;
            const std::string saved = config->get("model_browser_store_id");
            if (!saved.empty())
                uuid = [[[NSUUID alloc] initWithUUIDString:to_ns(saved)] autorelease];
            if (uuid == nil) {
                uuid = [NSUUID UUID];
                config->set("model_browser_store_id", to_std(uuid.UUIDString)); // saved on the next idle
            }
            store            = [[WKWebsiteDataStore dataStoreForIdentifier:uuid] retain];
            persistent_store = true;
        } else {
            // macOS 12-13 have no persistent store besides the default one: an isolated store
            // that lasts until EdgeSlicer quits (sign in again after a restart).
            store            = [[WKWebsiteDataStore nonPersistentDataStore] retain];
            persistent_store = false;
        }
    }
    if (store == nil || store == [WKWebsiteDataStore defaultDataStore]) {
        fail(_L("The model browser was not started because its isolation could not be confirmed."));
        return;
    }
    if (content == nil) {
        content = [[WKUserContentController alloc] init];
        [content addContentRuleList:rules];
    }

    NSView* parent = (NSView*) host->GetHandle();
    if (parent == nil) {
        fail(_L("The model browser could not start."));
        return;
    }
    view = [[EdgeSlicerModelBrowserWebView alloc] initWithFrame:parent.bounds configuration:make_configuration()];
    if (!verify_isolation(view)) {
        // Never show a view that could talk to the app or load what the rules keep out.
        fail(_L("The model browser was not started because its isolation could not be confirmed."));
        return;
    }
    if (delegate == nil) {
        EdgeSlicerModelBrowserDelegate* d = [[EdgeSlicerModelBrowserDelegate alloc] init];
        d->impl  = this;
        delegate = d;
    }
    view.navigationDelegate = (id<WKNavigationDelegate>) delegate;
    view.UIDelegate         = (id<WKUIDelegate>) delegate;
    view.allowsBackForwardNavigationGestures = YES;
    view.autoresizingMask   = NSViewWidthSizable | NSViewHeightSizable;
    if (@available(macOS 13.3, *)) {
#ifdef NDEBUG
        view.inspectable = NO;
#else
        view.inspectable = wxGetApp().app_config->get_bool("developer_mode") ? YES : NO;
#endif
    }
    [view addObserver:delegate forKeyPath:@"URL" options:NSKeyValueObservingOptionNew context:KVO_URL];
    [view addObserver:delegate forKeyPath:@"canGoBack" options:NSKeyValueObservingOptionNew context:KVO_HISTORY];
    [view addObserver:delegate forKeyPath:@"canGoForward" options:NSKeyValueObservingOptionNew context:KVO_HISTORY];
    observing = true;
    [parent addSubview:view];

    BOOST_LOG_TRIVIAL(info) << "ModelBrowser: started with " << (persistent_store ? "its own persistent" : "an isolated non-persistent")
                            << " website data store";
    creating = false;
    placeholder->Hide();
    host->Show();
    owner->Layout();
    apply_colours();
    resize();
    view.hidden = active ? NO : YES;
    update_nav_buttons();
    go_site(site_id);
    show_passkey_note();
}

// WKWebView cannot do passkeys (that needs Apple entitlements this app cannot get), so tell the user
// once, the first time the browser opens in this data folder, what to use instead.
void ModelBrowserPanel::Impl::show_passkey_note()
{
    AppConfig* config = wxGetApp().app_config;
    if (config == nullptr || config->get_bool("model_browser_passkey_note_shown"))
        return;
    config->set_bool("model_browser_passkey_note_shown", true);
    show_notice(_L("Passkeys aren't available in the model browser on macOS; sign in with a password, an email code or Google/Apple."),
                NoticeKind::Info);
}

// Reads back what a page could use to reach the app or this computer. Returns false unless the
// view provably has no bridge: our content controller (no script message handlers - the API has no
// way to list them, so they are removed outright - and no user scripts), our store, never the
// default one, and the blocking rules.
bool ModelBrowserPanel::Impl::verify_isolation(WKWebView* v) const
{
    if (v == nil || ![v isKindOfClass:[EdgeSlicerModelBrowserWebView class]])
        return false;
    WKWebViewConfiguration* cfg = v.configuration;
    WKUserContentController* ucc = cfg.userContentController;
    if (ucc != content || cfg.websiteDataStore != store || store == [WKWebsiteDataStore defaultDataStore])
        return false;
    if (@available(macOS 11.0, *))
        [ucc removeAllScriptMessageHandlers];
    const bool ok = ucc.userScripts.count == 0 && !cfg.preferences.javaScriptCanOpenWindowsAutomatically;
    assert(ok);
    if (!ok)
        BOOST_LOG_TRIVIAL(error) << "ModelBrowser: a view had user scripts or could open windows by itself";
    return ok;
}

void ModelBrowserPanel::Impl::close_browser()
{
    for (Popup& p : popups) {
        if (p.view != nil) {
            p.view.navigationDelegate = nil;
            p.view.UIDelegate         = nil;
            [p.view removeObserver:delegate forKeyPath:@"title" context:KVO_TITLE];
            [p.view stopLoading];
            [p.view removeFromSuperview];
            [p.view release];
            p.view = nil;
        }
        if (wxFrame* f = p.frame.get())
            f->Destroy();
    }
    popups.clear();
    if (view != nil) {
        if (observing) {
            [view removeObserver:delegate forKeyPath:@"URL" context:KVO_URL];
            [view removeObserver:delegate forKeyPath:@"canGoBack" context:KVO_HISTORY];
            [view removeObserver:delegate forKeyPath:@"canGoForward" context:KVO_HISTORY];
            observing = false;
        }
        view.navigationDelegate = nil;
        view.UIDelegate         = nil;
        [view stopLoading];
        [view removeFromSuperview];
        [view release];
        view = nil;
    }
}

void ModelBrowserPanel::Impl::apply_theme(WKWebView* v)
{
    if (v == nil)
        return;
    // prefers-color-scheme follows the view's appearance.
    v.appearance = [NSAppearance appearanceNamed:wxGetApp().dark_mode() ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
    if (@available(macOS 12.0, *))
        v.underPageBackgroundColor = to_ns_colour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
}

void ModelBrowserPanel::Impl::apply_theme_to_browser()
{
    apply_theme(view);
    for (Popup& p : popups)
        apply_theme(p.view);
}

void ModelBrowserPanel::Impl::resize()
{
    if (view == nil || host == nullptr)
        return;
    NSView* parent = (NSView*) host->GetHandle();
    if (parent != nil)
        view.frame = parent.bounds;
}

void ModelBrowserPanel::Impl::navigate(const std::string& url)
{
    if (view == nil)
        return;
    if (decide_navigation(url, current_url(), true).action != NavAction::Allow)
        return;
    NSURL* u = [NSURL URLWithString:to_ns(url)];
    if (u != nil)
        [view loadRequest:[NSURLRequest requestWithURL:u]];
}

void ModelBrowserPanel::Impl::go_site(const std::string& id)
{
    const Site* s = site_by_id(id);
    if (s == nullptr)
        return;
    site_id = s->id;
    wxGetApp().app_config->set("model_browser_site", site_id);
    apply_colours();
    hide_notice();
    if (view != nil)
        navigate(s->start_url);
    else if (!creating)
        create_browser();
}

// ------------------------------------------------------------------------------ navigation ----

WKNavigationActionPolicy ModelBrowserPanel::Impl::decide_action(WKWebView* sender, WKNavigationAction* action)
{
    const std::string url  = url_string(action.request.URL);
    const std::string page = url_string(sender.URL);
    WKFrameInfo*      target = action.targetFrame;
    // No target frame: a link that opens a new window. Its target is checked here like a page of
    // its own, and the window itself by createWebViewWithConfiguration (decide_new_window).
    const bool main_frame = target == nil || target.isMainFrame;
    if (target == nil && (url.empty() || url == "about:blank"))
        return WKNavigationActionPolicyAllow;

    const NavDecision d = decide_navigation(url, page, main_frame);
    if (d.action != NavAction::Allow) {
        // Cancelled whatever the scheme: WebKit never hands a link to another program (and nothing
        // here calls NSWorkspace). "Open in" links go to the app's downloader instead.
        bool visible = true;
        if (!main_frame) {
            // An "Open in" button may navigate a hidden frame; that is the user's click, so a refused
            // app link is reported. Blocked web frames (ads, probes) are not.
            const bool app_link = url.rfind("http", 0) != 0 && url.rfind("about:", 0) != 0 && url.rfind("blob:", 0) != 0 &&
                                  url.rfind("data:", 0) != 0;
            visible = d.action != NavAction::Block || app_link;
        }
        carry_out(d, url, visible);
        return WKNavigationActionPolicyCancel;
    }
    // <a download> and the like.
    if (action.shouldPerformDownload)
        return WKNavigationActionPolicyDownload;
    return WKNavigationActionPolicyAllow;
}

WKNavigationResponsePolicy ModelBrowserPanel::Impl::decide_response(WKWebView* sender, WKNavigationResponse* response)
{
    // The answer's own address (after redirects) is checked again.
    const std::string url = url_string(response.response.URL);
    const NavDecision d   = decide_navigation(url, url_string(sender.URL), response.isForMainFrame);
    if (d.action != NavAction::Allow) {
        carry_out(d, url, response.isForMainFrame);
        return WKNavigationResponsePolicyCancel;
    }
    bool attachment = false;
    if ([response.response isKindOfClass:[NSHTTPURLResponse class]]) {
        NSString* disposition = [((NSHTTPURLResponse*) response.response) valueForHTTPHeaderField:@"Content-Disposition"];
        attachment            = disposition != nil && [[disposition lowercaseString] hasPrefix:@"attachment"];
    }
    if (attachment || !response.canShowMIMEType)
        return WKNavigationResponsePolicyDownload;
    return WKNavigationResponsePolicyAllow;
}

// What a refused navigation turns into.
void ModelBrowserPanel::Impl::carry_out(const NavDecision& d, const std::string& url, bool user_visible)
{
    switch (d.action) {
    case NavAction::Allow: return;
    case NavAction::OpenLink: {
        // Pages may fire a link twice (location + iframe): one download per click.
        const auto now = std::chrono::steady_clock::now();
        if (d.open_link == last_app_link && now - last_app_link_time < std::chrono::seconds(3))
            return;
        last_app_link      = d.open_link;
        last_app_link_time = now;
        BOOST_LOG_TRIVIAL(info) << "ModelBrowser: \"Open in\" link handed to the downloader (" << d.reason << ")";
        const std::string link = d.open_link;
        owner->CallAfter([link]() { wxGetApp().start_download(link); });
        return;
    }
    case NavAction::Block: {
        untrusted::Url u;
        const bool     web = untrusted::parse_url(url, u);
        BOOST_LOG_TRIVIAL(warning) << "ModelBrowser: blocked a navigation (" << d.reason << ")" << (web ? " to " + u.host : std::string());
        if (!user_visible)
            return;
        const wxString text = (web && is_blocked_host(u.host)) ?
                                  _L("The model browser does not open addresses on this computer or your local network.") :
                                  format_wxstr(_L("The model browser did not open this link: %1%"), from_u8(d.reason));
        auto alive_ = alive;
        owner->CallAfter([this, alive_, text]() {
            if (*alive_)
                show_notice(text, NoticeKind::Warning);
        });
        return;
    }
    }
}

void ModelBrowserPanel::Impl::on_load_failed(WKWebView* sender, NSError* error)
{
    if (error == nil)
        return;
    const std::string domain = to_std(error.domain);
    const long        code   = long(error.code);
    // Cancelled loads, and the ones our own policy stopped (102: a navigation cancelled or turned
    // into a download), say nothing more.
    if ((domain == to_std(NSURLErrorDomain) && code == NSURLErrorCancelled) || (domain == "WebKitErrorDomain" && (code == 102 || code == 204)))
        return;
    BOOST_LOG_TRIVIAL(warning) << "ModelBrowser: a page did not load (" << domain << " " << code << ")";
    if (sender != view)
        return;
    const wxString why = from_u8(to_std(error.localizedDescription));
    auto alive_ = alive;
    owner->CallAfter([this, alive_, why]() {
        if (*alive_)
            show_notice(format_wxstr(_L("The page could not be loaded: %1%"), why), NoticeKind::Warning);
    });
}

void ModelBrowserPanel::Impl::on_content_process_ended(WKWebView* sender)
{
    BOOST_LOG_TRIVIAL(warning) << "ModelBrowser: a page's web content process ended";
    if (sender == view)
        [view reload];
    else
        close_popup_of(sender);
}

void ModelBrowserPanel::Impl::on_kvo(WKWebView* sender, void* context)
{
    if (context == KVO_URL && sender == view)
        update_address(current_url());
    else if (context == KVO_HISTORY && sender == view)
        update_nav_buttons();
    else if (context == KVO_TITLE) {
        for (Popup& p : popups)
            if (p.view == sender)
                if (wxFrame* f = p.frame.get()) {
                    const std::string title = to_std(sender.title);
                    if (!title.empty())
                        f->SetTitle(from_u8(title));
                }
    }
}

// ------------------------------------------------------------------------------ new windows ----

// window.open() / target=_blank. With javaScriptCanOpenWindowsAutomatically off, WebKit's popup
// blocker only lets a page open a window from a user gesture, so whatever reaches this was started
// by the user.
WKWebView* ModelBrowserPanel::Impl::create_popup(WKWebView* sender, WKWebViewConfiguration* configuration, WKNavigationAction* action,
                                                 WKWindowFeatures* features)
{
    const std::string target = url_string(action.request.URL);
    bool   sized = false;
    wxSize size  = owner->FromDIP(wxSize(520, 720));
    if (features != nil) {
        sized = features.width != nil || features.height != nil || features.x != nil || features.y != nil;
        if (features.width != nil && features.height != nil) {
            const int w = features.width.intValue, h = features.height.intValue;
            if (w >= 200 && h >= 200 && w <= 2000 && h <= 2000)
                size = owner->FromDIP(wxSize(w, h));
        }
    }
    const WindowDecision w = decide_new_window(target, url_string(sender.URL), true, sized);
    BOOST_LOG_TRIVIAL(info) << "ModelBrowser: new window, " << w.reason;
    switch (w.action) {
    case WindowAction::Popup: {
        // The popup is made from the configuration WebKit passes - the opener's, so the same isolated
        // store, content controller and rules - and checked like the main view.
        wxWindow* top   = wxGetTopLevelParent(owner);
        auto*     frame = new wxFrame(top, wxID_ANY, _L("Sign in"), wxDefaultPosition, size, wxDEFAULT_FRAME_STYLE | wxFRAME_FLOAT_ON_PARENT);
        frame->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        auto* body = new wxWindow(frame, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(body, 1, wxEXPAND);
        frame->SetSizer(sizer);
        frame->Layout();
        NSView* parent = (NSView*) body->GetHandle();
        WKWebView* v = parent != nil ? [[EdgeSlicerModelBrowserWebView alloc] initWithFrame:parent.bounds configuration:configuration] : nil;
        if (!verify_isolation(v)) {
            [v release];
            frame->Destroy();
            BOOST_LOG_TRIVIAL(error) << "ModelBrowser: a popup could not be created safely";
            return nil;
        }
        v.navigationDelegate = (id<WKNavigationDelegate>) delegate;
        v.UIDelegate         = (id<WKUIDelegate>) delegate;
        v.autoresizingMask   = NSViewWidthSizable | NSViewHeightSizable;
        if (@available(macOS 13.3, *))
            v.inspectable = NO;
        [v addObserver:delegate forKeyPath:@"title" options:NSKeyValueObservingOptionNew context:KVO_TITLE];
        apply_theme(v);
        [parent addSubview:v];
        Popup p;
        p.frame = frame;
        p.view  = v; // retained by alloc, released by close_popup
        popups.push_back(p);

        auto alive_ = alive;
        frame->Bind(wxEVT_CLOSE_WINDOW, [this, alive_, frame](wxCloseEvent&) {
            if (!*alive_ || !close_popup(frame))
                frame->Destroy();
        });
        body->Bind(wxEVT_SIZE, [body](wxSizeEvent& evt) {
            evt.Skip();
            NSView* parent = (NSView*) body->GetHandle();
            for (NSView* sub in parent.subviews)
                sub.frame = parent.bounds;
        });
        frame->CentreOnParent();
        frame->Show();
        return v; // WebKit loads the request in it
    }
    case WindowAction::NavigateInPlace:
        navigate(target);
        return nil;
    case WindowAction::SystemBrowser:
        // A web page outside the model sites, in the user's own web browser (http/https only).
        if (untrusted::is_safe_to_open_externally(target) && target.rfind("mailto:", 0) != 0) {
            const wxString url = from_u8(target);
            owner->CallAfter([url]() { wxLaunchDefaultBrowser(url); });
        }
        return nil;
    case WindowAction::AppLink:
        carry_out(w.nav, target, true);
        return nil;
    case WindowAction::Block:
    default: return nil;
    }
}

void ModelBrowserPanel::Impl::close_popup_of(WKWebView* v)
{
    for (Popup& p : popups)
        if (p.view == v) {
            wxWeakRef<wxFrame> frame = p.frame;
            auto alive_ = alive;
            // Not from inside WebKit's callback.
            owner->CallAfter([this, alive_, frame]() {
                if (*alive_ && frame.get() != nullptr)
                    close_popup(frame.get());
            });
            return;
        }
}

bool ModelBrowserPanel::Impl::close_popup(wxFrame* frame)
{
    for (auto it = popups.begin(); it != popups.end(); ++it)
        if (it->frame.get() == frame) {
            WKWebView* v = it->view;
            popups.erase(it);
            if (v != nil) {
                v.navigationDelegate = nil;
                v.UIDelegate         = nil;
                [v removeObserver:delegate forKeyPath:@"title" context:KVO_TITLE];
                [v stopLoading];
                [v removeFromSuperview];
                [v release];
            }
            frame->Destroy();
            return true;
        }
    return false;
}

// -------------------------------------------------------------------------------- downloads ----

int ModelBrowserPanel::Impl::download_id(id op) const
{
    for (const auto& [id_, dl] : downloads)
        if (dl.op == op)
            return id_;
    return 0;
}

void ModelBrowserPanel::Impl::adopt_download(WKDownload* download, WKWebView* sender)
{
    download.delegate = (id<WKDownloadDelegate>) delegate;
    Download dl;
    dl.op     = [download retain];
    dl.source = url_string(sender.URL); // the page that started it, until the response is known
    downloads[next_download++] = std::move(dl);
}

void ModelBrowserPanel::Impl::decide_destination(WKDownload* download, NSURLResponse* response, NSString* suggested, void (^handler)(NSURL*))
{
    const int id_ = download_id(download);
    if (id_ == 0) {
        handler(nil);
        return;
    }
    const std::string page = downloads[id_].source;
    NSURL*            from = response.URL != nil ? response.URL : download.originalRequest.URL;
    const std::string url  = url_string(from);
    const long long   total = response.expectedContentLength; // NSURLResponseUnknownLength (-1) when unknown
    const DownloadDecision d = decide_download(url, to_std(suggested), page, total);
    untrusted::Url u;
    const std::string host = untrusted::parse_url(url, u) ? u.host : untrusted::parse_url(page, u) ? u.host : std::string();
    BOOST_LOG_TRIVIAL(info) << "ModelBrowser: download of \"" << d.file_name << "\" from " << host << ": " << d.reason;
    downloads[id_].source = url;

    auto drop = [this, id_]() {
        auto it = downloads.find(id_);
        if (it != downloads.end()) {
            [it->second.op release];
            downloads.erase(it);
        }
    };
    switch (d.action) {
    case DownloadAction::Import: {
        NSURL* dest = nil;
        const fs::path folder = model_folder();
        if (folder.empty() || !begin_download(download, d.file_name, folder, true, &dest)) {
            handler(nil);
            drop();
            show_notice(format_wxstr(_L("\"%1%\" could not be saved: no usable download folder."), from_u8(d.file_name)), NoticeKind::Error);
            return;
        }
        handler(dest);
        return;
    }
    case DownloadAction::Refuse:
        handler(nil);
        drop();
        show_notice(format_wxstr(_L("The download of \"%1%\" was refused: %2%"), from_u8(d.file_name), from_u8(d.reason)), NoticeKind::Error);
        return;
    case DownloadAction::ImportAfterConfirm:
    case DownloadAction::OfferSave: {
        // Ask outside WebKit's callback; the download waits for the answer.
        pending_destinations[id_] = [handler copy];
        const bool        to_import = d.action == DownloadAction::ImportAfterConfirm;
        const std::string name      = d.file_name;
        auto              alive_    = alive;
        owner->CallAfter([this, alive_, id_, to_import, name, host]() {
            if (!*alive_)
                return; // ~Impl answered nil
            auto pending = pending_destinations.find(id_);
            if (pending == pending_destinations.end())
                return;
            const fs::path folder = to_import ? model_folder() : downloads_folder();
            const wxString msg = to_import ?
                format_wxstr(_L("\"%1%\" comes from %2%, which is not one of the model sites EdgeSlicer knows.\n\n"
                                "Only open it if you trust this site. Download and open it?"), from_u8(name), from_u8(host)) :
                format_wxstr(_L("\"%1%\" is not a model file.\n\nSave it to your Downloads folder?"), from_u8(name));
            MessageDialog dlg(wxGetTopLevelParent(owner), msg, to_import ? _L("Download from an unknown site") : _L("Save file"),
                              wxYES_NO | wxICON_QUESTION);
            dlg.SetButtonLabel(wxID_YES, to_import ? _L("Download and open") : _L("Save"));
            dlg.SetButtonLabel(wxID_NO, _L("Cancel"), true);
            const bool yes = dlg.ShowModal() == wxID_YES;
            if (!*alive_)
                return;
            pending = pending_destinations.find(id_);
            if (pending == pending_destinations.end())
                return;
            void (^answer)(NSURL*) = pending->second;
            pending_destinations.erase(pending);
            auto it = downloads.find(id_);
            NSURL* dest = nil;
            if (@available(macOS 12.0, *)) {
                if (yes && it != downloads.end() && !folder.empty())
                    begin_download((WKDownload*) it->second.op, name, folder, to_import, &dest);
            }
            answer(dest);
            [answer release];
            if (dest == nil && it != downloads.end()) {
                [it->second.op release];
                downloads.erase(it);
            }
        });
        return;
    }
    }
    handler(nil);
    drop();
}

// Claims a name in the folder (exclusive marker, the same scheme as the "Open in" downloads) and
// points WebKit at a temporary file next to it; the file is checked and moved into place without
// replacing anything when it is complete.
bool ModelBrowserPanel::Impl::begin_download(WKDownload* download, const std::string& name, const fs::path& folder, bool to_import, NSURL** destination)
{
    const int id_ = download_id(download);
    if (id_ == 0)
        return false;
    std::string claimed;
    FILE*       marker_file = untrusted::claim_unused_download_name(folder, name, {}, claimed);
    if (marker_file == nullptr)
        return false;
    fclose(marker_file);

    Download& dl = downloads[id_];
    dl.folder    = folder;
    dl.name      = claimed;
    dl.to_import = to_import;
    dl.marker    = untrusted::download_marker_path(folder, claimed);
    boost::system::error_code ec;
    for (int i = 0; i < 100; ++i) {
        fs::path part = dl.marker;
        part += i == 0 ? std::string(".part") : ".part" + std::to_string(i);
        if (!fs::exists(fs::symlink_status(part, ec))) {
            dl.part = part;
            break;
        }
    }
    if (dl.part.empty()) {
        fs::remove(dl.marker, ec);
        return false;
    }
    dl.started   = true;
    *destination = [NSURL fileURLWithPath:to_ns(dl.part.string()) isDirectory:NO];
    status->SetLabel(format_wxstr(_L("Downloading %1%..."), from_u8(claimed)));
    toolbar->Layout();
    if (!progress_timer.IsRunning())
        progress_timer.Start(500);
    return true;
}

void ModelBrowserPanel::Impl::update_progress()
{
    if (@available(macOS 12.0, *)) {
        for (auto& [id_, dl] : downloads) {
            if (!dl.started)
                continue;
            NSProgress* progress = ((WKDownload*) dl.op).progress;
            const long long got = progress.completedUnitCount, total = progress.totalUnitCount;
            status->SetLabel(total > 0 ? format_wxstr(_L("Downloading %1%... %2%%%"), from_u8(dl.name), int(std::min(got, total) * 100 / total)) :
                                         format_wxstr(_L("Downloading %1%... %2% MB"), from_u8(dl.name), int(got / (1024 * 1024))));
            toolbar->Layout();
            return;
        }
    }
    progress_timer.Stop();
}

void ModelBrowserPanel::Impl::on_download_finished(WKDownload* download)
{
    const int id_ = download_id(download);
    if (id_ == 0)
        return;
    Download dl = std::move(downloads[id_]);
    downloads.erase(id_);
    [dl.op release];
    dl.op = nil;
    if (downloads.empty()) {
        status->SetLabel(wxEmptyString);
        progress_timer.Stop();
    }
    finish_download(std::move(dl));
}

void ModelBrowserPanel::Impl::on_download_failed(WKDownload* download, NSError* error)
{
    const int id_ = download_id(download);
    if (id_ == 0)
        return;
    Download dl = std::move(downloads[id_]);
    downloads.erase(id_);
    [dl.op release];
    if (downloads.empty()) {
        status->SetLabel(wxEmptyString);
        progress_timer.Stop();
    }
    boost::system::error_code ec;
    if (!dl.part.empty())
        fs::remove(dl.part, ec);
    if (!dl.marker.empty())
        fs::remove(dl.marker, ec);
    const bool cancelled = error != nil && [error.domain isEqualToString:NSURLErrorDomain] && error.code == NSURLErrorCancelled;
    BOOST_LOG_TRIVIAL(warning) << "ModelBrowser: download of \"" << dl.name << "\" stopped (" << (error != nil ? to_std(error.domain) : std::string())
                               << " " << (error != nil ? long(error.code) : 0L) << ")";
    if (dl.started && !cancelled)
        show_notice(format_wxstr(_L("The download of \"%1%\" failed."), from_u8(dl.name)), NoticeKind::Error);
}

// Marks a saved file as downloaded from the web (Gatekeeper's quarantine), as Safari would.
void ModelBrowserPanel::Impl::quarantine(const fs::path& file, const std::string& source)
{
    NSURL*               url   = [NSURL fileURLWithPath:to_ns(file.string()) isDirectory:NO];
    NSMutableDictionary* props = [NSMutableDictionary dictionary];
    props[(NSString*) kLSQuarantineTypeKey]      = (NSString*) kLSQuarantineTypeWebDownload;
    props[(NSString*) kLSQuarantineAgentNameKey] = @"EdgeSlicer";
    if (NSURL* from = [NSURL URLWithString:to_ns(source)]; from != nil && ([from.scheme isEqualToString:@"https"] || [from.scheme isEqualToString:@"http"]))
        props[(NSString*) kLSQuarantineDataURLKey] = from;
    NSError* error = nil;
    if (![url setResourceValue:props forKey:NSURLQuarantinePropertiesKey error:&error])
        BOOST_LOG_TRIVIAL(warning) << "ModelBrowser: could not mark " << file.filename().string() << " as downloaded: "
                                   << to_std(error.localizedDescription);
}

void ModelBrowserPanel::Impl::finish_download(Download dl)
{
    boost::system::error_code ec;
    const fs::path file = dl.part;
    auto drop = [&](const wxString& why) {
        fs::remove(file, ec);
        fs::remove(dl.marker, ec);
        BOOST_LOG_TRIVIAL(warning) << "ModelBrowser: dropped \"" << dl.name << "\": " << into_u8(why);
        show_notice(format_wxstr(_L("\"%1%\" was not opened: %2%"), from_u8(dl.name), why), NoticeKind::Error);
    };
    // WebKit must have written where we told it to, inside the folder.
    if (file.empty() || !untrusted::is_path_within_root(dl.folder, file)) {
        drop(_L("it was saved outside the download folder"));
        return;
    }
    if (dl.to_import) {
        const uintmax_t size = fs::file_size(file, ec);
        if (ec || size == 0) {
            drop(_L("the file is empty"));
            return;
        }
        if (size > untrusted::MODEL_DOWNLOAD_SIZE_LIMIT) {
            drop(_L("the file is larger than the 500 MB limit for models"));
            return;
        }
        std::string head(512, '\0');
        {
            boost::nowide::ifstream in(file.string(), std::ios::binary);
            in.read(&head[0], std::streamsize(head.size()));
            head.resize(size_t(std::max<std::streamsize>(in.gcount(), 0)));
        }
        std::string why;
        if (!untrusted::content_matches_extension(dl.name, head, size, &why)) {
            drop(format_wxstr(_L("its content does not match its type (%1%)"), from_u8(why)));
            return;
        }
    }
    std::string name = dl.name;
    fs::path    dest;
    if (!untrusted::place_download_file(file, dl.folder, name, dest, ec)) {
        drop(_L("it could not be moved into the download folder"));
        return;
    }
    fs::remove(dl.marker, ec);
    quarantine(dest, dl.source);
    BOOST_LOG_TRIVIAL(info) << "ModelBrowser: saved " << dest.string();
    if (dl.to_import) {
        enqueue_import(dest);
    } else {
        show_notice(format_wxstr(_L("Saved \"%1%\" to your Downloads folder."), from_u8(name)), NoticeKind::Info, _L("Show in folder"),
                    [dest]() { desktop_open_any_folderEx(into_u8(from_path(dest))); });
    }
}

// ----------------------------------------------------------------------------------- import ----

// Imports one file at a time: a project load can show dialogs, and further downloads that finish
// meanwhile wait their turn instead of starting a second load inside the first.
void ModelBrowserPanel::Impl::enqueue_import(const fs::path& path)
{
    import_queue.push_back(path);
    auto alive_ = alive;
    owner->CallAfter([this, alive_]() {
        if (*alive_)
            process_imports();
    });
}

void ModelBrowserPanel::Impl::process_imports()
{
    if (importing || import_queue.empty())
        return;
    Plater*    plater = wxGetApp().plater();
    MainFrame* frame  = wxGetApp().mainframe;
    if (plater == nullptr || frame == nullptr)
        return;
    importing           = true;
    const fs::path path = import_queue.front();
    import_queue.pop_front();
    frame->select_tab(size_t(MainFrame::tp3DEditor));
    wxGetApp().mark_web_download(path);
    wxArrayString files;
    files.Add(from_path(path));
    // from_url: the user downloaded this specific file. Projects follow the Load Behaviour setting;
    // their settings go through the untrusted-settings check (UntrustedSettingsGuard) as always.
    plater->load_files(files, /* from_url */ true);
    importing = false;
    if (!import_queue.empty()) {
        auto alive_ = alive;
        owner->CallAfter([this, alive_]() {
            if (*alive_)
                process_imports();
        });
    }
}

ModelBrowserPanel::Impl::~Impl()
{
    *alive = false;
    // From here on the delegate refuses everything by itself.
    if (delegate != nil) {
        if (@available(macOS 12.0, *))
            ((EdgeSlicerModelBrowserDelegate*) delegate)->impl = nullptr;
    }
    notice_timer.Stop();
    progress_timer.Stop();
    // Every destination handler WebKit gave us is answered exactly once.
    for (auto& [id_, answer] : pending_destinations) {
        answer(nil);
        [answer release];
    }
    pending_destinations.clear();
    for (auto& [id_, dl] : downloads) {
        if (dl.op != nil) {
            if (@available(macOS 12.0, *)) {
                ((WKDownload*) dl.op).delegate = nil;
                [(WKDownload*) dl.op cancel:nil];
            }
            [dl.op release];
        }
        boost::system::error_code ec;
        if (!dl.marker.empty())
            fs::remove(dl.marker, ec);
        if (!dl.part.empty())
            fs::remove(dl.part, ec);
    }
    downloads.clear();
    close_browser();
    [delegate release];
    delegate = nil;
    [content release];
    content = nil;
    [rules release];
    rules = nil;
    [store release];
    store = nil;
}

// ---------------------------------------------------------------------------------- panel ----

bool model_browser_supported()
{
    // WKDownload (11.3) and the media-capture permission decision (12.0).
    if (@available(macOS 12.0, *))
        return true;
    return false;
}

ModelBrowserPanel::ModelBrowserPanel(wxWindow* parent, std::function<void()> on_leave)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL)
    , p(std::make_unique<Impl>(this, std::move(on_leave)))
{
    const std::string saved = wxGetApp().app_config->get("model_browser_site");
    if (site_by_id(saved) != nullptr)
        p->site_id = saved;
    p->build_ui();
    Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent& evt) {
        evt.Skip();
        CallAfter([this]() {
            p->apply_colours();
            p->resize();
        });
    });
}

ModelBrowserPanel::~ModelBrowserPanel() = default;

void ModelBrowserPanel::set_active(bool active)
{
    p->active = active;
    if (active && p->view == nil && !p->creating)
        p->create_browser();
    if (p->view != nil)
        p->view.hidden = active ? NO : YES;
    if (active)
        p->resize();
}

void ModelBrowserPanel::sys_color_changed()
{
    p->apply_colours();
}

} // namespace GUI
} // namespace Slic3r
