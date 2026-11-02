#include "ZipWorkDir.hpp"

#include "UntrustedInput.hpp"

#include <cctype>
#include <ctime>
#include <exception>
#include <string>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#ifdef _WIN32
#include <boost/nowide/convert.hpp>
#endif

namespace Slic3r {

namespace {

#if defined(_WIN32) || defined(__APPLE__)
constexpr bool zip_fs_case_insensitive = true;
#else
constexpr bool zip_fs_case_insensitive = false;
#endif

#ifdef _WIN32
boost::filesystem::path utf8_path(const std::string& name_utf8) { return boost::filesystem::path(boost::nowide::widen(name_utf8)); }
std::string             path_utf8(const boost::filesystem::path& p) { return boost::nowide::narrow(p.native()); }
#else
boost::filesystem::path utf8_path(const std::string& name_utf8) { return boost::filesystem::path(name_utf8); }
std::string             path_utf8(const boost::filesystem::path& p) { return p.string(); }
#endif

std::string ascii_lower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool paths_collide(const boost::filesystem::path& a, const boost::filesystem::path& b)
{
    const std::string ga = a.lexically_normal().generic_string();
    const std::string gb = b.lexically_normal().generic_string();
    if (zip_fs_case_insensitive)
        return ascii_lower(ga) == ascii_lower(gb);
    return ga == gb;
}

bool leaf_exists(const boost::filesystem::path& p)
{
    boost::system::error_code ec;
    const auto                st = boost::filesystem::symlink_status(p, ec);
    return !ec && boost::filesystem::exists(st);
}

bool case_variant_exists(const boost::filesystem::path& candidate)
{
    if (!zip_fs_case_insensitive)
        return leaf_exists(candidate);
    if (leaf_exists(candidate))
        return true;
    namespace fs = boost::filesystem;
    boost::system::error_code ec;
    const fs::path            parent = candidate.parent_path();
    if (parent.empty() || !fs::is_directory(parent, ec))
        return false;
    const std::string want = ascii_lower(candidate.filename().generic_string());
    for (fs::directory_iterator it(parent, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (ascii_lower(it->path().filename().generic_string()) == want)
            return true;
    }
    return false;
}

bool is_taken(const boost::filesystem::path& candidate, const std::vector<boost::filesystem::path>& taken)
{
    if (case_variant_exists(candidate))
        return true;
    for (const boost::filesystem::path& p : taken)
        if (paths_collide(p, candidate))
            return true;
    return false;
}

std::string local_timestamp()
{
    const std::time_t now = std::time(nullptr);
    std::tm           tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[16];
    if (std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm) == 0)
        return "00000000-000000";
    return buf;
}

void split_stem_ext(const std::string& leaf, std::string& stem, std::string& ext)
{
    const size_t dot = leaf.find_last_of('.');
    if (dot != std::string::npos && dot != 0) {
        stem = leaf.substr(0, dot);
        ext  = leaf.substr(dot);
    } else {
        stem = leaf;
        ext.clear();
    }
}

} // namespace

bool ZipWorkDir::ensure_root(const boost::filesystem::path& root, std::string& err)
{
    namespace fs = boost::filesystem;
    err.clear();
    if (root.empty()) {
        err = "zip working directory root is empty";
        return false;
    }
    boost::system::error_code ec;
    if (!fs::is_directory(root, ec)) {
        if (leaf_exists(root)) {
            err = path_utf8(root) + " is not a directory";
            return false;
        }
        fs::create_directories(root, ec);
        if (ec) {
            err = "create directory failed: " + path_utf8(root) + " (" + ec.message() + ")";
            return false;
        }
    }
    const fs::path marker = root / marker_name();
    if (leaf_exists(marker))
        return true;
    try {
        boost::nowide::ofstream out(path_utf8(marker), std::ios::binary | std::ios::trunc);
        if (!out) {
            err = "cannot write marker file: " + path_utf8(marker);
            return false;
        }
        out << "EdgeSlicer zip working directory\n";
        if (!out) {
            err = "cannot write marker file: " + path_utf8(marker);
            return false;
        }
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    return true;
}

bool ZipWorkDir::create_session(const boost::filesystem::path& root,
                                const boost::filesystem::path& zip_path,
                                boost::filesystem::path&       session_dir,
                                std::string&                   err)
{
    namespace fs = boost::filesystem;
    session_dir.clear();
    if (!ensure_root(root, err))
        return false;

    std::string stem = untrusted::sanitize_download_filename(zip_path.stem().string());
    if (stem.empty())
        stem = "zip";
    const std::string base = stem + "_" + local_timestamp();

    for (unsigned n = 0; n < 1000; ++n) {
        const std::string name = (n == 0) ? base : base + "_" + std::to_string(n);
        const fs::path    dir  = root / utf8_path(name);
        if (is_taken(dir, {}))
            continue;
        boost::system::error_code ec;
        if (!fs::create_directory(dir, ec) || ec) {
            if (ec == boost::system::errc::file_exists)
                continue;
            err = "create directory failed: " + path_utf8(dir) + " (" + ec.message() + ")";
            return false;
        }
        session_dir = dir;
        return true;
    }
    err = "could not allocate a unique zip working subdirectory under " + path_utf8(root);
    return false;
}

boost::filesystem::path ZipWorkDir::uniquify_entry_path(const boost::filesystem::path&              session_dir,
                                                        const std::string&                          relative_utf8,
                                                        const std::vector<boost::filesystem::path>& taken)
{
    namespace fs               = boost::filesystem;
    const size_t      slash    = relative_utf8.rfind('/');
    const std::string dir_part = slash == std::string::npos ? std::string() : relative_utf8.substr(0, slash);
    const std::string leaf     = slash == std::string::npos ? relative_utf8 : relative_utf8.substr(slash + 1);

    std::string stem;
    std::string ext;
    split_stem_ext(leaf, stem, ext);

    const fs::path parent = dir_part.empty() ? session_dir : session_dir / utf8_path(dir_part);
    for (unsigned n = 0; n < 10000; ++n) {
        const std::string name = (n == 0) ? (stem + ext) : (stem + "(" + std::to_string(n) + ")" + ext);
        const fs::path    dest = parent / utf8_path(name);
        if (!is_taken(dest, taken))
            return dest;
    }
    return parent / utf8_path(stem + "(" + std::to_string(10000) + ")" + ext);
}

} // namespace Slic3r
