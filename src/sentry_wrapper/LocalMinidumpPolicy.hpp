#pragma once

// Pure rules for the local crash dumps that test copies write (Snapmaker_Orca.cpp,
// install_local_minidumps): the file name and which old files to remove. No Windows calls here,
// so the slic3rutils tests can check them on any platform.

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <string>
#include <vector>

namespace Slic3r {
namespace local_minidump {

// The crash folder keeps at most this many files of ours. Startup prunes to one fewer, so the
// dump of the run that follows still fits.
constexpr size_t kMaxFiles = 10;

// Every file the writer makes starts with this: the dump itself (".dmp") and, when
// MiniDumpWriteDump fails, a short note with its error (".dmp.failed.txt").
constexpr const wchar_t* kPrefix = L"EdgeSlicer_";

// "EdgeSlicer_<pid>_<yyyymmdd>_<hhmmss>.dmp" into `buf`. Allocation-free: it runs in a crashing
// process. Returns the length written, or 0 when the buffer is too small.
inline size_t format_name(wchar_t* buf, size_t buf_len, unsigned long pid, int year, int month, int day, int hour, int minute,
                          int second)
{
    if (buf == nullptr || buf_len == 0)
        return 0;
    const int n = std::swprintf(buf, buf_len, L"EdgeSlicer_%lu_%04d%02d%02d_%02d%02d%02d.dmp", pid, year, month, day, hour, minute,
                                second);
    if (n < 0 || size_t(n) >= buf_len) {
        buf[0] = L'\0';
        return 0;
    }
    return size_t(n);
}

inline bool ends_with(const std::wstring& s, const wchar_t* suffix)
{
    const size_t n = std::wcslen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// A file the writer made, so one rotation may remove. Anything else in the folder is left alone.
inline bool is_our_file(const std::wstring& name)
{
    if (name.rfind(kPrefix, 0) != 0)
        return false;
    return ends_with(name, L".dmp") || ends_with(name, L".dmp.failed.txt");
}

struct FileEntry
{
    std::wstring  name;
    std::uint64_t size{0};
    std::uint64_t mtime{0}; // any monotonic file time (Windows FILETIME ticks in the app)
};

// Which files to delete so at most `keep` of ours remain. Empty dumps (an earlier writer's
// leftovers: they hold nothing to debug) always go; the rest go oldest first. Files that are not
// ours are never listed.
inline std::vector<std::wstring> files_to_remove(std::vector<FileEntry> files, size_t keep)
{
    std::vector<std::wstring> out;
    std::vector<FileEntry>    kept;
    for (FileEntry& f : files) {
        if (!is_our_file(f.name))
            continue;
        if (f.size == 0 && ends_with(f.name, L".dmp"))
            out.push_back(f.name);
        else
            kept.push_back(std::move(f));
    }
    if (kept.size() > keep) {
        // Newest first; the name (pid + time) breaks ties so the order never depends on the listing.
        std::sort(kept.begin(), kept.end(), [](const FileEntry& a, const FileEntry& b) {
            return a.mtime != b.mtime ? a.mtime > b.mtime : a.name > b.name;
        });
        for (size_t i = keep; i < kept.size(); ++i)
            out.push_back(kept[i].name);
    }
    return out;
}

} // namespace local_minidump
} // namespace Slic3r
