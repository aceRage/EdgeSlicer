#include "PluginGuard.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>

namespace Slic3r { namespace GUI {

const char *const kUltraNetMarkerName = "ultranet.txt";

bool is_ultranet_plugin(bool plugin_present, bool ultranet_marker)
{
    // The marker alone proves nothing - a stale ultranet.txt left behind after the user replaced
    // the plug-in with Bambu's must not lock the CDN path out. Both, or it is not ours.
    return plugin_present && ultranet_marker;
}

bool bambu_cdn_download_allowed(bool /*plugin_present*/, bool /*ultranet_marker*/)
{
    // Owner decision (2026-10): Bambu's network plug-in is never downloaded or installed, whatever
    // is (or is not) in data_dir/plugins. An empty folder is reported as "plug-in missing".
    return false;
}

bool bambu_cdn_package_allowed(const std::string &package_name)
{
    // Exact name only - this is a whitelist of one, not a pattern.
    return package_name == "camera_component.zip";
}

PluginMissingNotice network_plugin_missing_notice(bool ultranet_installed, bool user_requested, bool already_shown)
{
    if (ultranet_installed)
        return PluginMissingNotice::Suppress;
    if (user_requested)
        return PluginMissingNotice::Show;
    return already_shown ? PluginMissingNotice::Suppress : PluginMissingNotice::Show;
}

PluginSync plugin_sync_decision(bool sidecar_present,
                                bool installed_present,
                                bool installed_identical,
                                bool marker_present,
                                bool keep_foreign)
{
    // A build without the sidecar (a developer tree, or a platform we do not ship it for yet) has
    // nothing to install and no opinion about what is there.
    if (! sidecar_present)
        return PluginSync::Nothing;
    if (! installed_present)
        return PluginSync::InstallFresh;
    if (installed_identical)
        return marker_present ? PluginSync::Nothing : PluginSync::WriteMarkerOnly;
    // Different bytes. An older UltraNet after an upgrade, Bambu's package after a CDN download,
    // or a migrated data dir - none of them is what this build was tested with. The marker does
    // not change the answer: an old marker beside an old DLL still means "update it".
    return keep_foreign ? PluginSync::Nothing : PluginSync::ReplaceForeign;
}

const char *network_library_name()
{
#if defined(_WIN32)
    return "bambu_networking.dll";
#elif defined(__APPLE__)
    return "libbambu_networking.dylib";
#else
    return "libbambu_networking.so";
#endif
}

boost::filesystem::path ultranet_sidecar_dir(const boost::filesystem::path &exe_dir, bool macos_app_bundle)
{
    // <App>.app/Contents/MacOS/<exe> -> <App>.app/Contents/Resources/ultranet
    if (macos_app_bundle)
        return exe_dir.parent_path() / "Resources" / "ultranet";
    return exe_dir / "ultranet";
}

boost::filesystem::path ultranet_sidecar_dir(const boost::filesystem::path &exe_dir)
{
#if defined(__APPLE__)
    return ultranet_sidecar_dir(exe_dir, true);
#else
    return ultranet_sidecar_dir(exe_dir, false);
#endif
}

LoginGuardAction plugin_guard_decision(bool plugin_present,
                                       bool ultranet_marker,
                                       bool installed_networking,
                                       bool agent_loaded)
{
    // An agent is loaded: the ticket exchange has somewhere to go. Nothing to guard. The
    // preference is deliberately not consulted here - the running agent is the ground truth, and a
    // stale `installed_networking=false` must not block a session that is already working.
    if (agent_loaded)
        return LoginGuardAction::ShowLogin;

    // No agent, but our plug-in is sitting in the plug-ins folder. Offering Bambu's CDN download
    // here would overwrite it, and it would not help anyway: the usual cause is the first-run copy
    // landing after the plug-in load point, which a restart fixes.
    if (is_ultranet_plugin(plugin_present, ultranet_marker))
        return LoginGuardAction::RestartRequired;

    // No agent and no plug-in of ours. Either networking is switched off, or the folder is empty,
    // or a Bambu-original plug-in failed to load. There is no download to offer for any of them:
    // the plug-in is missing from this install and reinstalling EdgeSlicer is the fix.
    (void) installed_networking;
    return LoginGuardAction::PluginMissing;
}

// ---------------------------------------------------------------------------------------------
// Camera component (BambuSource).

const char *bambu_source_library_name()
{
#if defined(_WIN32)
    return "BambuSource.dll";
#elif defined(__APPLE__)
    return "libBambuSource.dylib";
#else
    return "libBambuSource.so";
#endif
}

namespace {

// Little-endian scalar reads out of a byte buffer, bounds-checked. The file is attacker-adjacent
// (it can be anything the user dropped in the plug-ins folder), so every read is guarded and a
// malformed image simply answers "not a real filter".
template<typename T> bool read_le(const std::vector<char> &buf, std::size_t off, T &out)
{
    if (off > buf.size() || buf.size() - off < sizeof(T))
        return false;
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i)
        v |= static_cast<std::uint64_t>(static_cast<unsigned char>(buf[off + i])) << (8 * i);
    out = static_cast<T>(v);
    return true;
}

// Map a PE relative virtual address to a file offset using the section table.
bool rva_to_offset(const std::vector<char> &buf,
                   std::size_t              section_table,
                   std::uint16_t            num_sections,
                   std::uint32_t            rva,
                   std::size_t &            out)
{
    for (std::uint16_t i = 0; i < num_sections; ++i) {
        const std::size_t sh = section_table + std::size_t(i) * 40; // IMAGE_SECTION_HEADER
        std::uint32_t virt_size = 0, virt_addr = 0, raw_size = 0, raw_ptr = 0;
        if (! read_le(buf, sh + 8, virt_size) || ! read_le(buf, sh + 12, virt_addr) ||
            ! read_le(buf, sh + 16, raw_size) || ! read_le(buf, sh + 20, raw_ptr))
            return false;
        // A section's in-memory size may exceed its raw size; only the raw part exists in the file.
        const std::uint32_t span = virt_size > raw_size ? raw_size : virt_size;
        if (span == 0 || rva < virt_addr || rva >= virt_addr + span)
            continue;
        const std::uint64_t off = std::uint64_t(raw_ptr) + (rva - virt_addr);
        if (off >= buf.size())
            return false;
        out = std::size_t(off);
        return true;
    }
    return false;
}

// Does the PE image in `buf` export the symbol `want`?
bool pe_exports(const std::vector<char> &buf, const char *want)
{
    std::uint16_t mz = 0;
    if (! read_le(buf, 0, mz) || mz != 0x5A4D) // "MZ"
        return false;
    std::uint32_t e_lfanew = 0;
    if (! read_le(buf, 0x3C, e_lfanew))
        return false;
    const std::size_t pe = e_lfanew;
    std::uint32_t sig = 0;
    if (! read_le(buf, pe, sig) || sig != 0x00004550) // "PE\0\0"
        return false;

    std::uint16_t num_sections = 0, opt_size = 0, magic = 0;
    if (! read_le(buf, pe + 6, num_sections) || ! read_le(buf, pe + 20, opt_size))
        return false;
    const std::size_t opt = pe + 24;
    if (! read_le(buf, opt, magic))
        return false;
    // The export directory is data directory 0; it sits at a different offset in PE32 vs PE32+.
    const std::size_t dd0 = opt + (magic == 0x20B ? 112 : 96);
    std::uint32_t export_rva = 0, export_size = 0;
    if (! read_le(buf, dd0, export_rva) || ! read_le(buf, dd0 + 4, export_size))
        return false;
    if (export_rva == 0 || export_size == 0)
        return false; // no export table at all - this is what our stub looks like

    const std::size_t section_table = opt + opt_size;
    std::size_t exp_off = 0;
    if (! rva_to_offset(buf, section_table, num_sections, export_rva, exp_off))
        return false;

    std::uint32_t num_names = 0, names_rva = 0;
    if (! read_le(buf, exp_off + 24, num_names) || ! read_le(buf, exp_off + 32, names_rva))
        return false;
    if (num_names == 0 || names_rva == 0)
        return false;
    std::size_t names_off = 0;
    if (! rva_to_offset(buf, section_table, num_sections, names_rva, names_off))
        return false;

    const std::size_t want_len = std::strlen(want);
    // Cap the walk: a corrupt header could claim a huge name count.
    const std::uint32_t cap = num_names > 65536u ? 65536u : num_names;
    for (std::uint32_t i = 0; i < cap; ++i) {
        std::uint32_t name_rva = 0;
        if (! read_le(buf, names_off + std::size_t(i) * 4, name_rva))
            return false;
        std::size_t name_off = 0;
        if (! rva_to_offset(buf, section_table, num_sections, name_rva, name_off))
            continue;
        // Compare in place, refusing to run off the end of the buffer.
        if (buf.size() - name_off <= want_len)
            continue;
        if (std::memcmp(buf.data() + name_off, want, want_len) == 0 && buf[name_off + want_len] == '\0')
            return true;
    }
    return false;
}

} // namespace

bool exports_dll_register_server(const boost::filesystem::path &dll)
{
    boost::system::error_code ec;
    if (! boost::filesystem::exists(dll, ec) || ec)
        return false;
    const boost::uintmax_t size = boost::filesystem::file_size(dll, ec);
    // Guard both ends: too small to be a PE at all, or too large to slurp (the real filter is ~5 MB;
    // 256 MB is far past anything legitimate).
    if (ec || size < 64 || size > 256ull * 1024 * 1024)
        return false;

    std::vector<char> buf;
    try {
        std::ifstream f(dll.string().c_str(), std::ios::binary);
        if (! f)
            return false;
        buf.resize(std::size_t(size));
        f.read(buf.data(), std::streamsize(buf.size()));
        if (std::size_t(f.gcount()) != buf.size())
            return false;
    } catch (...) {
        return false;
    }

    // The exported-symbol question is only meaningful for a PE. On macOS/Linux the shipped stub is
    // likewise a placeholder with no such entry point; a Mach-O/ELF is not a PE, so pe_exports()
    // answers false and the caller treats the file as "not a real filter", which is the behaviour
    // we want there too (the download path is Windows-only for now).
    return pe_exports(buf, "DllRegisterServer");
}

bool is_ultranet_bambusource_stub(const boost::filesystem::path &dll)
{
    boost::system::error_code ec;
    if (! boost::filesystem::exists(dll, ec) || ec)
        return false; // nothing there is "missing", not "our stub"
    // The marker is what makes this folder ours. Without it we must not claim the file is a stub.
    if (! boost::filesystem::exists(dll.parent_path() / kUltraNetMarkerName, ec) || ec)
        return false;
    return ! exports_dll_register_server(dll);
}

CameraToolsCopy camera_tools_copy_decision(bool plugins_copy_is_stub,
                                           bool cameratools_has_real_filter,
                                           bool cameratools_up_to_date)
{
    // A real filter already in cameratools is the best thing present - never overwrite it, and in
    // particular never with the stub (the second half of the reported bug: the user downloads the
    // component, then the next Play stamps our 9.7 KB placeholder over it).
    if (cameratools_has_real_filter)
        return CameraToolsCopy::KeepExisting;
    if (plugins_copy_is_stub)
        return CameraToolsCopy::SkipStubMissingComponent;
    return cameratools_up_to_date ? CameraToolsCopy::KeepExisting : CameraToolsCopy::CopyFromPlugins;
}

OtaPluginInstall ota_plugin_install_decision(bool update_flag)
{
    // The staged package is Bambu's, and Bambu's plug-in is never installed.
    return update_flag ? OtaPluginInstall::Refuse : OtaPluginInstall::NothingStaged;
}

const char *const *ota_plugin_staged_names(size_t &count)
{
#if defined(_WIN32)
    static const char *const names[] = { "bambu_networking.dll", "BambuSource.dll", "live555.dll", "network_plugins.json" };
#elif defined(__APPLE__)
    static const char *const names[] = { "libbambu_networking.dylib", "libBambuSource.dylib", "liblive555.dylib", "network_plugins.json" };
#else
    static const char *const names[] = { "libbambu_networking.so", "libBambuSource.so", "liblive555.so", "network_plugins.json" };
#endif
    count = sizeof(names) / sizeof(names[0]);
    return names;
}

bool may_overwrite_bambusource(bool dest_exists, bool dest_is_real_filter)
{
    // Upgrades re-run the first-run copier. A real filter the user fetched from Bambu must outlive
    // that; anything else (absent, or our own stub from a previous version) may be refreshed.
    return ! (dest_exists && dest_is_real_filter);
}

const char *const kUltraNetModuleTag = "UltraNet-module";

bool carries_ultranet_module_tag(const boost::filesystem::path &lib)
{
    boost::system::error_code ec;
    if (! boost::filesystem::exists(lib, ec) || ec)
        return false;
    const boost::uintmax_t size = boost::filesystem::file_size(lib, ec);
    if (ec || size == 0 || size > 256ull * 1024 * 1024)
        return false;
    try {
        std::ifstream f(lib.string().c_str(), std::ios::binary);
        if (! f)
            return false;
        // Stream it in chunks, keeping a tag-sized overlap so a match across a boundary is found.
        const std::string tag(kUltraNetModuleTag);
        std::string       window;
        std::vector<char> chunk(64 * 1024);
        while (f) {
            f.read(chunk.data(), std::streamsize(chunk.size()));
            const std::streamsize n = f.gcount();
            if (n <= 0)
                break;
            window.append(chunk.data(), std::size_t(n));
            if (window.find(tag) != std::string::npos)
                return true;
            if (window.size() > tag.size())
                window.erase(0, window.size() - tag.size());
        }
    } catch (...) {
    }
    return false;
}

bool is_real_camera_component(const boost::filesystem::path &lib)
{
#if defined(_WIN32)
    return exports_dll_register_server(lib);
#elif defined(__APPLE__)
    // The tag alone used to decide this, which called ANY untagged file "Bambu's camera module". A
    // Mach-O that really exports the player class is the only thing live view can use.
    return classify_camera_module(lib) == CameraModuleKind::BambuCamera;
#else
    boost::system::error_code ec;
    return boost::filesystem::exists(lib, ec) && ! ec && ! carries_ultranet_module_tag(lib);
#endif
}

bool bambusource_needs_refresh(bool bundled_present,
                               bool installed_present,
                               bool identical,
                               bool installed_is_real_filter,
                               bool keep_foreign)
{
    if (! bundled_present || keep_foreign)
        return false;
    if (installed_present && identical)
        return false;
    return may_overwrite_bambusource(installed_present, installed_is_real_filter);
}

bool storage_browser_use_lan_url(bool lan_mode,
                                 bool local_proto,
                                 bool remote_proto,
                                 bool has_ip,
                                 bool has_access_code,
                                 bool lan_only_tunnel)
{
    if ((lan_mode || ! remote_proto) && local_proto && has_ip)
        return true; // stock
    return lan_only_tunnel && has_ip && has_access_code;
}

// ---------------------------------------------------------------------------------------------
// The camera module, kept apart from the storage module.

const char *const kBambuPlayerClassSymbol = "_OBJC_CLASS_$_BambuPlayer";
const char *const kCameraBundleRecordName = "edgeslicer_bundled.txt";

namespace {

// Big-endian read, for the fat (universal) header.
bool read_be32(const std::string &buf, std::size_t off, std::uint32_t &out)
{
    if (off > buf.size() || buf.size() - off < 4)
        return false;
    out = 0;
    for (std::size_t i = 0; i < 4; ++i)
        out = (out << 8) | static_cast<unsigned char>(buf[off + i]);
    return true;
}

bool read_be64(const std::string &buf, std::size_t off, std::uint64_t &out)
{
    std::uint32_t hi = 0, lo = 0;
    if (! read_be32(buf, off, hi) || ! read_be32(buf, off + 4, lo))
        return false;
    out = (std::uint64_t(hi) << 32) | lo;
    return true;
}

template<typename T> bool read_le_s(const std::string &buf, std::size_t off, T &out)
{
    if (off > buf.size() || buf.size() - off < sizeof(T))
        return false;
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i)
        v |= static_cast<std::uint64_t>(static_cast<unsigned char>(buf[off + i])) << (8 * i);
    out = static_cast<T>(v);
    return true;
}

// ULEB128 as used by the dyld export trie. Bounded by `end`.
bool read_uleb(const std::string &buf, std::size_t &pos, std::size_t end, std::uint64_t &out)
{
    out = 0;
    unsigned shift = 0;
    while (pos < end) {
        const unsigned char b = static_cast<unsigned char>(buf[pos++]);
        if (shift < 64)
            out |= std::uint64_t(b & 0x7f) << shift;
        if ((b & 0x80) == 0)
            return true;
        shift += 7;
        if (shift > 63)
            return false;
    }
    return false;
}

// Walk the dyld export trie [start, start + size) for `symbol`.
bool trie_exports(const std::string &buf, std::size_t start, std::size_t size, const char *symbol)
{
    if (start > buf.size() || size == 0 || buf.size() - start < size)
        return false;
    const std::size_t end  = start + size;
    std::size_t       node = start;
    const char *      s    = symbol;
    // Each step consumes at least one character of the symbol, so the walk is bounded; the cap
    // protects against a malicious trie whose edges are empty strings pointing back at themselves.
    for (int steps = 0; steps < 4096; ++steps) {
        std::size_t   pos           = node;
        std::uint64_t terminal_size = 0;
        if (! read_uleb(buf, pos, end, terminal_size))
            return false;
        if (*s == '\0')
            return terminal_size != 0;
        if (terminal_size > end - pos)
            return false;
        pos += std::size_t(terminal_size);
        if (pos >= end)
            return false;
        const unsigned child_count = static_cast<unsigned char>(buf[pos++]);
        bool descended = false;
        for (unsigned c = 0; c < child_count; ++c) {
            const std::size_t edge = pos;
            while (pos < end && buf[pos] != '\0')
                ++pos;
            if (pos >= end)
                return false;
            const std::size_t edge_len = pos - edge;
            ++pos; // the NUL
            std::uint64_t child_off = 0;
            if (! read_uleb(buf, pos, end, child_off))
                return false;
            if (edge_len == 0 || descended)
                continue;
            if (std::strncmp(buf.data() + edge, s, edge_len) == 0) {
                if (child_off >= size)
                    return false;
                s += edge_len;
                node      = start + std::size_t(child_off);
                descended = true;
            }
        }
        if (! descended)
            return false;
    }
    return false;
}

// One thin Mach-O image at [base, base + len) of `buf`.
bool macho_slice_exports(const std::string &buf, std::size_t base, std::size_t len, const char *symbol)
{
    if (base > buf.size() || buf.size() - base < len || len < 28)
        return false;
    std::uint32_t magic = 0;
    if (! read_le_s(buf, base, magic))
        return false;
    bool is64 = false;
    if (magic == 0xFEEDFACFu)
        is64 = true;
    else if (magic != 0xFEEDFACEu)
        return false; // big-endian (PowerPC) images are not something we will ever be handed
    const std::size_t header_size = is64 ? 32 : 28;
    std::uint32_t ncmds = 0, sizeofcmds = 0;
    if (! read_le_s(buf, base + 16, ncmds) || ! read_le_s(buf, base + 20, sizeofcmds))
        return false;
    if (header_size + std::size_t(sizeofcmds) > len || ncmds > 4096)
        return false;

    const std::size_t slice_end = base + len;
    std::size_t       cmd       = base + header_size;
    const std::size_t cmds_end  = cmd + sizeofcmds;
    const std::string want(symbol);
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        std::uint32_t c = 0, cmdsize = 0;
        if (! read_le_s(buf, cmd, c) || ! read_le_s(buf, cmd + 4, cmdsize))
            return false;
        if (cmdsize < 8 || cmd + cmdsize > cmds_end)
            return false;

        if (c == 0x2) { // LC_SYMTAB
            std::uint32_t symoff = 0, nsyms = 0, stroff = 0, strsize = 0;
            if (read_le_s(buf, cmd + 8, symoff) && read_le_s(buf, cmd + 12, nsyms) &&
                read_le_s(buf, cmd + 16, stroff) && read_le_s(buf, cmd + 20, strsize)) {
                const std::size_t entry = is64 ? 16 : 12;
                const std::size_t syms  = base + symoff;
                const std::size_t strs  = base + stroff;
                const bool tables_fit = nsyms <= (1u << 24) && syms <= slice_end &&
                                        std::size_t(nsyms) * entry <= slice_end - syms &&
                                        strs <= slice_end && strsize <= slice_end - strs;
                if (tables_fit) {
                    for (std::uint32_t k = 0; k < nsyms; ++k) {
                        const std::size_t e = syms + std::size_t(k) * entry;
                        std::uint32_t n_strx = 0;
                        read_le_s(buf, e, n_strx);
                        const unsigned char n_type = static_cast<unsigned char>(buf[e + 4]);
                        // External, defined in a section, not a debugging (stab) entry.
                        if ((n_type & 0xe0) != 0 || (n_type & 0x01) == 0 || (n_type & 0x0e) != 0x0e)
                            continue;
                        if (n_strx >= strsize || strsize - n_strx <= want.size())
                            continue;
                        const char *name = buf.data() + strs + n_strx;
                        if (std::memcmp(name, want.data(), want.size()) == 0 && name[want.size()] == '\0')
                            return true;
                    }
                }
            }
        } else if (c == 0x22 || c == 0x80000022u) { // LC_DYLD_INFO(_ONLY)
            std::uint32_t export_off = 0, export_size = 0;
            if (read_le_s(buf, cmd + 40, export_off) && read_le_s(buf, cmd + 44, export_size) &&
                export_size > 0 && export_off <= len && export_size <= len - export_off &&
                trie_exports(buf, base + export_off, export_size, symbol))
                return true;
        } else if (c == 0x80000033u) { // LC_DYLD_EXPORTS_TRIE
            std::uint32_t dataoff = 0, datasize = 0;
            if (read_le_s(buf, cmd + 8, dataoff) && read_le_s(buf, cmd + 12, datasize) &&
                datasize > 0 && dataoff <= len && datasize <= len - dataoff &&
                trie_exports(buf, base + dataoff, datasize, symbol))
                return true;
        }
        cmd += cmdsize;
    }
    return false;
}

