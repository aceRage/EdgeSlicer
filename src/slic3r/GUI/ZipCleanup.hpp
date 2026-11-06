#ifndef slic3r_GUI_ZipCleanup_hpp_
#define slic3r_GUI_ZipCleanup_hpp_

#include <functional>
#include <string>

#include <boost/filesystem/path.hpp>

#include "libslic3r/ZipWorkCleaner.hpp"

namespace Slic3r {
namespace GUI {

// Background clean-up of the ZIP working folder (PR C). Runs on one low-priority worker thread,
// never on the UI thread: at startup, after each zip open, and from Preferences (Clean now). What
// may be deleted is decided by ZipWorkCleaner (only sessions with this install's record, older
// than the configured days, not links, not in use).
namespace ZipCleanup {

enum class Mode {
    UsageOnly, // just measure the folder (Preferences)
    Auto,      // the configured age, only when clean-up is on
    Now        // Preferences > Clean now: every session not in use, whatever the setting
};

// The configured root (zip_work_dir) or the default local cache folder.
boost::filesystem::path work_root();

// A session folder this run created (or loaded from). Kept until the app exits.
void note_session(const boost::filesystem::path& session);

// Queue a pass. Reads the settings and the paths in use on the calling (UI) thread. Returns at
// once; a pass already running picks the request up when it ends.
void start(Mode mode);

// Called on the UI thread after each pass with the folder's usage and, for a clean, its result.
// One listener (the Preferences page); pass {} to remove it.
using Listener = std::function<void(const ZipWorkCleaner::Usage& usage, const ZipWorkCleaner::Result* result)>;
void set_listener(Listener listener);

// Cancel and join the worker (MainFrame::shutdown). Later start() calls are ignored.
void shutdown();

} // namespace ZipCleanup
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_ZipCleanup_hpp_
