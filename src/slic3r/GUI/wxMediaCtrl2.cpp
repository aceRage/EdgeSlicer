#include "wxMediaCtrl2.h"
#include "libslic3r/Time.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include "PluginGuard.hpp"
#include <boost/filesystem/operations.hpp>
#ifdef __WIN32__
#include <winuser.h>
#include <versionhelpers.h>
#include <wx/msw/registry.h>
#include <shellapi.h>
#endif

#ifdef __LINUX__
#include "Printer/gstbambusrc.h"
#include <gst/gst.h> // main gstreamer header
class WXDLLIMPEXP_MEDIA
    wxGStreamerMediaBackend : public wxMediaBackendCommonBase
{
public:
    GstElement *m_playbin; // GStreamer media element
};
#endif

wxDEFINE_EVENT(EVT_MEDIA_CTRL_STAT, wxCommandEvent);

wxMediaCtrl2::wxMediaCtrl2(wxWindow *parent)
{
#ifdef __WIN32__
    auto hModExe = GetModuleHandle(NULL);
    // BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl2: GetModuleHandle " << hModExe;
    auto NvOptimusEnablement = (DWORD *) GetProcAddress(hModExe, "NvOptimusEnablement");
    auto AmdPowerXpressRequestHighPerformance = (int *) GetProcAddress(hModExe, "AmdPowerXpressRequestHighPerformance");
    if (NvOptimusEnablement) {
        // BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl2: NvOptimusEnablement " << *NvOptimusEnablement;
        *NvOptimusEnablement = 0;
    }
    if (AmdPowerXpressRequestHighPerformance) {
        // BOOST_LOG_TRIVIAL(info) << "wxMediaCtrl2: AmdPowerXpressRequestHighPerformance " << *AmdPowerXpressRequestHighPerformance;
        *AmdPowerXpressRequestHighPerformance = 0;
    }
#endif
    wxMediaCtrl::Create(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxMEDIACTRLPLAYERCONTROLS_NONE);
#ifdef __LINUX__
    /* Register only after we have created the wxMediaCtrl, since only then are we guaranteed to have fired up Gstreamer's plugin registry. */
    auto playbin = reinterpret_cast<wxGStreamerMediaBackend *>(m_imp)->m_playbin;
    g_object_set (G_OBJECT (playbin),
                  "audio-sink", NULL,
                   NULL);
    gstbambusrc_register();
    Bind(wxEVT_MEDIA_LOADED, [this](auto & e) {
        m_loaded = true;
    });
#endif
}

#define CLSID_BAMBU_SOURCE L"{233E64FB-2041-4A6C-AFAB-FF9BCF83E7AA}"

