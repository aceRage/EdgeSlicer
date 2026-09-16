#ifndef slic3r_FFDeviceTab_hpp_
#define slic3r_FFDeviceTab_hpp_

#include <wx/panel.h>
#include <wx/stattext.h>
#include <wx/simplebook.h>

namespace Slic3r {
namespace GUI {

class DeviceListPanel;
class SingleDeviceState;

// Ultra: Flashforge Device tab - hosts Orca-Flashforge's Device List and Device
// Status pages (the pages their MonitorPanel mounts), routed by the
// EVT_SWITCH_TO_DEVICE_LIST/STATUS events posted via MainFrame::jump_to_monitor.
class FFDeviceTab : public wxPanel
{
public:
    FFDeviceTab(wxWindow* parent);

    void OnActivate();

private:
    // The page shown when FlashForge's FlashNetwork library did not load. Without it the device
    // pages have nothing to talk to and render blank, which tells a user nothing - so this page
    // names the paths that were searched and offers Download / Locate instead.
    wxWindow* build_unavailable_page();
    void      update_unavailable_text();
    void      on_locate();
    void      on_download();
    // True when the stack is up; also flips the book to the right page.
    bool      refresh_availability();

    wxSimplebook*      m_book { nullptr };
    DeviceListPanel*   m_device_list { nullptr };
    SingleDeviceState* m_device_status { nullptr };
    wxWindow*          m_unavailable { nullptr };
    wxStaticText*      m_unavailable_text { nullptr };
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_FFDeviceTab_hpp_
