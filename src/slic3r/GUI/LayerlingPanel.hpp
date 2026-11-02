#ifndef slic3r_GUI_LayerlingPanel_hpp_
#define slic3r_GUI_LayerlingPanel_hpp_

#include <memory>

#include <wx/panel.h>

namespace Slic3r {
namespace GUI {

// Whether this system can show the Layerling tab: Windows (WebView2), macOS 12 or later (WKWebView
// with WKDownload) and Linux (WebKitGTK).
bool layerling_supported();

// Whether the tab is shown: layerling_supported() and Preferences > "Show the Layerling tab"
// (app_config "show_layerling_tab", on by default).
bool layerling_tab_enabled();

// The Layerling tab: Layerling (https://github.com/henmedia/layerling, AGPL-3.0, by henmedia), a
// browser CAD for 3D printing, as an optional download that EdgeSlicer runs locally.
//
//  - Not installed: a "Get Layerling" page (what it is, its licence and source, its funding links,
//    a Download button with progress and Cancel). EdgeSlicer never bundles Layerling.
//  - Download: the latest release's static web build from GitHub (layerling::parse_release_json),
//    checked against the size and the SHA-256 digest GitHub publishes, unpacked into
//    <data dir>/layerling/<version>/ (never outside it). The previous version stays until the new
//    one has loaded once.
//  - Updates: when the tab is opened, at most once a day, the latest release is looked up; a newer
//    one is offered ("Update to vX"), never installed by itself.
//  - Served by LayerlingServer from 127.0.0.1 on a fixed port, shown in a LayerlingBrowser.
//    The page address is app_config "layerling_base_url" when that holds an http(s) URL (a
//    self-hosted Layerling; no UI for it yet), else the local server.
//  - Send to Plate: every 3MF/STL/OBJ/STEP the page exports is handed over instead of being saved;
//    3MF colours become filaments (one per colour, an existing filament of the same colour reused).
//  - Designs live in the browser's storage (LayerlingBrowser.hpp); an update never touches it.
//    "Reset Layerling data" (in the tab's menu) deletes it after asking.
class LayerlingPanel : public wxPanel
{
public:
    explicit LayerlingPanel(wxWindow *parent);
    ~LayerlingPanel() override;

    // The tab was selected (true) or left (false). Nothing is started before the first true.
    void set_active(bool active);
    void sys_color_changed();
    // Before the main window goes: stops the server and any download.
    void shutdown();

    struct Impl;

private:
    std::unique_ptr<Impl> p;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_LayerlingPanel_hpp_