bool read_whole_file(const boost::filesystem::path &file, std::string &out)
{
    boost::system::error_code ec;
    if (! boost::filesystem::exists(file, ec) || ec)
        return false;
    const boost::uintmax_t size = boost::filesystem::file_size(file, ec);
    if (ec || size == 0 || size > 256ull * 1024 * 1024)
        return false;
    try {
        boost::filesystem::ifstream f(file, std::ios::binary);
        if (! f)
            return false;
        out.resize(std::size_t(size));
        f.read(&out[0], std::streamsize(out.size()));
        return std::size_t(f.gcount()) == out.size();
    } catch (...) {
        return false;
    }
}

} // namespace

bool pe_image_exports(const std::string &image, const char *symbol)
{
    try {
        const std::vector<char> buf(image.begin(), image.end());
        return pe_exports(buf, symbol);
    } catch (...) {
        return false;
    }
}

bool macho_image_exports(const std::string &image, const char *symbol)
{
    std::uint32_t be_magic = 0;
    if (! read_be32(image, 0, be_magic))
        return false;
    if (be_magic == 0xCAFEBABEu || be_magic == 0xCAFEBABFu) {
        // Universal binary: a big-endian table of slices. The same magic opens a Java class file,
        // whose "slice count" is its version number - far past this cap.
        const bool    fat64 = be_magic == 0xCAFEBABFu;
        std::uint32_t nfat  = 0;
        if (! read_be32(image, 4, nfat) || nfat == 0 || nfat > 16)
            return false;
        for (std::uint32_t i = 0; i < nfat; ++i) {
            std::uint64_t off = 0, size = 0;
            if (fat64) {
                const std::size_t a = 8 + std::size_t(i) * 32;
                if (! read_be64(image, a + 8, off) || ! read_be64(image, a + 16, size))
                    return false;
            } else {
                const std::size_t a  = 8 + std::size_t(i) * 20;
                std::uint32_t     o32 = 0, s32 = 0;
                if (! read_be32(image, a + 8, o32) || ! read_be32(image, a + 12, s32))
                    return false;
                off = o32, size = s32;
            }
            if (off > image.size() || size > image.size() - off)
                continue;
            if (macho_slice_exports(image, std::size_t(off), std::size_t(size), symbol))
                return true;
        }
        return false;
    }
    return macho_slice_exports(image, 0, image.size(), symbol);
}

