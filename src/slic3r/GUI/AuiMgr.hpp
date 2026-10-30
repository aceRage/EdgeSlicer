#ifndef slic3r_GUI_AuiMgr_hpp_
#define slic3r_GUI_AuiMgr_hpp_

// Minimal port of Orca's AuiMgr, named CadAuiMgr because Plater.cpp has its own AuiMgr (OrcaSlicer @3610d8bb). DesignPanel docks its feature-tree
// sidebar through wxAUI; Edge has no other CadAuiMgr users. Wayland floating is left enabled
// (Edge has no LinuxDisplayBackend); a Wayland session that cannot float will still dock.

#include <wx/aui/framemanager.h>

namespace Slic3r { namespace GUI {

class CadAuiMgr : public wxAuiManager
{
public:
    void init(wxWindow *window);
    void apply_color_mode();
    void track_docked_size(wxWindow *window);

    static wxAuiPaneInfo sidebar_pane_info();

    wxAuiFloatingFrame *CreateFloatingFrame(wxWindow *parent, const wxAuiPaneInfo &pane) override;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_AuiMgr_hpp_
