#ifndef __PluginInstallStatus_HPP__
#define __PluginInstallStatus_HPP__

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <functional>
#include "slic3r/GUI/Jobs/Job.hpp"
#include <wx/window.h>

// These includes and the alias below were reached through the old UpgradeNetworkJob.hpp, and a lot
// of GUI code picked them up transitively via GUI_App.hpp. Kept so none of it has to change.
namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {

// Progress reporting for the one Bambu CDN fetch that is left, the camera component
// (GUI_App::install_bambu_camera_component). The job/dialog machinery that used to download
// Bambu's network plug-in with these is gone: EdgeSlicer never downloads that plug-in.
enum PluginInstallStatus {
    InstallStatusNormal = 0,
    InstallStatusDownloadFailed = 1,
    InstallStatusDownloadCompleted = 2,
    InstallStatusUnzipFailed = 3,
    InstallStatusInstallCompleted = 4,
};

typedef std::function<void(int status, int percent, bool& cancel)> InstallProgressFn;

}} // namespace Slic3r::GUI

#endif