CameraModuleKind classify_camera_module(const boost::filesystem::path &lib)
{
    boost::system::error_code ec;
    if (! boost::filesystem::exists(lib, ec) || ec)
        return CameraModuleKind::Missing;
    std::string image;
    if (! read_whole_file(lib, image))
        return CameraModuleKind::Other;

    // UltraNet's modules say so (the tag on macOS/Linux, the LAN-only tunnel export everywhere). That
    // outranks anything else the image exports: it is never Bambu's.
    if (image.find(kUltraNetModuleTag) != std::string::npos)
        return CameraModuleKind::UltraNetStorage;

    if (image.size() >= 2 && image[0] == 'M' && image[1] == 'Z') {
        if (pe_image_exports(image, "DllRegisterServer"))
            return CameraModuleKind::BambuCamera;
        if (pe_image_exports(image, "EdgeSlicer_TunnelLanOnly"))
            return CameraModuleKind::UltraNetStorage;
        return CameraModuleKind::Other;
    }
    if (macho_image_exports(image, kBambuPlayerClassSymbol))
        return CameraModuleKind::BambuCamera;
    if (macho_image_exports(image, "_EdgeSlicer_TunnelLanOnly"))
        return CameraModuleKind::UltraNetStorage;
    return CameraModuleKind::Other;
}

