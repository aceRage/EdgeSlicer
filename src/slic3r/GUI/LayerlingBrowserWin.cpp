// The Layerling tab's web view on Windows: WebView2 in an environment and user data folder of its
// own. See LayerlingBrowser.hpp for what every implementation does.

#include "LayerlingBrowser.hpp"

#ifdef _WIN32

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Layerling.hpp"
#include "libslic3r/UntrustedInput.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>

#include <wx/utils.h>
#include <wx/window.h>

#include <map>
#include <memory>

#include <windows.h>
#include <wrl.h>
#include <WebView2.h>

namespace Slic3r {
namespace GUI {

namespace fs = boost::filesystem;
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

std::string take_wstring(LPWSTR s)
{
    if (s == nullptr)
        return std::string();
    std::string r = boost::nowide::narrow(s);
    CoTaskMemFree(s);
    return r;
}

typedef HRESULT(STDAPICALLTYPE *CreateEnvironment_t)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions *,
                                                     ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);

class LayerlingBrowserWin : public LayerlingBrowser
{
public:
    LayerlingBrowserWin(wxWindow *host, const std::string &origin, const fs::path &download_dir, Callbacks cb)
        : m_host(host), m_origin(origin), m_download_dir(download_dir), m_cb(std::move(cb))
    {}

    ~LayerlingBrowserWin() override
    {
        *m_alive = false;
        for (auto &[id, d] : m_downloads) {
            if (d.op) {
                d.op->remove_StateChanged(d.token);
                d.op->Cancel();
            }
        }
        m_downloads.clear();
        if (m_controller)
            m_controller->Close();
        m_controller.Reset();
        m_view.Reset();
        m_env.Reset();
    }

    void load(const std::string &url) override
    {
        m_pending_url = url;
        if (m_view) {
            m_view->Navigate(boost::nowide::widen(url).c_str());
            return;
        }
        if (!m_creating)
            create();
    }

    void reload() override
    {
        if (m_view)
            m_view->Reload();
        else if (!m_creating)
            create();
    }

    void set_allowed_origin(const std::string &origin) override { m_origin = origin; }

    void set_visible(bool visible) override
    {
        m_visible = visible;
        if (m_controller)
            m_controller->put_IsVisible(visible ? TRUE : FALSE);
    }

    void resize() override
    {
        if (!m_controller || m_host == nullptr)
            return;
        RECT r;
        ::GetClientRect((HWND) m_host->GetHWND(), &r);
        m_controller->put_Bounds(r);
    }

    void focus() override
    {
        if (m_controller)
            m_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }

    bool can_clear_data() const override { return true; }

    void clear_data(std::function<void(bool)> done) override
    {
        ComPtr<ICoreWebView2_13> v13;
        ComPtr<ICoreWebView2Profile> profile;
        ComPtr<ICoreWebView2Profile2> p2;
        if (!m_view || FAILED(m_view.As(&v13)) || FAILED(v13->get_Profile(&profile)) || !profile || FAILED(profile.As(&p2))) {
            if (done)
                done(false);
            return;
        }
        auto alive = m_alive;
        const HRESULT hr = p2->ClearBrowsingDataAll(Callback<ICoreWebView2ClearBrowsingDataCompletedHandler>([alive, done](HRESULT result) -> HRESULT {
                                                       if (*alive && done)
                                                           wxGetApp().CallAfter([done, result]() { done(SUCCEEDED(result)); });
                                                       return S_OK;
                                                   }).Get());
        if (FAILED(hr) && done)
            done(false);
    }

    void set_dark(bool dark) override
    {
        m_dark = dark;
        apply_dark();
    }

private:
    struct Download
    {
        ComPtr<ICoreWebView2DownloadOperation> op;
        EventRegistrationToken                 token {};
        std::string                            name;
        fs::path                               file;
    };

    wxWindow                        *m_host;
    std::string                      m_origin;
    fs::path                         m_download_dir;
    Callbacks                        m_cb;
    std::shared_ptr<bool>            m_alive { std::make_shared<bool>(true) };
    ComPtr<ICoreWebView2Environment> m_env;
    ComPtr<ICoreWebView2Controller>  m_controller;
    ComPtr<ICoreWebView2>            m_view;
    bool                             m_creating { false };
    bool                             m_visible { true };
    bool                             m_dark { false };
    std::string                      m_pending_url;
    std::map<int, Download>          m_downloads;
    int                              m_next_download { 1 };

    void fail(const std::string &why)
    {
        m_creating = false;
        if (m_controller)
            m_controller->Close();
        m_controller.Reset();
        m_view.Reset();
        BOOST_LOG_TRIVIAL(error) << "Layerling: " << why;
        if (m_cb.on_error)
            m_cb.on_error(why);
    }

