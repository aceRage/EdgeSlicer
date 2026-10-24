#ifndef slic3r_Utils_VendorInstallStamp_hpp_
#define slic3r_Utils_VendorInstallStamp_hpp_

// When to copy a vendor bundle (today: OrcaFilamentLibrary) from <resources>/profiles into
// <data_dir>/system at startup.
//
// Upstream Orca reinstalled the filament library on every start ("always install filament
// library"): remove the installed folder, copy ~970 files back. On the owner's PC that cost
// ~1 s on a warm start and 5-15 s on a cold one (the removal alone 5.4 s in one profiled run),
// and because every file got new stats, the library's CBOR preset cache missed and was rewritten
// on every start too.
//
// Now an install leaves a stamp behind: a digest of the resource folder it copied from and a
// digest of the installed folder it produced (relative path, size and mtime of every file; no
// file is opened). A later start reinstalls only when the installed bundle is missing, the
// resource version is newer, there is no usable stamp, the resource side changed (a new build or
// an edited profile in a dev tree) or the installed side changed (a file edited, added or deleted
// under system/). In every other case the previous install is byte-for-byte what a new one would
// produce, so skipping it changes nothing but the time.

#include <string>

namespace Slic3r { namespace Utils {

// Digest of a directory tree: FNV-1a 64 over the sorted "<rel>|<size>|<mtime>" lines of every
// regular file under `dir` (recursive), plus the same line for `extra_file` when it is not empty
// (the vendor's <name>.json, which sits next to its folder). Empty when `dir` does not exist, has
// no files, or could not be listed completely; callers treat empty as "unknown: reinstall".
std::string tree_stamp_digest(const std::string &dir, const std::string &extra_file = std::string());

struct VendorInstallInputs
{
    bool        installed_json_exists = false; // <data_dir>/system/<vendor>.json
    bool        installed_dir_has_files = false; // <data_dir>/system/<vendor>/ exists and is not empty
    std::string resource_version;              // "version" of <resources>/profiles/<vendor>.json
    std::string installed_version;             // "version" of <data_dir>/system/<vendor>.json
    std::string stamp_record;                  // contents of the stamp file ("" when none)
    std::string resource_digest;               // tree_stamp_digest of the resource side now
    std::string installed_digest;              // tree_stamp_digest of the installed side now
};

struct VendorInstallDecision
{
    bool        install = true;
    std::string reason; // for the log: "missing", "newer version", "no stamp", ...
};

VendorInstallDecision decide_vendor_install(const VendorInstallInputs &in);

// The stamp file's text for an install that just finished. Empty when either digest is empty
// (nothing trustworthy to remember; the caller removes any old stamp instead).
std::string make_vendor_install_stamp(const std::string &resource_digest, const std::string &installed_digest);

// Numeric dotted-version compare of any component count: <0, 0, >0.
int compare_dotted_versions(const std::string &a, const std::string &b);

}} // namespace Slic3r::Utils

#endif // slic3r_Utils_VendorInstallStamp_hpp_
