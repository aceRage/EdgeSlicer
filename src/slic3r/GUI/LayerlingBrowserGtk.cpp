// The Layerling tab's web view on Linux: a plain wxWebView (WebKitGTK), with WebKitGTK's download
// signals for the exports. See LayerlingBrowser.hpp for what every implementation does.
//
// WebKitGTK is reached the way Widgets/WebView.cpp does it: the few functions used are declared
// here (the GTK build links webkit2gtk-4.x through wxWidgets' webview library), so no WebKit
// headers are needed. Downloads arrive on the shared default web context, for every web view of
// the app; only the ones whose web view is this tab's are taken.

#include "LayerlingBrowser.hpp"

#if defined(__linux__) && !defined(__APPLE__)

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"

#include "libslic3r/Layerling.hpp"
#include "libslic3r/UntrustedInput.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <wx/sizer.h>
#include <wx/utils.h>
#include <wx/webview.h>
#include <wx/window.h>

#include <map>
#include <memory>

#include <gtk/gtk.h>

#define LAYERLING_WEBKIT_API
struct WebKitWebView;
struct WebKitWebContext;
struct WebKitDownload;
struct WebKitUserContentManager;
struct WebKitUserScript;
extern "C" {
LAYERLING_WEBKIT_API WebKitWebContext *webkit_web_view_get_context(WebKitWebView *web_view);
LAYERLING_WEBKIT_API WebKitWebView *webkit_download_get_web_view(WebKitDownload *download);
LAYERLING_WEBKIT_API void webkit_download_set_destination(WebKitDownload *download, const gchar *uri);
LAYERLING_WEBKIT_API const gchar *webkit_download_get_destination(WebKitDownload *download);
LAYERLING_WEBKIT_API void webkit_download_cancel(WebKitDownload *download);
LAYERLING_WEBKIT_API WebKitUserContentManager *webkit_web_view_get_user_content_manager(WebKitWebView *web_view);
// The enum parameters (WebKitUserContentInjectedFrames, WebKitUserScriptInjectionTime) are ints.
LAYERLING_WEBKIT_API WebKitUserScript *webkit_user_script_new(const gchar *source, int injected_frames, int injection_time,
                                                              const gchar *const *allow_list, const gchar *const *block_list);
LAYERLING_WEBKIT_API void webkit_user_content_manager_add_script(WebKitUserContentManager *manager, WebKitUserScript *script);
LAYERLING_WEBKIT_API void webkit_user_script_unref(WebKitUserScript *user_script);
}

namespace Slic3r {
namespace GUI {

namespace fs = boost::filesystem;

namespace {

constexpr int WEBKIT_USER_CONTENT_INJECT_TOP_FRAME  = 1;
constexpr int WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START = 0;

class LayerlingBrowserGtk : public LayerlingBrowser
{
public:
    LayerlingBrowserGtk(wxWindow *host, const std::string &origin, const fs::path &download_dir, Callbacks cb)
        : m_host(host), m_origin(origin), m_download_dir(download_dir), m_cb(std::move(cb))
    {}

    ~LayerlingBrowserGtk() override
    {
        *m_alive = false;
        if (m_context != nullptr && m_signal != 0)
            g_signal_handler_disconnect(m_context, m_signal);
        for (auto &[download, p] : m_downloads)
            g_object_unref(download);
        m_downloads.clear();
        // m_view is a child of m_host and goes with it.
    }

    void load(const std::string &url) override
    {
        if (m_view == nullptr && !create())
            return;
        m_view->LoadURL(wxString::FromUTF8(url));
    }

    void reload() override
    {
        if (m_view != nullptr)
            m_view->Reload();
    }

    void set_allowed_origin(const std::string &origin) override { m_origin = origin; }

    void set_visible(bool visible) override
    {
        if (m_view != nullptr)
            m_view->Show(visible);
    }

    void resize() override
    {
        if (m_host != nullptr)
            m_host->Layout();
    }

    void focus() override
    {
        if (m_view != nullptr)
            m_view->SetFocus();
    }

    bool can_clear_data() const override { return false; }
    void clear_data(std::function<void(bool)> done) override
    {
        if (done)
            done(false);
    }

    void set_dark(bool) override {}

private:
    struct Pending
    {
        std::string name;
        fs::path    file;
    };

    wxWindow                          *m_host;
    std::string                        m_origin;
    fs::path                           m_download_dir;
    Callbacks                          m_cb;
    std::shared_ptr<bool>              m_alive { std::make_shared<bool>(true) };
    wxWebView                         *m_view { nullptr };
    WebKitWebView                     *m_native { nullptr };
    WebKitWebContext                  *m_context { nullptr };
    gulong                             m_signal { 0 };
    std::map<WebKitDownload *, Pending> m_downloads;

