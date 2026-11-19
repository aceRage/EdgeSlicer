#ifndef slic3r_GUI_HomeDownload_hpp_
#define slic3r_GUI_HomeDownload_hpp_

#include <atomic>
#include <functional>
#include <string>

#include <boost/filesystem/path.hpp>

namespace Slic3r {
namespace GUI {

// Saving a model file that comes from outside into a folder of the user's, the safe way: the same
// checks the Models browser and the "Open in" downloads make (libslic3r/UntrustedInput.hpp). Used by
// the Home tab's Vaults, and written so that Vendors' downloads can move onto it too (if the two
// sections are merged, design: tests/design_vaults_home_tab.md).
//   * only model types (.3mf .stl .step .stp .obj .zip), under a sanitized name that nothing in the
//     folder uses (an exclusive marker claims it while the file arrives; nothing is ever replaced),
//   * streamed to a ".part" file, stopped past untrusted::MODEL_DOWNLOAD_SIZE_LIMIT (500 MB),
//   * its first bytes must match its type (a 3MF is a ZIP, an STL's size agrees with its triangles...),
//   * then moved into place; the marker and any partial file are removed on every failure.
// Runs on the calling thread: call it off the GUI thread.
namespace HomeDownload {

// Fetches the file, handing each piece to `sink` (false = stop). Returns true when the whole file
// came; on failure sets `error` (for the user, without a secret or a URL).
using FetchFn = std::function<bool(const std::function<bool(const char* data, size_t size)>& sink, std::string& error)>;

struct Result
{
    bool        ok { false };
    std::string path;  // the saved file (UTF-8)
    std::string error; // for the user
};

Result save_model(const boost::filesystem::path& folder, const std::string& name, const FetchFn& fetch, const std::atomic<bool>& cancel);

} // namespace HomeDownload
} // namespace GUI
} // namespace Slic3r

#endif
