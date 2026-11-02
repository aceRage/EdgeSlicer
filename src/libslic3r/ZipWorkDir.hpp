#ifndef slic3r_ZipWorkDir_hpp_
#define slic3r_ZipWorkDir_hpp_

#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>

namespace Slic3r {

// Per-extraction working folder under a caller-supplied root. PR B wires the default
// Documents/EdgeSlicer/Zip; this helper only takes the root as a parameter.
//
// Layout: <root>/<zip-stem>_<yyyymmdd-hhmmss>[_n]/<entry relative path>
// A marker file `.edge-zip-work` is written in `root` so later cleanup (PR C) can refuse
// to run against an unmarked folder.
class ZipWorkDir
{
public:
    static constexpr const char* marker_name() { return ".edge-zip-work"; }

    // Create `root` if needed and write the marker (left in place if it already exists).
    static bool ensure_root(const boost::filesystem::path& root, std::string& err);

    // ensure_root, then create a unique session subdirectory named
    // <zip-stem>_<yyyymmdd-hhmmss> or <zip-stem>_<yyyymmdd-hhmmss>_n on collision.
    static bool create_session(const boost::filesystem::path& root,
                               const boost::filesystem::path& zip_path,
                               boost::filesystem::path&       session_dir,
                               std::string&                   err);

    // Choose a dest path under `session_dir` for `relative_utf8` (`a/b.stl`).
    // Duplicate names become `name(1).ext`, `name(2).ext`, ...
    // On Windows and macOS, case-insensitive collisions (`A.stl` vs `a.stl`) also get a suffix.
    // `taken` is the set of dests already claimed by this extraction (not yet necessarily on disk).
    static boost::filesystem::path uniquify_entry_path(const boost::filesystem::path&              session_dir,
                                                       const std::string&                          relative_utf8,
                                                       const std::vector<boost::filesystem::path>& taken = {});
};

} // namespace Slic3r

#endif // slic3r_ZipWorkDir_hpp_
