#ifndef slic3r_ZipWorkCleaner_hpp_
#define slic3r_ZipWorkCleaner_hpp_

#include <cstdint>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>

namespace Slic3r {

// AppConfig keys of the ZIP work-folder cleaner (PR C). New names: an earlier test build seeded
// zip_work_autoclean = 0, which must not turn this off.
inline constexpr const char* ZIP_WORK_CLEANUP_KEY      = "zip_work_cleanup";       // bool, default on
inline constexpr const char* ZIP_WORK_CLEANUP_DAYS_KEY = "zip_work_cleanup_days";  // default 30
// Random per-install id. Session records carry it; the cleaner only deletes a session whose record
// has this id, so nothing an archive extracts (or a folder another install made) is trusted.
inline constexpr const char* ZIP_WORK_INSTALL_ID_KEY   = "zip_work_install_id";

inline constexpr unsigned ZIP_WORK_CLEANUP_DEFAULT_DAYS = 30;
inline constexpr unsigned ZIP_WORK_CLEANUP_MAX_DAYS     = 3650;

// 32 lowercase hex characters from std::random_device.
std::string generate_zip_install_id();

// "30" -> 30. Empty, non-numeric or 0 gives the default; values above the max are clamped.
unsigned zip_cleanup_days_from_value(const std::string& value);

// Trust model (review A-3):
// - Only `<root>/.edge-zip-index/<session>.rec` records say a folder is ours. The index sits in
//   the root, beside the sessions, never inside an extracted tree, and archives only ever write
//   inside a session folder, so an archive cannot plant a record. A record must name this
//   install's id, so a record copied from elsewhere, or a folder made by another install, is
//   ignored. Marker files (`.edge-zip-work`) are not trusted for deletion at all; the root marker
//   only has to be present.
// - The session named by a record must be a direct child of the root, must be a real folder (a
//   symlink or Windows junction / reparse point is never followed and never deleted through), and
//   every path removed is checked to lie inside it.
// - Sessions in use (the open project, files of this run) are skipped.
class ZipWorkCleaner
{
public:
    static constexpr const char* index_dir_name() { return ".edge-zip-index"; }
    static constexpr const char* record_ext() { return ".rec"; }

    // Writes the record for `session_dir` (a direct child of `root`). `created` 0 = now.
    static bool write_session_record(const boost::filesystem::path& root,
                                     const boost::filesystem::path& session_dir,
                                     const std::string&             install_id,
                                     std::string&                   err,
                                     std::time_t                    created = 0);

    struct Options
    {
        boost::filesystem::path              root;
        std::string                          install_id;
        // Sessions created at least this many days ago are removed. 0 removes every session that
        // is not in use (Preferences > Clean now).
        unsigned                             max_age_days = ZIP_WORK_CLEANUP_DEFAULT_DAYS;
        std::time_t                          now          = 0; // 0 = time(nullptr)
        // Paths in use: a session that is, or contains, one of these is kept.
        std::vector<boost::filesystem::path> in_use;
        std::function<bool()>                cancelled;
    };

    struct Result
    {
        bool                     root_ok        = false; // root exists, is a real folder, has the marker
        unsigned                 removed        = 0;
        unsigned                 kept_young     = 0;
        unsigned                 kept_in_use    = 0;
        unsigned                 untrusted      = 0;     // record with another id, bad name, or link
        unsigned                 failed         = 0;
        unsigned                 orphan_records = 0;     // records whose folder is gone (removed)
        std::uint64_t            bytes_freed    = 0;
        std::vector<std::string> errors;
    };

    static Result clean(const Options& opt);

    struct Usage
    {
        std::uint64_t bytes    = 0;
        unsigned      sessions = 0; // folders directly under the root
    };
    // Size of everything under `root`, links not followed. Empty / missing root gives zeros.
    static Usage usage(const boost::filesystem::path& root, const std::function<bool()>& cancelled = {});

    // True when `p` is a symlink, or on Windows any reparse point (junction, mount point, cloud
    // placeholder link); such entries are removed as links, never descended into.
    static bool is_link_or_reparse(const boost::filesystem::path& p);
};

} // namespace Slic3r

#endif // slic3r_ZipWorkCleaner_hpp_