void wxMediaCtrl2::Load(wxURI url)
{
#ifdef __WIN32__
    InvalidateBestSize();
    if (m_imp == nullptr) {
        static bool notified = false;
        if (!notified) CallAfter([] {
            auto res = wxMessageBox(_L("Windows Media Player is required for this task! Do you want to enable 'Windows Media Player' for your operation system?"), _L("Error"), wxOK | wxCANCEL);
            if (res == wxOK) {
                wxString url = IsWindows10OrGreater() 
                        ? "ms-settings:optionalfeatures?activationSource=SMC-Article-14209" 
                        : "https://support.microsoft.com/en-au/windows/get-windows-media-player-81718e0d-cfce-25b1-aee3-94596b658287";
                wxExecute("cmd /c start " + url, wxEXEC_HIDE_CONSOLE);
            }
        });
        m_error = 100;
        wxMediaEvent event(wxEVT_MEDIA_STATECHANGED);
        event.SetId(GetId());
        event.SetEventObject(this);
        wxPostEvent(this, event);
        return;
    }
    {
        wxRegKey key11(wxRegKey::HKCU, L"SOFTWARE\\Classes\\CLSID\\" CLSID_BAMBU_SOURCE L"\\InProcServer32");
        wxRegKey key12(wxRegKey::HKCR, L"CLSID\\" CLSID_BAMBU_SOURCE L"\\InProcServer32");
        wxString path = key11.Exists() ? key11.QueryDefaultValue() 
                                       : key12.Exists() ? key12.QueryDefaultValue() : wxString{};
        wxRegKey key2(wxRegKey::HKCR, "bambu");
        wxString clsid;
        if (key2.Exists())
            key2.QueryRawValue("Source Filter", clsid);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": clsid %1% path %2%") % clsid % path;

        // Ultra (live view): the camera module lives in <data_dir>\cameratools - shipped with the app
        // (CAMERA_BIN_DIR) and seeded there at start-up, or fetched as the camera component.
        // <data_dir>\plugins\BambuSource.dll is UltraNet's storage module, never the filter.
        const auto ours = Slic3r::GUI::camera_module_dir(boost::filesystem::path(Slic3r::data_dir())) / "BambuSource.dll";
        const bool registered_real = !path.empty() && wxFile::Exists(path) &&
                                     Slic3r::GUI::exports_dll_register_server(boost::filesystem::path(path.ToStdWstring()));
        const auto decision = Slic3r::GUI::filter_registration_decision(clsid == CLSID_BAMBU_SOURCE, registered_real,
                                                                        Slic3r::GUI::exports_dll_register_server(ours));
        bool usable = decision == Slic3r::GUI::FilterRegistration::UseRegistered;
        if (decision == Slic3r::GUI::FilterRegistration::RegisterOurs) {
            // Nothing usable registered (none at all, a dangling path, or a path that now holds
            // UltraNet's storage module): register ours for this user, silently - no UAC.
            BOOST_LOG_TRIVIAL(info) << "[camera component] registered filter unusable (clsid " << clsid << ", path " << path
                                    << "); registering " << ours.string();
            usable = Slic3r::GUI::wxGetApp().register_bambu_source_filter();
            if (usable) {
                path  = wxString(ours.wstring());
                clsid = CLSID_BAMBU_SOURCE;
            }
        }
        if (!usable) {
            // This build carries no camera files and nothing usable is registered: offer Bambu's
            // camera component rather than the stock "re-register it" prompt, which could only ever
            // run regsvr32 on a DLL with no DllRegisterServer.
            BOOST_LOG_TRIVIAL(info) << "[camera component] no usable BambuSource filter (clsid " << clsid << ", path " << path
                                    << "); offering the component";
            if (decision == Slic3r::GUI::FilterRegistration::OfferComponent) CallAfter([this] {
                if (Slic3r::GUI::wxGetApp().offer_bambu_camera_component(this)) {
                    // Installed and registered: let the user hit Play again on a working filter.
                    wxMediaEvent event(wxEVT_MEDIA_STATECHANGED);
                    event.SetId(GetId());
                    event.SetEventObject(this);
                    wxPostEvent(this, event);
                }
            });
            m_error = clsid != CLSID_BAMBU_SOURCE ? 101 : path.empty() ? 102 : 103;
            wxMediaEvent event(wxEVT_MEDIA_STATECHANGED);
            event.SetId(GetId());
            event.SetEventObject(this);
            wxPostEvent(this, event);
            return;
        }
        // A working filter registered by another install (OrcaSlicer, Bambu Studio) is used as it is
        // and left registered, so the apps do not fight over HKCU; no "different install" nag.
        wxRegKey keyWmp(wxRegKey::HKCU, "SOFTWARE\\Microsoft\\MediaPlayer\\Player\\Extensions\\.");
        keyWmp.Create();
        long permissions = 0;
        if (keyWmp.HasValue("Permissions"))
            keyWmp.QueryValue("Permissions", &permissions);
        if ((permissions & 32) == 0) {
            permissions |= 32;
            keyWmp.SetValue("Permissions", permissions);
        }
    }
    url = wxURI(url.BuildURI().append("&hwnd=").append(boost::lexical_cast<std::string>(GetHandle())).append("&tid=").append(
        boost::lexical_cast<std::string>(GetCurrentThreadId())));
