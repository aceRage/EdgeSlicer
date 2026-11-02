#include "ZipOpen.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <limits>

#ifndef ZIP_OPEN_STANDALONE
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#ifdef _WIN32
#include <boost/nowide/convert.hpp>
#endif
#endif

namespace Slic3r {

namespace {

std::string ascii_lower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool starts_with(const std::string& s, const std::string& prefix)
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool iends_with(const std::string& name, std::string_view ext)
{
    if (name.size() < ext.size())
        return false;
    const std::string tail = ascii_lower(name.substr(name.size() - ext.size()));
    const std::string want = ascii_lower(std::string(ext));
    return tail == want;
}

bool iequals(const std::string& a, const std::string& b) { return ascii_lower(a) == ascii_lower(b); }

void trim_inplace(std::string& s)
{
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
}

bool has_extension(const std::string& name, std::string_view ext) { return iends_with(name, ext); }

bool has_any_extension(const std::string& name, const std::vector<std::string_view>& exts)
{
    for (std::string_view ext : exts)
        if (has_extension(name, ext))
            return true;
    return false;
}

std::string slash_normalized(std::string name)
{
    std::replace(name.begin(), name.end(), '\\', '/');
    return name;
}

std::string parent_dir(const std::string& name)
{
    const std::string n     = slash_normalized(name);
    const size_t      slash = n.find_last_of('/');
    return slash == std::string::npos ? std::string() : n.substr(0, slash);
}

std::string leaf_name(const std::string& name)
{
    const std::string n     = slash_normalized(name);
    const size_t      slash = n.find_last_of('/');
    return slash == std::string::npos ? n : n.substr(slash + 1);
}

std::string leaf_stem(const std::string& name)
{
    const std::string leaf = leaf_name(name);
    const size_t      dot  = leaf.find_last_of('.');
    if (dot == std::string::npos || dot == 0)
        return leaf;
    return leaf.substr(0, dot);
}

std::uint64_t parse_u64(const std::string& s, std::uint64_t fallback)
{
    if (s.empty())
        return fallback;
    try {
        size_t                   idx = 0;
        const unsigned long long v   = std::stoull(s, &idx, 10);
        if (idx != s.size())
            return fallback;
        return static_cast<std::uint64_t>(v);
    } catch (const std::exception&) {
        return fallback;
    }
}

#ifndef ZIP_OPEN_STANDALONE
#ifdef _WIN32
boost::filesystem::path utf8_path(const std::string& name_utf8) { return boost::filesystem::path(boost::nowide::widen(name_utf8)); }
#else
boost::filesystem::path utf8_path(const std::string& name_utf8) { return boost::filesystem::path(name_utf8); }
#endif

std::string expand_home(const std::string& raw, const char* home)
{
    if (raw.size() >= 5 && raw.compare(0, 5, "$HOME") == 0 && home && *home)
        return std::string(home) + raw.substr(5);
    if (!raw.empty() && raw[0] == '~' && home && *home)
        return std::string(home) + raw.substr(1);
    return raw;
}

bool dir_exists(const boost::filesystem::path& p)
{
    boost::system::error_code ec;
    return !p.empty() && boost::filesystem::is_directory(p, ec);
}

boost::filesystem::path read_xdg_documents()
{
    const char* home = std::getenv("HOME");
    if (const char* env = std::getenv("XDG_DOCUMENTS_DIR"); env && *env) {
        const boost::filesystem::path p(expand_home(env, home));
        if (!p.empty())
            return p;
    }

    boost::filesystem::path config;
    if (const char* xdg_config = std::getenv("XDG_CONFIG_HOME"); xdg_config && *xdg_config)
        config = boost::filesystem::path(xdg_config) / "user-dirs.dirs";
    else if (home && *home)
        config = boost::filesystem::path(home) / ".config" / "user-dirs.dirs";
    if (config.empty())
        return {};

    boost::nowide::ifstream in(config.string());
    if (!in)
        return {};
    std::string line;
    while (std::getline(in, line)) {
        const auto hash = line.find('#');
        if (hash != std::string::npos)
            line.erase(hash);
        const auto key = line.find("XDG_DOCUMENTS_DIR");
        if (key == std::string::npos)
            continue;
        const auto eq = line.find('=', key + 17);
        if (eq == std::string::npos)
            continue;
        std::string val = line.substr(eq + 1);
        trim_inplace(val);
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
            val = val.substr(1, val.size() - 2);
        return boost::filesystem::path(expand_home(val, home));
    }
    return {};
}
#endif

} // namespace

const std::vector<std::string_view>& zip_model_extensions()
{
    static const std::vector<std::string_view> exts = {
        ".3mf", ".stl", ".oltp", ".stp", ".step", ".svg", ".amf", ".obj", ".glb", ".gltf"
#ifdef __APPLE__
        ,
        ".usd", ".usda", ".usdc", ".usdz", ".abc", ".ply"
#endif
    };
    return exts;
}

const std::vector<std::string_view>& zip_gcode_extensions()
{
    static const std::vector<std::string_view> exts = {".bgcode", ".gcode"};
    return exts;
}

const std::vector<std::string_view>& zip_sidecar_image_extensions()
{
    static const std::vector<std::string_view> exts = {".png", ".jpg", ".jpeg", ".bmp", ".tga", ".tif", ".tiff", ".webp"};
    return exts;
}

bool zip_entry_name_is_ignored(const std::string& decoded_name, bool is_directory)
{
    if (is_directory)
        return true;
    const std::string n = slash_normalized(decoded_name);
    if (n == "__MACOSX" || starts_with(n, "__MACOSX/") || n.find("/__MACOSX/") != std::string::npos)
        return true;
    return starts_with(leaf_name(n), "._");
}

ZipEntryKind classify_zip_entry_name(const std::string& decoded_name, bool is_directory)
{
    if (zip_entry_name_is_ignored(decoded_name, is_directory))
        return ZipEntryKind::Ignored;
    if (has_extension(decoded_name, ".zip") && !has_extension(decoded_name, ".zip.amf"))
        return ZipEntryKind::NestedZip;
    if (has_any_extension(decoded_name, zip_gcode_extensions()))
        return ZipEntryKind::GCode;
    if (has_any_extension(decoded_name, zip_model_extensions()))
        return ZipEntryKind::Model;
    return ZipEntryKind::Unsupported;
}

bool zip_entry_is_selectable(ZipEntryKind kind, bool is_encrypted, bool is_symlink)
{
    return (kind == ZipEntryKind::Model || kind == ZipEntryKind::GCode) && !is_encrypted && !is_symlink;
}

bool should_auto_open_zip(std::size_t selectable_count, bool option_enabled)
{
    return option_enabled && selectable_count == 1;
}

bool zip_archive_is_all_encrypted(const std::vector<ZipListedEntry>& entries)
{
    bool any_file = false;
    for (const ZipListedEntry& e : entries) {
        if (zip_entry_name_is_ignored(e.decoded_name, e.is_directory))
            continue;
        any_file = true;
        if (!e.is_encrypted)
            return false;
    }
    return any_file;
}

std::vector<std::uint32_t> zip_obj_sidecar_indices(const ZipListedEntry& obj, const std::vector<ZipListedEntry>& all)
{
    std::vector<std::uint32_t> extra;
    if (!has_extension(obj.decoded_name, ".obj"))
        return extra;
    const std::string dir  = parent_dir(obj.decoded_name);
    const std::string stem = leaf_stem(obj.decoded_name);
    for (const ZipListedEntry& e : all) {
        if (e.index == obj.index || e.is_directory || e.is_encrypted || e.is_symlink)
            continue;
        if (parent_dir(e.decoded_name) != dir)
            continue;
        const bool mtl = has_extension(e.decoded_name, ".mtl") && iequals(leaf_stem(e.decoded_name), stem);
        const bool img = has_any_extension(e.decoded_name, zip_sidecar_image_extensions());
        if (mtl || img)
            extra.push_back(e.index);
    }
    return extra;
}

bool is_zip_archive_path(const std::string& path)
{
    return has_extension(path, ".zip") && !has_extension(path, ".zip.amf");
}

#ifndef ZIP_OPEN_STANDALONE
bool running_in_flatpak()
{
    if (const char* id = std::getenv("FLATPAK_ID"); id && *id)
        return true;
    boost::system::error_code ec;
    return boost::filesystem::exists("/.flatpak-info", ec);
}

boost::filesystem::path default_zip_work_root(const boost::filesystem::path& documents_dir,
                                              const boost::filesystem::path& app_data_dir)
{
    namespace fs  = boost::filesystem;
    fs::path docs = documents_dir;
    if (!dir_exists(docs)) {
        const fs::path xdg = read_xdg_documents();
        if (dir_exists(xdg))
            docs = xdg;
        else {
            const char* home = std::getenv("HOME");
#ifdef _WIN32
            if (!(home && *home))
                home = std::getenv("USERPROFILE");
#endif
            if (home && *home) {
                const fs::path home_docs = fs::path(home) / "Documents";
                docs                     = dir_exists(home_docs) ? home_docs : fs::path(home);
            }
        }
    }

    if (running_in_flatpak() && !dir_exists(docs) && !app_data_dir.empty())
        return app_data_dir / "Zip";

    if (docs.empty())
        return fs::path("EdgeSlicer") / "Zip";
    return docs / "EdgeSlicer" / "Zip";
}

boost::filesystem::path zip_session_entry_dest(const boost::filesystem::path& session_dir, const std::string& extract_name)
{
    namespace fs           = boost::filesystem;
    const std::string n    = slash_normalized(extract_name);
    fs::path          dest = session_dir;
    size_t            start = 0;
    while (start < n.size()) {
        const size_t      slash = n.find('/', start);
        const std::string part  = n.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!part.empty() && part != ".")
            dest /= utf8_path(part);
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    return dest;
}
#endif

ZipCapValues zip_extract_caps_from_values(const std::string& max_entries_scanned,
                                          const std::string& max_entry_uncompressed,
                                          const std::string& max_total_uncompressed,
                                          const std::string& max_compression_ratio)
{
    ZipCapValues        caps;
    const std::uint64_t scanned = parse_u64(max_entries_scanned, caps.max_entries_scanned);
    caps.max_entries_scanned    = scanned > std::numeric_limits<std::uint32_t>::max() ?
                                      std::numeric_limits<std::uint32_t>::max() :
                                      static_cast<std::uint32_t>(scanned);
    caps.max_entry_uncompressed = parse_u64(max_entry_uncompressed, caps.max_entry_uncompressed);
    caps.max_total_uncompressed = parse_u64(max_total_uncompressed, caps.max_total_uncompressed);
    caps.max_compression_ratio  = parse_u64(max_compression_ratio, caps.max_compression_ratio);
    return caps;
}

std::string format_zip_size(std::uint64_t bytes)
{
    char buf[64];
    if (bytes < 1024)
        std::snprintf(buf, sizeof(buf), "%u B", static_cast<unsigned>(bytes));
    else if (bytes < 1024ull * 1024)
        std::snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    else if (bytes < 1024ull * 1024 * 1024)
        std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
    else
        std::snprintf(buf, sizeof(buf), "%.1f GB", bytes / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

} // namespace Slic3r
