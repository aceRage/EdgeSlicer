#include "HomeDownload.hpp"

#include "I18N.hpp"

#include "libslic3r/UntrustedInput.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <cstdio>

namespace Slic3r {
namespace GUI {
namespace HomeDownload {

namespace fs = boost::filesystem;

static std::string u8(const fs::path& p) { return boost::nowide::narrow(p.wstring()); }

Result save_model(const fs::path& folder, const std::string& name, const FetchFn& fetch, const std::atomic<bool>& cancel)
{
    Result out;
    if (!untrusted::has_model_extension(name)) {
        out.error = _u8L("EdgeSlicer only downloads model files (3MF, STL, STEP, OBJ or ZIP).");
        return out;
    }
    boost::system::error_code ec;
    fs::create_directories(folder, ec);
    if (!fs::is_directory(folder, ec)) {
        out.error = _u8L("The downloads folder could not be made:") + " " + u8(folder);
        return out;
    }
    // Claim a name nobody uses, with this process's exclusive marker.
    std::string claimed;
    if (FILE* marker = untrusted::claim_unused_download_name(folder, name, {}, claimed))
        fclose(marker);
    else {
        out.error = _u8L("No free file name for it in the downloads folder.");
        return out;
    }
    const fs::path marker = untrusted::download_marker_path(folder, claimed);
    fs::path       part;
    for (int i = 0; i < 100 && part.empty(); ++i) {
        fs::path p = marker;
        p += i == 0 ? std::string(".part") : ".part" + std::to_string(i);
        if (!fs::exists(fs::symlink_status(p, ec)))
            part = p;
    }
    auto fail = [&](const std::string& why) {
        if (!part.empty())
            fs::remove(part, ec);
        fs::remove(marker, ec);
        out.ok    = false;
        out.error = why;
        return out;
    };
    if (part.empty())
        return fail(_u8L("No free file name for it in the downloads folder."));

    size_t      written = 0;
    bool        too_big = false, write_failed = false;
    std::string error;
    bool        fetched = false;
    {
        boost::nowide::ofstream f(u8(part).c_str(), std::ios::binary | std::ios::trunc);
        if (!f)
            return fail(_u8L("The file could not be written."));
        fetched = fetch(
            [&](const char* data, size_t size) {
                if (cancel)
                    return false;
                written += size;
                if (written > untrusted::MODEL_DOWNLOAD_SIZE_LIMIT) {
                    too_big = true;
                    return false;
                }
                f.write(data, std::streamsize(size));
                if (!f)
                    write_failed = true;
                return bool(f);
            },
            error);
    }
    if (too_big)
        return fail(_u8L("The file is larger than the 500 MB limit for models."));
    if (write_failed)
        return fail(_u8L("The file could not be written."));
    if (cancel)
        return fail(_u8L("The download was stopped."));
    if (!fetched)
        return fail(error.empty() ? _u8L("The download failed.") : error);

    const uintmax_t size = fs::file_size(part, ec);
    if (ec || size == 0)
        return fail(_u8L("The file is empty."));
    std::string head(512, '\0');
    {
        boost::nowide::ifstream in(u8(part).c_str(), std::ios::binary);
        in.read(&head[0], std::streamsize(head.size()));
        head.resize(size_t(std::max<std::streamsize>(in.gcount(), 0)));
    }
    std::string why;
    if (!untrusted::content_matches_extension(claimed, head, size, &why))
        return fail(_u8L("Its content does not match its type") + " (" + why + ").");
    std::string final_name = claimed;
    fs::path    dest;
    if (!untrusted::place_download_file(part, folder, final_name, dest, ec))
        return fail(_u8L("It could not be moved into the downloads folder."));
    fs::remove(marker, ec);
    out.ok   = true;
    out.path = u8(dest);
    return out;
}

} // namespace HomeDownload
} // namespace GUI
} // namespace Slic3r
