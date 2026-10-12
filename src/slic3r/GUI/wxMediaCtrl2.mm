//
//  wxMediaCtrl2.m
//  Snapmaker_Orca
//
//  Created by cmguo on 2021/12/7.
//

#import "wxMediaCtrl2.h"
#import "wx/mediactrl.h"
#include <boost/log/trivial.hpp>

#import <Foundation/Foundation.h>
#import "BambuPlayer/BambuPlayer.h"
#import "../Utils/NetworkAgent.hpp"
#include "PluginGuard.hpp"
#include "libslic3r/Utils.hpp"

#include <stdlib.h>
#include <dlfcn.h>
#include <vector>
#include <boost/filesystem.hpp>

wxDEFINE_EVENT(EVT_MEDIA_CTRL_STAT, wxCommandEvent);

#define BAMBU_DYNAMIC

void wxMediaCtrl2::bambu_log(void const * ctx, int level, char const * msg)
{
    if (level == 1) {
        wxString msg2(msg);
        if (msg2.EndsWith("]")) {
            int n = msg2.find_last_of('[');
            if (n != wxString::npos) {
                long val = 0;
                wxMediaCtrl2 * ctrl = (wxMediaCtrl2 *) ctx;
                if (msg2.SubString(n + 1, msg2.Length() - 2).ToLong(&val))
                    ctrl->m_error = (int) val;
            }
        } else if (strstr(msg, "stat_log")) {
            wxMediaCtrl2 * ctrl = (wxMediaCtrl2 *) ctx;
            wxCommandEvent evt(EVT_MEDIA_CTRL_STAT);
            evt.SetEventObject(ctrl);
            evt.SetString(strchr(msg, ' ') + 1);
            wxPostEvent(ctrl, evt);
        }
    } else if (level < 0) {
        wxMediaCtrl2 * ctrl = (wxMediaCtrl2 *) ctx;
        ctrl->NotifyStopped();
    }
    BOOST_LOG_TRIVIAL(info) << msg;
}

wxMediaCtrl2::wxMediaCtrl2(wxWindow * parent)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
{
    NSView * imageView = (NSView *) GetHandle();
    imageView.layer = [[CALayer alloc] init];
    CGColorRef color = CGColorCreateGenericRGB(0, 0, 0, 1.0f);
    imageView.layer.backgroundColor = color;
    CGColorRelease(color);
    imageView.wantsLayer = YES;
    create_player();
}

wxMediaCtrl2::~wxMediaCtrl2()
{
    BambuPlayer * player = (BambuPlayer *) m_player;
    [player dealloc];
}

// The live-view player's own module: Bambu's libBambuSource.dylib from a camera folder, NOT the
// libBambuSource.dylib in <data_dir>/plugins - that one is UltraNet's storage module (the storage
// browser, PrinterFileSystem), which has no player. Both carry the same file name, which is why they
// live in different folders (see PluginGuard.hpp, "The camera module").
//
// Searched in <data_dir>/cameratools (seeded at start-up from the app bundle, or filled by hand),
// then the bundle's own Contents/Resources/cameratools. liblive555.dylib, which the module loads at
// run time for RTSP/RTSPS cameras (X1, H2D, H2C), sits next to it; it is loaded first by its full
// path so the camera module finds it already in the process, whatever search the module uses.
// Loaded once and never unloaded (players hold its classes for the life of the process). A failed
// search is retried on the next player (files may be copied in while the app runs) but logged once.
static const char *dl_error()
{
    const char *e = dlerror();
    return e ? e : "unknown error";
}

