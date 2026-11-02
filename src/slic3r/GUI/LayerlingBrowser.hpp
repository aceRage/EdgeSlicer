#ifndef slic3r_GUI_LayerlingBrowser_hpp_
#define slic3r_GUI_LayerlingBrowser_hpp_

#include <functional>
#include <memory>
#include <string>

#include <boost/filesystem/path.hpp>

class wxWindow;

namespace Slic3r {
namespace GUI {

// The web view of the Layerling tab, one per system: WebView2 on Windows (LayerlingBrowserWin.cpp),
// WKWebView on macOS (LayerlingBrowserMac.mm), WebKitGTK through wxWebView on Linux
// (LayerlingBrowserGtk.cpp). LayerlingPanel owns it and does everything else.
//
// What every implementation does the same way:
//  - Persistent storage of its own: Layerling keeps the designs in the page's IndexedDB and
//    localStorage, keyed by the page's origin. Windows: a WebView2 user data folder at
//    <data dir>/layerling/.profile (never touched by an update). macOS 14+: a persistent
//    WKWebsiteDataStore with a fixed identifier; older macOS: the default persistent store.
//    Linux: WebKitGTK's default (persistent) context.
//  - The page stays on its origin (the configured base URL: our 127.0.0.1 server by default).
//    Links anywhere else, and new windows, open in the system browser.
//  - No script bridge to the app (no web messages, no host objects): the page cannot call
//    EdgeSlicer. The bridge works on the page's downloads, which the browser hands over instead
//    of saving them; that works whatever the page's origin is.
//  - window.showSaveFilePicker is hidden before the page's scripts run, so Layerling's exports
//    take its download path (the one it uses in Firefox and Safari) instead of a save dialog that
//    writes past us. The same document-start script (layerling::document_start_script) sets
//    Layerling's own "install hint dismissed" flag, so its "Install as an app" banner, which
//    points at browser controls this view does not have, stays away. Nothing of Layerling's own
//    files is changed.
//  - Permissions are denied except the ones an editor in a tab needs (clipboard, several
//    downloads in a row, opening local files the user picks).
class LayerlingBrowser
{
public:
    struct Callbacks
    {
        // A top-level page load finished (or failed: ok false, with a short reason).
        std::function<void(bool ok, const std::string &url, const std::string &error)> on_loaded;
        // The page downloaded a file: it is complete at `file` (inside the download folder given to
        // create()), `suggested_name` is the name the page gave it. The panel owns the file now.
        std::function<void(const std::string &suggested_name, const boost::filesystem::path &file)> on_download;
        // A download failed or was refused.
        std::function<void(const std::string &suggested_name, const std::string &error)> on_download_failed;
        // The browser could not start, or its content process died.
        std::function<void(const std::string &error)> on_error;
    };

    // `host`: the window the view fills. `allowed_origin`: e.g. "http://127.0.0.1:23456".
    // `download_dir`: where downloads are written (created when missing).
    // Null when this system has no implementation.
    static std::unique_ptr<LayerlingBrowser> create(wxWindow *host, const std::string &allowed_origin,
                                                    const boost::filesystem::path &download_dir, Callbacks callbacks);

    virtual ~LayerlingBrowser() = default;

    // Starts the browser (asynchronous on Windows) and loads `url` once it is ready.
    virtual void load(const std::string &url) = 0;
    virtual void reload() = 0;
    // The origin pages may stay on. Navigations elsewhere open in the system browser.
    virtual void set_allowed_origin(const std::string &origin) = 0;
    virtual void set_visible(bool visible) = 0;
    // The host window was resized.
    virtual void resize() = 0;
    virtual void focus() = 0;
    // Deletes what the pages stored (IndexedDB, local storage, caches, service workers, cookies).
    // `done(ok)` runs on the GUI thread. Not every system can do this while running.
    virtual bool can_clear_data() const = 0;
    virtual void clear_data(std::function<void(bool ok)> done) = 0;
    // Light or dark for the page's prefers-color-scheme, where the system lets us set it.
    virtual void set_dark(bool dark) = 0;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_LayerlingBrowser_hpp_
