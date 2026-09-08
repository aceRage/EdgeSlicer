#ifndef slic3r_PrinterWebView_hpp_
#define slic3r_PrinterWebView_hpp_


#include "wx/artprov.h"
#include "wx/cmdline.h"
#include "wx/notifmsg.h"
#include "wx/settings.h"
#include <wx/webview.h>
#include <wx/string.h>

#if wxUSE_WEBVIEW_EDGE
#include "wx/msw/webview_edge.h"
#endif

#include "wx/webviewarchivehandler.h"
#include "wx/webviewfshandler.h"
#include "wx/numdlg.h"
#include "wx/infobar.h"
#include "wx/filesys.h"
#include "wx/fs_arc.h"
#include "wx/fs_mem.h"
#include "wx/stdpaths.h"
#include <wx/panel.h>
#include <wx/tbarbase.h>
#include "wx/textctrl.h"
#include <wx/timer.h>

#include <string>
#include <vector>

#include "../Utils/PrintHostDevices.hpp"

class wxChoice;
class wxStaticText;


namespace Slic3r {
namespace GUI {


class PrinterWebView : public wxPanel{
public:
    PrinterWebView(wxWindow *parent);
    virtual ~PrinterWebView();

    void load_url(wxString& url, wxString apikey = "");
    void UpdateState();
    void OnClose(wxCloseEvent& evt);
    void OnError(wxWebViewEvent& evt);
    void OnLoaded(wxWebViewEvent& evt);
    void OnScriptMessage(wxWebViewEvent& evt);
    void reload();
    void update_mode();
    bool isSnapmakerPage();
    void sendMessage(const std::string& msg);
    wxWebView* get_browser() const { return m_browser; }

    // The printers of this model, by address. With more than nothing in the list the tab grows a
    // picker above the page and shows whichever device is chosen there, rather than always the one
    // address the preset happens to hold. An empty list hides the picker again (a Bambu or
    // Snapmaker printer, or a model with no devices), which is what the tab did before.
    void set_devices(const std::string& model_key, const std::vector<PrintHostDevices::Device>& devices, const std::string& select_id);
    // The device the picker is on, "" when there is none.
    std::string picked_device() const;

private:
    void SendAPIKey();
    // Loads the picked device's web UI. Uses the preset's print_host_webui only for the device that
    // carries the preset's own address: that override is a property of that one machine's URL.
    void load_picked_device();

    wxWebView* m_browser;
    long m_zoomFactor;
    wxString m_apikey;
    wxPanel*      m_device_bar { nullptr };
    wxChoice*     m_device_choice { nullptr };
    std::string   m_model_key;
    std::vector<PrintHostDevices::Device> m_devices;

    // DECLARE_EVENT_TABLE()
};

} // GUI
} // Slic3r

#endif /* slic3r_Tab_hpp_ */
