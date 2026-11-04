#include "ZipWorkCleaner.hpp"

#include "ZipWorkDir.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <random>
#include <sstream>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <boost/nowide/convert.hpp>
#endif

namespace Slic3r {

namespace fs = boost::filesystem;

namespace {

constexpr const char* RECORD_HEADER = "EdgeSlicer zip session v1";
// Folder depth of an extracted tree is bounded by the path length; this only stops a runaway walk.
constexpr unsigned MAX_DELETE_DEPTH = 256;

std::string path_utf8(const fs::path& p)
{
#ifdef _WIN32
    return boost::nowide::narrow(p.native());
#else
    return p.string();
#endif
}

fs::path utf8_path(const std::string& s)
{
#ifdef _WIN32
    return fs::path(boost::nowide::widen(s));
#else
    return fs::path(s);
#endif
}

bool cancelled(const std::function<bool()>& c) { return c && c(); }

std::string lower_generic(const fs::path& p)
{
    std::string s = path_utf8(p.lexically_normal());
    std::replace(s.begin(), s.end(), '\\', '/');
    while (s.size() > 1 && s.back() == '/')
        s.pop_back();
#if defined(_WIN32) || defined(__APPLE__)
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif
    return s;
}

// `p` equals `dir` or lies below it (lexically, case-folded where the file system folds case).
bool is_same_or_inside(const fs::path& p, const fs::path& dir)
{
    const std::string a = lower_generic(p);
    const std::string b = lower_generic(dir);
    if (b.empty())
        return false;
    return a == b || (a.size() > b.size() && a.compare(0, b.size(), b) == 0 && a[b.size()] == '/');
}

fs::path absolute_or_self(const fs::path& p)
{
    boost::system::error_code ec;
    fs::path                  a = fs::absolute(p, ec);
    return ec ? p : a;
}

// A session name from a record file name: one plain path segment we could have created.
bool valid_session_name(const std::string& name)
{
    if (name.empty() || name == "." || name == ".." || name[0] == '.')
        return false;
    for (unsigned char c : name)
        if (c == '/' || c == '\\' || c == ':' || c < 0x20)
            return false;
    return true;
}

bool real_directory(const fs::path& p)
{
    boost::system::error_code ec;
    const fs::file_status     st = fs::symlink_status(p, ec);
    return !ec && fs::is_directory(st) && !ZipWorkCleaner::is_link_or_reparse(p);
}

bool real_regular_file(const fs::path& p)
{
    boost::system::error_code ec;
    const fs::file_status     st = fs::symlink_status(p, ec);
    return !ec && fs::is_regular_file(st) && !ZipWorkCleaner::is_link_or_reparse(p);
}

// Removes a link itself (never its target).
bool remove_link(const fs::path& p, boost::system::error_code& ec)
{
#ifdef _WIN32
    const DWORD attr = ::GetFileAttributesW(p.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        ec.assign(static_cast<int>(::GetLastError()), boost::system::system_category());
        return false;
    }
    // A directory symlink or junction is removed with RemoveDirectory, which deletes the link
    // and leaves the target alone; a file symlink with DeleteFile.
    const BOOL ok = (attr & FILE_ATTRIBUTE_DIRECTORY) ? ::RemoveDirectoryW(p.c_str()) : ::DeleteFileW(p.c_str());
    if (!ok) {
        ec.assign(static_cast<int>(::GetLastError()), boost::system::system_category());
        return false;
    }
    ec.clear();
    return true;
#else
    fs::remove(p, ec); // removes the symlink, not what it points to
    return !ec;
#endif
}

struct DeleteStats
{
    std::uint64_t bytes  = 0;
    unsigned      errors = 0;
    std::string   first_error;
};

void note_error(DeleteStats& stats, const fs::path& p, const boost::system::error_code& ec)
{
    if (stats.errors++ == 0)
        stats.first_error = path_utf8(p) + ": " + ec.message();
}

// Deletes the contents of `dir` and then `dir`, never following a link and never touching a path
// outside `bound`.
void delete_tree(const fs::path& dir, const fs::path& bound, unsigned depth, DeleteStats& stats, const std::function<bool()>& stop)
{
    if (depth > MAX_DELETE_DEPTH || !is_same_or_inside(dir, bound)) {
        note_error(stats, dir, boost::system::errc::make_error_code(boost::system::errc::operation_not_permitted));
        return;
    }
    boost::system::error_code ec;
    std::vector<fs::path>     children;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        children.push_back(it->path());
    if (ec) {
        note_error(stats, dir, ec);
        return;
    }
    for (const fs::path& child : children) {
        if (cancelled(stop))
            return;
        if (!is_same_or_inside(child, bound) || child == dir) {
            note_error(stats, child, boost::system::errc::make_error_code(boost::system::errc::operation_not_permitted));
            continue;
        }
        if (ZipWorkCleaner::is_link_or_reparse(child)) {
            if (!zip_fs_retry([&](boost::system::error_code& e) { remove_link(child, e); }, ec))
                note_error(stats, child, ec);
            continue;
        }
        const fs::file_status st = fs::symlink_status(child, ec);
        if (ec) {
            note_error(stats, child, ec);
            continue;
        }
        if (fs::is_directory(st)) {
            delete_tree(child, bound, depth + 1, stats, stop);
            continue;
        }
        const std::uintmax_t size = fs::is_regular_file(st) ? fs::file_size(child, ec) : 0;
        if (ec)
            ec.clear();
        if (zip_fs_retry([&](boost::system::error_code& e) { fs::remove(child, e); }, ec))
            stats.bytes += size == static_cast<std::uintmax_t>(-1) ? 0 : size;
        else
            note_error(stats, child, ec);
    }
    if (cancelled(stop))
        return;
    if (!zip_fs_retry([&](boost::system::error_code& e) { fs::remove(dir, e); }, ec))
        note_error(stats, dir, ec);
}

struct Record
{
    std::string id;
    std::time_t created = 0;
};

bool read_record(const fs::path& file, Record& rec)
{
    boost::system::error_code ec;
    if (!real_regular_file(file) || fs::file_size(file, ec) > 4096 || ec)
        return false;
    boost::nowide::ifstream in(path_utf8(file), std::ios::binary);
    if (!in)
        return false;
    std::string line;
    if (!std::getline(in, line))
        return false;
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    if (line != RECORD_HEADER)
        return false;
    bool have_created = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.compare(0, 3, "id=") == 0)
            rec.id = line.substr(3);
        else if (line.compare(0, 8, "created=") == 0) {
            try {
                size_t                  idx = 0;
                const long long         v   = std::stoll(line.substr(8), &idx, 10);
                if (idx == line.size() - 8 && v > 0) {
                    rec.created  = static_cast<std::time_t>(v);
                    have_created = true;
                }
            } catch (const std::exception&) {
            }
        }
    }
    return !rec.id.empty() && have_created;
}

} // namespace