const char *live555_library_name()
{
#if defined(_WIN32)
    return "live555.dll";
#elif defined(__APPLE__)
    return "liblive555.dylib";
#else
    return "liblive555.so";
#endif
}

boost::filesystem::path bundled_camera_dir(const boost::filesystem::path &exe_dir, bool macos_app_bundle)
{
    if (macos_app_bundle)
        return exe_dir.parent_path() / "Resources" / "cameratools";
    return exe_dir / "cameratools";
}

boost::filesystem::path bundled_camera_dir(const boost::filesystem::path &exe_dir)
{
#if defined(__APPLE__)
    return bundled_camera_dir(exe_dir, true);
#else
    return bundled_camera_dir(exe_dir, false);
#endif
}

boost::filesystem::path camera_module_dir(const boost::filesystem::path &data_dir)
{
    return data_dir / "cameratools";
}

PluginsCameraRepair plugins_camera_repair_decision(bool plugins_has_camera_module,
                                                   bool cameratools_has_camera_module,
                                                   bool identical)
{
    if (! plugins_has_camera_module)
        return PluginsCameraRepair::Nothing;
    if (! cameratools_has_camera_module)
        return PluginsCameraRepair::MoveToCameraTools;
    return identical ? PluginsCameraRepair::RemoveDuplicate : PluginsCameraRepair::MoveAsideDuplicate;
}

