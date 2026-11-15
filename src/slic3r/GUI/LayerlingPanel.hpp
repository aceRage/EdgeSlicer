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

// "Edit in Layerling" from the plate (object and part context menus): something selected that has
// model parts, and the tab is on.
bool layerling_can_edit_selection();
// Opens the selection in the Layerling tab: an object sent from a design of the tab's Layerling
// opens that design again; anything else is exported (STEP when every part has exact CAD geometry,
// a coloured 3MF when it has several filament colours, else STL) and handed to Layerling's own
// import, which makes a new design of it (layerling::import_file_script).
void layerling_edit_selection();

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
//  - Layerling source (tab menu, or "Use my own Layerling" on the Get page): the downloaded copy, or
//    the user's own Layerling at an http(s) address (a NAS or home server, Docker, a dev server,
//    layerling.com), checked through its /version.json or start page; app_config
//    "layerling_source" + "layerling_base_url" (layerling::select_source). No downloads or update
//    checks for an address of the user's. Designs belong to the address they were made at.
//  - Edit in Layerling (plate context menus): see layerling_edit_selection(). An object keeps the
//    design it came from (ModelObject::layerling_design); Send to Plate from that design offers to
//    replace the object, keeping its place, filament, settings and modifiers.
//  - Connect an AI client (MCP): the settings text for Layerling's MCP server, to copy.
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
    // See layerling_edit_selection().
    void edit_selection();
    // Before the main window goes: stops the server and any download.
    void shutdown();

    struct Impl;

private:
    std::unique_ptr<Impl> p;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_LayerlingPanel_hpp_