std::string generate_zip_install_id()
{
    std::random_device                      rd;
    std::uniform_int_distribution<unsigned> nibble(0, 15);
    static const char* const                hex = "0123456789abcdef";
    std::string                             id;
    id.reserve(32);
    for (int i = 0; i < 32; ++i)
        id.push_back(hex[nibble(rd)]);
    return id;
}

unsigned zip_cleanup_days_from_value(const std::string& value)
{
    try {
        size_t                   idx = 0;
        const unsigned long long v   = std::stoull(value, &idx, 10);
        if (idx != value.size() || v == 0)
            return ZIP_WORK_CLEANUP_DEFAULT_DAYS;
        return static_cast<unsigned>(std::min<unsigned long long>(v, ZIP_WORK_CLEANUP_MAX_DAYS));
    } catch (const std::exception&) {
        return ZIP_WORK_CLEANUP_DEFAULT_DAYS;
    }
}

bool ZipWorkCleaner::is_link_or_reparse(const fs::path& p)
{
#ifdef _WIN32
    WIN32_FIND_DATAW data;
    const HANDLE     h = ::FindFirstFileW(p.c_str(), &data);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD attr = ::GetFileAttributesW(p.c_str());
        return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT);
    }
    ::FindClose(h);
    if (!(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return false;
    // Cloud Files placeholders (OneDrive and others, IO_REPARSE_TAG_CLOUD_*) are ordinary files and
    // folders to the user; every other reparse point (symlink, junction, mount point, ...) is a link.
    const DWORD tag = data.dwReserved0;
    return (tag & 0xFFFF0FFFu) != 0x9000001Au;
#else
    boost::system::error_code ec;
    return fs::is_symlink(fs::symlink_status(p, ec)) && !ec;
#endif
}

bool ZipWorkCleaner::write_session_record(const fs::path&    root,
                                          const fs::path&    session_dir,
                                          const std::string& install_id,
                                          std::string&       err,
                                          std::time_t        created)
{
    err.clear();
    if (install_id.empty()) {
        err = "no install id";
        return false;
    }
    const std::string name = path_utf8(session_dir.filename());
    if (!valid_session_name(name) || lower_generic(session_dir.parent_path()) != lower_generic(root)) {
        err = "session folder is not a direct child of the zip working folder";
        return false;
    }
    const fs::path            index = root / index_dir_name();
    boost::system::error_code ec;
    if (!fs::exists(fs::symlink_status(index, ec))) {
        fs::create_directory(index, ec);
        if (ec) {
            err = "cannot create " + path_utf8(index) + " (" + ec.message() + ")";
            return false;
        }
    }
    if (!real_directory(index)) {
        err = path_utf8(index) + " is not a plain folder";
        return false;
    }
    const fs::path file = index / utf8_path(name + record_ext());
    try {
        boost::nowide::ofstream out(path_utf8(file), std::ios::binary | std::ios::trunc);
        out << RECORD_HEADER << "\n"
            << "id=" << install_id << "\n"
            << "created=" << static_cast<long long>(created > 0 ? created : std::time(nullptr)) << "\n";
        if (!out) {
            err = "cannot write " + path_utf8(file);
            return false;
        }
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    return true;
}

ZipWorkCleaner::Result ZipWorkCleaner::clean(const Options& opt)
{
    Result r;
    if (opt.root.empty() || opt.install_id.empty())
        return r;
    const fs::path root = absolute_or_self(opt.root).lexically_normal();
    // The root itself must be a real folder (not a link to somewhere else) carrying our marker.
    if (!real_directory(root) || !real_regular_file(root / ZipWorkDir::marker_name()))
        return r;
    const fs::path index = root / index_dir_name();
    r.root_ok            = true;
    if (!real_directory(index))
        return r;

    std::vector<fs::path> in_use;
    for (const fs::path& p : opt.in_use)
        if (!p.empty())
            in_use.push_back(absolute_or_self(p));

    const std::time_t now = opt.now > 0 ? opt.now : std::time(nullptr);
    const std::time_t max_age = static_cast<std::time_t>(opt.max_age_days) * 24 * 60 * 60;

    std::vector<fs::path>     records;
    boost::system::error_code ec;
    for (fs::directory_iterator it(index, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path p = it->path();
        if (p.extension() == fs::path(record_ext()))
            records.push_back(p);
    }
    std::sort(records.begin(), records.end());

    for (const fs::path& rec_file : records) {
        if (cancelled(opt.cancelled))
            break;
        Record rec;
        const std::string name = path_utf8(rec_file.stem());
        if (!read_record(rec_file, rec) || rec.id != opt.install_id || !valid_session_name(name)) {
            ++r.untrusted;
            continue;
        }
        const fs::path session = root / utf8_path(name);
        if (lower_generic(session.parent_path()) != lower_generic(root)) {
            ++r.untrusted;
            continue;
        }
        // We only ever create plain folders; a link (or junction) in a session's place is left
        // alone, checked before anything else so it is never mistaken for a missing folder.
        if (is_link_or_reparse(session)) {
            ++r.untrusted;
            continue;
        }
        const fs::file_status st = fs::symlink_status(session, ec);
        if (ec || !fs::exists(st)) {
            // The folder is gone (deleted by hand): drop the stale record.
            ec.clear();
            if (zip_fs_retry([&](boost::system::error_code& e) { fs::remove(rec_file, e); }, ec))
                ++r.orphan_records;
            continue;
        }
        if (!fs::is_directory(st)) {
            ++r.untrusted;
            continue;
        }
        bool used = false;
        for (const fs::path& u : in_use)
            if (is_same_or_inside(u, session)) {
                used = true;
                break;
            }
        if (used) {
            ++r.kept_in_use;
            continue;
        }
        // A session made at or after the moment the clean started (another zip opened meanwhile)
        // is never removed, even by Clean now.
        if (rec.created >= now || (opt.max_age_days > 0 && now - rec.created < max_age)) {
            ++r.kept_young;
            continue;
        }
        DeleteStats stats;
        delete_tree(session, session, 0, stats, opt.cancelled);
        r.bytes_freed += stats.bytes;
        if (stats.errors > 0 || fs::exists(fs::symlink_status(session, ec))) {
            ++r.failed;
            if (!stats.first_error.empty())
                r.errors.push_back(stats.first_error);
            continue;
        }
        ++r.removed;
        zip_fs_retry([&](boost::system::error_code& e) { fs::remove(rec_file, e); }, ec);
    }
    if (r.removed > 0 || r.failed > 0)
        BOOST_LOG_TRIVIAL(info) << "ZIP work folder clean-up in " << path_utf8(root) << ": removed " << r.removed << " session(s), "
                                << r.bytes_freed << " bytes; kept " << r.kept_young << " recent, " << r.kept_in_use << " in use; "
                                << r.failed << " failed, " << r.untrusted << " untrusted";
    return r;
}

ZipWorkCleaner::Usage ZipWorkCleaner::usage(const fs::path& root, const std::function<bool()>& stop)
{
    Usage u;
    if (root.empty() || !real_directory(root))
        return u;
    boost::system::error_code ec;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().filename() != fs::path(index_dir_name()) && real_directory(it->path()))
            ++u.sessions;
    ec.clear();
    // recursive_directory_iterator does not follow directory symlinks by default; Windows
    // junctions are skipped explicitly.
    fs::recursive_directory_iterator it(root, ec), end;
    while (!ec && it != end) {
        if (cancelled(stop))
            break;
        const fs::path p = it->path();
        if (is_link_or_reparse(p)) {
            it.disable_recursion_pending();
        } else {
            const fs::file_status st = fs::symlink_status(p, ec);
            if (!ec && fs::is_regular_file(st)) {
                const std::uintmax_t s = fs::file_size(p, ec);
                if (!ec)
                    u.bytes += s;
            }
            ec.clear();
        }
        it.increment(ec);
    }
    return u;
}

} // namespace Slic3r
