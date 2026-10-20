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
#include "../Utils/DevicePageLoad.hpp"
#include "PrinterStoragePanel.hpp"

class wxChoice;
class wxStaticText;
class Button;


namespace Slic3r {
namespace GUI {

class ElegooCanvasPanel;


class PrinterWebView : public wxPanel{
public:
    PrinterWebView(wxWindow *parent);
    virtual ~PrinterWebView();

    // Navigates the view. A Reload loads the page even when the view shows it already (only a
    // second request while that very URL is loading is dropped); a Show leaves our own page alone
    // when the view already shows it. Before startup is over every request is held and the last one
    // is loaded by release_startup_hold() (DevicePageLoad.hpp).
    void load_url(wxString& url, wxString apikey = "", DevicePageLoad::Kind kind = DevicePageLoad::Kind::Reload);
    // Startup is over, or the tab is being opened: load whatever was asked for meanwhile. `why`
    // goes to the log.
    void release_startup_hold(const char* why);
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

    // The Storage tab (PrinterStoragePanel) beside the printer's page: the files on the printer the
    // page is for. A print host device (`key` "host" for the preset's own address, "ph:<id>" for a
    // device of the model's list) gets one when it has an address; whether that printer speaks a
    // file API the tab knows is the tab's business (it says so when it does not). An empty source
    // hides the tab and goes back to the page.
    void set_storage_device(const PrintHostDevices::Device& device, const std::string& key);
    void set_storage_source(PrinterStoragePanel::Source source);

    // The read-only CANVAS card above the page (ElegooCanvasUI.hpp): shown for a Centauri Carbon 1
    // on Elegoo Link, hidden for everything else. `preset_model` is the printer preset's
    // printer_model, for a device saved without one.
    void set_canvas_device(const PrintHostDevices::Device& device, const std::string& preset_model);

    void sys_color_changed();
    void msw_rescale();

private:
    void SendAPIKey();
    // Loads the picked device's web UI. Uses the preset's print_host_webui only for the device that
    // carries the preset's own address: that override is a property of that one machine's URL.
    void load_picked_device();
    // A loopback page that failed to load comes back on this timer (WebLoadRetry.hpp).
    void OnRetryTimer(wxTimerEvent& evt);
    // The actual navigation behind load_url() and release_startup_hold().
    void navigate(const wxString& url, const wxString& apikey, bool register_view, DevicePageLoad::Kind kind);
    // Page or Storage, and the bar above them: tab buttons when there is a Storage tab, the
    // device picker when the model has devices, nothing at all when neither.
    void show_storage(bool show);
    void update_bar();

    wxWebView* m_browser;
    long m_zoomFactor;
    wxString m_apikey;
    wxPanel*      m_device_bar { nullptr };
    wxStaticText* m_device_label { nullptr };
    wxChoice*     m_device_choice { nullptr };
    ::Button*     m_tab_page { nullptr };
    ::Button*     m_tab_storage { nullptr };
    PrinterStoragePanel* m_storage { nullptr };
    ElegooCanvasPanel*   m_canvas { nullptr };
    std::string          m_canvas_model; // the preset's printer_model the card was last set for
    bool          m_storage_shown { false };
    bool          m_storage_has_source { false };
    int           m_bar_state { 0 }; // what update_bar() last laid out (all hidden at first)
    std::string   m_model_key;
    std::vector<PrintHostDevices::Device> m_devices;
    wxTimer       m_retry_timer;
    std::string   m_retry_url;      // the URL to load again when the timer fires
    std::string   m_retry_page;     // its page (no query), which the retry count belongs to
    int           m_retry_count { 0 };
    // Startup hold and load-once bookkeeping (DevicePageLoad.hpp).
    DevicePageLoad::Gate m_load_gate { true };
    // The held load is the constructor's own page, which load_url() never registered with
    // fltviews(); keep it that way when it is finally loaded.
    bool          m_held_is_initial { false };

    // DECLARE_EVENT_TABLE()
};

} // GUI
} // Slic3r

#endif /* slic3r_Tab_hpp_ */
