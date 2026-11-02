// The Layerling tab's web view on macOS: a WKWebView of its own (macOS 12 or later, for WKDownload).
// See LayerlingBrowser.hpp for what every implementation does. Manual reference counting, like the
// other .mm files of the app.

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include "LayerlingBrowser.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Layerling.hpp"
#include "libslic3r/UntrustedInput.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <wx/utils.h>
#include <wx/window.h>

#include <map>
#include <memory>
#include <set>

namespace fs = boost::filesystem;

namespace Slic3r { namespace GUI { class LayerlingBrowserMac; } }

API_AVAILABLE(macos(12.0))
@interface EdgeSlicerLayerlingDelegate : NSObject <WKNavigationDelegate, WKUIDelegate, WKDownloadDelegate>
{
@public
    Slic3r::GUI::LayerlingBrowserMac *owner;
}
@end

// File drops go into the page, as in a browser: Layerling's own drop zones (import, fonts, images)
// take them. MacDarkMode.mm swaps WKWebView's drag methods for ones that load a dropped file as the
// whole page (its "...2" selectors now hold WebKit's own implementations); this view calls WebKit's
// own ones instead. A file dropped where the page takes none makes WebKit navigate to its file://
// URL, which decide_action() refuses (Navigation::Block): drops never replace Layerling.
@interface WKWebView (EdgeSlicerLayerlingWebKitDrag)
// WebKit implements the NSDraggingDestination methods; declared here so super can be called.
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender;
- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender;
- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)sender;
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender;
- (NSDragOperation)draggingEntered2:(id<NSDraggingInfo>)sender;
- (NSDragOperation)draggingUpdated2:(id<NSDraggingInfo>)sender;
- (BOOL)prepareForDragOperation2:(id<NSDraggingInfo>)sender;
- (BOOL)performDragOperation2:(id<NSDraggingInfo>)sender;
@end

@interface EdgeSlicerLayerlingWebView : WKWebView
@end

@implementation EdgeSlicerLayerlingWebView
// After the swap in MacDarkMode.mm, WKWebView's "...2" selectors run WebKit's own implementation.
// Without that swap (the category not loaded), the plain selectors are WebKit's own: call super.
static bool layerling_drag_swapped() { return [WKWebView instancesRespondToSelector:@selector(performDragOperation2:)]; }
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender
{
    return layerling_drag_swapped() ? [super draggingEntered2:sender] : [super draggingEntered:sender];
}
- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender
{
    return layerling_drag_swapped() ? [super draggingUpdated2:sender] : [super draggingUpdated:sender];
}
- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)sender
{
    return layerling_drag_swapped() ? [super prepareForDragOperation2:sender] : [super prepareForDragOperation:sender];
}
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
    return layerling_drag_swapped() ? [super performDragOperation2:sender] : [super performDragOperation:sender];
}
@end

namespace Slic3r {
namespace GUI {

namespace {

std::string to_std(NSString *s) { return s == nil ? std::string() : std::string([s UTF8String] ? [s UTF8String] : ""); }
std::string url_string(NSURL *u) { return u == nil ? std::string() : to_std(u.absoluteString); }
NSString   *to_ns(const std::string &s) { return [NSString stringWithUTF8String:s.c_str()]; }

// The host website data records are listed under ("127.0.0.1", "layerling.com").
std::string origin_host(const std::string &origin)
{
    untrusted::Url u;
    return untrusted::parse_url(origin, u) ? u.host : std::string();
}

// Reloads after the page's content process died, before the tab gives up and shows an error.
constexpr int MAX_CRASH_RELOADS = 3;
// A crash this long after the last one starts the count again.
constexpr double CRASH_WINDOW_SECONDS = 120.0;

void open_external(const std::string &url)
{
    if (!untrusted::is_safe_to_open_externally(url))
        return;
    wxGetApp().CallAfter([url]() { wxLaunchDefaultBrowser(wxString::FromUTF8(url)); });
}

} // namespace

class API_AVAILABLE(macos(12.0)) LayerlingBrowserMac : public LayerlingBrowser
{
public:
    LayerlingBrowserMac(wxWindow *host, const std::string &origin, const fs::path &download_dir, Callbacks cb)
        : m_host(host), m_origin(origin), m_download_dir(download_dir), m_cb(std::move(cb))
    {}

