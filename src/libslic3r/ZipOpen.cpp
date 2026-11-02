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

bool zip_entry_is_selectable(ZipEntryKind kind, bool is_encrypted, bool is_symlink, bool is_safe)
{
    return (kind == ZipEntryKind::Model || kind == ZipEntryKind::GCode) && !is_encrypted && !is_symlink && is_safe;
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

namespace {

std::vector<std::string> split_lines(const std::string& text)
{
    std::vector<std::string> lines;
    size_t                   start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(std::move(line));
        if (end == text.size())
            break;
        start = end + 1;
    }
    return lines;
}

// Keyword (lower case) and the rest of the line, trimmed. Comments start at '#'.
bool split_keyword(std::string line, std::string& keyword, std::string& rest)
{
    const auto hash = line.find('#');
    if (hash != std::string::npos)
        line.erase(hash);
    trim_inplace(line);
    if (line.empty())
        return false;
    size_t sp = 0;
    while (sp < line.size() && !std::isspace(static_cast<unsigned char>(line[sp])))
        ++sp;
    keyword = ascii_lower(line.substr(0, sp));
    rest    = line.substr(sp);
    trim_inplace(rest);
    return true;
}

} // namespace

namespace {

// Whitespace-separated tokens; a token starting with '"' runs to the closing quote (spaces kept,
// quotes dropped). An unterminated quote takes the rest of the line.
std::vector<std::string> split_tokens(const std::string& text)
{
    std::vector<std::string> out;
    size_t                   i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])))
            ++i;
        if (i >= text.size())
            break;
        if (text[i] == '"') {
            const size_t close = text.find('"', i + 1);
            out.push_back(text.substr(i + 1, close == std::string::npos ? std::string::npos : close - i - 1));
            i = close == std::string::npos ? text.size() : close + 1;
        } else {
            size_t end = i;
            while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])))
                ++end;
            out.push_back(text.substr(i, end - i));
            i = end;
        }
    }
    return out;
}

// Plain ASCII number check ([+-]digits[.digits][e[+-]digits]); unlike strtod it does not depend
// on the C locale's decimal separator and does not accept inf/nan/hex.
bool is_number(const std::string& tok)
{
    size_t i      = 0;
    auto   digits = [&]() {
        const size_t start = i;
        while (i < tok.size() && tok[i] >= '0' && tok[i] <= '9')
            ++i;
        return i - start;
    };
    if (i < tok.size() && (tok[i] == '+' || tok[i] == '-'))
        ++i;
    size_t n = digits();
    if (i < tok.size() && tok[i] == '.') {
        ++i;
        n += digits();
    }
    if (n == 0)
        return false;
    if (i < tok.size() && (tok[i] == 'e' || tok[i] == 'E')) {
        ++i;
        if (i < tok.size() && (tok[i] == '+' || tok[i] == '-'))
            ++i;
        if (digits() == 0)
            return false;
    }
    return i == tok.size();
}

void push_unique(std::vector<std::string>& out, const std::string& v)
{
    if (!v.empty() && std::find(out.begin(), out.end(), v) == out.end())
        out.push_back(v);
}

} // namespace

std::vector<ZipMtllibLine> parse_obj_mtllib_lines(const std::string& obj_text)
{
    std::vector<ZipMtllibLine> out;
    std::string                keyword, rest;
    for (const std::string& line : split_lines(obj_text)) {
        if (!split_keyword(line, keyword, rest) || keyword != "mtllib" || rest.empty())
            continue;
        ZipMtllibLine l;
        std::vector<std::string> toks = split_tokens(rest);
        if (rest.find('"') == std::string::npos && toks.size() > 1)
            l.whole = rest;
        for (const std::string& tok : toks)
            push_unique(l.names, tok);
        out.push_back(std::move(l));
    }
    return out;
}

std::vector<std::string> parse_obj_mtllib(const std::string& obj_text)
{
    // The OBJ spec allows several names per line, but exporters also write one name with spaces
    // in it. Quoted names are taken as written; otherwise the whole remainder is listed first and
    // then each whitespace token. The sidecar lookup uses the tokens only if the whole name is
    // not in the archive.
    std::vector<std::string> out;
    for (const ZipMtllibLine& l : parse_obj_mtllib_lines(obj_text)) {
        push_unique(out, l.whole);
        for (const std::string& n : l.names)
            push_unique(out, n);
    }
    return out;
}