StorageModuleRestore storage_module_restore_decision(bool plugins_has_storage_module,
                                                     bool sidecar_has_storage_module,
                                                     bool backup_is_storage_module)
{
    if (plugins_has_storage_module)
        return StorageModuleRestore::Nothing;
    if (sidecar_has_storage_module)
        return StorageModuleRestore::FromSidecar;
    if (backup_is_storage_module)
        return StorageModuleRestore::FromBackup;
    return StorageModuleRestore::Unavailable;
}

CameraModuleSync camera_module_sync_decision(bool bundled_is_camera_module,
                                             bool installed_present,
                                             bool installed_is_camera_module,
                                             bool identical,
                                             bool installed_from_bundle)
{
    // Only ever install a real camera module - never a stub, never whatever happens to be bundled.
    if (! bundled_is_camera_module)
        return CameraModuleSync::Nothing;
    if (! installed_present)
        return CameraModuleSync::Install;
    if (identical)
        return CameraModuleSync::Nothing;
    // Not a camera module at all (a stub, a truncated copy): nothing worth keeping.
    if (! installed_is_camera_module)
        return CameraModuleSync::Refresh;
    // Our own earlier copy: an upgrade brings the newer one.
    if (installed_from_bundle)
        return CameraModuleSync::Refresh;
    // A real module we did not put there (the user's, or a camera-component download): keep it.
    return CameraModuleSync::KeepUser;
}

