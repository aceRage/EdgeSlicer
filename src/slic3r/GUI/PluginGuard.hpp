#ifndef slic3r_GUI_PluginGuard_hpp_
#define slic3r_GUI_PluginGuard_hpp_

#include <string>

#include <boost/filesystem/path.hpp>

namespace Slic3r { namespace GUI {

// EdgeSlicer ships its own clean-room network plug-in ("UltraNet"), a drop-in for Bambu's
// bambu_networking.dll. Two things then have to be kept straight, and both are decided here so the
// decision can be tested without wx, a config file, a network or a printer:
//
//  1. Sign-in needs SOME plug-in. Bambu cloud sign-in with Google goes login webview -> system
//     browser -> our loopback on 13650 -> ticket exchange through the plug-in. With no plug-in
//     loaded the ticket has nowhere to go, so opening the sign-in page at all is a dead end;
//     offer the download instead.
//
//  2. UltraNet must never be overwritten. install_plugin() unzips Bambu's CDN package straight
//     over data_dir/plugins, and the "plug-in needs updating" path calls into the same code, so
//     every download entry point has to be shut off while our own plug-in is the one installed.
//
// UltraNet is recognised by a marker file written next to the DLLs (see kUltraNetMarkerName): the
// DLL name is Bambu's, so the DLL alone cannot tell the two apart. A folder holding
// bambu_networking.dll and no marker is a Bambu-original (or CDN-updated) plug-in and keeps the
// stock behaviour in full.
extern const char *const kUltraNetMarkerName; // "ultranet.txt"

// What the app should do when the user asks to sign in to a Bambu account.
enum class LoginGuardAction {
    // A plug-in is loaded and usable: open the sign-in page as before.
    ShowLogin,
    // No agent. UltraNet is installed, so the download dialog would be wrong (and dangerous) -
    // the plug-in is there and simply did not load, e.g. it needs the restart after a first-run
    // install. Tell the user to restart rather than offering Bambu's CDN.
    RestartRequired,
    // No agent and no UltraNet: offer the Bambu network plug-in download.
    OfferPluginDownload,
};

// The single decision behind every guarded entry point.
//
//   plugin_present      - data_dir/plugins holds a bambu_networking library
//   ultranet_marker     - ... and our marker file sits beside it
//   installed_networking- the app_config preference
//   agent_loaded        - wxGetApp().getAgent() != nullptr
LoginGuardAction plugin_guard_decision(bool plugin_present,
                                       bool ultranet_marker,
                                       bool installed_networking,
                                       bool agent_loaded);

// True when the installed plug-in is ours. This is the one that gates every Bambu CDN download and
// update prompt: the home-page banner command, the Device-tab install link, the Preferences
// checkbox's download offer, the wizard's offer, and m_networking_need_update handling.
// A marker with no DLL beside it is a leftover, not an installation.
bool is_ultranet_plugin(bool plugin_present, bool ultranet_marker);

// True when the Bambu CDN download/update path may run. It is the exact complement of the above,
// so an empty plug-in folder or a Bambu-original plug-in keeps working untouched.
bool bambu_cdn_download_allowed(bool plugin_present, bool ultranet_marker);


// ---------------------------------------------------------------------------------------------
// The camera component (BambuSource), which is a different thing from the network plug-in.
//
// Live view on a Bambu printer is played through BambuSource, a proprietary DirectShow source
// filter registered under CLSID {233E64FB-...}. It is NOT part of UltraNet and we cannot clean-room
// it: EdgeSlicer ships only a ~9.7 KB stub named BambuSource.dll so NetworkAgent's LoadLibrary
// probe of the plug-ins folder succeeds. The stub exports no DllRegisterServer, so the stock
// "press Yes to re-register it" path can only ever fail with "the entry-point DllRegisterServer
// was not found" - the reported bug.
//
// Everything here therefore has to tell the stub from the real filter, and must do so without
// running the DLL: the probe reads the PE export directory off the file on disk, never LoadLibrary
// (which would run DllMain of an untrusted binary and pin the file).

// The name of the DirectShow filter, per platform.
const char *bambu_source_library_name();

// True when `dll` is a real Bambu camera filter: it exists and its PE export table (or the
// platform equivalent) exports DllRegisterServer. Missing file, unreadable file, or a malformed
// image all answer false - the caller then treats it as "no real component", which is safe.
bool exports_dll_register_server(const boost::filesystem::path &dll);

// True when `dll` is EdgeSlicer's placeholder rather than Bambu's filter: our marker file sits in
// the same folder AND the library does not export DllRegisterServer. Both halves are required - a
// real filter dropped into our plug-ins folder keeps the marker beside it and must still be
// recognised as real, and a stray unexporting DLL somewhere without a marker is not ours to judge.
bool is_ultranet_bambusource_stub(const boost::filesystem::path &dll);

// What start_stream_service() should do about <data_dir>/cameratools/BambuSource.dll, decided from
// three facts so it can be tested without a filesystem.
enum class CameraToolsCopy {
    // cameratools has no usable filter and the plug-ins copy is only our stub: copying would
    // install a DLL that cannot be registered. Skip it and let the caller report the component as
    // missing.
    SkipStubMissingComponent,
    // cameratools already holds a real filter. Never overwrite it - in particular never with the
    // stub, which is the second half of the reported bug.
    KeepExisting,
    // Stock behaviour: the plug-ins copy is a real filter and cameratools is absent or stale.
    CopyFromPlugins,
};

CameraToolsCopy camera_tools_copy_decision(bool plugins_copy_is_stub,
                                           bool cameratools_has_real_filter,
                                           bool cameratools_up_to_date);

// Whether the first-run/upgrade copier may write BambuSource over `dest`. A real filter the user
// obtained from Bambu must survive an EdgeSlicer upgrade, so the sidecar stub never replaces it.
bool may_overwrite_bambusource(bool dest_exists, bool dest_is_real_filter);

} } // namespace Slic3r::GUI

#endif // slic3r_GUI_PluginGuard_hpp_