    ~LayerlingBrowserMac() override
    {
        *m_alive = false;
        if (m_delegate != nil) {
            m_delegate->owner = nullptr;
            [m_delegate release];
            m_delegate = nil;
        }
        for (auto &[key, d] : m_downloads)
        {
            [(WKDownload *) d.op cancel:nil];
            [(id) d.op release];
        }
        m_downloads.clear();
        if (m_view != nil) {
            m_view.navigationDelegate = nil;
            m_view.UIDelegate         = nil;
            [m_view removeFromSuperview];
            [m_view release];
            m_view = nil;
        }
        if (m_store != nil)
            [m_store release];
    }

    void load(const std::string &url) override
    {
        m_pending_url = url;
        if (m_view == nil) {
            // The view is made once the data store is chosen (asynchronous on a first start on
            // macOS 14+), and then loads m_pending_url.
            if (!m_choosing_store)
                create();
            return;
        }
        load_now(url);
    }

    void load_now(const std::string &url)
    {
        NSURL *u = [NSURL URLWithString:to_ns(url)];
        if (u != nil && m_view != nil)
            [m_view loadRequest:[NSURLRequest requestWithURL:u]];
    }

    void reload() override
    {
        if (m_view != nil)
            [m_view reload];
    }

    void set_allowed_origin(const std::string &origin) override { m_origin = origin; }

    void set_visible(bool visible) override
    {
        m_visible = visible;
        if (m_view != nil)
            m_view.hidden = visible ? NO : YES;
    }

    void resize() override
    {
        if (m_view == nil || m_host == nullptr)
            return;
        NSView *parent = (NSView *) m_host->GetHandle();
        if (parent != nil)
            m_view.frame = parent.bounds;
    }

    void focus() override
    {
        if (m_view != nil && m_view.window != nil)
            [m_view.window makeFirstResponder:m_view];
    }

    bool can_clear_data() const override { return true; }

    void clear_data(std::function<void(bool)> done) override
    {
        WKWebsiteDataStore *store = m_store != nil ? m_store : [WKWebsiteDataStore defaultDataStore];
        const std::string   host  = origin_host(m_origin);
        NSSet              *types = [WKWebsiteDataStore allWebsiteDataTypes];
        auto                done_copy = done;
        if (host.empty()) {
            if (done_copy)
                done_copy(false);
            return;
        }
        [store fetchDataRecordsOfTypes:types
                     completionHandler:^(NSArray<WKWebsiteDataRecord *> *records) {
                         // Only the records of Layerling's host: the default store also holds the
                         // app's other web views.
                         NSMutableArray *ours = [NSMutableArray array];
                         for (WKWebsiteDataRecord *r in records)
                             if (to_std(r.displayName) == host)
                                 [ours addObject:r];
                         [store removeDataOfTypes:types
                                   forDataRecords:ours
                                completionHandler:^{
                                    // removeDataOfTypes reports no error: look again, and say it
                                    // worked only when nothing of the host is left.
                                    [store fetchDataRecordsOfTypes:types
                                                 completionHandler:^(NSArray<WKWebsiteDataRecord *> *left) {
                                                     bool clean = true;
                                                     for (WKWebsiteDataRecord *r in left)
                                                         if (to_std(r.displayName) == host)
                                                             clean = false;
                                                     if (done_copy)
                                                         wxGetApp().CallAfter([done_copy, clean]() { done_copy(clean); });
                                                 }];
                                }];
                     }];
    }

    std::string current_url() const override { return m_view != nil ? url_string(m_view.URL) : std::string(); }

    void run_script(const std::string &script, std::function<void(bool, const std::string &)> done) override
    {
        if (m_view == nil) {
            if (done)
                done(false, "no page");
            return;
        }
        auto done_copy = done;
        [m_view evaluateJavaScript:to_ns(script)
                 completionHandler:^(id result, NSError *error) {
                     std::string value;
                     if ([result isKindOfClass:[NSString class]])
                         value = to_std((NSString *) result);
                     else if (error != nil)
                         value = to_std(error.localizedDescription);
                     const bool ok = error == nil;
                     if (done_copy)
                         wxGetApp().CallAfter([done_copy, ok, value]() { done_copy(ok, value); });
                 }];
    }