    static std::wstring user_data_folder()
    {
        // Next to the version folders, never inside one: an update replaces those, never this.
        fs::path dir = fs::path(boost::nowide::widen(data_dir())) / "layerling" / ".profile";
        boost::system::error_code ec;
        fs::create_directories(dir, ec);
        return dir.wstring();
    }

    void create()
    {
        if (m_creating || m_controller)
            return;
        m_creating = true;
        static HMODULE loader = LoadLibraryW(L"WebView2Loader.dll");
        auto create_env = loader ? reinterpret_cast<CreateEnvironment_t>(GetProcAddress(loader, "CreateCoreWebView2EnvironmentWithOptions")) : nullptr;
        if (create_env == nullptr) {
            fail(_u8L("Layerling needs the Microsoft Edge WebView2 Runtime, which could not be loaded."));
            return;
        }
        auto alive  = m_alive;
        auto on_env = Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this, alive](HRESULT result, ICoreWebView2Environment *created) -> HRESULT {
                if (!*alive)
                    return S_OK;
                if (FAILED(result) || created == nullptr) {
                    fail(_u8L("The Layerling view could not start. Is the Microsoft Edge WebView2 Runtime installed?"));
                    return S_OK;
                }
                m_env = created;
                const HRESULT hr = m_env->CreateCoreWebView2Controller(
                    (HWND) m_host->GetHWND(),
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([this, alive](HRESULT result, ICoreWebView2Controller *c) -> HRESULT {
                        if (!*alive) {
                            if (c)
                                c->Close();
                            return S_OK;
                        }
                        if (FAILED(result) || c == nullptr) {
                            fail(_u8L("The Layerling view could not start."));
                            return S_OK;
                        }
                        m_controller = c;
                        m_controller->get_CoreWebView2(&m_view);
                        if (!m_view || !configure()) {
                            fail(_u8L("The Layerling view was not started because its settings could not be applied."));
                            return S_OK;
                        }
                        attach_handlers();
                        m_creating = false;
                        apply_dark();
                        resize();
                        m_controller->put_IsVisible(m_visible ? TRUE : FALSE);
                        if (!m_pending_url.empty())
                            m_view->Navigate(boost::nowide::widen(m_pending_url).c_str());
                        return S_OK;
                    }).Get());
                if (FAILED(hr))
                    fail(_u8L("The Layerling view could not start."));
                return S_OK;
            });
        const std::wstring folder = user_data_folder();
        BOOST_LOG_TRIVIAL(info) << "Layerling: browser data in " << boost::nowide::narrow(folder);
        const HRESULT hr = create_env(nullptr, folder.c_str(), nullptr, on_env.Get());
        if (FAILED(hr))
            fail(_u8L("The Layerling view could not start. Is the Microsoft Edge WebView2 Runtime installed?"));
    }

    // No bridge to the app: web messages and host objects off (read back to be sure).
    bool configure()
    {
        ComPtr<ICoreWebView2Settings> s;
        if (FAILED(m_view->get_Settings(&s)) || !s)
            return false;
        s->put_IsWebMessageEnabled(FALSE);
        s->put_AreHostObjectsAllowed(FALSE);
        s->put_AreDevToolsEnabled(wxGetApp().app_config->get_bool("developer_mode") ? TRUE : FALSE);
        s->put_AreDefaultScriptDialogsEnabled(TRUE);
        s->put_IsStatusBarEnabled(FALSE);
        s->put_IsZoomControlEnabled(TRUE);
        s->put_IsBuiltInErrorPageEnabled(TRUE);
        ComPtr<ICoreWebView2Settings4> s4;
        if (SUCCEEDED(s.As(&s4))) {
            s4->put_IsPasswordAutosaveEnabled(FALSE);
            s4->put_IsGeneralAutofillEnabled(FALSE);
        }
        BOOL messages = TRUE, host_objects = TRUE;
        if (FAILED(s->get_IsWebMessageEnabled(&messages)) || FAILED(s->get_AreHostObjectsAllowed(&host_objects)))
            return false;
        if (messages != FALSE || host_objects != FALSE)
            return false;
        m_view->AddScriptToExecuteOnDocumentCreated(boost::nowide::widen(layerling::document_start_script()).c_str(), nullptr);
        return true;
    }

    void apply_dark()
    {
        if (!m_view)
            return;
        ComPtr<ICoreWebView2_13>     v13;
        ComPtr<ICoreWebView2Profile> profile;
        if (SUCCEEDED(m_view.As(&v13)) && SUCCEEDED(v13->get_Profile(&profile)) && profile)
            profile->put_PreferredColorScheme(m_dark ? COREWEBVIEW2_PREFERRED_COLOR_SCHEME_DARK : COREWEBVIEW2_PREFERRED_COLOR_SCHEME_LIGHT);
        ComPtr<ICoreWebView2Controller2> c2;
        if (m_controller && SUCCEEDED(m_controller.As(&c2)))
            c2->put_DefaultBackgroundColor(m_dark ? COREWEBVIEW2_COLOR{255, 0x24, 0x24, 0x28} : COREWEBVIEW2_COLOR{255, 255, 255, 255});
    }

    static void open_external(const std::string &url)
    {
        if (!untrusted::is_safe_to_open_externally(url))
            return;
        wxGetApp().CallAfter([url]() { wxLaunchDefaultBrowser(wxString::FromUTF8(url)); });
    }

    void attach_handlers()
    {
        auto                   alive = m_alive;
        EventRegistrationToken token;

        m_view->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
                                           [this, alive](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
                                               if (!*alive)
                                                   return S_OK;
                                               LPWSTR uri = nullptr;
                                               args->get_Uri(&uri);
                                               const std::string url = take_wstring(uri);
                                               const auto        d   = layerling::decide_navigation(url, m_origin);
                                               if (d == layerling::Navigation::Allow)
                                                   return S_OK;
                                               args->put_Cancel(TRUE);
                                               BOOL user = FALSE;
                                               args->get_IsUserInitiated(&user);
                                               if (d == layerling::Navigation::External && user)
                                                   open_external(url);
                                               return S_OK;
                                           })
                                           .Get(),
                                       &token);
        // Frames: anything but local files and other programs.
        m_view->add_FrameNavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
                                                [this, alive](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
                                                    if (!*alive)
                                                        return S_OK;
                                                    LPWSTR uri = nullptr;
                                                    args->get_Uri(&uri);
                                                    if (layerling::decide_navigation(take_wstring(uri), m_origin) == layerling::Navigation::Block)
                                                        args->put_Cancel(TRUE);
                                                    return S_OK;
                                                })
                                                .Get(),
                                            &token);
        m_view->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                           [alive](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *args) -> HRESULT {
                                               args->put_Handled(TRUE); // never a second browser window
                                               if (!*alive)
                                                   return S_OK;
                                               LPWSTR uri = nullptr;
                                               args->get_Uri(&uri);
                                               const std::string url = take_wstring(uri);
                                               if (!layerling::origin_of(url).empty())
                                                   open_external(url);
                                               return S_OK;
                                           })
                                           .Get(),
                                       &token);
        m_view->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                            [this, alive](ICoreWebView2 *sender, ICoreWebView2NavigationCompletedEventArgs *args) -> HRESULT {
                                                if (!*alive)
                                                    return S_OK;
                                                BOOL ok = FALSE;
                                                args->get_IsSuccess(&ok);
                                                COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                                                args->get_WebErrorStatus(&status);
                                                LPWSTR src = nullptr;
                                                sender->get_Source(&src);
                                                const std::string url = take_wstring(src);
                                                // A navigation we cancelled ourselves is not a failure.
                                                if (!ok && status == COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED)
                                                    return S_OK;
                                                if (m_cb.on_loaded)
                                                    m_cb.on_loaded(ok == TRUE, url, ok ? std::string() : "error " + std::to_string(int(status)));
                                                return S_OK;
                                            })
                                            .Get(),
                                        &token);
        ComPtr<ICoreWebView2_4> v4;
        if (SUCCEEDED(m_view.As(&v4)))
            v4->add_DownloadStarting(Callback<ICoreWebView2DownloadStartingEventHandler>(
                                         [this, alive](ICoreWebView2 *, ICoreWebView2DownloadStartingEventArgs *args) -> HRESULT {
                                             if (!*alive) {
                                                 args->put_Cancel(TRUE);
                                                 return S_OK;
                                             }
                                             handle_download(args);
                                             return S_OK;
                                         })
                                         .Get(),
                                     &token);
        m_view->add_PermissionRequested(Callback<ICoreWebView2PermissionRequestedEventHandler>(
                                            [this](ICoreWebView2 *, ICoreWebView2PermissionRequestedEventArgs *args) -> HRESULT {
                                                COREWEBVIEW2_PERMISSION_KIND kind = COREWEBVIEW2_PERMISSION_KIND_UNKNOWN_PERMISSION;
                                                args->get_PermissionKind(&kind);
                                                LPWSTR uri = nullptr;
                                                args->get_Uri(&uri);
                                                const bool ours = layerling::origin_of(take_wstring(uri)) == m_origin;
                                                const bool allowed = ours && (kind == COREWEBVIEW2_PERMISSION_KIND_CLIPBOARD_READ ||
                                                                              kind == COREWEBVIEW2_PERMISSION_KIND_MULTIPLE_AUTOMATIC_DOWNLOADS ||
                                                                              kind == COREWEBVIEW2_PERMISSION_KIND_FILE_READ_WRITE);
                                                args->put_State(allowed ? COREWEBVIEW2_PERMISSION_STATE_ALLOW : COREWEBVIEW2_PERMISSION_STATE_DENY);
                                                return S_OK;
                                            })
                                            .Get(),
                                        &token);
        m_view->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>(
                                      [this, alive](ICoreWebView2 *, ICoreWebView2ProcessFailedEventArgs *args) -> HRESULT {
                                          if (!*alive)
                                              return S_OK;
                                          COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
                                          args->get_ProcessFailedKind(&kind);
                                          if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED) {
                                              wxGetApp().CallAfter([this, alive]() {
                                                  if (!*alive)
                                                      return;
                                                  m_env.Reset();
                                                  fail(_u8L("The Layerling view stopped."));
                                              });
                                          } else if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED && m_view) {
                                              m_view->Reload();
                                          }
                                          return S_OK;
                                      })
                                      .Get(),
                                  &token);
    }

    void handle_download(ICoreWebView2DownloadStartingEventArgs *args)
    {
        ComPtr<ICoreWebView2DownloadOperation> op;
        if (FAILED(args->get_DownloadOperation(&op)) || !op) {
            args->put_Cancel(TRUE);
            return;
        }
        LPWSTR suggested = nullptr;
        args->get_ResultFilePath(&suggested);
        std::string       suggested_path = take_wstring(suggested);
        const size_t      sep            = suggested_path.find_last_of("/\\");
        const std::string name           = layerling::sanitize_export_name(sep == std::string::npos ? suggested_path : suggested_path.substr(sep + 1));
        // Our own handling instead of the browser's download bubble.
        args->put_Handled(TRUE);

        boost::system::error_code ec;
        const fs::path            dir = m_download_dir / fs::unique_path("%%%%%%%%%%%%");
        fs::create_directories(dir, ec);
        const fs::path file = dir / fs::path(boost::nowide::widen(name));
        if (ec || !untrusted::is_path_within_root(m_download_dir, file) || FAILED(args->put_ResultFilePath(file.wstring().c_str()))) {
            args->put_Cancel(TRUE);
            if (m_cb.on_download_failed)
                m_cb.on_download_failed(name, "no usable temporary folder");
            return;
        }
        Download d;
        d.op   = op;
        d.name = name;
        d.file = file;
        const int id    = m_next_download++;
        auto      alive = m_alive;
        op->add_StateChanged(Callback<ICoreWebView2StateChangedEventHandler>([this, alive, id](ICoreWebView2DownloadOperation *, IUnknown *) -> HRESULT {
                                 if (*alive)
                                     on_download_state(id);
                                 return S_OK;
                             }).Get(),
                             &d.token);
        m_downloads[id] = std::move(d);
        BOOST_LOG_TRIVIAL(info) << "Layerling: download of \"" << name << "\" started";
    }

    void on_download_state(int id)
    {
        auto it = m_downloads.find(id);
        if (it == m_downloads.end())
            return;
        COREWEBVIEW2_DOWNLOAD_STATE state = COREWEBVIEW2_DOWNLOAD_STATE_IN_PROGRESS;
        it->second.op->get_State(&state);
        if (state == COREWEBVIEW2_DOWNLOAD_STATE_IN_PROGRESS)
            return;
        Download d = std::move(it->second);
        m_downloads.erase(it);
        d.op->remove_StateChanged(d.token);
        if (state == COREWEBVIEW2_DOWNLOAD_STATE_INTERRUPTED) {
            COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON reason = COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_NONE;
            d.op->get_InterruptReason(&reason);
            boost::system::error_code ec;
            fs::remove_all(d.file.parent_path(), ec);
            BOOST_LOG_TRIVIAL(warning) << "Layerling: download of \"" << d.name << "\" interrupted, reason " << int(reason);
            if (m_cb.on_download_failed && reason != COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_USER_CANCELED &&
                reason != COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_USER_SHUTDOWN)
                m_cb.on_download_failed(d.name, "the download was interrupted");
            return;
        }
        LPWSTR result = nullptr;
        d.op->get_ResultFilePath(&result);
        const std::string result_path = take_wstring(result);
        const fs::path    file        = result_path.empty() ? d.file : fs::path(boost::nowide::widen(result_path));
        if (!untrusted::is_path_within_root(m_download_dir, file)) {
            if (m_cb.on_download_failed)
                m_cb.on_download_failed(d.name, "it was saved outside the temporary folder");
            return;
        }
        if (m_cb.on_download)
            m_cb.on_download(d.name, file);
    }
};

} // namespace

std::unique_ptr<LayerlingBrowser> LayerlingBrowser::create(wxWindow *host, const std::string &allowed_origin, const fs::path &download_dir,
                                                           Callbacks callbacks)
{
    return std::make_unique<LayerlingBrowserWin>(host, allowed_origin, download_dir, std::move(callbacks));
}

} // namespace GUI
} // namespace Slic3r

#endif // _WIN32
