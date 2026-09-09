#ifndef slic3r_GUI_PluginGuard_hpp_
#define slic3r_GUI_PluginGuard_hpp_

#include <string>

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

} } // namespace Slic3r::GUI

#endif // slic3r_GUI_PluginGuard_hpp_