std::vector<std::string> parse_mtl_texture_refs(const std::string& mtl_text)
{
    static const std::vector<std::string> keys = {"map_kd", "map_ka", "map_ks", "map_ns", "map_d", "map_bump", "bump", "disp",
                                                  "decal",  "refl",   "norm",   "map_pr", "map_pm", "map_ps", "map_ke"};
    // Options and how many arguments they take. Numeric options take up to N numbers (-o/-s/-t
    // allow 1..3); word options take exactly one token (on/off, a channel, a type name).
    struct Opt { const char* name; int max_args; bool word; };
    static const std::vector<Opt> opts = {
        {"-blendu", 1, true}, {"-blendv", 1, true}, {"-cc", 1, true},   {"-clamp", 1, true},   {"-imfchan", 1, true},
        {"-type", 1, true},   {"-boost", 1, false}, {"-mm", 2, false},  {"-o", 3, false},      {"-s", 3, false},
        {"-t", 3, false},     {"-texres", 1, false}, {"-bm", 1, false},
    };
    std::vector<std::string> out;
    std::string              keyword, rest;
    for (const std::string& line : split_lines(mtl_text)) {
        if (!split_keyword(line, keyword, rest) || std::find(keys.begin(), keys.end(), keyword) == keys.end())
            continue;
        const std::vector<std::string> toks = split_tokens(rest);
        size_t i = 0;
        while (i < toks.size() && toks[i].size() > 1 && toks[i][0] == '-' && !is_number(toks[i])) {
            const std::string opt = ascii_lower(toks[i]);
            ++i;
            const Opt* known = nullptr;
            for (const Opt& o : opts)
                if (opt == o.name)
                    known = &o;
            if (!known) {
                // An unknown "-x..." may be a file whose name starts with '-' (`-x.png`). List
                // the remainder as a candidate first; the lookup keeps whichever names exist.
                std::string name = toks[i - 1];
                for (size_t k = i; k < toks.size(); ++k)
                    name += " " + toks[k];
                push_unique(out, name);
            }
            if (known && known->word) {
                if (i < toks.size())
                    ++i;
            } else {
                // Known numeric option, or an unknown one: skip the numbers that follow it.
                const int max_args = known ? known->max_args : 3;
                for (int n = 0; n < max_args && i < toks.size() && is_number(toks[i]); ++n)
                    ++i;
            }
        }
        if (i >= toks.size())
            continue;
        // The name is the remainder; an unquoted name may contain spaces.
        std::string name = toks[i];
        for (size_t k = i + 1; k < toks.size(); ++k)
            name += " " + toks[k];
        push_unique(out, name);
    }
    return out;
}

std::string resolve_zip_relative(const std::string& dir, const std::string& ref)
{
    const std::string        joined = dir.empty() ? slash_normalized(ref) : slash_normalized(dir) + "/" + slash_normalized(ref);
    std::vector<std::string> parts;
    size_t                   start = 0;
    while (start <= joined.size()) {
        size_t end = joined.find('/', start);
        if (end == std::string::npos)
            end = joined.size();
        const std::string part = joined.substr(start, end - start);
        if (part == "..") {
            if (parts.empty())
                return {};
            parts.pop_back();
        } else if (!part.empty() && part != ".")
            parts.push_back(part);
        if (end == joined.size())
            break;
        start = end + 1;
    }
    std::string out;
    for (const std::string& p : parts)
        out += (out.empty() ? "" : "/") + p;
    return out;
}