std::string camera_file_fingerprint(const boost::filesystem::path &file)
{
    boost::system::error_code ec;
    if (! boost::filesystem::exists(file, ec) || ec)
        return {};
    try {
        boost::filesystem::ifstream f(file, std::ios::binary);
        if (! f)
            return {};
        std::uint64_t     hash = 1469598103934665603ull; // FNV-1a 64
        std::uint64_t     size = 0;
        std::vector<char> chunk(64 * 1024);
        while (f) {
            f.read(chunk.data(), std::streamsize(chunk.size()));
            const std::streamsize n = f.gcount();
            if (n <= 0)
                break;
            for (std::streamsize i = 0; i < n; ++i) {
                hash ^= static_cast<unsigned char>(chunk[std::size_t(i)]);
                hash *= 1099511628211ull;
            }
            size += std::uint64_t(n);
        }
        char hex[17];
        for (int i = 15; i >= 0; --i) {
            hex[i] = "0123456789abcdef"[hash & 0xf];
            hash >>= 4;
        }
        hex[16] = '\0';
        return std::to_string(size) + " " + hex;
    } catch (...) {
        return {};
    }
}

namespace {
// Splits the record into lines, tolerating CRLF.
std::vector<std::string> record_lines(const std::string &text)
{
    std::vector<std::string> lines;
    std::string              line;
    for (char c : text) {
        if (c == '\n') {
            if (! line.empty() && line.back() == '\r')
                line.pop_back();
            lines.push_back(line);
            line.clear();
        } else {
            line.push_back(c);
        }
    }
    if (! line.empty())
        lines.push_back(line);
    return lines;
}
} // namespace

std::string camera_record_lookup(const std::string &record_text, const std::string &name)
{
    if (name.empty())
        return {};
    for (const std::string &line : record_lines(record_text)) {
        if (line.size() > name.size() && line.compare(0, name.size(), name) == 0 && line[name.size()] == ' ')
            return line.substr(name.size() + 1);
    }
    return {};
}

std::string camera_record_update(const std::string &record_text, const std::string &name, const std::string &fingerprint)
{
    std::string out;
    for (const std::string &line : record_lines(record_text)) {
        if (line.empty())
            continue;
        if (line.size() > name.size() && line.compare(0, name.size(), name) == 0 && line[name.size()] == ' ')
            continue;
        out += line + "\n";
    }
    if (! fingerprint.empty())
        out += name + " " + fingerprint + "\n";
    return out;
}

FilterRegistration filter_registration_decision(bool clsid_matches,
                                                bool registered_path_is_real_filter,
                                                bool ours_is_real_filter)
{
    if (clsid_matches && registered_path_is_real_filter)
        return FilterRegistration::UseRegistered;
    return ours_is_real_filter ? FilterRegistration::RegisterOurs : FilterRegistration::OfferComponent;
}

namespace {

namespace fs = boost::filesystem;

bool files_identical(const fs::path &a, const fs::path &b)
{
    boost::system::error_code ec;
    if (! fs::exists(a, ec) || ! fs::exists(b, ec))
        return false;
    const boost::uintmax_t sa = fs::file_size(a, ec);
    if (ec)
        return false;
    const boost::uintmax_t sb = fs::file_size(b, ec);
    if (ec || sa != sb || sa == 0)
        return false;
    std::string da, db;
    return read_whole_file(a, da) && read_whole_file(b, db) && da == db;
}

// Puts `from` at `to`: by rename when it can (a DLL another EdgeSlicer instance has loaded can be
// renamed on Windows, not overwritten), else by copy. An existing `to` is moved aside to
// `<to>.replaced` first and put back if the new file cannot be placed. `move` removes `from`.
bool place_file(const fs::path &from, const fs::path &to, bool move, std::string &err)
{
    boost::system::error_code ec;
    const fs::path aside       = to.string() + ".replaced";
    bool           moved_aside = false;
    if (fs::exists(to, ec)) {
        fs::remove(aside, ec);
        ec.clear();
        fs::rename(to, aside, ec);
        if (ec) {
            err = "cannot move the old " + to.filename().string() + " aside: " + ec.message();
            return false;
        }
        moved_aside = true;
    }
    if (move) {
        fs::rename(from, to, ec);
        if (! ec)
            return true;
        ec.clear();
    }
    fs::copy_file(from, to, fs::copy_option::overwrite_if_exists, ec);
    if (ec) {
        err = ec.message();
        boost::system::error_code ec2;
        if (moved_aside && ! fs::exists(to, ec2))
            fs::rename(aside, to, ec2);
        return false;
    }
    if (move)
        fs::remove(from, ec);
    return true;
}

std::string read_text(const fs::path &file)
{
    std::string text;
    boost::system::error_code ec;
    if (fs::exists(file, ec) && ! ec)
        read_whole_file(file, text);
    return text;
}

bool write_text(const fs::path &file, const std::string &text)
{
    try {
        fs::ofstream f(file, std::ios::binary | std::ios::trunc);
        if (! f)
            return false;
        f << text;
        return bool(f);
    } catch (...) {
        return false;
    }
}

} // namespace