    bool create()
    {
        m_view = wxWebView::New(m_host, wxID_ANY, wxWebViewDefaultURLStr, wxDefaultPosition, wxDefaultSize, wxWebViewBackendDefault, wxBORDER_NONE);
        if (m_view == nullptr) {
            if (m_cb.on_error)
                m_cb.on_error(_u8L("The Layerling view could not start."));
            return false;
        }
        auto *sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(m_view, 1, wxEXPAND);
        m_host->SetSizer(sizer);
        m_host->Layout();
        m_view->EnableContextMenu(true);

        m_view->Bind(wxEVT_WEBVIEW_NAVIGATING, [this](wxWebViewEvent &evt) {
            const std::string url = into_u8(evt.GetURL());
            const auto        d   = layerling::decide_navigation(url, m_origin);
            if (d == layerling::Navigation::Allow)
                return;
            if (d == layerling::Navigation::External && evt.GetTarget().empty()) {
                // A frame showing another site may load; a top-level link opens outside.
                evt.Veto();
                if (untrusted::is_safe_to_open_externally(url))
                    wxLaunchDefaultBrowser(evt.GetURL());
                return;
            }
            evt.Veto();
        });
        m_view->Bind(wxEVT_WEBVIEW_NEWWINDOW, [](wxWebViewEvent &evt) {
            const std::string url = into_u8(evt.GetURL());
            if (!layerling::origin_of(url).empty() && untrusted::is_safe_to_open_externally(url))
                wxLaunchDefaultBrowser(evt.GetURL());
        });
        m_view->Bind(wxEVT_WEBVIEW_LOADED, [this](wxWebViewEvent &evt) {
            if (m_cb.on_loaded)
                m_cb.on_loaded(true, into_u8(evt.GetURL()), std::string());
        });
        m_view->Bind(wxEVT_WEBVIEW_ERROR, [this](wxWebViewEvent &evt) {
            if (evt.GetInt() == wxWEBVIEW_NAV_ERR_USER_CANCELLED)
                return;
            if (m_cb.on_loaded)
                m_cb.on_loaded(false, into_u8(evt.GetURL()), into_u8(evt.GetString()));
        });

        m_native = static_cast<WebKitWebView *>(m_view->GetNativeBackend());
        if (m_native != nullptr) {
            if (WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(m_native)) {
                WebKitUserScript *script = webkit_user_script_new(layerling::document_start_script().c_str(), WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                                                  WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
                webkit_user_content_manager_add_script(ucm, script);
                webkit_user_script_unref(script);
            }
            m_context = webkit_web_view_get_context(m_native);
            if (m_context != nullptr)
                m_signal = g_signal_connect(m_context, "download-started", G_CALLBACK(&LayerlingBrowserGtk::on_download_started), this);
        }
        BOOST_LOG_TRIVIAL(info) << "Layerling: browser started (WebKitGTK default context)";
        return true;
    }

    static void on_download_started(WebKitWebContext *, WebKitDownload *download, gpointer user_data)
    {
        auto *self = static_cast<LayerlingBrowserGtk *>(user_data);
        if (self == nullptr || !*self->m_alive || webkit_download_get_web_view(download) != self->m_native)
            return; // another web view's download: not ours to touch
        g_object_ref(download);
        self->m_downloads[download] = Pending();
        g_signal_connect(download, "decide-destination", G_CALLBACK(&LayerlingBrowserGtk::on_decide_destination), self);
        g_signal_connect(download, "finished", G_CALLBACK(&LayerlingBrowserGtk::on_finished), self);
        g_signal_connect(download, "failed", G_CALLBACK(&LayerlingBrowserGtk::on_failed), self);
    }

    static gboolean on_decide_destination(WebKitDownload *download, gchar *suggested, gpointer user_data)
    {
        auto *self = static_cast<LayerlingBrowserGtk *>(user_data);
        auto  it   = self->m_downloads.find(download);
        if (!*self->m_alive || it == self->m_downloads.end()) {
            webkit_download_cancel(download);
            return TRUE;
        }
        const std::string name = layerling::sanitize_export_name(suggested != nullptr ? suggested : "");
        boost::system::error_code ec;
        const fs::path dir = self->m_download_dir / fs::unique_path("%%%%%%%%%%%%");
        fs::create_directories(dir, ec);
        const fs::path file = dir / name;
        gchar *uri = ec ? nullptr : g_filename_to_uri(file.string().c_str(), nullptr, nullptr);
        if (uri == nullptr || !untrusted::is_path_within_root(self->m_download_dir, file)) {
            if (uri != nullptr)
                g_free(uri);
            webkit_download_cancel(download);
            if (self->m_cb.on_download_failed)
                self->m_cb.on_download_failed(name, "no usable temporary folder");
            return TRUE;
        }
        it->second.name = name;
        it->second.file = file;
        webkit_download_set_destination(download, uri);
        g_free(uri);
        return TRUE;
    }

    static void on_finished(WebKitDownload *download, gpointer user_data)
    {
        auto *self = static_cast<LayerlingBrowserGtk *>(user_data);
        auto  it   = self->m_downloads.find(download);
        if (it == self->m_downloads.end())
            return;
        const Pending p = it->second;
        self->m_downloads.erase(it);
        g_object_unref(download);
        // "finished" also follows "failed": only a file that is there counts.
        boost::system::error_code ec;
        if (!*self->m_alive || p.file.empty() || !fs::is_regular_file(p.file, ec))
            return;
        if (self->m_cb.on_download)
            self->m_cb.on_download(p.name, p.file);
    }

    static void on_failed(WebKitDownload *download, GError *error, gpointer user_data)
    {
        auto *self = static_cast<LayerlingBrowserGtk *>(user_data);
        auto  it   = self->m_downloads.find(download);
        if (it == self->m_downloads.end())
            return;
        boost::system::error_code ec;
        if (!it->second.file.empty())
            fs::remove_all(it->second.file.parent_path(), ec);
        const std::string name = it->second.name;
        it->second.file.clear();
        if (*self->m_alive && self->m_cb.on_download_failed)
            self->m_cb.on_download_failed(name, error != nullptr && error->message != nullptr ? error->message : "the download failed");
    }
};

} // namespace

std::unique_ptr<LayerlingBrowser> LayerlingBrowser::create(wxWindow *host, const std::string &allowed_origin, const fs::path &download_dir,
                                                           Callbacks callbacks)
{
    return std::make_unique<LayerlingBrowserGtk>(host, allowed_origin, download_dir, std::move(callbacks));
}

} // namespace GUI
} // namespace Slic3r

#endif // __linux__
