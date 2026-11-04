#ifndef slic3r_GUI_CrealityDiscoveryDialog_hpp_
#define slic3r_GUI_CrealityDiscoveryDialog_hpp_

#include <memory>
#include <vector>

#include "GUI_Utils.hpp"
#include "../Utils/CrealityHostDiscovery.hpp"

class Button;
class wxDataViewListCtrl;
class wxStaticText;

namespace Slic3r {
namespace GUI {

// Lists the Creality K-series printers on the LAN (model, address, CFS) and returns the one picked.
//
// Port of OrcaSlicer #13752's CrealityDiscoveryDialog (598df9ff0b). Upstream ran the scan on the UI
// thread behind a wxWindowDisabler; here it runs on a worker thread, so the dialog stays live, can
// be closed mid-scan (the scan is cancelled) and offers Rescan.
class CrealityDiscoveryDialog : public DPIDialog
{
public:
    // ok_label: "Use" in the address editor, "Add" in the device list.
    CrealityDiscoveryDialog(wxWindow* parent, const wxString& ok_label);
    ~CrealityDiscoveryDialog() override;

    // Valid after ShowModal() returned wxID_OK.
    const CrealityHost& selected() const { return m_selected; }

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;
    void on_sys_color_changed() override {}

private:
    struct Session; // shared with the worker thread

    void start_scan();
    void on_scan_done(std::vector<CrealityHost> hosts);
    void accept();

    std::shared_ptr<Session>  m_session;
    std::vector<CrealityHost> m_hosts;
    CrealityHost              m_selected;

    wxStaticText*       m_status { nullptr };
    wxDataViewListCtrl* m_list { nullptr };
    Button*             m_btn_rescan { nullptr };
    Button*             m_btn_ok { nullptr };
    Button*             m_btn_cancel { nullptr };
};

} // namespace GUI
} // namespace Slic3r

#endif
