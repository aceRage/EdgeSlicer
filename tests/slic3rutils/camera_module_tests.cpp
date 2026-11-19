// The camera module, kept apart from the storage module (owner decision 2026-10-11).
//
// Bambu's camera module (live view: the DirectShow filter on Windows, the BambuPlayer class on
// macOS) and UltraNet's storage module (the storage browser's LAN tunnel) share one file name. They
// live in different folders now - cameratools/ and plugins/ - and these tests cover the parts that
// decide which file is which and where it goes:
//
//  * telling the two apart from the bytes alone (PE exports, Mach-O symbol table and export trie,
//    thin and universal images), with synthetic images - no Bambu binary is used or downloaded;
//  * the decision tables (plugins/ repair, storage-module restore, cameratools seeding, the Windows
//    filter registration);
//  * the start-up file work end to end on dummy files in a temp folder, including the owner's
//    2.4.5.4 Mac state (Bambu's module + live555 in plugins/, UltraNet's kept as .ultranet).
//
// Nothing here loads a library, touches the registry, the network or a printer.

#include <catch2/catch.hpp>

#include "slic3r/GUI/PluginGuard.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

using namespace Slic3r::GUI;
namespace fs = boost::filesystem;

namespace {

// ---- synthetic images ------------------------------------------------------------------------

void put_le32(std::string &b, std::size_t off, std::uint32_t v)
{
    if (b.size() < off + 4)
        b.resize(off + 4, '\0');
    for (int i = 0; i < 4; ++i)
        b[off + i] = char((v >> (8 * i)) & 0xff);
}
void put_le16(std::string &b, std::size_t off, std::uint16_t v)
{
    if (b.size() < off + 2)
        b.resize(off + 2, '\0');
    b[off]     = char(v & 0xff);
    b[off + 1] = char((v >> 8) & 0xff);
}
void put_be32(std::string &b, std::size_t off, std::uint32_t v)
{
    if (b.size() < off + 4)
        b.resize(off + 4, '\0');
    for (int i = 0; i < 4; ++i)
        b[off + i] = char((v >> (8 * (3 - i))) & 0xff);
}

struct Sym
{
    std::string   name;
    unsigned char n_type; // 0x0f exported (N_SECT|N_EXT), 0x0e local, 0x01 undefined import
};

// A thin 64-bit Mach-O dylib whose only load command is LC_SYMTAB.
std::string macho_symtab(const std::vector<Sym> &syms)
{
    std::string strtab(1, '\0');
    std::vector<std::uint32_t> strx;
    for (const Sym &s : syms) {
        strx.push_back(std::uint32_t(strtab.size()));
        strtab += s.name;
        strtab.push_back('\0');
    }
    const std::uint32_t symoff = 32 + 24;
    const std::uint32_t stroff = symoff + std::uint32_t(syms.size()) * 16;
    std::string b;
    put_le32(b, 0, 0xFEEDFACFu);
    put_le32(b, 4, 0x0100000Cu); // arm64
    put_le32(b, 12, 6);          // MH_DYLIB
    put_le32(b, 16, 1);          // ncmds
    put_le32(b, 20, 24);         // sizeofcmds
    put_le32(b, 32, 0x2);        // LC_SYMTAB
    put_le32(b, 36, 24);
    put_le32(b, 40, symoff);
    put_le32(b, 44, std::uint32_t(syms.size()));
    put_le32(b, 48, stroff);
    put_le32(b, 52, std::uint32_t(strtab.size()));
    for (std::size_t i = 0; i < syms.size(); ++i) {
        const std::size_t e = symoff + i * 16;
        put_le32(b, e, strx[i]);
        b.resize(std::max(b.size(), e + 16), '\0');
        b[e + 4] = char(syms[i].n_type);
        b[e + 5] = 1; // n_sect
    }
    b.resize(stroff, '\0');
    b += strtab;
    return b;
}

// Export trie node: terminal or not, with edges to children.
struct TrieNode
{
    bool                                       terminal = false;
    std::vector<std::pair<std::string, TrieNode>> children;
};

// Serialises a trie (every offset fits one ULEB byte: the test tries are tiny).
std::string trie_bytes(const TrieNode &root)
{
    // Breadth-first layout; each node's size is known from its own content.
    std::vector<const TrieNode *> order{&root};
    for (std::size_t i = 0; i < order.size(); ++i)
        for (const auto &c : order[i]->children)
            order.push_back(&c.second);
    auto node_size = [](const TrieNode &n) {
        std::size_t s = n.terminal ? 3 : 1; // terminal size byte (+ flags + address)
        s += 1;                             // child count
        for (const auto &c : n.children)
            s += c.first.size() + 1 + 1;
        return s;
    };
    std::vector<std::size_t> offset(order.size());
    std::size_t              total = 0;
    for (std::size_t i = 0; i < order.size(); ++i) {
        offset[i] = total;
        total += node_size(*order[i]);
    }
    REQUIRE(total < 128);
    std::string out;
    std::size_t next_child = 1;
    for (std::size_t i = 0; i < order.size(); ++i) {
        const TrieNode &n = *order[i];
        if (n.terminal) {
            out.push_back(char(2)); // terminal info: flags 0, address 0
            out.push_back(char(0));
            out.push_back(char(0));
        } else {
            out.push_back(char(0));
        }
        out.push_back(char(n.children.size()));
        for (const auto &c : n.children) {
            out += c.first;
            out.push_back('\0');
            out.push_back(char(offset[next_child++]));
        }
    }
    return out;
}

// A thin 64-bit Mach-O dylib with no symbol table, only an export trie, behind either
// LC_DYLD_EXPORTS_TRIE (new linkers) or LC_DYLD_INFO_ONLY (older ones).
std::string macho_trie(const TrieNode &root, bool dyld_info)
{
    const std::string   trie    = trie_bytes(root);
    const std::uint32_t cmdsize = dyld_info ? 48 : 16;
    const std::uint32_t dataoff = 32 + cmdsize;
    std::string b;
    put_le32(b, 0, 0xFEEDFACFu);
    put_le32(b, 4, 0x01000007u); // x86_64
    put_le32(b, 12, 6);
    put_le32(b, 16, 1);
    put_le32(b, 20, cmdsize);
    if (dyld_info) {
        put_le32(b, 32, 0x80000022u);
        put_le32(b, 36, cmdsize);
        put_le32(b, 32 + 40, dataoff);
        put_le32(b, 32 + 44, std::uint32_t(trie.size()));
    } else {
        put_le32(b, 32, 0x80000033u);
        put_le32(b, 36, cmdsize);
        put_le32(b, 40, dataoff);
        put_le32(b, 44, std::uint32_t(trie.size()));
    }
    b.resize(dataoff, '\0');
    b += trie;
    return b;
}

// A universal (fat) binary around the given slices, page-aligned like lipo's output.
std::string fat(const std::vector<std::string> &slices)
{
    std::string b;
    put_be32(b, 0, 0xCAFEBABEu);
    put_be32(b, 4, std::uint32_t(slices.size()));
    std::size_t off = 4096;
    for (std::size_t i = 0; i < slices.size(); ++i) {
        const std::size_t a = 8 + i * 20;
        put_be32(b, a, i == 0 ? 0x01000007u : 0x0100000Cu);
        put_be32(b, a + 8, std::uint32_t(off));
        put_be32(b, a + 12, std::uint32_t(slices[i].size()));
        put_be32(b, a + 16, 12);
        off += (slices[i].size() + 4095) / 4096 * 4096;
    }
    off = 4096;
    for (const std::string &s : slices) {
        b.resize(off, '\0');
        b += s;
        off += (s.size() + 4095) / 4096 * 4096;
    }
    return b;
}

// A minimal PE32+ image whose export directory names `names`.
std::string pe(const std::vector<std::string> &names)
{
    std::string b(0x400, '\0');
    b[0] = 'M';
    b[1] = 'Z';
    put_le32(b, 0x3C, 0x40);
    b[0x40] = 'P';
    b[0x41] = 'E';
    put_le16(b, 0x44, 0x8664);
    put_le16(b, 0x46, 1);      // one section
    put_le16(b, 0x54, 240);    // SizeOfOptionalHeader (PE32+)
    put_le16(b, 0x58, 0x20B);  // PE32+ magic
    if (! names.empty()) {
        put_le32(b, 0x58 + 112, 0x1000); // export directory RVA
        put_le32(b, 0x58 + 116, 0x100);  // size
    }
    const std::size_t sh = 0x58 + 240;
    std::memcpy(&b[sh], ".edata", 6);
    put_le32(b, sh + 8, 0x200);   // VirtualSize
    put_le32(b, sh + 12, 0x1000); // VirtualAddress
    put_le32(b, sh + 16, 0x200);  // SizeOfRawData
    put_le32(b, sh + 20, 0x200);  // PointerToRawData
    put_le32(b, 0x200 + 24, std::uint32_t(names.size()));
    put_le32(b, 0x200 + 32, 0x1040); // AddressOfNames
    std::size_t str = 0x280;
    for (std::size_t i = 0; i < names.size(); ++i) {
        put_le32(b, 0x240 + i * 4, std::uint32_t(0x1000 + (str - 0x200)));
        std::memcpy(&b[str], names[i].c_str(), names[i].size() + 1);
        str += names[i].size() + 1;
    }
    return b;
}

// What the two Mac modules look like, as far as the classifier is concerned.
std::string mac_bambu_camera(char flavour = 'A')
{
    std::string img = macho_symtab({{"_OBJC_CLASS_$_BambuPlayer", 0x0f}, {"_Bambu_Open", 0x0f}, {"_dlopen", 0x01}});
    return img + std::string(64, flavour); // a distinct tail per "version"
}
std::string mac_ultranet_storage()
{
    std::string img = macho_symtab({{"_Bambu_Open", 0x0f}, {"_EdgeSlicer_TunnelLanOnly", 0x0f}, {"_ultranet_module_tag", 0x0f}});
    return img + kUltraNetModuleTag + std::string(":BambuSource") + std::string(1, '\0');
}

struct TempDir
{
    fs::path path;
    TempDir() : path(fs::temp_directory_path() / fs::unique_path("edgeslicer_camera_%%%%-%%%%-%%%%")) { fs::create_directories(path); }
    ~TempDir()
    {
        boost::system::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write(const fs::path &p, const std::string &bytes)
{
    fs::create_directories(p.parent_path());
    std::ofstream f(p.string(), std::ios::binary | std::ios::trunc);
    f << bytes;
}
std::string read(const fs::path &p)
{
    std::ifstream f(p.string(), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
bool file_exists(const fs::path &p)
{
    boost::system::error_code ec;
    return fs::exists(p, ec);
}
bool any_error(const std::vector<CameraSyncLog> &log)
{
    for (const CameraSyncLog &l : log)
        if (l.level == CameraSyncLog::Error)
            return true;
    return false;
}
bool log_mentions(const std::vector<CameraSyncLog> &log, const std::string &needle)
{
    for (const CameraSyncLog &l : log)
        if (l.text.find(needle) != std::string::npos)
            return true;
    return false;
}

// The Mac layout under a temp root: data dir plugins/ + cameratools/, the app's sidecar and camera folder.
CameraSyncPaths mac_paths(const fs::path &root)
{
    CameraSyncPaths p;
    p.plugins_dir = root / "data" / "plugins";
    p.camera_dir  = camera_module_dir(root / "data");
    p.sidecar_dir = ultranet_sidecar_dir(root / "EdgeSlicer.app" / "Contents" / "MacOS", true);
    p.bundled_dir = bundled_camera_dir(root / "EdgeSlicer.app" / "Contents" / "MacOS", true);
    p.source_name = "libBambuSource.dylib";
    p.live_name   = "liblive555.dylib";
    return p;
}

} // namespace

// ---- telling the files apart -------------------------------------------------------------------

TEST_CASE("Mach-O: the player class is found in the symbol table, exported definitions only", "[CameraModule]")
{
    const std::string cam = macho_symtab({{"_OBJC_CLASS_$_BambuPlayer", 0x0f}});
    CHECK(macho_image_exports(cam, kBambuPlayerClassSymbol));
    CHECK_FALSE(macho_image_exports(cam, "_OBJC_CLASS_$_BambuPlayerX"));
    CHECK_FALSE(macho_image_exports(cam, "_OBJC_CLASS_$_Bambu"));
    // A reference to the class (an undefined import) or a private definition is not an export.
    CHECK_FALSE(macho_image_exports(macho_symtab({{"_OBJC_CLASS_$_BambuPlayer", 0x01}}), kBambuPlayerClassSymbol));
    CHECK_FALSE(macho_image_exports(macho_symtab({{"_OBJC_CLASS_$_BambuPlayer", 0x0e}}), kBambuPlayerClassSymbol));
    // UltraNet's storage module has the tunnel, not the player.
    CHECK_FALSE(macho_image_exports(mac_ultranet_storage(), kBambuPlayerClassSymbol));
    CHECK(macho_image_exports(mac_ultranet_storage(), "_EdgeSlicer_TunnelLanOnly"));
}

TEST_CASE("Mach-O: the export trie answers when the symbol table does not", "[CameraModule]")
{
    // Root -> "_OBJC_CLASS_$_" -> { "BambuPlayer" (terminal), "Other" (terminal) } and "_Bambu_Open".
    TrieNode leaf;
    leaf.terminal = true;
    TrieNode mid;
    mid.children = {{"BambuPlayer", leaf}, {"Other", leaf}};
    TrieNode root;
    root.children = {{"_OBJC_CLASS_$_", mid}, {"_Bambu_Open", leaf}};
    for (bool dyld_info : {false, true}) {
        const std::string img = macho_trie(root, dyld_info);
        CHECK(macho_image_exports(img, kBambuPlayerClassSymbol));
        CHECK(macho_image_exports(img, "_OBJC_CLASS_$_Other"));
        CHECK(macho_image_exports(img, "_Bambu_Open"));
        // A prefix that is an inner node, a longer name, an unrelated one.
        CHECK_FALSE(macho_image_exports(img, "_OBJC_CLASS_$_"));
        CHECK_FALSE(macho_image_exports(img, "_OBJC_CLASS_$_BambuPlayer2"));
        CHECK_FALSE(macho_image_exports(img, "_EdgeSlicer_TunnelLanOnly"));
    }
}

TEST_CASE("Mach-O: universal binaries are searched slice by slice", "[CameraModule]")
{
    const std::string intel = macho_symtab({{"_Bambu_Open", 0x0f}});
    const std::string arm   = macho_symtab({{"_OBJC_CLASS_$_BambuPlayer", 0x0f}});
    CHECK(macho_image_exports(fat({intel, arm}), kBambuPlayerClassSymbol));
    CHECK(macho_image_exports(fat({arm, intel}), kBambuPlayerClassSymbol));
    CHECK_FALSE(macho_image_exports(fat({intel, intel}), kBambuPlayerClassSymbol));
    // A Java class file shares the 0xCAFEBABE magic; its "slice count" is a version number.
    std::string java;
    put_be32(java, 0, 0xCAFEBABEu);
    put_be32(java, 4, 0x34);
    java += std::string(200, 'j');
    CHECK_FALSE(macho_image_exports(java, kBambuPlayerClassSymbol));
}

TEST_CASE("Malformed images answer false, never crash", "[CameraModule]")
{
    const std::string good = macho_symtab({{"_OBJC_CLASS_$_BambuPlayer", 0x0f}});
    // Every truncation of a valid image.
    for (std::size_t n = 0; n < good.size(); n += 7)
        CHECK_FALSE(macho_image_exports(good.substr(0, n), kBambuPlayerClassSymbol));
    // Absurd counts and offsets.
    std::string bad = good;
    put_le32(bad, 44, 0xFFFFFFFFu); // nsyms
    CHECK_FALSE(macho_image_exports(bad, kBambuPlayerClassSymbol));
    bad = good;
    put_le32(bad, 48, 0x7FFFFFF0u); // stroff
    CHECK_FALSE(macho_image_exports(bad, kBambuPlayerClassSymbol));
    bad = good;
    put_le32(bad, 36, 4); // cmdsize < 8
    CHECK_FALSE(macho_image_exports(bad, kBambuPlayerClassSymbol));
    // A trie whose only edge points back at the root, forever.
    std::string loop = macho_trie(TrieNode{}, false);
    loop.resize(loop.size() - 2);
    loop += std::string("\x00\x01" "_\x00\x00", 5);
    put_le32(loop, 44, 5);
    CHECK_FALSE(macho_image_exports(loop, "_____"));
    CHECK_FALSE(macho_image_exports(std::string(), kBambuPlayerClassSymbol));
    CHECK_FALSE(pe_image_exports(std::string("MZ"), "DllRegisterServer"));
}

TEST_CASE("PE: the filter exports DllRegisterServer, UltraNet's module the LAN-only tunnel", "[CameraModule]")
{
    CHECK(pe_image_exports(pe({"DllCanUnloadNow", "DllGetClassObject", "DllRegisterServer"}), "DllRegisterServer"));
    CHECK_FALSE(pe_image_exports(pe({"Bambu_Open", "EdgeSlicer_TunnelLanOnly"}), "DllRegisterServer"));
    CHECK(pe_image_exports(pe({"Bambu_Open", "EdgeSlicer_TunnelLanOnly"}), "EdgeSlicer_TunnelLanOnly"));
    CHECK_FALSE(pe_image_exports(pe({}), "DllRegisterServer")); // the old ~9.7 KB stub: no exports
}

TEST_CASE("classify_camera_module reads files of every format", "[CameraModule]")
{
    TempDir t;
    const fs::path d = t.path;
    write(d / "bambu.dylib", mac_bambu_camera());
    write(d / "bambu_fat.dylib", fat({macho_symtab({{"_x", 0x0f}}), mac_bambu_camera()}));
    write(d / "ultranet.dylib", mac_ultranet_storage());
    write(d / "tagged_player.dylib", mac_bambu_camera() + kUltraNetModuleTag); // the tag always wins
    write(d / "other.dylib", macho_symtab({{"_Bambu_Open", 0x0f}}));
    write(d / "BambuSource.dll", pe({"DllRegisterServer", "DllGetClassObject"}));
    write(d / "ultranet.dll", pe({"Bambu_Open", "EdgeSlicer_TunnelLanOnly"}));
    write(d / "stub.dll", pe({}));
    write(d / "garbage.bin", std::string(5000, 'x'));
    write(d / "empty.bin", std::string());

    CHECK(classify_camera_module(d / "bambu.dylib") == CameraModuleKind::BambuCamera);
    CHECK(classify_camera_module(d / "bambu_fat.dylib") == CameraModuleKind::BambuCamera);
    CHECK(classify_camera_module(d / "ultranet.dylib") == CameraModuleKind::UltraNetStorage);
    CHECK(classify_camera_module(d / "tagged_player.dylib") == CameraModuleKind::UltraNetStorage);
    CHECK(classify_camera_module(d / "other.dylib") == CameraModuleKind::Other);
    CHECK(classify_camera_module(d / "BambuSource.dll") == CameraModuleKind::BambuCamera);
    CHECK(classify_camera_module(d / "ultranet.dll") == CameraModuleKind::UltraNetStorage);
    CHECK(classify_camera_module(d / "stub.dll") == CameraModuleKind::Other);
    CHECK(classify_camera_module(d / "garbage.bin") == CameraModuleKind::Other);
    CHECK(classify_camera_module(d / "empty.bin") == CameraModuleKind::Other);
    CHECK(classify_camera_module(d / "missing.dylib") == CameraModuleKind::Missing);

#if defined(__APPLE__)
    // On the Mac this is the "real camera component" question everywhere (copier, player).
    CHECK(is_real_camera_component(d / "bambu.dylib"));
    CHECK_FALSE(is_real_camera_component(d / "ultranet.dylib"));
    CHECK_FALSE(is_real_camera_component(d / "other.dylib"));
#elif defined(_WIN32)
    CHECK(is_real_camera_component(d / "BambuSource.dll"));
    CHECK_FALSE(is_real_camera_component(d / "ultranet.dll"));
    CHECK_FALSE(is_real_camera_component(d / "stub.dll"));
#endif
}

// ---- where the files live ----------------------------------------------------------------------

TEST_CASE("The bundled camera folder sits beside the exe, or in the bundle's Resources", "[CameraModule]")
{
    CHECK(bundled_camera_dir(fs::path("C:/Program Files/EdgeSlicer"), false) == fs::path("C:/Program Files/EdgeSlicer") / "cameratools");
    CHECK(bundled_camera_dir(fs::path("/Applications/EdgeSlicer.app/Contents/MacOS"), true) ==
          fs::path("/Applications/EdgeSlicer.app/Contents/Resources/cameratools"));
    CHECK(camera_module_dir(fs::path("/data")) == fs::path("/data") / "cameratools");
    const std::string live = live555_library_name();
#if defined(_WIN32)
    CHECK(live == "live555.dll");
#elif defined(__APPLE__)
    CHECK(live == "liblive555.dylib");
#else
    CHECK(live == "liblive555.so");
#endif
}

// ---- decision tables ---------------------------------------------------------------------------

TEST_CASE("plugins/ repair: a camera module there always leaves, nothing is lost", "[CameraModule]")
{
    CHECK(plugins_camera_repair_decision(false, false, false) == PluginsCameraRepair::Nothing);
    CHECK(plugins_camera_repair_decision(false, true, false) == PluginsCameraRepair::Nothing);
    CHECK(plugins_camera_repair_decision(true, false, false) == PluginsCameraRepair::MoveToCameraTools);
    CHECK(plugins_camera_repair_decision(true, true, true) == PluginsCameraRepair::RemoveDuplicate);
    CHECK(plugins_camera_repair_decision(true, true, false) == PluginsCameraRepair::MoveAsideDuplicate);
}

TEST_CASE("Storage module restore: sidecar first, then a genuine hand-kept copy", "[CameraModule]")
{
    CHECK(storage_module_restore_decision(true, true, true) == StorageModuleRestore::Nothing);
    CHECK(storage_module_restore_decision(false, true, true) == StorageModuleRestore::FromSidecar);
    CHECK(storage_module_restore_decision(false, false, true) == StorageModuleRestore::FromBackup);
    CHECK(storage_module_restore_decision(false, false, false) == StorageModuleRestore::Unavailable);
}

TEST_CASE("cameratools seeding: install, refresh our own copy, never a user's", "[CameraModule]")
{
    // Nothing real bundled: never install anything (in particular never a stub).
    for (bool present : {false, true})
        CHECK(camera_module_sync_decision(false, present, present, false, false) == CameraModuleSync::Nothing);
    CHECK(camera_module_sync_decision(true, false, false, false, false) == CameraModuleSync::Install);
    CHECK(camera_module_sync_decision(true, true, true, true, false) == CameraModuleSync::Nothing);
    // Junk where the module should be: replace.
    CHECK(camera_module_sync_decision(true, true, false, false, false) == CameraModuleSync::Refresh);
    // An older copy EdgeSlicer installed: the upgrade brings the bundled one.
    CHECK(camera_module_sync_decision(true, true, true, false, true) == CameraModuleSync::Refresh);
    // A real module the user put there (or downloaded): kept.
    CHECK(camera_module_sync_decision(true, true, true, false, false) == CameraModuleSync::KeepUser);
}

TEST_CASE("Windows filter registration: keep a working one, replace a broken one", "[CameraModule]")
{
    // OrcaSlicer's or Bambu Studio's working filter: used as it is, not re-pointed.
    CHECK(filter_registration_decision(true, true, true) == FilterRegistration::UseRegistered);
    CHECK(filter_registration_decision(true, true, false) == FilterRegistration::UseRegistered);
    // Nothing registered, a dangling path, or a path that now holds UltraNet's module.
    CHECK(filter_registration_decision(false, false, true) == FilterRegistration::RegisterOurs);
    CHECK(filter_registration_decision(true, false, true) == FilterRegistration::RegisterOurs);
    CHECK(filter_registration_decision(false, true, true) == FilterRegistration::RegisterOurs);
    // And no camera files of our own: the component offer.
    CHECK(filter_registration_decision(false, false, false) == FilterRegistration::OfferComponent);
    CHECK(filter_registration_decision(true, false, false) == FilterRegistration::OfferComponent);
}

TEST_CASE("The bundle record: fingerprints, lookup and update", "[CameraModule]")
{
    TempDir t;
    write(t.path / "a", "one");
    write(t.path / "b", "one");
    write(t.path / "c", "two");
    const std::string fa = camera_file_fingerprint(t.path / "a");
    CHECK_FALSE(fa.empty());
    CHECK(fa == camera_file_fingerprint(t.path / "b"));
    CHECK(fa != camera_file_fingerprint(t.path / "c"));
    CHECK(camera_file_fingerprint(t.path / "missing").empty());

    std::string rec = camera_record_update("", "live555.dll", "10 aa");
    rec             = camera_record_update(rec, "BambuSource.dll", "20 bb");
    CHECK(camera_record_lookup(rec, "live555.dll") == "10 aa");
    CHECK(camera_record_lookup(rec, "BambuSource.dll") == "20 bb");
    rec = camera_record_update(rec, "BambuSource.dll", "30 cc");
    CHECK(camera_record_lookup(rec, "BambuSource.dll") == "30 cc");
    CHECK(camera_record_lookup(rec, "live555.dll") == "10 aa");
    // A name that is a prefix of another is not confused with it; CRLF is tolerated.
    CHECK(camera_record_lookup("live555.dll.bak 1 x\r\nlive555.dll 2 y\r\n", "live555.dll") == "2 y");
    CHECK(camera_record_lookup(rec, "BambuSource").empty());
    CHECK(camera_record_lookup("", "x").empty());
}

// ---- the start-up file work, end to end on dummy files -------------------------------------------

TEST_CASE("The owner's 2.4.5.4 Mac state is put right", "[CameraModule]")
{
    // plugins/ holds Bambu's libBambuSource.dylib + liblive555.dylib (live view worked, storage
    // broke) and UltraNet's module kept by hand as libBambuSource.dylib.ultranet. The app ships the
    // same camera files and its UltraNet sidecar.
    TempDir         t;
    CameraSyncPaths p   = mac_paths(t.path);
    const std::string cam = mac_bambu_camera(), live = "LIVE555-A", storage = mac_ultranet_storage();
    write(p.plugins_dir / "libBambuSource.dylib", cam);
    write(p.plugins_dir / "liblive555.dylib", live);
    write(p.plugins_dir / "libBambuSource.dylib.ultranet", storage);
    write(p.plugins_dir / "libbambu_networking.dylib", "network");
    write(p.sidecar_dir / "libBambuSource.dylib", storage);
    write(p.bundled_dir / "libBambuSource.dylib", cam);
    write(p.bundled_dir / "liblive555.dylib", live);

    const auto log = sync_camera_module_files(p);
    CHECK_FALSE(any_error(log));
    // Storage module back where PrinterFileSystem loads it; live555 gone from plugins/.
    CHECK(classify_camera_module(p.plugins_dir / "libBambuSource.dylib") == CameraModuleKind::UltraNetStorage);
    CHECK_FALSE(file_exists(p.plugins_dir / "liblive555.dylib"));
    CHECK(read(p.plugins_dir / "libbambu_networking.dylib") == "network");
    // The hand-kept copy is left alone.
    CHECK(file_exists(p.plugins_dir / "libBambuSource.dylib.ultranet"));
    // Camera files where the player loads them.
    CHECK(classify_camera_module(p.camera_dir / "libBambuSource.dylib") == CameraModuleKind::BambuCamera);
    CHECK(read(p.camera_dir / "liblive555.dylib") == live);
    CHECK(log_mentions(log, "moved Bambu's camera module"));
    CHECK(log_mentions(log, "restored EdgeSlicer's storage module"));
    // Identical to what this build ships, so it is adopted for later refreshes.
    CHECK_FALSE(camera_record_lookup(read(p.camera_dir / kCameraBundleRecordName), "libBambuSource.dylib").empty());

    // A second start changes nothing.
    const auto log2 = sync_camera_module_files(p);
    CHECK_FALSE(any_error(log2));
    CHECK_FALSE(log_mentions(log2, "moved"));
    CHECK_FALSE(log_mentions(log2, "restored"));
    CHECK_FALSE(log_mentions(log2, "installed"));
    CHECK(classify_camera_module(p.plugins_dir / "libBambuSource.dylib") == CameraModuleKind::UltraNetStorage);
}

TEST_CASE("Mixed state in a build with no sidecar and no camera files", "[CameraModule]")
{
    // A developer build: the storage module comes back from the hand-kept .ultranet copy.
    TempDir         t;
    CameraSyncPaths p = mac_paths(t.path);
    write(p.plugins_dir / "libBambuSource.dylib", mac_bambu_camera());
    write(p.plugins_dir / "liblive555.dylib", "LIVE");
    write(p.plugins_dir / "libBambuSource.dylib.ultranet", mac_ultranet_storage());

    const auto log = sync_camera_module_files(p);
    CHECK_FALSE(any_error(log));
    CHECK(classify_camera_module(p.plugins_dir / "libBambuSource.dylib") == CameraModuleKind::UltraNetStorage);
    CHECK_FALSE(file_exists(p.plugins_dir / "libBambuSource.dylib.ultranet"));
    CHECK(classify_camera_module(p.camera_dir / "libBambuSource.dylib") == CameraModuleKind::BambuCamera);
    CHECK(file_exists(p.camera_dir / "liblive555.dylib"));
    CHECK(log_mentions(log, "carries no camera files"));
}

TEST_CASE("A .ultranet file that is not UltraNet's module is never put in plugins/", "[CameraModule]")
{
    TempDir         t;
    CameraSyncPaths p = mac_paths(t.path);
    write(p.plugins_dir / "libBambuSource.dylib", mac_bambu_camera());
    write(p.plugins_dir / "libBambuSource.dylib.ultranet", mac_bambu_camera('B')); // mislabelled
    const auto log = sync_camera_module_files(p);
    CHECK_FALSE(file_exists(p.plugins_dir / "libBambuSource.dylib"));
    CHECK(file_exists(p.plugins_dir / "libBambuSource.dylib.ultranet"));
    CHECK(log_mentions(log, "storage browser stays unavailable"));
}

TEST_CASE("A different camera module already in cameratools wins; the plugins/ one is parked", "[CameraModule]")
{
    TempDir         t;
    CameraSyncPaths p = mac_paths(t.path);
    write(p.camera_dir / "libBambuSource.dylib", mac_bambu_camera('U')); // the user's own
    write(p.camera_dir / "liblive555.dylib", "LIVE-U");
    write(p.plugins_dir / "libBambuSource.dylib", mac_bambu_camera('P'));
    write(p.plugins_dir / "liblive555.dylib", "LIVE-P");
    write(p.sidecar_dir / "libBambuSource.dylib", mac_ultranet_storage());

    const auto log = sync_camera_module_files(p);
    CHECK_FALSE(any_error(log));
    CHECK(read(p.camera_dir / "libBambuSource.dylib") == mac_bambu_camera('U'));
    CHECK(read(p.camera_dir / "libBambuSource.dylib.from-plugins") == mac_bambu_camera('P'));
    CHECK(read(p.camera_dir / "liblive555.dylib") == "LIVE-U");
    CHECK(read(p.camera_dir / "liblive555.dylib.from-plugins") == "LIVE-P");
    CHECK(classify_camera_module(p.plugins_dir / "libBambuSource.dylib") == CameraModuleKind::UltraNetStorage);
}

TEST_CASE("Fresh install, upgrade, and a user's own module", "[CameraModule]")
{
    TempDir         t;
    CameraSyncPaths p = mac_paths(t.path);
    write(p.sidecar_dir / "libBambuSource.dylib", mac_ultranet_storage());
    write(p.plugins_dir / "libBambuSource.dylib", mac_ultranet_storage());
    write(p.bundled_dir / "libBambuSource.dylib", mac_bambu_camera('1'));
    write(p.bundled_dir / "liblive555.dylib", "LIVE-1");

    // Fresh data dir: both files installed and recorded.
    auto log = sync_camera_module_files(p);
    CHECK_FALSE(any_error(log));
    CHECK(read(p.camera_dir / "libBambuSource.dylib") == mac_bambu_camera('1'));
    CHECK(read(p.camera_dir / "liblive555.dylib") == "LIVE-1");
    CHECK(log_mentions(log, "installed Bambu's camera module"));
    // plugins/ was already right and is not touched.
    CHECK(read(p.plugins_dir / "libBambuSource.dylib") == mac_ultranet_storage());

    // The next release ships newer camera files: our own copy is refreshed.
    write(p.bundled_dir / "libBambuSource.dylib", mac_bambu_camera('2'));
    write(p.bundled_dir / "liblive555.dylib", "LIVE-2");
    log = sync_camera_module_files(p);
    CHECK_FALSE(any_error(log));
    CHECK(read(p.camera_dir / "libBambuSource.dylib") == mac_bambu_camera('2'));
    CHECK(read(p.camera_dir / "liblive555.dylib") == "LIVE-2");
    CHECK(log_mentions(log, "updated Bambu's camera module"));

    // The user drops in a module of their own: a later release never overwrites it.
    write(p.camera_dir / "libBambuSource.dylib", mac_bambu_camera('u'));
    write(p.camera_dir / "liblive555.dylib", "LIVE-u");
    write(p.bundled_dir / "libBambuSource.dylib", mac_bambu_camera('3'));
    log = sync_camera_module_files(p);
    CHECK_FALSE(any_error(log));
    CHECK(read(p.camera_dir / "libBambuSource.dylib") == mac_bambu_camera('u'));
    CHECK(read(p.camera_dir / "liblive555.dylib") == "LIVE-u");
    CHECK(log_mentions(log, "keeping the camera module"));

    // ...but a stub or junk in its place is replaced.
    write(p.camera_dir / "libBambuSource.dylib", mac_ultranet_storage());
    log = sync_camera_module_files(p);
    CHECK(read(p.camera_dir / "libBambuSource.dylib") == mac_bambu_camera('3'));
}

TEST_CASE("A packaging mistake (UltraNet's module in the camera folder) installs nothing", "[CameraModule]")
{
    TempDir         t;
    CameraSyncPaths p = mac_paths(t.path);
    write(p.bundled_dir / "libBambuSource.dylib", mac_ultranet_storage());
    const auto log = sync_camera_module_files(p);
    CHECK(any_error(log));
    CHECK_FALSE(file_exists(p.camera_dir / "libBambuSource.dylib"));
}

TEST_CASE("The developer escape hatch leaves plugins/ alone", "[CameraModule]")
{
    TempDir         t;
    CameraSyncPaths p = mac_paths(t.path);
    p.keep_foreign    = true;
    write(p.plugins_dir / "libBambuSource.dylib", mac_bambu_camera());
    write(p.plugins_dir / "liblive555.dylib", "LIVE");
    write(p.sidecar_dir / "libBambuSource.dylib", mac_ultranet_storage());
    const auto log = sync_camera_module_files(p);
    CHECK(classify_camera_module(p.plugins_dir / "libBambuSource.dylib") == CameraModuleKind::BambuCamera);
    CHECK(file_exists(p.plugins_dir / "liblive555.dylib"));
    CHECK(log_mentions(log, "ultranet_keep_foreign_plugin"));
}

TEST_CASE("Windows layout: an old camera-component download in plugins/ moves to cameratools", "[CameraModule]")
{
    // Before this change the CDN component was copied into plugins/ AND cameratools/, which put
    // Bambu's filter where UltraNet's storage module belongs.
    TempDir         t;
    CameraSyncPaths p;
    p.plugins_dir = t.path / "data" / "plugins";
    p.camera_dir  = camera_module_dir(t.path / "data");
    p.sidecar_dir = ultranet_sidecar_dir(t.path / "EdgeSlicer", false);
    p.bundled_dir = bundled_camera_dir(t.path / "EdgeSlicer", false);
    p.source_name = "BambuSource.dll";
    p.live_name   = "live555.dll";
    const std::string filter = pe({"DllGetClassObject", "DllRegisterServer"}), storage = pe({"Bambu_Open", "EdgeSlicer_TunnelLanOnly"});
    write(p.plugins_dir / "BambuSource.dll", filter);
    write(p.plugins_dir / "live555.dll", "LIVE");
    write(p.camera_dir / "BambuSource.dll", filter); // the second copy of the same download
    write(p.camera_dir / "live555.dll", "LIVE");
    write(p.sidecar_dir / "BambuSource.dll", storage);

    const auto log = sync_camera_module_files(p);
    CHECK_FALSE(any_error(log));
    CHECK(read(p.plugins_dir / "BambuSource.dll") == storage);
    CHECK_FALSE(file_exists(p.plugins_dir / "live555.dll"));
    CHECK(read(p.camera_dir / "BambuSource.dll") == filter);
    CHECK_FALSE(file_exists(p.camera_dir / "BambuSource.dll.from-plugins")); // identical: just dropped
    CHECK(log_mentions(log, "an identical copy is already in"));
}