    void set_dark(bool dark) override
    {
        m_dark = dark;
        if (m_view != nil)
            m_view.appearance = [NSAppearance appearanceNamed:dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
    }

    // ---- called by the delegate

    WKNavigationActionPolicy decide_action(WKNavigationAction *action)
    {
        if (action.shouldPerformDownload)
            return WKNavigationActionPolicyDownload;
        const std::string url = url_string(action.request.URL);
        const bool        main_frame = action.targetFrame == nil || action.targetFrame.isMainFrame;
        const auto        d   = layerling::decide_navigation(url, m_origin);
        if (d == layerling::Navigation::Allow)
            return WKNavigationActionPolicyAllow;
        if (!main_frame && d == layerling::Navigation::External)
            return WKNavigationActionPolicyAllow; // a frame showing another site
        if (d == layerling::Navigation::External && action.navigationType == WKNavigationTypeLinkActivated)
            open_external(url);
        return WKNavigationActionPolicyCancel;
    }

    WKNavigationResponsePolicy decide_response(WKNavigationResponse *response)
    {
        if (!response.canShowMIMEType)
            return WKNavigationResponsePolicyDownload;
        if ([response.response isKindOfClass:[NSHTTPURLResponse class]]) {
            NSString *disposition = [((NSHTTPURLResponse *) response.response) valueForHTTPHeaderField:@"Content-Disposition"];
            if (disposition != nil && [[disposition lowercaseString] hasPrefix:@"attachment"])
                return WKNavigationResponsePolicyDownload;
        }
        return WKNavigationResponsePolicyAllow;
    }

    WKWebView *new_window(WKNavigationAction *action)
    {
        const std::string url = url_string(action.request.URL);
        if (!layerling::origin_of(url).empty())
            open_external(url);
        return nil; // never a second browser window
    }

    void adopt_download(WKDownload *download)
    {
        download.delegate = (id<WKDownloadDelegate>) m_delegate;
    }

    void decide_destination(WKDownload *download, NSString *suggested, void (^handler)(NSURL *))
    {
        const std::string name = layerling::sanitize_export_name(to_std(suggested));
        boost::system::error_code ec;
        // The error_code overloads: nothing may throw inside a WebKit callback.
        const fs::path dir = m_download_dir / fs::unique_path("%%%%%%%%%%%%", ec);
        if (!ec)
            fs::create_directories(dir, ec);
        const fs::path file = dir / name;
        if (ec || !untrusted::is_path_within_root(m_download_dir, file)) {
            // Reported here, with the reason; WebKit then fails the download as cancelled, which
            // download_failed() must not report a second time (it would replace this reason).
            m_refused.insert((void *) download);
            handler(nil);
            if (m_cb.on_download_failed)
                m_cb.on_download_failed(name, "no usable temporary folder");
            return;
        }
        Pending p;
        p.op   = [download retain];
        p.name = name;
        p.file = file;
        m_downloads[(void *) download] = p;
        handler([NSURL fileURLWithPath:to_ns(file.string())]);
    }

    void download_finished(WKDownload *download)
    {
        auto it = m_downloads.find((void *) download);
        if (it == m_downloads.end())
            return;
        Pending p = it->second;
        m_downloads.erase(it);
        [(id) p.op release];
        if (m_cb.on_download)
            m_cb.on_download(p.name, p.file);
    }

    void download_failed(WKDownload *download, NSError *error)
    {
        if (m_refused.erase((void *) download) > 0)
            return; // decide_destination() refused it and said why
        auto it = m_downloads.find((void *) download);
        std::string name;
        if (it != m_downloads.end()) {
            name = it->second.name;
            boost::system::error_code ec;
            fs::remove_all(it->second.file.parent_path(), ec);
            [(id) it->second.op release];
            m_downloads.erase(it);
        }
        BOOST_LOG_TRIVIAL(warning) << "Layerling: download failed: " << to_std(error.localizedDescription);
        if (m_cb.on_download_failed)
            m_cb.on_download_failed(name, to_std(error.localizedDescription));
    }

    void loaded(bool ok, NSError *error)
    {
        if (!ok && error != nil) {
            // Our own cancellations (a link sent to the system browser, a download) are not failures.
            if ([error.domain isEqualToString:NSURLErrorDomain] && error.code == NSURLErrorCancelled)
                return;
            // 102 "Frame load interrupted": a navigation that became a download (an export) or that
            // decide_action() cancelled. 101 "cannot show URL" is a real failure and is reported.
            if ([error.domain isEqualToString:@"WebKitErrorDomain"] && error.code == 102)
                return;
        }
        if (ok)
            m_crash_reloads = 0; // a page that loads again ends a crash series
        if (m_cb.on_loaded)
            m_cb.on_loaded(ok, m_view != nil ? url_string(m_view.URL) : std::string(), ok ? std::string() : to_std(error.localizedDescription));
    }

    // The page's content process died (a crash, or macOS ended it under memory pressure). Reload a
    // few times; when it keeps dying, stop, show a short error page and tell the panel, which
    // offers Retry instead of reloading for ever.
    void content_process_died()
    {
        if (m_view == nil)
            return;
        const NSTimeInterval now = [NSDate timeIntervalSinceReferenceDate];
        if (m_crash_reloads > 0 && now - m_last_crash > CRASH_WINDOW_SECONDS)
            m_crash_reloads = 0;
        m_last_crash = now;
        if (m_crash_reloads < MAX_CRASH_RELOADS) {
            ++m_crash_reloads;
            BOOST_LOG_TRIVIAL(warning) << "Layerling: the page's content process ended, reloading (" << m_crash_reloads << "/" << MAX_CRASH_RELOADS << ")";
            [m_view reload];
            return;
        }
        BOOST_LOG_TRIVIAL(error) << "Layerling: the page's content process keeps ending, not reloading again";
        [m_view loadHTMLString:@"<!doctype html><meta charset=utf-8><body style=\"font:14px -apple-system,sans-serif;margin:40px;color:#666\">"
                               @"Layerling stopped several times in a row.</body>"
                       baseURL:nil];
        if (m_cb.on_error)
            m_cb.on_error(_u8L("Layerling stopped several times in a row and was not reloaded again."));
    }

private:
    struct Pending
    {
        void       *op = nullptr; // WKDownload, retained
        std::string name;
        fs::path    file;
    };

    wxWindow                    *m_host;
    std::string                  m_origin;
    fs::path                     m_download_dir;
    Callbacks                    m_cb;
    WKWebView                   *m_view { nil };
    WKWebsiteDataStore          *m_store { nil };
    EdgeSlicerLayerlingDelegate *m_delegate { nil };
    std::map<void *, Pending>    m_downloads;
    std::set<void *>             m_refused; // downloads decide_destination() refused (reported there)
    bool                         m_visible { true };
    bool                         m_dark { false };
    bool                         m_choosing_store { false };
    std::shared_ptr<bool>        m_alive { std::make_shared<bool>(true) };
    std::string                  m_pending_url;
    int                          m_crash_reloads { 0 };
    NSTimeInterval               m_last_crash { 0 };

    // Which persistent data store holds the designs (app_config "layerling_store"):
    //  - macOS 12/13: the default persistent store (dataStoreForIdentifier: needs macOS 14).
    //  - macOS 14+: a store of its own ("own", identifier in "layerling_store_id"), so the app's
    //    other web views do not share it - unless the default store already holds Layerling's
    //    data: then this Mac ran the tab on macOS 12/13 and was upgraded, and switching stores
    //    would hide every design made before. Such a system keeps the default store ("default").
    //    The choice is made once, on the first start on 14+, and remembered. (The default store's
    //    records are per host, not per port: another web view of the app on 127.0.0.1 also counts
    //    as "has data", which only means the tab keeps the store macOS 12/13 used.)
    // Phase 1 kept no "layerling_store": a saved identifier means "own" (its designs are there).
    static constexpr const char *KEY_STORE    = "layerling_store";
    static constexpr const char *KEY_STORE_ID = "layerling_store_id";

    void create()
    {
        if (@available(macOS 14.0, *)) {
            AppConfig        *app    = wxGetApp().app_config;
            const std::string choice = app->get(KEY_STORE);
            if (choice == "default") {
                make_view(nil);
                return;
            }
            if (choice == "own" || !app->get(KEY_STORE_ID).empty()) {
                make_view(own_store());
                return;
            }
            // First start on 14+: does the default store already hold Layerling's data?
            const std::string host = origin_host(m_origin);
            m_choosing_store       = true;
            auto alive             = m_alive;
            [[WKWebsiteDataStore defaultDataStore]
                fetchDataRecordsOfTypes:[WKWebsiteDataStore allWebsiteDataTypes]
                      completionHandler:^(NSArray<WKWebsiteDataRecord *> *records) {
                          if (!*alive)
                              return;
                          bool has_data = false;
                          for (WKWebsiteDataRecord *r in records)
                              if (!host.empty() && to_std(r.displayName) == host)
                                  has_data = true;
                          m_choosing_store = false;
                          AppConfig *cfg   = wxGetApp().app_config;
                          if (has_data) {
                              BOOST_LOG_TRIVIAL(info) << "Layerling: the default data store already holds data of " << host
                                                      << " (tab used on macOS 12/13): keeping it";
                              cfg->set(KEY_STORE, "default");
                              make_view(nil);
                          } else if (@available(macOS 14.0, *)) {
                              make_view(own_store());
                          }
                      }];
            return;
        }
        make_view(nil);
    }

    API_AVAILABLE(macos(14.0)) WKWebsiteDataStore *own_store()
    {
        AppConfig        *app   = wxGetApp().app_config;
        NSUUID           *uuid  = nil;
        const std::string saved = app->get(KEY_STORE_ID);
        if (!saved.empty())
            uuid = [[[NSUUID alloc] initWithUUIDString:to_ns(saved)] autorelease];
        if (uuid == nil) {
            uuid = [NSUUID UUID];
            app->set(KEY_STORE_ID, to_std(uuid.UUIDString));
        }
        app->set(KEY_STORE, "own");
        return [WKWebsiteDataStore dataStoreForIdentifier:uuid];
    }

    // `store` nil: the default persistent store.
    void make_view(WKWebsiteDataStore *store)
    {
        if (m_view != nil)
            return;
        NSView *parent = (NSView *) m_host->GetHandle();
        if (parent == nil) {
            if (m_cb.on_error)
                m_cb.on_error(_u8L("The Layerling view could not start."));
            return;
        }
        if (store != nil)
            m_store = [store retain];
        WKWebViewConfiguration *config = [[[WKWebViewConfiguration alloc] init] autorelease];
        config.websiteDataStore = m_store != nil ? m_store : [WKWebsiteDataStore defaultDataStore];
        WKUserScript *script = [[[WKUserScript alloc] initWithSource:to_ns(layerling::document_start_script())
                                                       injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                                    forMainFrameOnly:YES] autorelease];
        [config.userContentController addUserScript:script];

        m_view = [[EdgeSlicerLayerlingWebView alloc] initWithFrame:parent.bounds configuration:config];
        m_delegate        = [[EdgeSlicerLayerlingDelegate alloc] init];
        m_delegate->owner = this;
        m_view.navigationDelegate = (id<WKNavigationDelegate>) m_delegate;
        m_view.UIDelegate         = (id<WKUIDelegate>) m_delegate;
        m_view.autoresizingMask   = NSViewWidthSizable | NSViewHeightSizable;
        if (@available(macOS 13.3, *))
            m_view.inspectable = wxGetApp().app_config->get_bool("developer_mode") ? YES : NO;
        m_view.hidden = m_visible ? NO : YES;
        [parent addSubview:m_view];
        set_dark(m_dark);
        BOOST_LOG_TRIVIAL(info) << "Layerling: browser started (" << (m_store != nil ? "own" : "default") << " persistent data store)";
        if (!m_pending_url.empty())
            load_now(m_pending_url);
    }
};

std::unique_ptr<LayerlingBrowser> LayerlingBrowser::create(wxWindow *host, const std::string &allowed_origin, const fs::path &download_dir,
                                                           Callbacks callbacks)
{
    if (@available(macOS 12.0, *))
        return std::make_unique<LayerlingBrowserMac>(host, allowed_origin, download_dir, std::move(callbacks));
    return nullptr;
}

} // namespace GUI
} // namespace Slic3r

using Slic3r::GUI::LayerlingBrowserMac;

@implementation EdgeSlicerLayerlingDelegate

- (void)webView:(WKWebView *)webView
    decidePolicyForNavigationAction:(WKNavigationAction *)navigationAction
                    decisionHandler:(void (^)(WKNavigationActionPolicy))decisionHandler
{
    decisionHandler(owner != nullptr ? owner->decide_action(navigationAction) : WKNavigationActionPolicyCancel);
}

- (void)webView:(WKWebView *)webView
    decidePolicyForNavigationResponse:(WKNavigationResponse *)navigationResponse
                      decisionHandler:(void (^)(WKNavigationResponsePolicy))decisionHandler
{
    decisionHandler(owner != nullptr ? owner->decide_response(navigationResponse) : WKNavigationResponsePolicyCancel);
}

- (void)webView:(WKWebView *)webView navigationAction:(WKNavigationAction *)navigationAction didBecomeDownload:(WKDownload *)download
{
    if (owner != nullptr)
        owner->adopt_download(download);
    else
        [download cancel:nil];
}

- (void)webView:(WKWebView *)webView navigationResponse:(WKNavigationResponse *)navigationResponse didBecomeDownload:(WKDownload *)download
{
    if (owner != nullptr)
        owner->adopt_download(download);
    else
        [download cancel:nil];
}

- (void)webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)navigation
{
    if (owner != nullptr)
        owner->loaded(true, nil);
}

