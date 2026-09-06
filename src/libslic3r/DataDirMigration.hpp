#ifndef slic3r_DataDirMigration_hpp_
#define slic3r_DataDirMigration_hpp_

#include <string>
#include <vector>

namespace Slic3r {

// First start after a rename: the data directory moves with the app key, so
// %APPDATA%\Snapmaker_Orca became %APPDATA%\UltraOne and is now %APPDATA%\EdgeSlicer (and
// the matching paths on macOS and Linux). Everything the user cares about lives in there -
// the presets, and inside hub/ the phone token, the VAPID key pair every Web Push
// subscription is bound to, the relay destinations and the tailnet allow-list - so it has
// to come across, once, without being asked about.
//
// There have been two renames now, so the source is chosen from an ordered list rather
// than being a single name: SLIC3R_LEGACY_APP_KEYS in common_func.hpp, newest first, and
// the first entry that exists on disk wins. A user upgrading from UltraOne gets their
// UltraOne data; a user who skipped that release and comes straight from Snapmaker_Orca
// gets theirs; a user who has both - because the UltraOne migration copied rather than
// moved, so it always leaves the older directory behind - gets the newer one, which is
// the one they were actually using.
//
// The copy is a copy and never a move. The old directory is left byte-for-byte as it
// was, so the previous install keeps working and a user who dislikes the new build has a
// real rollback rather than a promise of one.
//
// The old directories' names come from SLIC3R_LEGACY_APP_KEYS in common_func.hpp and from
// nowhere else, so a later release drops all of this by emptying that list and deleting
// one call and this file.
struct DataDirMigrationResult
{
    bool        ran               = false; // a copy actually happened on this call
    bool        skipped_new_exists = false;
    bool        skipped_no_old     = false;
    std::string legacy_key; // which of SLIC3R_LEGACY_APP_KEYS was chosen; empty if none
    std::string old_dir;
    std::string new_dir;
    size_t      files_copied         = 0;
    unsigned long long bytes_copied  = 0;
    size_t      conf_paths_rewritten = 0;
    std::string error; // empty on success; non-empty means nothing was published
};

// `parent` is the directory all the data dirs are siblings in (%APPDATA%,
// ~/Library/Application Support, $XDG_CONFIG_HOME). `new_dir` is where we live now.
// Does nothing at all if `new_dir` is already in use, or if none of the old ones exist.
// `include_archive` false leaves hub/saves and hub/uploads behind - the G-code archive,
// which is the bulk of the bytes and the only part that is genuinely optional.
DataDirMigrationResult migrate_data_dir(const std::string& parent,
                                        const std::string& new_dir,
                                        bool               include_archive = true);

// The directories we used to live in, newest first.
const std::vector<std::string>& legacy_data_dir_names();

// The oldest of them - the name this fork started out with - for callers that only need
// to say one thing.
const char* legacy_data_dir_name();

// The embedded browser's profile (Windows: %LOCALAPPDATA%\<app name>\EBWebView) is keyed on the
// app name too, and it is where the web pages keep their localStorage (the Stream tab's layout and
// selections) and cookies (the Snapmaker account session). It lives beside, not inside, the data
// dir, so migrate_data_dir never saw it - a rename silently reset the Stream tab and logged the
// user out (2026-09-06). `local_parent` is the directory the per-app local folders are siblings
// in; `new_local_dir` is ours. Copies `<legacy>/EBWebView` (newest legacy first) into
// `new_local_dir/EBWebView` when ours does not exist yet, and leaves a marker so it runs once.
// Must run before any WebView is created. Returns true when a copy was made.
bool migrate_webview_profile(const std::string& local_parent, const std::string& new_local_dir, std::string* copied_from = nullptr);

} // namespace Slic3r

#endif // slic3r_DataDirMigration_hpp_
