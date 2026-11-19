#ifndef slic3r_GUI_PluginGuard_hpp_
#define slic3r_GUI_PluginGuard_hpp_

#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>

namespace Slic3r { namespace GUI {

// EdgeSlicer ships its own clean-room network plug-in ("UltraNet"), a drop-in for Bambu's
// bambu_networking.dll. Two things then have to be kept straight, and both are decided here so the
// decision can be tested without wx, a config file, a network or a printer:
//
//  1. Sign-in needs SOME plug-in. Bambu cloud sign-in with Google goes login webview -> system
//     browser -> our loopback on 13650 -> ticket exchange through the plug-in. With no plug-in
//     loaded the ticket has nowhere to go, so opening the sign-in page at all is a dead end;
//     say so (the plug-in is missing, reinstall EdgeSlicer) instead of opening it.
//
//  2. EdgeSlicer NEVER downloads or installs Bambu's network plug-in (owner decision, 2026-10).
//     There used to be a Bambu CDN download behind the sign-in path, the Device tab, the setup
//     wizard, Preferences, the home-page banner and the "plug-in needs updating" check; every one
//     of them now shows the "network plug-in is missing" message (network_plugin_missing_notice()
//     decides when) and the CDN code itself is gone or refused (bambu_cdn_download_allowed() is
//     always false). The only package still fetched from the CDN is the camera component, see
//     bambu_cdn_package_allowed().
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
    // No agent and no UltraNet: the network plug-in is missing from this install. Say so; there is
    // nothing to download (see bambu_cdn_download_allowed()).
    PluginMissing,
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

// Whether Bambu's network plug-in may be downloaded from the Bambu CDN. NEVER: EdgeSlicer ships its
// own plug-in, and a build without one reports it missing rather than fetching Bambu's. The
// parameters are kept so every call site documents the state it was in; the answer ignores them.
bool bambu_cdn_download_allowed(bool plugin_present, bool ultranet_marker);

// The one package still fetched through GUI_App::download_plugin(): the Windows camera component
// (BambuSource), which the user is asked about first. Anything else - network_plugin.zip,
// camera_tools.zip, a name nobody has thought of yet - is refused.
bool bambu_cdn_package_allowed(const std::string &package_name);

enum class PluginMissingNotice {
    Show,     // tell the user the network plug-in is missing
    Suppress, // say nothing
};

// When to tell the user "EdgeSlicer's network plug-in is missing".
//
//   ultranet_installed - our plug-in is installed. Nothing is missing (if it failed to load, the
//                        restart prompt of the sign-in path is the right message, not this one).
//   user_requested     - the user just asked for something that needs the plug-in (clicked Sign in,
//                        opened the Device tab, ticked the Preferences box). Always answered.
//   already_shown      - the message was already shown this session. Automatic triggers (start-up
//                        checks, the home-page banner, notifications) stay quiet after the first.
PluginMissingNotice network_plugin_missing_notice(bool ultranet_installed, bool user_requested, bool already_shown);

// What the start-up copier does with <data_dir>/plugins, given the sidecar folder ("ultranet"
// beside the exe) that a released build ships. Decided from four facts so it is testable.
//
// The old rule was "copy only if absent - never clobber a user/CDN-updated copy". That left two
// holes a user actually fell into: (1) after an EdgeSlicer upgrade the older UltraNet stayed in
// place and, since it no longer matched the sidecar byte for byte, got no marker - so every Bambu
// CDN entry point came back to life and the "plug-in needs updating" prompt unzipped Bambu's
// package over ours; (2) a data dir migrated from Snapmaker Orca arrived with Bambu's plug-in
// already in plugins/, which our host cannot use on Windows, and ours was never installed at all.
// There is no supported way to run a Bambu-original plug-in in EdgeSlicer, so the sidecar copy is
// authoritative: anything else in that slot is replaced, unless the user sets the escape hatch
// (app config `ultranet_keep_foreign_plugin`, for developers testing another build of the DLL).
enum class PluginSync {
    Nothing,         // no sidecar, or an identical copy already marked, or the escape hatch is set
    InstallFresh,    // plugins/ holds no network library: copy ours in and write the marker
    ReplaceForeign,  // plugins/ holds a different library (older UltraNet, Bambu's, or a hand copy):
                     // replace it with ours and (re)write the marker
    WriteMarkerOnly, // byte-identical to ours but unmarked (a 2.3.7.0 install): just mark it
};

PluginSync plugin_sync_decision(bool sidecar_present,
                                bool installed_present,
                                bool installed_identical,
                                bool marker_present,
                                bool keep_foreign);

// The network plug-in's file name on this platform - the name NetworkAgent loads from
// <data_dir>/plugins and the name the sidecar ships it under: bambu_networking.dll on Windows,
// libbambu_networking.dylib on macOS, libbambu_networking.so on Linux.
const char *network_library_name();

// Where a released build keeps the bundled plug-in (the "sidecar"), given the folder of the
// running executable. <exe dir>/ultranet on Windows and Linux (the AppImage's bin/ultranet);
// Contents/Resources/ultranet inside the macOS app bundle, because Contents/MacOS may hold only
// code - a text file there (the marker) makes the bundle unsignable.
boost::filesystem::path ultranet_sidecar_dir(const boost::filesystem::path &exe_dir);
// The same rule with the layout spelled out, so both can be tested on any platform.
boost::filesystem::path ultranet_sidecar_dir(const boost::filesystem::path &exe_dir, bool macos_app_bundle);

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

// What copy_network_if_available() does at startup with a plug-in staged in <data_dir>/ota by the
// old Bambu plug-in update check (`update_network_plugin` = true in the app config). Stock code
// copied ota/bambu_networking.dll, BambuSource and live555 straight over <data_dir>/plugins. No
// build stages anything there any more, but a data dir written by an older EdgeSlicer can still
// carry the flag and the files, and installing Bambu's package is never allowed, whatever
// `ultranet_keep_foreign_plugin` says: that switch keeps a hand-copied DLL, it does not make the
// CDN package acceptable.
enum class OtaPluginInstall {
    NothingStaged, // the flag is not set: nothing to do
    Refuse,        // clear the flag, remove the staged files by exact name, log
};

OtaPluginInstall ota_plugin_install_decision(bool update_flag);

// The exact file names in <data_dir>/ota that a refused install removes: the network library,
// BambuSource and live555 for this platform, and the network_plugins.json that described them.
// Never a pattern and never a path outside ota/.
const char *const *ota_plugin_staged_names(size_t &count);

// Whether the first-run/upgrade copier may write BambuSource over `dest`. A real filter the user
// obtained from Bambu must survive an EdgeSlicer upgrade, so the sidecar stub never replaces it.
bool may_overwrite_bambusource(bool dest_exists, bool dest_is_real_filter);

// macOS/Linux have no DllRegisterServer to look for, so there the placeholder identifies itself
// instead: every camera module UltraNet builds for those platforms carries this string (as an
// exported constant), and Bambu's own libBambuSource never does.
extern const char *const kUltraNetModuleTag; // "UltraNet-module"

// True when the file at `lib` contains kUltraNetModuleTag. Reads the file; never loads it. A
// missing, unreadable or oversized file answers false.
bool carries_ultranet_module_tag(const boost::filesystem::path &lib);

// The copier's "is what is already in plugins/ a real camera component?" question, per platform:
// a PE exporting DllRegisterServer on Windows (exports_dll_register_server, unchanged); on macOS a
// Mach-O that exports Bambu's player class (classify_camera_module() == BambuCamera); on Linux any
// existing library that does not carry the UltraNet tag - i.e. Bambu's.
bool is_real_camera_component(const boost::filesystem::path &lib);
// Whether start-up should copy the sidecar's BambuSource over <data_dir>/plugins/BambuSource.dll.
// BambuSource is checked on its own rather than only when the network plug-in is replaced: since
// it carries the storage browser it can change while bambu_networking.dll stays byte-identical,
// and a copy that failed once (the file was in use) must be retried on the next start. Never over
// Bambu's real camera filter, and not while the user keeps a foreign plug-in on purpose.
bool bambusource_needs_refresh(bool bundled_present,
                               bool installed_present,
                               bool identical,
                               bool installed_is_real_filter,
                               bool keep_foreign);

// ---------------------------------------------------------------------------------------------
// The camera module, kept apart from the storage module (owner decision 2026-10-11).
//
// Two different libraries share Bambu's file name (BambuSource.dll / libBambuSource.dylib):
//
//  * UltraNet's STORAGE module, in <data_dir>/plugins. It carries the storage browser's LAN
//    tunnel (Bambu_Open & co.) and nothing else. PrinterFileSystem loads it through
//    NetworkAgent::get_bambu_source_entry().
//  * Bambu's own CAMERA module, which plays live video: the DirectShow filter on Windows
//    (registered under CLSID {233E64FB-...}, loaded by Windows Media Player), the BambuPlayer
//    Objective-C class on macOS. It needs live555 next to it for RTSP/RTSPS cameras
//    (X1/H2D/H2C). It lives in <data_dir>/cameratools, seeded from the app's own cameratools
//    folder: EdgeSlicer ships Bambu's camera files until its own go2rtc live view is proven on
//    every platform. Bambu's network plug-in is still never shipped or downloaded.
//
// With both in plugins/ under the one name, whichever is there breaks the other feature: the 2.4.5.4
// Mac build had UltraNet's there (live view: "Failed to start liveview"), and a hand-copied Bambu
// module there broke the storage browser. Linux keeps the old single-module behaviour.

// What a library file is, read off its bytes (never loaded). Works on any platform for any of the
// three formats, so the rules can be tested anywhere:
//   PE      - BambuCamera when it exports DllRegisterServer (the COM filter).
//   Mach-O  - BambuCamera when it exports _OBJC_CLASS_$_BambuPlayer and does not carry the UltraNet
//             tag. Thin or universal (fat) images; the symbol table and the dyld export trie are
//             both consulted, so a stripped local symbol table does not hide the export.
//   any     - UltraNetStorage when it carries kUltraNetModuleTag or exports EdgeSlicer_TunnelLanOnly.
enum class CameraModuleKind {
    Missing,         // no such file
    BambuCamera,     // Bambu's camera module (live view)
    UltraNetStorage, // EdgeSlicer's storage module (or its older placeholder)
    Other,           // something else: unreadable, malformed, or a library we cannot place
};
CameraModuleKind classify_camera_module(const boost::filesystem::path &lib);

// The exported-symbol probes behind classify_camera_module(), on an in-memory image. Exposed so the
// tests can feed synthetic images. Never throw; a malformed image answers false.
bool pe_image_exports(const std::string &image, const char *symbol);
bool macho_image_exports(const std::string &image, const char *symbol); // thin or fat; "_name" form

// The Mach-O symbol of Bambu's player class, the one wxMediaCtrl2.mm instantiates.
extern const char *const kBambuPlayerClassSymbol; // "_OBJC_CLASS_$_BambuPlayer"

// The camera module's runtime dependency for RTSP/RTSPS cameras, per platform: live555.dll,
// liblive555.dylib, liblive555.so.
const char *live555_library_name();

// Where the app bundle carries Bambu's camera files (packaging: CAMERA_BIN_DIR), given the folder of
// the running executable: <exe dir>/cameratools on Windows, Contents/Resources/cameratools in the
// macOS bundle (Contents/MacOS may hold only code; macos_sign_app.sh signs the dylibs where they
// are). Mirrors ultranet_sidecar_dir().
boost::filesystem::path bundled_camera_dir(const boost::filesystem::path &exe_dir, bool macos_app_bundle);
boost::filesystem::path bundled_camera_dir(const boost::filesystem::path &exe_dir);

// The data-dir folder the camera module is used from: <data_dir>/cameratools.
boost::filesystem::path camera_module_dir(const boost::filesystem::path &data_dir);

// Start-up: <data_dir>/plugins holds Bambu's camera module where UltraNet's storage module belongs
// (a hand copy, an older camera-component download, or the owner's 2.4.5.4 Mac workaround). What to
// do with it, given what cameratools already has:
enum class PluginsCameraRepair {
    Nothing,            // plugins/ holds no camera module
    MoveToCameraTools,  // cameratools has no camera module: this one becomes it
    RemoveDuplicate,    // cameratools already has a byte-identical copy: drop the plugins/ one
    MoveAsideDuplicate, // cameratools has a different camera module: keep that one, park this one
                        // beside it as <name>.from-plugins so nothing the user put there is lost
};
PluginsCameraRepair plugins_camera_repair_decision(bool plugins_has_camera_module,
                                                   bool cameratools_has_camera_module,
                                                   bool identical);

// After a repair (or whenever plugins/ has no storage module): where UltraNet's storage module comes
// back from. The app's sidecar copy first; failing that the copy the owner set aside by hand as
// <name>.ultranet - but only when that file really is UltraNet's storage module.
enum class StorageModuleRestore {
    Nothing,     // plugins/ already holds the storage module
    FromSidecar, // copy the app's bundled UltraNet module in
    FromBackup,  // rename plugins/<name>.ultranet back to plugins/<name>
    Unavailable, // nothing to restore from; the storage browser is unavailable until a reinstall
};
StorageModuleRestore storage_module_restore_decision(bool plugins_has_storage_module,
                                                     bool sidecar_has_storage_module,
                                                     bool backup_is_storage_module);

// Seeding <data_dir>/cameratools from the app's bundled camera folder.
enum class CameraModuleSync {
    Nothing,  // nothing bundled (or the bundled file is not a camera module), or already identical
    Install,  // cameratools has no camera module: copy the bundled one in
    Refresh,  // cameratools has what an earlier EdgeSlicer put there (or junk): replace it
    KeepUser, // cameratools has a real camera module the user put there: never overwritten
};
//   bundled_is_camera_module - the app's cameratools holds a real camera module
//   installed_present        - <data_dir>/cameratools/<name> exists
//   installed_is_camera_module - ... and is a real camera module
//   identical                - byte-identical to the bundled one
//   installed_from_bundle    - it is the copy an earlier EdgeSlicer installed (the record matches)
CameraModuleSync camera_module_sync_decision(bool bundled_is_camera_module,
                                             bool installed_present,
                                             bool installed_is_camera_module,
                                             bool identical,
                                             bool installed_from_bundle);

// The record kept in <data_dir>/cameratools of what EdgeSlicer installed there from its bundle, so
// an upgrade can refresh its own copy without ever touching one the user supplied. One line per
// file: "<name> <size> <fnv1a-64 hex>".
extern const char *const kCameraBundleRecordName; // "edgeslicer_bundled.txt"
std::string camera_file_fingerprint(const boost::filesystem::path &file); // "<size> <hash>", "" if unreadable
// The fingerprint recorded for `name` in `record_text`, or "" when it has none.
std::string camera_record_lookup(const std::string &record_text, const std::string &name);
// `record_text` with the line for `name` replaced (or added).
std::string camera_record_update(const std::string &record_text, const std::string &name, const std::string &fingerprint);

// Windows live view: whether the registered DirectShow filter can be used as it is, or EdgeSlicer
// should register its own cameratools copy (per user, HKCU, no UAC). A working registration that
// points at another install (OrcaSlicer, Bambu Studio) is left alone so the apps do not fight over
// it; a missing one, a dangling path or one that points at a non-filter (UltraNet's storage module
// after a repair) is replaced when we have a real filter to offer.
enum class FilterRegistration {
    UseRegistered,     // the registered filter is fine
    RegisterOurs,      // register <data_dir>/cameratools/BambuSource.dll for this user
    OfferComponent,    // nothing usable registered and no filter of ours: offer the component
};
FilterRegistration filter_registration_decision(bool clsid_matches,
                                                bool registered_path_is_real_filter,
                                                bool ours_is_real_filter);

// The start-up file work behind GUI_App::sync_bambu_camera_module(), on plain paths so it can be
// run against dummy files in a temp folder. Three steps, each decided by the functions above:
//  1. plugins/<source> is Bambu's camera module -> it moves to camera_dir (or is dropped / parked
//     as <source>.from-plugins); plugins/<live> always moves with it (UltraNet ships no live555).
//  2. plugins/<source> is now missing -> UltraNet's storage module comes back from sidecar_dir, else
//     from plugins/<source>.ultranet when that really is UltraNet's module.
//  3. camera_dir is seeded from bundled_dir (source + live555), with the record that lets a later
//     upgrade refresh EdgeSlicer's own copy and never a user's.
// Steps 1-2 are skipped with keep_foreign (the developer escape hatch). A file another process has
// loaded is moved aside to <name>.replaced rather than overwritten (removed on the next run).
// Returns what was done, for the log. Never throws.
struct CameraSyncPaths {
    boost::filesystem::path plugins_dir; // <data_dir>/plugins
    boost::filesystem::path camera_dir;  // <data_dir>/cameratools
    boost::filesystem::path sidecar_dir; // the app's UltraNet sidecar (ultranet_sidecar_dir)
    boost::filesystem::path bundled_dir; // the app's camera files (bundled_camera_dir)
    std::string             source_name; // bambu_source_library_name()
    std::string             live_name;   // live555_library_name()
    bool                    keep_foreign = false;
};
struct CameraSyncLog {
    enum Level { Info, Warning, Error } level;
    std::string text;
};
std::vector<CameraSyncLog> sync_camera_module_files(const CameraSyncPaths &paths);

// ---------------------------------------------------------------------------------------------
// The Device tab's storage browser (MediaFilePanel::fetchUrl): whether to hand the tunnel library
// the printer's LAN address (bambu:///local/<ip>...) rather than ask the network agent for a
// cloud relay URL.
//
// Stock rule: the printer is in LAN-only mode or offers no remote file protocol, it offers the
// local one, and its IP is known. A tunnel library that can only reach printers on the LAN
// (PrinterFileSystem::TunnelIsLanOnly, EdgeSlicer's) has no other route at all, so it also takes
// the LAN address for a cloud-bound printer - whenever the IP and the access code are both known.
bool storage_browser_use_lan_url(bool lan_mode,
                                 bool local_proto,
                                 bool remote_proto,
                                 bool has_ip,
                                 bool has_access_code,
                                 bool lan_only_tunnel);

} } // namespace Slic3r::GUI

#endif // slic3r_GUI_PluginGuard_hpp_
