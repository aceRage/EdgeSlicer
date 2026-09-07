#ifndef slic3r_PrintHostDevicesDialog_hpp_
#define slic3r_PrintHostDevicesDialog_hpp_

#include <string>
#include <vector>

#include "GUI_Utils.hpp"
#include "../Utils/PrintHostDevices.hpp"

class wxButton;
class wxDataViewListCtrl;
class wxStaticText;

namespace Slic3r {

class DynamicPrintConfig;

namespace GUI {

// The device list of one printer model: alias, address, host type, and (from phase 2) status.
//
// Phase 1 keeps the preset as the single source every send path reads: "Set as current" writes the
// chosen device's address and credentials into the printer preset's host fields, exactly as the
// single-address editor beside it does, so Send to printer, the hub and the phone all keep working
// unchanged. Phase 3 replaces that bridge with a real fan-out.
class PrintHostDevicesDialog : public DPIDialog
{
public:
    PrintHostDevicesDialog(wxWindow* parent, DynamicPrintConfig* config, const std::string& model_key, const wxString& model_label);
    ~PrintHostDevicesDialog() override = default;

    // True when the preset's host fields were changed here, so the caller can refresh its own view.
    bool config_changed() const { return m_config_changed; }

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;
    void on_sys_color_changed() override {}

private:
    enum Column { COL_ALIAS, COL_ADDRESS, COL_TYPE, COL_STATUS };

    void reload();
    void on_add();
    void on_edit();
    void on_remove();
    void on_test();
    void on_set_current();
    void update_buttons();
    // The device the row is on, false when nothing is selected.
    bool selected(PrintHostDevices::Device& out) const;

    DynamicPrintConfig*                     m_config { nullptr };
    std::string                             m_model_key;
    std::vector<PrintHostDevices::Device>   m_devices;
    std::string                             m_current;
    bool                                    m_config_changed { false };

    wxDataViewListCtrl* m_list { nullptr };
    wxButton*           m_btn_add { nullptr };
    wxButton*           m_btn_edit { nullptr };
    wxButton*           m_btn_remove { nullptr };
    wxButton*           m_btn_test { nullptr };
    wxButton*           m_btn_current { nullptr };
};

// Opens the dialog for the printer preset that is selected right now, after importing that preset's
// own print_host as device 1 if it has never been imported. Returns true when the preset's host
// fields were changed. Safe to call with no preset bundle (does nothing).
bool show_print_host_devices_dialog(wxWindow* parent, DynamicPrintConfig* config = nullptr);

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_PrintHostDevicesDialog_hpp_