#endif
#ifdef __WXGTK3__
    GstElementFactory *factory;
    int hasplugins = 1;
    
    factory = gst_element_factory_find("h264parse");
    if (!factory) {
        hasplugins = 0;
    } else {
        gst_object_unref(factory);
    }
    
    factory = gst_element_factory_find("openh264dec");
    if (!factory) {
        factory = gst_element_factory_find("avdec_h264");
    }
    if (!factory) {
        factory = gst_element_factory_find("vaapih264dec");
    }
    if (!factory) {
        hasplugins = 0;
    } else {
        gst_object_unref(factory);
    }
    
    if (!hasplugins) {
        CallAfter([] {
            wxMessageBox(_L("Your system is missing H.264 codecs for GStreamer, which are required to play video.  (Try installing the gstreamer1.0-plugins-bad or gstreamer1.0-libav packages, then restart EdgeSlicer?)"), _L("Error"), wxOK);
        });
        m_error = 101;
        wxMediaEvent event(wxEVT_MEDIA_STATECHANGED);
        event.SetId(GetId());
        event.SetEventObject(this);
        wxPostEvent(this, event);
        return;
    }
    wxLog::EnableLogging(false);
#endif
    m_error = 0;
    m_loaded = false;
    wxMediaCtrl::Load(url);

#ifdef __WXGTK3__
        wxMediaEvent event(wxEVT_MEDIA_STATECHANGED);
        event.SetId(GetId());
        event.SetEventObject(this);
        wxPostEvent(this, event);
#endif
}

void wxMediaCtrl2::Play() { wxMediaCtrl::Play(); }

void wxMediaCtrl2::Stop()
{
    wxMediaCtrl::Stop();
}

#ifdef __LINUX__
extern "C" int gst_bambu_last_error;
#endif

int wxMediaCtrl2::GetLastError() const
{
#ifdef __LINUX__
    return gst_bambu_last_error;
#else
    return m_error;
#endif
}

wxSize wxMediaCtrl2::GetVideoSize() const
{
#ifdef __LINUX__
    // Gstreamer doesn't give us a VideoSize until we're playing, which
    // confuses the MediaPlayCtrl into claiming that it is stuck
    // "Loading...".  Fake it out for now.
    return m_loaded ? wxSize(1280, 720) : wxSize{};
#else
    wxSize size = m_imp ? m_imp->GetVideoSize() : wxSize(0, 0);
    if (size.GetWidth() > 0)
        const_cast<wxSize&>(m_video_size) = size;
    return size;
#endif
}

wxSize wxMediaCtrl2::DoGetBestSize() const
{
    return {-1, -1};
}

#ifdef __WIN32__

WXLRESULT wxMediaCtrl2::MSWWindowProc(WXUINT   nMsg,
                                   WXWPARAM wParam,
                                   WXLPARAM lParam)
{
    if (nMsg == WM_USER + 1000) {
        wxString msg((wchar_t const *) lParam);
        if (wParam == 1) {
            if (msg.EndsWith("]")) {
                int n = msg.find_last_of('[');
                if (n != wxString::npos) {
                    long val = 0;
                    if (msg.SubString(n + 1, msg.Length() - 2).ToLong(&val))
                        m_error = (int) val;
                }
            } else if (msg.Contains("stat_log")) {
                wxCommandEvent evt(EVT_MEDIA_CTRL_STAT);
                evt.SetEventObject(this);
                evt.SetString(msg.Mid(msg.Find(' ') + 1));
                wxPostEvent(this, evt);
            }
        }
        BOOST_LOG_TRIVIAL(trace) << msg.ToUTF8().data();
        return 0;
    }
    return wxMediaCtrl::MSWWindowProc(nMsg, wParam, lParam);
}

#endif