std::vector<std::uint32_t> zip_obj_sidecar_indices(const ZipListedEntry&              obj,
                                                   const std::vector<ZipListedEntry>& all,
                                                   const ZipEntryReader&              read_entry,
                                                   bool                               case_insensitive)
{
    auto same = [case_insensitive](const std::string& a, const std::string& b) { return case_insensitive ? iequals(a, b) : a == b; };
    std::vector<std::uint32_t> extra;
    if (!has_extension(obj.decoded_name, ".obj"))
        return extra;

    auto usable = [&](const ZipListedEntry& e) { return e.index != obj.index && !e.is_directory && !e.is_encrypted && !e.is_symlink && e.is_safe; };
    auto find   = [&](const std::string& rel) -> const ZipListedEntry* {
        if (rel.empty())
            return nullptr;
        for (const ZipListedEntry& e : all)
            if (usable(e) && resolve_zip_relative({}, e.decoded_name) == rel)
                return &e;
        if (case_insensitive)
            for (const ZipListedEntry& e : all)
                if (usable(e) && same(resolve_zip_relative({}, e.decoded_name), rel))
                    return &e;
        return nullptr;
    };
    auto add = [&](const ZipListedEntry* e) {
        if (e && std::find(extra.begin(), extra.end(), e->index) == extra.end()) {
            extra.push_back(e->index);
            return true;
        }
        return false;
    };

    const std::string           obj_dir = parent_dir(obj.decoded_name);
    std::vector<const ZipListedEntry*> mtls;
    std::string                 text;
    if (read_entry && read_entry(obj.index, text)) {
        for (const ZipMtllibLine& line : parse_obj_mtllib_lines(text)) {
            // A whole-line name that exists wins; its tokens are not tried.
            if (!line.whole.empty())
                if (const ZipListedEntry* e = find(resolve_zip_relative(obj_dir, line.whole))) {
                    if (add(e))
                        mtls.push_back(e);
                    continue;
                }
            for (const std::string& name : line.names)
                if (const ZipListedEntry* e = find(resolve_zip_relative(obj_dir, name)); e && add(e))
                    mtls.push_back(e);
        }
    }
    if (mtls.empty()) {
        // No readable mtllib line (or the OBJ is over the read cap): the same-stem MTL beside it.
        const std::string stem = leaf_stem(obj.decoded_name);
        for (const ZipListedEntry& e : all)
            if (usable(e) && parent_dir(e.decoded_name) == obj_dir && has_extension(e.decoded_name, ".mtl") &&
                same(leaf_stem(e.decoded_name), stem) && add(&e))
                mtls.push_back(&e);
    }
    for (const ZipListedEntry* mtl : mtls) {
        text.clear();
        if (!read_entry || !read_entry(mtl->index, text))
            continue;
        const std::string mtl_dir = parent_dir(mtl->decoded_name);
        for (const std::string& ref : parse_mtl_texture_refs(text))
            add(find(resolve_zip_relative(mtl_dir, ref)));
    }
    return extra;
}

ZipSidecarMap zip_sidecar_map(const std::vector<std::uint32_t>&  objs,
                              const std::vector<ZipListedEntry>& all,
                              const ZipEntryReader&              read_entry,
                              const std::function<bool()>&       cancelled,
                              bool                               case_insensitive)
{
    ZipSidecarMap out;
    for (std::uint32_t idx : objs) {
        if (cancelled && cancelled())
            break;
        for (const ZipListedEntry& e : all)
            if (e.index == idx) {
                std::vector<std::uint32_t> extra = zip_obj_sidecar_indices(e, all, read_entry, case_insensitive);
                if (!extra.empty())
                    out.emplace(idx, std::move(extra));
                break;
            }
    }
    return out;
}

std::vector<std::uint32_t> zip_extract_order(const std::vector<std::uint32_t>& selected, const std::vector<std::uint32_t>& sidecars)
{
    std::vector<std::uint32_t> out;
    out.reserve(selected.size() + sidecars.size());
    auto push = [&out](std::uint32_t i) {
        if (std::find(out.begin(), out.end(), i) == out.end())
            out.push_back(i);
    };
    for (std::uint32_t i : sidecars)
        push(i);
    for (std::uint32_t i : selected)
        push(i);
    return out;
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
    caps.max_entries_scanned    = std::max(caps.max_entries_scanned, ZIP_MIN_ENTRIES_SCANNED);
    caps.max_entry_uncompressed = std::max(caps.max_entry_uncompressed, ZIP_MIN_ENTRY_UNCOMPRESSED);
    caps.max_total_uncompressed = std::max(caps.max_total_uncompressed, ZIP_MIN_TOTAL_UNCOMPRESSED);
    caps.max_compression_ratio  = std::max(caps.max_compression_ratio, ZIP_MIN_COMPRESSION_RATIO);
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
