#include "ArchiveImport.hpp"

#include "libslic3r/UntrustedInput.hpp"
#include "libslic3r/miniz_extension.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <cstdlib>
#include <cstring>

namespace Slic3r { namespace GUI { namespace ArchiveImport {

namespace fs = boost::filesystem;

std::string clean_name(const std::string& name)
{
    const std::string clean = untrusted::sanitize_download_filename(name);
    if (clean.size() <= std::strlen(".gcode.3mf") || !boost::algorithm::iends_with(clean, ".gcode.3mf")) return {};
    return clean;
}

// "Metadata/plate_12.gcode" -> 12; anything else -> -1.
static int gcode_plate_of(const std::string& entry)
{
    static const char* prefix = "Metadata/plate_";
    static const char* suffix = ".gcode";
    const size_t       np = std::strlen(prefix), ns = std::strlen(suffix);
    if (entry.size() <= np + ns || entry.compare(0, np, prefix) != 0 || entry.compare(entry.size() - ns, ns, suffix) != 0) return -1;
    const std::string digits = entry.substr(np, entry.size() - np - ns);
    if (digits.empty() || digits.size() > 4 || digits.find_first_not_of("0123456789") != std::string::npos) return -1;
    return std::atoi(digits.c_str());
}

Package check_package(const std::string& path)
{
    Package p;
    boost::system::error_code ec;
    const std::uint64_t size = (std::uint64_t) fs::file_size(fs::path(path), ec);
    if (ec) { p.error = "the uploaded file cannot be read"; return p; }
    if (size > untrusted::MODEL_DOWNLOAD_SIZE_LIMIT) { p.error = "the file is larger than 500 MB"; return p; }
    {
        boost::nowide::ifstream f(path.c_str(), std::ios::binary);
        char                    head[64] = {};
        f.read(head, sizeof(head));
        std::string why;
        if (!f.gcount() || !untrusted::content_matches_extension("job.3mf", std::string(head, (size_t) f.gcount()), size, &why)) {
            p.error = "the file is not a .gcode.3mf (" + (why.empty() ? std::string("not a ZIP") : why) + ")";
            return p;
        }
    }
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!open_zip_reader(&zip, path)) { p.error = "the file is not a readable .gcode.3mf (ZIP)"; return p; }
    struct Closer { mz_zip_archive& z; ~Closer() { close_zip_reader(&z); } } closer { zip };

    const mz_uint n = mz_zip_reader_get_num_files(&zip);
    if (n == 0) { p.error = "the file is empty"; return p; }
    if (n > MAX_ENTRIES) { p.error = "the file holds too many entries for a sliced job"; return p; }
    p.entries = n;
    int gcode_plates = 0;
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) { p.error = "the file's table of contents is damaged"; return p; }
        if (zip_entry_is_symlink(st)) { p.error = "the file contains a link, which a sliced job never does"; return p; }
        const std::string raw = decode_archive_entry_path(&zip, st);
        std::string       name;
        const untrusted::ArchiveEntryName verdict = untrusted::normalize_archive_entry_path(raw, name);
        if (verdict == untrusted::ArchiveEntryName::Reject) { p.error = "the file contains an entry with an unsafe name"; return p; }
        if (verdict == untrusted::ArchiveEntryName::Skip || st.m_is_directory) continue;
        p.uncompressed += (std::uint64_t) st.m_uncomp_size;
        if (p.uncompressed > MAX_UNCOMPRESSED) { p.error = "the file would unpack to more than 4 GB"; return p; }
        const int plate = gcode_plate_of(name);
        if (plate > 0) {
            ++gcode_plates;
            if (p.plate < 0) p.plate = plate;
        }
    }
    if (gcode_plates == 0) { p.error = "the file holds no G-code; slice the plate and export it as a .gcode.3mf"; return p; }
    if (gcode_plates > 1) { p.error = "the file holds G-code for several plates; export one plate"; return p; }

    // The plate's picture, for the Reprint list. Optional: a job without one is still a job.
    const std::string want = "Metadata/plate_" + std::to_string(p.plate) + ".png";
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        std::string name;
        if (untrusted::normalize_archive_entry_path(decode_archive_entry_path(&zip, st), name) != untrusted::ArchiveEntryName::Ok) continue;
        if (name != want) continue;
        std::string png, why;
        if (read_zip_entry_capped(zip, i, MAX_THUMBNAIL, png, &why) && png.size() > 8 && png.compare(1, 3, "PNG") == 0) p.thumbnail_png = png;
        break;
    }
    return p;
}

}}} // namespace Slic3r::GUI::ArchiveImport