- (void)webView:(WKWebView *)webView didFailProvisionalNavigation:(WKNavigation *)navigation withError:(NSError *)error
{
    if (owner != nullptr)
        owner->loaded(false, error);
}

- (void)webView:(WKWebView *)webView didFailNavigation:(WKNavigation *)navigation withError:(NSError *)error
{
    if (owner != nullptr)
        owner->loaded(false, error);
}

- (void)webViewWebContentProcessDidTerminate:(WKWebView *)webView
{
    if (owner != nullptr)
        owner->content_process_died();
}

- (WKWebView *)webView:(WKWebView *)webView
    createWebViewWithConfiguration:(WKWebViewConfiguration *)configuration
               forNavigationAction:(WKNavigationAction *)navigationAction
                    windowFeatures:(WKWindowFeatures *)windowFeatures
{
    return owner != nullptr ? owner->new_window(navigationAction) : nil;
}

- (void)webView:(WKWebView *)webView
    runJavaScriptAlertPanelWithMessage:(NSString *)message
                      initiatedByFrame:(WKFrameInfo *)frame
                     completionHandler:(void (^)(void))completionHandler
{
    NSAlert *alert    = [[[NSAlert alloc] init] autorelease];
    alert.messageText = @"Layerling";
    alert.informativeText = message != nil ? message : @"";
    [alert addButtonWithTitle:@"OK"];
    [alert runModal];
    completionHandler();
}