static void *load_bambu_camera_module()
{
    static void *module = nullptr;
    static bool  quiet  = false;
    if (module)
        return module;
    namespace fs = boost::filesystem;
    std::vector<fs::path> dirs;
    dirs.push_back(Slic3r::GUI::camera_module_dir(fs::path(Slic3r::data_dir())));
    if (NSString *exe = [[NSBundle mainBundle] executablePath])
        dirs.push_back(Slic3r::GUI::bundled_camera_dir(fs::path([exe fileSystemRepresentation]).parent_path()));
    for (const fs::path &dir : dirs) {
        const fs::path lib  = dir / Slic3r::GUI::bambu_source_library_name();
        const fs::path live = dir / Slic3r::GUI::live555_library_name();
        const auto     kind = Slic3r::GUI::classify_camera_module(lib);
        if (kind != Slic3r::GUI::CameraModuleKind::BambuCamera) {
            if (!quiet) BOOST_LOG_TRIVIAL(info) << "[camera module] " << lib.string() << ": "
                                    << (kind == Slic3r::GUI::CameraModuleKind::Missing         ? "missing" :
                                        kind == Slic3r::GUI::CameraModuleKind::UltraNetStorage ? "EdgeSlicer's storage module, not a camera module" :
                                                                                                  "not Bambu's camera module (no BambuPlayer export)");
            continue;
        }
        boost::system::error_code ec;
        if (fs::exists(live, ec)) {
            if (void *h = dlopen(live.string().c_str(), RTLD_NOW | RTLD_GLOBAL))
                BOOST_LOG_TRIVIAL(info) << "[camera module] loaded " << live.string() << " (" << h << ")";
            else
                BOOST_LOG_TRIVIAL(warning) << "[camera module] could not load " << live.string() << ": " << dl_error()
                                           << " - RTSP cameras (X1/H2D/H2C) will not play";
        } else {
            BOOST_LOG_TRIVIAL(warning) << "[camera module] no " << live.string() << " beside the camera module - RTSP cameras (X1/H2D/H2C) will not play";
        }
        module = dlopen(lib.string().c_str(), RTLD_LAZY);
        if (module) {
            BOOST_LOG_TRIVIAL(info) << "[camera module] live view uses " << lib.string() << " (" << module << ")";
            return module;
        }
        BOOST_LOG_TRIVIAL(error) << "[camera module] could not load " << lib.string() << ": " << dl_error();
    }
    if (!quiet)
        BOOST_LOG_TRIVIAL(warning) << "[camera module] no Bambu camera module found; Bambu live view is unavailable "
                                      "(copy libBambuSource.dylib + liblive555.dylib into " << dirs.front().string() << ")";
    quiet = true;
    return nullptr;
}

void wxMediaCtrl2::create_player()
{
	auto module = load_bambu_camera_module();
	if (!module) {
		BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": no camera module";
		m_error = -2;
		return;
	}
    Class cls = (__bridge Class) dlsym(module, "OBJC_CLASS_$_BambuPlayer");
    if (cls == nullptr) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": the camera module has no BambuPlayer class";
        m_error = -2;
        return;
    }
    NSView * imageView = (NSView *) GetHandle();
    BambuPlayer * player = [cls alloc];
    [player initWithImageView: imageView];
    [player setLogger: bambu_log withContext: this];
    m_player = player;
}

void wxMediaCtrl2::Load(wxURI url)
{
	if (!m_player) {
		create_player();
		if (!m_player) {
			BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": create_player failed currently!";
			return;
		}
	}

    BambuPlayer * player = (BambuPlayer *) m_player;
    if (player) {
        [player close];
        m_error = 0;
        m_error = [player open: url.BuildURI().ToUTF8()];
    }
    wxMediaEvent event(wxEVT_MEDIA_STATECHANGED);
    event.SetId(GetId());
    event.SetEventObject(this);
    wxPostEvent(this, event);
}

void wxMediaCtrl2::Play()
{
	if (!m_player) {
		create_player();
		if (!m_player) {
			BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": create_player failed currently!";
			return;
		}
	}
    BambuPlayer * player2 = (BambuPlayer *) m_player;
    [player2 play];
    if (m_state != wxMEDIASTATE_PLAYING) {
        m_state = wxMEDIASTATE_PLAYING;
        wxMediaEvent event(wxEVT_MEDIA_STATECHANGED);
        event.SetId(GetId());
        event.SetEventObject(this);
        wxPostEvent(this, event);
    }
}

void wxMediaCtrl2::Stop()
{
	if (!m_player) {
		create_player();
		if (!m_player) {
			BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": create_player failed currently!";
			return;
		}
	}
    BambuPlayer * player2 = (BambuPlayer *) m_player;
    [player2 close];
    NotifyStopped();
}

void wxMediaCtrl2::NotifyStopped()
{
    if (m_state != wxMEDIASTATE_STOPPED) {
        m_state = wxMEDIASTATE_STOPPED;
        wxMediaEvent event(wxEVT_MEDIA_STATECHANGED);
        event.SetId(GetId());
        event.SetEventObject(this);
        wxPostEvent(this, event);
    }
}

wxMediaState wxMediaCtrl2::GetState() const
{
    return m_state;
}

wxSize wxMediaCtrl2::GetVideoSize() const
{
    BambuPlayer * player2 = (BambuPlayer *) m_player;
    if (player2) {
        NSSize size = [player2 videoSize];
        if (size.width > 0)
            const_cast<wxSize&>(m_video_size) = {(int) size.width, (int) size.height};
        return {(int) size.width, (int) size.height};
    } else {
        return {0, 0};
    }
}
