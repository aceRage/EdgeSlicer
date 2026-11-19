#ifndef slic3r_WebGuideDialog_hpp_
#define slic3r_WebGuideDialog_hpp_

#include "wx/artprov.h"
#include "wx/cmdline.h"
#include "wx/notifmsg.h"
#include "wx/settings.h"
#include "wx/webview.h"

#if wxUSE_WEBVIEW_IE
#include "wx/msw/webview_ie.h"
#endif
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
#include <wx/frame.h>
#include <wx/tbarbase.h>
#include "wx/textctrl.h"

#include "GUI_App.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/Utils/PresetUpdater.hpp"

#include <nlohmann/json.hpp>
#include <atomic>
#include <unordered_set>

namespace Slic3r { namespace GUI {

// How apply_enabled_filaments() brings the loaded presets in line with the new [filaments] section.
enum class FilamentsReload
{
    None,       // nothing: the section did not change
    Visibility, // only re-read which filament / process presets are shown; nothing from disk, unsaved edits stay
    Full,       // PresetBundle::load_presets(): ask about unsaved edits first (check_and_keep_current_preset_changes)
};

// The filament half of GuideFrame::apply_config, shared with the Filament manager (PresetManagerDialog):
// stores `enabled` as the [filaments] section, reloads as asked, switches the active filament to an
// enabled one when it no longer is, and exports the selections. keep_shown_active (the manager): an
// active filament that is still shown stays active even when it is not in `enabled` (a user preset).
void apply_enabled_filaments(AppConfig &app_config, PresetBundle &bundle, const std::map<std::string, std::string> &enabled,
                             FilamentsReload reload, const PresetBundle::PresetPreferences &preferences = {},
                             bool keep_shown_active = false);

class GuideFrame : public DPIDialog
{
public:
    GuideFrame(GUI_App *pGUI, long style = wxCAPTION | wxCLOSE_BOX | wxSYSTEM_MENU);
    virtual ~GuideFrame();

    enum GuidePage {
        BBL_WELCOME,
        BBL_REGION,
        BBL_MODELS,
        BBL_FILAMENTS,
        BBL_FILAMENT_ONLY,
        BBL_MODELS_ONLY
    }m_page;

    //Web Function
    void load_url(wxString &url);
    wxString SetStartPage(GuidePage startpage=BBL_WELCOME, bool load = true);

    void UpdateState();
    void OnIdle(wxIdleEvent &evt);
    // void OnClose(wxCloseEvent &evt);

    void OnNavigationRequest(wxWebViewEvent &evt);
    void OnNavigationComplete(wxWebViewEvent &evt);
    void OnDocumentLoaded(wxWebViewEvent &evt);
    void OnNewWindow(wxWebViewEvent &evt);
    void OnError(wxWebViewEvent &evt);
    void OnTitleChanged(wxWebViewEvent &evt);
    void OnFullScreenChanged(wxWebViewEvent &evt);
    void OnScriptMessage(wxWebViewEvent &evt);

    void OnScriptResponseMessage(wxCommandEvent &evt);
    void RunScript(const wxString &javascript);

    //Logic
    bool IsFirstUse();

    //Model - Machine - Filaments
    int LoadProfileData();
    int SaveProfileData();
    int LoadProfileFamily(std::string strVendor, std::string strFilePath);
    int SaveProfile();
    int GetFilamentInfo( std::string VendorDirectory, const json & pFilaList, std::string filepath, std::string &sVendor, std::string &sType);
    int GetFilamentInfo(const std::string& VendorDirectory, const json& pFilaList, const std::string& filepath,
                        std::string& sVendor, std::string& sType, std::unordered_set<std::string>& visiting);


    bool apply_config(AppConfig *app_config, PresetBundle *preset_bundle, const PresetUpdater *updater, bool& apply_keeped_changes);
    bool run();

    void        StrReplace(std::string &strBase, std::string strSrc, std::string strDes);
    std::string w2s(wxString sSrc);
    void        GetStardardFilePath(std::string &FilePath);
    bool LoadFile(std::string jPath, std::string & sContent);

    // install plugin
    int ShowPluginStatus(int status, int percent, bool &cancel);

    void on_dpi_changed(const wxRect &suggested_rect) {}

private:
    GUI_App *m_MainPtr;
    AppConfig m_appconfig_new;

    wxWebView *m_browser;
    wxButton * m_TestBtn;

    wxString m_SectionName;

    bool sm_bundle_rsrc;
    boost::filesystem::path vendor_dir;
    boost::filesystem::path rsrc_vendor_dir;

    //First Load
    bool bFirstComplete{false};
    std::atomic<bool> m_destroy{false};
    boost::thread* m_load_task{ nullptr };
    // The loading page gets userguide_profile_load_finish once both are true;
    // every instance of it reports in, so the flag is never reset.
    bool m_profile_ready{false};
    bool m_page_ready{false};
    void notify_profile_loaded();

    // User Config
    bool PrivacyUse;
    bool StealthMode;
    std::string m_Region;

    bool InstallNetplugin;
    bool network_plugin_ready {false};

    json m_OrcaFilaList;
    std::string m_OrcaFilaLibPath;

#if wxUSE_WEBVIEW_IE
    wxMenuItem *m_script_object_el;
    wxMenuItem *m_script_date_el;
    wxMenuItem *m_script_array_el;
#endif
    // Last executed JavaScript snippet, for convenience.
    wxString m_javascript;
    wxString m_response_js;

    wxString m_sm_user_agent;
    std::string m_editing_filament_id;
};

}} // namespace Slic3r::GUI

#endif /* slic3r_Tab_hpp_ */