- (void)webView:(WKWebView *)webView
    runJavaScriptConfirmPanelWithMessage:(NSString *)message
                        initiatedByFrame:(WKFrameInfo *)frame
                       completionHandler:(void (^)(BOOL))completionHandler
{
    NSAlert *alert    = [[[NSAlert alloc] init] autorelease];
    alert.messageText = @"Layerling";
    alert.informativeText = message != nil ? message : @"";
    [alert addButtonWithTitle:@"OK"];
    [alert addButtonWithTitle:@"Cancel"];
    completionHandler([alert runModal] == NSAlertFirstButtonReturn);
}

- (void)webView:(WKWebView *)webView
    runJavaScriptTextInputPanelWithPrompt:(NSString *)prompt
                              defaultText:(NSString *)defaultText
                         initiatedByFrame:(WKFrameInfo *)frame
                        completionHandler:(void (^)(NSString *))completionHandler
{
    NSAlert *alert    = [[[NSAlert alloc] init] autorelease];
    alert.messageText = @"Layerling";
    alert.informativeText = prompt != nil ? prompt : @"";
    NSTextField *field = [[[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 300, 24)] autorelease];
    field.stringValue  = defaultText != nil ? defaultText : @"";
    alert.accessoryView = field;
    [alert addButtonWithTitle:@"OK"];
    [alert addButtonWithTitle:@"Cancel"];
    completionHandler([alert runModal] == NSAlertFirstButtonReturn ? field.stringValue : nil);
}

