#include "VendorInstallStamp.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <system_error>
#include <vector>

namespace Slic3r { namespace Utils {

namespace sfs = std::filesystem;

namespace {

const char *const kStampHeader = "edgeslicer-vendor-install-stamp 1";

bool stamp_line(const sfs::path &file, const std::string &rel, std::vector<std::string> &out)
{
    std::error_code ec;
    const auto size = sfs::file_size(file, ec);
    if (ec)
        return false;
    const auto mtime = sfs::last_write_time(file, ec);
    if (ec)
        return false;
    out.push_back(rel + "|" + std::to_string(size) + "|" + std::to_string((long long) mtime.time_since_epoch().count()));
    return true;
}

std::string fnv1a64_hex(const std::vector<std::string> &lines)
{
    uint64_t h = 14695981039346656037ULL;
    auto     feed = [&h](unsigned char c) {
        h ^= c;
        h *= 1099511628211ULL;
    };
    for (const std::string &l : lines) {
        for (unsigned char c : l)
            feed(c);
        feed('\n');
    }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long) h);
    // The line count too: two trees cannot then collide by moving bytes between lines.
    return std::string(buf) + "-" + std::to_string(lines.size());
}

std::string field(const std::string &record, const std::string &key)
{
    std::istringstream in(record);
    std::string        line;
    const std::string  prefix = key + "=";
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.compare(0, prefix.size(), prefix) == 0)
            return line.substr(prefix.size());
    }
    return std::string();
}

bool has_header(const std::string &record)
{
    std::istringstream in(record);
    std::string        first;
    std::getline(in, first);
    if (!first.empty() && first.back() == '\r')
        first.pop_back();
    return first == kStampHeader;
}

} // namespace

std::string tree_stamp_digest(const std::string &dir, const std::string &extra_file)
{
    try {
        const sfs::path root = sfs::u8path(dir);
        std::error_code ec;
        if (!sfs::is_directory(root, ec))
            return std::string();
        std::vector<std::string> lines;
        sfs::recursive_directory_iterator it(root, ec), end;
        if (ec)
            return std::string();
        for (; it != end; it.increment(ec)) {
            if (ec)
                return std::string();
            std::error_code fec;
            if (!it->is_regular_file(fec))
                continue;
            const std::string rel = it->path().lexically_relative(root).generic_u8string();
            if (!stamp_line(it->path(), rel, lines))
                return std::string();
        }
        if (ec)
            return std::string();
        if (lines.empty())
            return std::string();
        std::sort(lines.begin(), lines.end());
        if (!extra_file.empty()) {
            const sfs::path extra = sfs::u8path(extra_file);
            if (!stamp_line(extra, "<json>/" + extra.filename().u8string(), lines))
                return std::string();
        }
        return fnv1a64_hex(lines);
    } catch (...) {
        return std::string();
    }
}

int compare_dotted_versions(const std::string &a, const std::string &b)
{
    auto next = [](const std::string &s, size_t &pos) -> long {
        if (pos >= s.size())
            return 0;
        const size_t      dot = s.find('.', pos);
        const std::string tok = (dot == std::string::npos) ? s.substr(pos) : s.substr(pos, dot - pos);
        pos                   = (dot == std::string::npos) ? s.size() : dot + 1;
        return std::strtol(tok.c_str(), nullptr, 10);
    };
    size_t pa = 0, pb = 0;
    while (pa < a.size() || pb < b.size()) {
        const long va = next(a, pa), vb = next(b, pb);
        if (va != vb)
            return va < vb ? -1 : 1;
    }
    return 0;
}

VendorInstallDecision decide_vendor_install(const VendorInstallInputs &in)
{
    if (!in.installed_json_exists || !in.installed_dir_has_files)
        return {true, "missing"};
    if (!in.resource_version.empty() && compare_dotted_versions(in.resource_version, in.installed_version) > 0)
        return {true, "newer version " + in.resource_version + " (installed " + in.installed_version + ")"};
    if (in.stamp_record.empty() || !has_header(in.stamp_record))
        return {true, "no install stamp"};
    if (in.resource_digest.empty() || field(in.stamp_record, "resource") != in.resource_digest)
        return {true, "resource bundle changed"};
    if (in.installed_digest.empty() || field(in.stamp_record, "installed") != in.installed_digest)
        return {true, "installed copy changed"};
    return {false, "unchanged since the last install"};
}

std::string make_vendor_install_stamp(const std::string &resource_digest, const std::string &installed_digest)
{
    if (resource_digest.empty() || installed_digest.empty())
        return std::string();
    return std::string(kStampHeader) + "\nresource=" + resource_digest + "\ninstalled=" + installed_digest + "\n";
}

}} // namespace Slic3r::Utils
