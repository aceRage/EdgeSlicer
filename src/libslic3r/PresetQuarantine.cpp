#include "PresetQuarantine.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/format.hpp>
#include <boost/system/error_code.hpp>

#include <mutex>
#include <utility>

namespace fs = boost::filesystem;

namespace Slic3r {
namespace PresetQuarantine {

const char *dir_name = "unloadable";

// Move one file, choosing a non-colliding destination name. Returns the destination, or an
// empty string if the move could not be done. Never deletes anything.
//
// suffix_index is threaded through by the caller so a .json and its .info land under the same
// number: picking a free name independently for each would separate the pair the moment one of
// the two names is already taken.
static std::string move_into(const fs::path &src, const fs::path &dest_dir, int suffix_index)
{
    boost::system::error_code ec;

    const std::string stem      = src.stem().string();
    const std::string extension = src.extension().string();

    fs::path dest = dest_dir / (suffix_index == 0
        ? stem + extension
        : stem + "." + std::to_string(suffix_index) + extension);

    // fs::rename overwrites an existing destination on POSIX, so the free-name search above is
    // what protects the earlier casualty; re-check here because another process may have
    // created the name in between.
    if (fs::exists(dest, ec)) {
        BOOST_LOG_TRIVIAL(error) << "PresetQuarantine: destination " << dest.string()
                                 << " appeared unexpectedly; leaving " << src.string() << " in place";
        return std::string();
    }

    fs::rename(src, dest, ec);
    if (ec) {
        // Cross-device, or a locked file. Try a copy, and only remove the source when the copy
        // is known to have succeeded - if it did not, the user keeps the original.
        boost::system::error_code copy_ec;
        fs::copy_file(src, dest, copy_ec);
        if (copy_ec) {
            BOOST_LOG_TRIVIAL(error) << "PresetQuarantine: could not move " << src.string() << " to "
                                     << dest.string() << " (" << ec.message() << " / " << copy_ec.message()
                                     << "); leaving it where it is";
            return std::string();
        }
        boost::system::error_code remove_ec;
        fs::remove(src, remove_ec);
        if (remove_ec) {
            // The copy is in quarantine and the original is still there. Harmless: the original
            // will simply be quarantined again next start, under the next free suffix.
            BOOST_LOG_TRIVIAL(warning) << "PresetQuarantine: copied " << src.string() << " to "
                                       << dest.string() << " but could not remove the original ("
                                       << remove_ec.message() << ")";
        }
    }
    return dest.string();
}

// Lowest index at which neither the .json name nor the .info name is taken.
static int first_free_suffix(const fs::path &dest_dir, const fs::path &json_src)
{
    boost::system::error_code ec;
    const std::string stem = json_src.stem().string();
    const std::string ext  = json_src.extension().string();

    for (int i = 0; i < 10000; ++ i) {
        const std::string qualifier = (i == 0) ? std::string() : ("." + std::to_string(i));
        if (! fs::exists(dest_dir / (stem + qualifier + ext), ec) &&
            ! fs::exists(dest_dir / (stem + qualifier + ".info"), ec))
            return i;
    }
    // Pathological; the caller will find the destination occupied and leave the file alone.
    return 10000;
}

Entry quarantine(const std::string &file_path, const std::string &reason)
{
    Entry entry;
    entry.original_path = file_path;
    entry.reason        = reason;

    boost::system::error_code ec;
    const fs::path src(file_path);
    if (! fs::exists(src, ec)) {
        // Nothing to preserve. Still reported, so the count matches what the log says.
        BOOST_LOG_TRIVIAL(warning) << "PresetQuarantine: " << file_path << " no longer exists";
        return entry;
    }

    const fs::path dest_dir = src.parent_path() / dir_name;
    fs::create_directories(dest_dir, ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(error) << "PresetQuarantine: could not create " << dest_dir.string()
                                 << " (" << ec.message() << "); leaving " << file_path << " in place";
        return entry;
    }

    const int suffix = first_free_suffix(dest_dir, src);

    entry.quarantined_path = move_into(src, dest_dir, suffix);
    if (entry.quarantined_path.empty())
        return entry;

    // The .info carries the preset's cloud/sync identity. It is only meaningful next to its
    // .json, so it travels with it - under the same suffix.
    fs::path info_src(src);
    info_src.replace_extension(".info");
    if (fs::exists(info_src, ec))
        move_into(info_src, dest_dir, suffix);

    BOOST_LOG_TRIVIAL(error) << boost::format("PresetQuarantine: %1% could not be parsed (%2%); moved to %3% instead of deleting it")
                                    % entry.original_path % reason % entry.quarantined_path;
    return entry;
}

std::string Report::directory() const
{
    for (const Entry &e : m_entries)
        if (! e.quarantined_path.empty())
            return fs::path(e.quarantined_path).parent_path().string();
    return std::string();
}

void Report::append(const Report &other)
{
    m_entries.insert(m_entries.end(), other.m_entries.begin(), other.m_entries.end());
}

static std::mutex  g_mutex;
static Report      g_pending;

void record(Entry entry)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pending.add(std::move(entry));
}

Report take()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    Report out = g_pending;
    g_pending.clear();
    return out;
}

Report peek()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_pending;
}

} // namespace PresetQuarantine
} // namespace Slic3r