- (void)webView:(WKWebView *)webView
    runOpenPanelWithParameters:(WKOpenPanelParameters *)parameters
              initiatedByFrame:(WKFrameInfo *)frame
             completionHandler:(void (^)(NSArray<NSURL *> *))completionHandler
{
    if (owner == nullptr) {
        completionHandler(nil);
        return;
    }
    NSOpenPanel *panel            = [NSOpenPanel openPanel];
    panel.canChooseFiles          = YES;
    panel.canChooseDirectories    = NO;
    panel.allowsMultipleSelection = parameters.allowsMultipleSelection;
    panel.resolvesAliases         = YES;
    completionHandler([panel runModal] == NSModalResponseOK ? panel.URLs : nil);
}

- (void)download:(WKDownload *)download
    decideDestinationUsingResponse:(NSURLResponse *)response
                 suggestedFilename:(NSString *)suggestedFilename
                 completionHandler:(void (^)(NSURL *))completionHandler
{
    if (owner != nullptr)
        owner->decide_destination(download, suggestedFilename, completionHandler);
    else
        completionHandler(nil);
}

- (void)downloadDidFinish:(WKDownload *)download
{
    if (owner != nullptr)
        owner->download_finished(download);
}

- (void)download:(WKDownload *)download didFailWithError:(NSError *)error resumeData:(NSData *)resumeData
{
    if (owner != nullptr)
        owner->download_failed(download, error);
}

@end
