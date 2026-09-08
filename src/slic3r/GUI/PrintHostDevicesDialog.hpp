#ifndef slic3r_PrintHostDevicesDialog_hpp_
#define slic3r_PrintHostDevicesDialog_hpp_

#include <string>
#include <vector>

#include "GUI_Utils.hpp"
#include "../Utils/PrintHostDevices.hpp"

class Button;
class wxDataViewListCtrl;
class wxStaticText;

namespace Slic3r {

class DynamicPrintConfig;

namespace GUI {

// The device list of one printer model: alias, address, host type and status.
//
// No device is "the" device. The preset keeps whatever address the user typed into it and this
// dialog never writes to it; a send picks a device from this list at send time, and the only thing
// the store remembers is which one that was last time (last_used_id), so the next send can
// preselect it.
class PrintHostDevicesDialog : public DPIDialog
{
public:
    PrintHostDevicesDialog(wxWindow* parent, DynamicPrintConfig* config, const std::string& model_key, const wxString& model_label);
    ~PrintHostDevicesDialog() override = default;

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
    // The old single-address editor (PhysicalPrinterDialog), behind an explicit click. Nothing
    // else opens it any more: the sidebar's connection icon lands on this list instead.
    void on_edit_connection();
    void update_buttons();
    // The device the row is on, false when nothing is selected.
    bool selected(PrintHostDevices::Device& out) const;

    DynamicPrintConfig*                     m_config { nullptr };
    std::string                             m_model_key;
    std::vector<PrintHostDevices::Device>   m_devices;
    std::string                             m_last_used;

    wxDataViewListCtrl* m_list { nullptr };
    Button*             m_btn_add { nullptr };
    Button*             m_btn_edit { nullptr };
    Button*             m_btn_remove { nullptr };
    Button*             m_btn_test { nullptr };
    Button*             m_btn_connection { nullptr };
    Button*             m_btn_close { nullptr };
};

// Opens the dialog for the printer preset that is selected right now, after importing that preset's
// own print_host as device 1 if it has never been imported. Safe to call with no preset bundle
// (does nothing).
void show_print_host_devices_dialog(wxWindow* parent, DynamicPrintConfig* config = nullptr);

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_PrintHostDevicesDialog_hpp_