std::vector<CameraSyncLog> sync_camera_module_files(const CameraSyncPaths &paths)
{
    std::vector<CameraSyncLog> log;
    auto info  = [&log](const std::string &t) { log.push_back({CameraSyncLog::Info, t}); };
    auto warn  = [&log](const std::string &t) { log.push_back({CameraSyncLog::Warning, t}); };
    auto error = [&log](const std::string &t) { log.push_back({CameraSyncLog::Error, t}); };
    try {
        boost::system::error_code ec;
        const fs::path &pf  = paths.plugins_dir;
        const fs::path &cam = paths.camera_dir;
        const fs::path  p_src  = pf / paths.source_name;
        const fs::path  p_live = pf / paths.live_name;
        const fs::path  c_src  = cam / paths.source_name;
        const fs::path  c_live = cam / paths.live_name;

        // What an earlier run moved aside (see place_file).
        for (const std::string &name : {paths.source_name, paths.live_name}) {
            fs::remove(cam / (name + ".replaced"), ec);
            ec.clear();
        }

        // 1. plugins/ is UltraNet's. A Bambu camera module there (and its live555) moves out.
        const bool p_is_cam = classify_camera_module(p_src) == CameraModuleKind::BambuCamera;
        if (paths.keep_foreign) {
            if (p_is_cam)
                warn("ultranet_keep_foreign_plugin is set; leaving Bambu's camera module in " + pf.string() +
                     " (the storage browser does not work with it there)");
        } else {
            bool moved_camera_out = false;
            const bool c_is_cam = classify_camera_module(c_src) == CameraModuleKind::BambuCamera;
            const PluginsCameraRepair repair =
                plugins_camera_repair_decision(p_is_cam, c_is_cam, p_is_cam && c_is_cam && files_identical(p_src, c_src));
            std::string err;
            if (repair != PluginsCameraRepair::Nothing)
                fs::create_directories(cam, ec);
            switch (repair) {
            case PluginsCameraRepair::MoveToCameraTools:
                if (place_file(p_src, c_src, true, err)) {
                    moved_camera_out = true;
                    warn("moved Bambu's camera module from " + p_src.string() + " to " + c_src.string() +
                         " (plugins/ is for EdgeSlicer's storage module)");
                } else
                    error("could not move " + p_src.string() + " to " + c_src.string() + ": " + err);
                break;
            case PluginsCameraRepair::RemoveDuplicate:
                fs::remove(p_src, ec);
                if (! ec) {
                    moved_camera_out = true;
                    warn("removed " + p_src.string() + ": an identical copy is already in " + cam.string());
                } else
                    error("could not remove the duplicate " + p_src.string() + ": " + ec.message());
                ec.clear();
                break;
            case PluginsCameraRepair::MoveAsideDuplicate: {
                const fs::path parked = cam / (paths.source_name + ".from-plugins");
                if (place_file(p_src, parked, true, err)) {
                    moved_camera_out = true;
                    warn(cam.string() + " already has a camera module; moved the different one from " + p_src.string() +
                         " to " + parked.string());
                } else
                    error("could not move " + p_src.string() + " aside: " + err);
                break;
            }
            case PluginsCameraRepair::Nothing: break;
            }

            // live555 belongs with the camera module; UltraNet ships none.
            if (fs::exists(p_live, ec)) {
                fs::create_directories(cam, ec);
                const bool c_live_present = fs::exists(c_live, ec);
                const PluginsCameraRepair live_repair =
                    plugins_camera_repair_decision(true, c_live_present, c_live_present && files_identical(p_live, c_live));
                std::string lerr;
                if (live_repair == PluginsCameraRepair::MoveToCameraTools) {
                    if (place_file(p_live, c_live, true, lerr))
                        warn("moved " + p_live.string() + " to " + c_live.string());
                    else
                        error("could not move " + p_live.string() + ": " + lerr);
                } else if (live_repair == PluginsCameraRepair::RemoveDuplicate) {
                    fs::remove(p_live, ec);
                    if (! ec)
                        warn("removed " + p_live.string() + ": an identical copy is already in " + cam.string());
                    ec.clear();
                } else if (live_repair == PluginsCameraRepair::MoveAsideDuplicate) {
                    const fs::path parked = cam / (paths.live_name + ".from-plugins");
                    if (place_file(p_live, parked, true, lerr))
                        warn("moved the different " + p_live.string() + " to " + parked.string());
                    else
                        error("could not move " + p_live.string() + " aside: " + lerr);
                }
            }

            // 2. UltraNet's storage module back into plugins/. Anything still there - even a camera
            //    module a failed move left behind - is not overwritten.
            const bool     p_has_storage     = classify_camera_module(p_src) != CameraModuleKind::Missing;
            const fs::path sc_src            = paths.sidecar_dir / paths.source_name;
            const auto     sc_kind           = classify_camera_module(sc_src);
            const bool     sc_has_storage    = sc_kind != CameraModuleKind::Missing && sc_kind != CameraModuleKind::BambuCamera;
            const fs::path backup            = pf / (paths.source_name + ".ultranet");
            const bool     backup_is_storage = classify_camera_module(backup) == CameraModuleKind::UltraNetStorage;
            std::string    rerr;
            switch (storage_module_restore_decision(p_has_storage, sc_has_storage, backup_is_storage)) {
            case StorageModuleRestore::FromSidecar:
                fs::create_directories(pf, ec);
                if (place_file(sc_src, p_src, false, rerr)) {
                    warn("restored EdgeSlicer's storage module " + p_src.string() + " from " + sc_src.string());
                    if (fs::exists(backup, ec))
                        info("left the hand-kept copy " + backup.string() + " in place");
                } else
                    error("could not restore the storage module from " + sc_src.string() + ": " + rerr);
                break;
            case StorageModuleRestore::FromBackup:
                fs::rename(backup, p_src, ec);
                if (! ec)
                    warn("restored EdgeSlicer's storage module " + p_src.string() + " from " + backup.string());
                else
                    error("could not restore the storage module from " + backup.string() + ": " + ec.message());
                ec.clear();
                break;
            case StorageModuleRestore::Unavailable:
                if (moved_camera_out)
                    warn("no EdgeSlicer storage module to put in " + pf.string() +
                         "; the storage browser stays unavailable until EdgeSlicer is reinstalled");
                break;
            case StorageModuleRestore::Nothing: break;
            }
        }

        // 3. Seed cameratools from the app's bundled camera files (CAMERA_BIN_DIR at packaging).
        const fs::path b_src  = paths.bundled_dir / paths.source_name;
        const fs::path b_live = paths.bundled_dir / paths.live_name;
        if (! fs::exists(b_src, ec)) {
            info("this build carries no camera files (" + b_src.string() + ")");
            return log;
        }
        const bool b_is_cam = classify_camera_module(b_src) == CameraModuleKind::BambuCamera;
        if (! b_is_cam) {
            error("the bundled " + b_src.string() + " is not Bambu's camera module; not installing it");
            return log;
        }
        const fs::path    record_path = cam / kCameraBundleRecordName;
        std::string       record      = read_text(record_path);
        const bool        c_present   = fs::exists(c_src, ec);
        const bool        c_cam       = classify_camera_module(c_src) == CameraModuleKind::BambuCamera;
        const bool        c_same      = c_present && files_identical(b_src, c_src);
        const std::string c_print     = c_present ? camera_file_fingerprint(c_src) : std::string();
        const bool        c_ours      = ! c_print.empty() && camera_record_lookup(record, paths.source_name) == c_print;
        const CameraModuleSync action = camera_module_sync_decision(b_is_cam, c_present, c_cam, c_same, c_ours);
        bool record_changed = false;
        std::string err;
        if (action == CameraModuleSync::Install || action == CameraModuleSync::Refresh) {
            fs::create_directories(cam, ec);
            if (place_file(b_src, c_src, false, err)) {
                record         = camera_record_update(record, paths.source_name, camera_file_fingerprint(c_src));
                record_changed = true;
                info(std::string(action == CameraModuleSync::Install ? "installed" : "updated") + " Bambu's camera module " +
                     c_src.string() + " from " + b_src.string());
                if (fs::exists(b_live, ec)) {
                    if (place_file(b_live, c_live, false, err)) {
                        record = camera_record_update(record, paths.live_name, camera_file_fingerprint(c_live));
                        info("installed " + c_live.string());
                    } else
                        error("copying " + b_live.string() + " failed: " + err);
                } else
                    warn("the app bundle has no " + paths.live_name + "; RTSP cameras (X1/H2D/H2C) will not play");
            } else
                error("copying " + b_src.string() + " failed: " + err + " (will retry on the next start)");
        } else {
            if (action == CameraModuleSync::KeepUser)
                info("keeping the camera module in " + cam.string() + " (not one EdgeSlicer installed)");
            // Byte-identical to what this build ships (e.g. the hand copy step 1 just moved here):
            // adopt it, so the next EdgeSlicer that ships a newer module may refresh it.
            if (c_same && ! c_ours) {
                record         = camera_record_update(record, paths.source_name, c_print);
                record_changed = true;
                if (files_identical(b_live, c_live))
                    record = camera_record_update(record, paths.live_name, camera_file_fingerprint(c_live));
            }
            // A missing dependency is never better than the bundled one.
            if (! fs::exists(c_live, ec) && fs::exists(b_live, ec)) {
                fs::create_directories(cam, ec);
                if (place_file(b_live, c_live, false, err)) {
                    record         = camera_record_update(record, paths.live_name, camera_file_fingerprint(c_live));
                    record_changed = true;
                    info("installed the missing " + c_live.string());
                } else
                    error("copying " + b_live.string() + " failed: " + err);
            }
        }
        if (record_changed && ! write_text(record_path, record))
            warn("could not write " + record_path.string());
    } catch (const std::exception &e) {
        error(std::string("start-up sync failed: ") + e.what());
    } catch (...) {
        error("start-up sync failed");
    }
    return log;
}

} } // namespace Slic3r::GUI
