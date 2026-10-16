#ifndef slic3r_Utils_BambuConnect_hpp_
#define slic3r_Utils_BambuConnect_hpp_

// "Send to Bambu Connect": hand a sliced Bambu .gcode.3mf to the user's own Bambu Connect app
// through Bambu's documented URL scheme (wiki.bambulab.com/en/software/bambu-connect):
//
//   bambu-connect://import-file?path=<encodeURIComponent(abs path)>&name=<encodeURIComponent(name)>&version=1.0.0
//
// Bambu Connect then prints via the user's Bambu account (cloud) or LAN; EdgeSlicer never talks to
// Bambu's cloud or the printer on this path. Research: tests/research_bambu_connect.md (main tree).
//
// URL building, naming and the installation decision are pure functions so they can be unit tested
// without wxWidgets, a registry or a filesystem (tests/slic3rutils/bambu_connect_tests.cpp). The real
// probes (registry, NSWorkspace, xdg-mime, temp folder) are thin adapters at the bottom.

#include <ctime>
#include <functional>
#include <string>
#include <vector>

namespace Slic3r {
namespace BambuConnect {

// "bambu-connect"
extern const char *const URL_SCHEME;
// The fixed `version` parameter Bambu documents ("1.0.0").
extern const char *const PROTOCOL_VERSION;
// Bambu's own Bambu Connect page (description + download links). Shown as a link only.
extern const char *const INFO_URL;

// -------------------------------------------------------------------------------------------
// URL building
// -------------------------------------------------------------------------------------------

// JavaScript encodeURIComponent over UTF-8 bytes: A-Z a-z 0-9 - _ . ! ~ * ' ( ) stay, every other
// byte becomes %XX (upper-case hex). A space is %20, never '+'.
std::string encode_uri_component(const std::string &utf8);

// The full import URL. `abs_path_utf8` is the native absolute path (Windows back-slashes are kept,
// as OrcaSlicer's handoff does); `display_name_utf8` is shown by Bambu Connect as the job name.
std::string build_import_url(const std::string &abs_path_utf8, const std::string &display_name_utf8);

// -------------------------------------------------------------------------------------------
// Naming
// -------------------------------------------------------------------------------------------

// Job name for Connect from the export file name ("Cube_PLA_1h2m.gcode.3mf" -> "Cube_PLA_1h2m").
// Drops a trailing ".gcode.3mf" / ".3mf" / ".gcode" (case-insensitive) and replaces & = ? # % + with
// '_' so a lenient query parser on Connect's side can never split the value. Control characters
// are dropped, white space is trimmed. Empty -> "plate".
std::string display_name_for(const std::string &export_file_name_utf8);

// File name (with ".gcode.3mf") for the copy handed to Connect: the display name with characters
// that are invalid in Windows/macOS file names (< > : " / \ | ? *) replaced by '_', trailing dots and
// spaces trimmed, Windows device names (CON, NUL, COM1...) suffixed, and the stem cut to at most
// 100 bytes on a UTF-8 character boundary. UTF-8 (non-ASCII) characters are kept.
std::string temp_file_name_for(const std::string &display_name_utf8);

// -------------------------------------------------------------------------------------------
// Installation detection
// -------------------------------------------------------------------------------------------

enum class Installed {
    Yes,     // a handler for bambu-connect:// is registered (and, where checkable, still on disk)
    No,      // no handler: show the "Bambu Connect is not installed" dialog instead of opening
    Unknown, // could not tell (e.g. Linux without xdg-mime): try to open and report a failure
};

using FileExists = std::function<bool(const std::string &path)>;

// What Windows has under HKEY_CLASSES_ROOT\bambu-connect (HKCU and HKLM Software\Classes merged).
struct WindowsRegistration
{
    bool        key_exists       = false; // HKCR\bambu-connect
    bool        has_url_protocol = false; // the "URL Protocol" value exists (it is usually empty)
    std::string open_command;             // HKCR\bambu-connect\shell\open\command default value
};

// The executable of a shell open command: the quoted first token ("C:\a b\x.exe" "%1"), or for an
// unquoted command everything up to and including the first ".exe" (case-insensitive), or else the
// first space-separated token. Empty when the command is empty.
std::string executable_from_command(const std::string &command);

// Key + "URL Protocol" + open command required. A command whose executable no longer exists (a key
// left behind by an uninstall) counts as not installed; an unparsable command is trusted.
Installed detect_windows(const WindowsRegistration &reg, const FileExists &exists);

// macOS: the application NSWorkspace would open bambu-connect:// with (empty = none).
Installed detect_macos(const std::string &handler_app_path);

// Linux: `xdg-mime query default x-scheme-handler/bambu-connect`. `query_ran` false (xdg-mime missing
// or failed) -> Unknown; otherwise a non-blank .desktop name -> Yes, blank -> No.
Installed detect_linux(bool query_ran, const std::string &xdg_mime_output);

// The real probe for this platform (registry / NSWorkspace / xdg-mime).
Installed detect_installed();

// -------------------------------------------------------------------------------------------
// Temp folder housekeeping
// -------------------------------------------------------------------------------------------

struct TempEntry
{
    std::string file_name; // leaf name only
    std::time_t modified = 0;
};

// Leaf names to delete from our own temp folder: only "*.gcode.3mf" files, only those last written
// more than `max_age_s` seconds before `now`. Anything else in the folder is never touched.
std::vector<std::string> stale_temp_files(const std::vector<TempEntry> &entries, std::time_t now, std::time_t max_age_s);

// Files handed to Connect stay this long: Connect reads the file when the user presses Print there,
// which can be much later than the handoff.
constexpr std::time_t TEMP_MAX_AGE_S = 3 * 24 * 60 * 60;

// <system temp>/EdgeSlicer/bambu-connect as UTF-8, created if missing; empty on failure. Stale files
// (stale_temp_files) are removed from it on every call.
std::string prepare_temp_dir();

#ifdef __APPLE__
// BambuConnectMac.mm: path of the app registered for bambu-connect://, empty when there is none.
std::string macos_url_handler_app_path();
#endif

} // namespace BambuConnect
} // namespace Slic3r

#endif // slic3r_Utils_BambuConnect_hpp_
