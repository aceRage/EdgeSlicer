#include <catch2/catch.hpp>

#ifndef ZIP_OPEN_STANDALONE
#include "libslic3r/AppConfig.hpp"
#include <boost/filesystem.hpp>
#include <boost/nowide/convert.hpp>
#endif
#include "libslic3r/ZipOpen.hpp"

using namespace Slic3r;

TEST_CASE("classify_zip_entry_name lists FT_MODEL extensions as models", "[ZipOpen]")
{
    CHECK(classify_zip_entry_name("models/part.stl", false) == ZipEntryKind::Model);
    CHECK(classify_zip_entry_name("part.3MF", false) == ZipEntryKind::Model);
    CHECK(classify_zip_entry_name("a.obj", false) == ZipEntryKind::Model);
    CHECK(classify_zip_entry_name("a.amf", false) == ZipEntryKind::Model);
    CHECK(classify_zip_entry_name("a.zip.amf", false) == ZipEntryKind::Model);
    CHECK(classify_zip_entry_name("a.step", false) == ZipEntryKind::Model);
    CHECK(classify_zip_entry_name("a.stp", false) == ZipEntryKind::Model);
    CHECK(classify_zip_entry_name("a.oltp", false) == ZipEntryKind::Model);
    CHECK(classify_zip_entry_name("a.glb", false) == ZipEntryKind::Model);
    CHECK(classify_zip_entry_name("a.gltf", false) == ZipEntryKind::Model);
    CHECK(classify_zip_entry_name("a.svg", false) == ZipEntryKind::Model);
    // A sliced 3MF is still a 3MF project, not a G-code file.
    CHECK(classify_zip_entry_name("job.gcode.3mf", false) == ZipEntryKind::Model);
}

TEST_CASE("classify_zip_entry_name lists gcode and nested zips", "[ZipOpen]")
{
    CHECK(classify_zip_entry_name("a.gcode", false) == ZipEntryKind::GCode);
    CHECK(classify_zip_entry_name("a.GCODE", false) == ZipEntryKind::GCode);
    // EdgeSlicer has no binary G-code reader, so .bgcode is not offered (review B-7).
    CHECK(classify_zip_entry_name("a.bgcode", false) == ZipEntryKind::Unsupported);
    CHECK(classify_zip_entry_name("inner/bundle.zip", false) == ZipEntryKind::NestedZip);
    CHECK(classify_zip_entry_name("inner/bundle.ZIP", false) == ZipEntryKind::NestedZip);
    CHECK(classify_zip_entry_name("notes.txt", false) == ZipEntryKind::Unsupported);
}

TEST_CASE("classify_zip_entry_name ignores directories, __MACOSX and AppleDouble", "[ZipOpen]")
{
    CHECK(classify_zip_entry_name("models/", true) == ZipEntryKind::Ignored);
    CHECK(classify_zip_entry_name("__MACOSX/._part.stl", false) == ZipEntryKind::Ignored);
    CHECK(classify_zip_entry_name("__MACOSX", false) == ZipEntryKind::Ignored);
    CHECK(classify_zip_entry_name("models/._part.stl", false) == ZipEntryKind::Ignored);
    CHECK(classify_zip_entry_name("._part.stl", false) == ZipEntryKind::Ignored);
}

TEST_CASE("zip_entry_is_selectable requires a model or gcode that is not locked", "[ZipOpen]")
{
    CHECK(zip_entry_is_selectable(ZipEntryKind::Model, false, false, true));
    CHECK(zip_entry_is_selectable(ZipEntryKind::GCode, false, false, true));
    CHECK_FALSE(zip_entry_is_selectable(ZipEntryKind::Model, true, false, true));
    CHECK_FALSE(zip_entry_is_selectable(ZipEntryKind::GCode, false, true, true));
    CHECK_FALSE(zip_entry_is_selectable(ZipEntryKind::Model, false, false, false)); // unsafe path
    CHECK_FALSE(zip_entry_is_selectable(ZipEntryKind::NestedZip, false, false, true));
    CHECK_FALSE(zip_entry_is_selectable(ZipEntryKind::Unsupported, false, false, true));
    CHECK_FALSE(zip_entry_is_selectable(ZipEntryKind::Ignored, false, false, true));
}

TEST_CASE("should_auto_open_zip is on only for a single selectable entry", "[ZipOpen]")
{
    CHECK(should_auto_open_zip(1, true));
    CHECK_FALSE(should_auto_open_zip(1, false));
    CHECK_FALSE(should_auto_open_zip(0, true));
    CHECK_FALSE(should_auto_open_zip(2, true));
}

TEST_CASE("zip_archive_is_all_encrypted ignores mac metadata", "[ZipOpen]")
{
    ZipListedEntry stl;
    stl.decoded_name = "part.stl";
    stl.is_encrypted = true;
    ZipListedEntry mac;
    mac.decoded_name = "__MACOSX/._part.stl";
    mac.is_encrypted = false;

    CHECK(zip_archive_is_all_encrypted({stl}));
    CHECK(zip_archive_is_all_encrypted({stl, mac}));

    ZipListedEntry open_stl;
    open_stl.decoded_name = "other.stl";
    open_stl.is_encrypted = false;
    CHECK_FALSE(zip_archive_is_all_encrypted({stl, open_stl}));
    CHECK_FALSE(zip_archive_is_all_encrypted({}));
}

namespace {
ZipListedEntry listed(std::uint32_t index, const char* name)
{
    ZipListedEntry e;
    e.index        = index;
    e.decoded_name = name;
    return e;
}
} // namespace

TEST_CASE("parse_obj_mtllib and parse_mtl_texture_refs read names", "[ZipOpen]")
{
    const auto libs = parse_obj_mtllib("# c\nv 0 0 0\nmtllib mats/My Part.mtl\r\n  mtllib  b.mtl \nusemtl x\n");
    // Unquoted line with a space: the whole name first, then each token.
    CHECK(libs == std::vector<std::string>{"mats/My Part.mtl", "mats/My", "Part.mtl", "b.mtl"});

    const auto refs = parse_mtl_texture_refs("newmtl a\nKd 1 1 1\nmap_Kd tex/diffuse.png\n"
                                             "map_Bump -bm 0.5 normals/n.png\nbump -o 0 0 0 -s 1 1 1 b.png\n# map_Kd no.png\n");
    REQUIRE(refs.size() == 3);
    CHECK(refs[0] == "tex/diffuse.png");
    CHECK(refs[1] == "normals/n.png");
    CHECK(refs[2] == "b.png");
}

TEST_CASE("parse_obj_mtllib reads quoted and several names per line", "[ZipOpen]")
{
    CHECK(parse_obj_mtllib("mtllib a.mtl b.mtl\n") == std::vector<std::string>{"a.mtl b.mtl", "a.mtl", "b.mtl"});
    CHECK(parse_obj_mtllib("mtllib \"My Part.mtl\" other.mtl\n") == std::vector<std::string>{"My Part.mtl", "other.mtl"});
    CHECK(parse_obj_mtllib("mtllib \"open name.mtl\n") == std::vector<std::string>{"open name.mtl"});
    CHECK(parse_obj_mtllib("mtllib a.mtl\nmtllib a.mtl\n") == std::vector<std::string>{"a.mtl"});
}

TEST_CASE("parse_mtl_texture_refs skips word options, unknown flags and quotes", "[ZipOpen]")
{
    CHECK(parse_mtl_texture_refs("map_Kd -cc on t.png\n") == std::vector<std::string>{"t.png"});
    CHECK(parse_mtl_texture_refs("map_Kd -clamp off -blendu on -blendv off t1.png\n") == std::vector<std::string>{"t1.png"});
    CHECK(parse_mtl_texture_refs("map_Kd -texres 512 -imfchan r t2.png\n") == std::vector<std::string>{"t2.png"});
    CHECK(parse_mtl_texture_refs("map_Kd -o 0.5 -s 2 2 t3.png\n") == std::vector<std::string>{"t3.png"});
    // Unknown flags also list the remainder as a possible name; the lookup keeps whichever exists.
    CHECK(parse_mtl_texture_refs("map_Kd -foo 1 2 t4.png\n") == std::vector<std::string>{"-foo 1 2 t4.png", "t4.png"});
    CHECK(parse_mtl_texture_refs("map_Kd -x.png\n") == std::vector<std::string>{"-x.png"});
    CHECK(parse_mtl_texture_refs("map_Kd -type sphere -mm 0 1 \"my tex.png\"\n") == std::vector<std::string>{"my tex.png"});
    CHECK(parse_mtl_texture_refs("map_Kd my tex.png\n") == std::vector<std::string>{"my tex.png"});
    CHECK(parse_mtl_texture_refs("map_Kd -cc on\n").empty());
}

TEST_CASE("parse_mtl_texture_refs reads numbers without the C locale", "[ZipOpen]")
{
    CHECK(parse_mtl_texture_refs("map_Bump -bm 0.5 n.png\n") == std::vector<std::string>{"n.png"});
    CHECK(parse_mtl_texture_refs("map_Bump -bm -1.5e-1 n.png\n") == std::vector<std::string>{"n.png"});
    CHECK(parse_mtl_texture_refs("map_Kd -o +2 .5 3. t.png\n") == std::vector<std::string>{"t.png"});
    // "inf"/"0x1"/"0,5" are not numbers here, so they become the name.
    CHECK(parse_mtl_texture_refs("map_Bump -bm 0,5 n.png\n") == std::vector<std::string>{"0,5 n.png"});
    CHECK(parse_mtl_texture_refs("map_Bump -bm inf\n") == std::vector<std::string>{"inf"});
}

TEST_CASE("zip_obj_sidecar_indices resolves -x.png and stops at a whole mtllib name", "[ZipOpen]")
{
    const std::vector<ZipListedEntry> all = {listed(0, "a/part.obj"), listed(1, "a/My Part.mtl"), listed(2, "a/My"),
                                             listed(3, "a/Part.mtl"), listed(4, "a/-x.png")};
    const ZipEntryReader reader = [](std::uint32_t index, std::string& out) {
        if (index == 0) { out = "mtllib My Part.mtl\n"; return true; }
        if (index == 1) { out = "map_Kd -x.png\n"; return true; }
        return false;
    };
    // The whole name exists, so "My" and "Part.mtl" are not pulled; "-x.png" is a file, not a flag.
    CHECK(zip_obj_sidecar_indices(all[0], all, reader, false) == std::vector<std::uint32_t>{1, 4});

    const std::vector<ZipListedEntry> split = {listed(0, "a/part.obj"), listed(1, "a/My"), listed(2, "a/Part.mtl")};
    const ZipEntryReader reader2 = [](std::uint32_t index, std::string& out) {
        if (index == 0) { out = "mtllib My Part.mtl\n"; return true; }
        return false;
    };
    // No whole-name entry: fall back to the tokens.
    CHECK(zip_obj_sidecar_indices(split[0], split, reader2, false) == std::vector<std::uint32_t>{1, 2});
}

TEST_CASE("zip_obj_sidecar_indices matches case only where the platform does", "[ZipOpen]")
{
    const std::vector<ZipListedEntry> all = {listed(0, "a/part.obj"), listed(1, "a/Mats.MTL"), listed(2, "a/T.png"),
                                             listed(3, "a/mats.mtl"), listed(4, "a/t.png")};
    const ZipEntryReader reader = [](std::uint32_t index, std::string& out) {
        if (index == 0) { out = "mtllib Mats.MTL\n"; return true; }
        if (index == 1 || index == 3) { out = "map_Kd -cc on t.png\n"; return true; }
        return false;
    };
    // Exact names always win, on every platform.
    CHECK(zip_obj_sidecar_indices(all[0], all, reader, false) == std::vector<std::uint32_t>{1, 4});
    CHECK(zip_obj_sidecar_indices(all[0], all, reader, true) == std::vector<std::uint32_t>{1, 4});

    const std::vector<ZipListedEntry> only_upper = {listed(0, "a/part.obj"), listed(1, "a/Mats.MTL"), listed(2, "a/T.png")};
    const ZipEntryReader reader2 = [](std::uint32_t index, std::string& out) {
        if (index == 0) { out = "mtllib mats.mtl\n"; return true; }
        if (index == 1) { out = "map_Kd t.png\n"; return true; }
        return false;
    };
    // Case-sensitive: mtllib misses and the same-stem fallback is exact too, so nothing is pulled.
    CHECK(zip_obj_sidecar_indices(only_upper[0], only_upper, reader2, false).empty());
    CHECK(zip_obj_sidecar_indices(only_upper[0], only_upper, reader2, true) == std::vector<std::uint32_t>{1, 2});
    CHECK(zip_obj_sidecar_indices(only_upper[0], only_upper, reader2) ==
          (ZIP_SIDECAR_CASE_INSENSITIVE ? std::vector<std::uint32_t>{1, 2} : std::vector<std::uint32_t>{}));
}

TEST_CASE("zip_sidecar_map keeps only OBJs with sidecars and honours cancel", "[ZipOpen]")
{
    const std::vector<ZipListedEntry> all = {listed(0, "a/part.obj"), listed(1, "a/part.mtl"), listed(2, "a/t.png"), listed(3, "b/x.obj"),
                                             listed(4, "c.stl")};
    const ZipEntryReader reader = [](std::uint32_t index, std::string& out) {
        if (index == 0 || index == 3) { out = "v 0 0 0\n"; return true; }
        if (index == 1) { out = "map_Kd t.png\n"; return true; }
        return false;
    };
    const ZipSidecarMap map = zip_sidecar_map({0, 3, 4}, all, reader, {}, false);
    REQUIRE(map.size() == 1);
    CHECK(map.at(0) == std::vector<std::uint32_t>{1, 2});
    CHECK(zip_sidecar_map({0, 3}, all, reader, [] { return true; }, false).empty());
}

TEST_CASE("zip_extract_order puts sidecars first, each index once", "[ZipOpen]")
{
    CHECK(zip_extract_order({5, 1}, {2, 3, 2}) == std::vector<std::uint32_t>{2, 3, 5, 1});
    CHECK(zip_extract_order({4, 2}, {2}) == std::vector<std::uint32_t>{2, 4});
    CHECK(zip_extract_order({}, {}).empty());
}

TEST_CASE("resolve_zip_relative folds dots and refuses escapes", "[ZipOpen]")
{
    CHECK(resolve_zip_relative("models", "tex/a.png") == "models/tex/a.png");
    CHECK(resolve_zip_relative("models/sub", "../tex\\a.png") == "models/tex/a.png");
    CHECK(resolve_zip_relative("", "./a.mtl") == "a.mtl");
    CHECK(resolve_zip_relative("models", "../../a.png").empty());
}

TEST_CASE("zip_obj_sidecar_indices follows mtllib and MTL texture refs only", "[ZipOpen]")
{
    const std::vector<ZipListedEntry> all = {
        listed(0, "models/part.obj"),      listed(1, "models/mats/part_mats.mtl"),
        listed(2, "models/mats/tex/Diffuse.PNG"), listed(3, "models/unused.png"),
        listed(4, "models/part.mtl"),      listed(5, "shared/bump.jpg"),
    };
    const ZipEntryReader reader = [](std::uint32_t index, std::string& out) {
        if (index == 0) { out = "mtllib mats/part_mats.mtl\nv 0 0 0\n"; return true; }
        if (index == 1) { out = "newmtl m\nmap_Kd tex/diffuse.png\nbump ../../shared/bump.jpg\nmap_Ks missing.png\n"; return true; }
        return false;
    };
    const std::vector<std::uint32_t> extra = zip_obj_sidecar_indices(all[0], all, reader, true);
    // mtllib wins over the same-stem part.mtl; unused.png is not pulled by extension.
    CHECK(extra == std::vector<std::uint32_t>{1, 2, 5});
}

TEST_CASE("zip_obj_sidecar_indices falls back to the same-stem MTL", "[ZipOpen]")
{
    const std::vector<ZipListedEntry> all = {listed(0, "a/part.obj"), listed(1, "a/part.mtl"), listed(2, "a/t.png"), listed(3, "a/x.png")};
    const ZipEntryReader reader = [](std::uint32_t index, std::string& out) {
        if (index == 0) { out = "v 0 0 0\n"; return true; }
        if (index == 1) { out = "map_Kd t.png\n"; return true; }
        return false;
    };
    CHECK(zip_obj_sidecar_indices(all[0], all, reader) == std::vector<std::uint32_t>{1, 2});
    // Without a reader only the same-stem MTL can be found.
    CHECK(zip_obj_sidecar_indices(all[0], all, {}) == std::vector<std::uint32_t>{1});
}

TEST_CASE("is_zip_archive_path rejects zip.amf", "[ZipOpen]")
{
    CHECK(is_zip_archive_path("bundle.zip"));
    CHECK(is_zip_archive_path("C:/dl/Bundle.ZIP"));
    CHECK_FALSE(is_zip_archive_path("model.zip.amf"));
    CHECK_FALSE(is_zip_archive_path("model.stl"));
}

TEST_CASE("default zip work root is the platform's local cache folder", "[ZipOpen][ZipRoot]")
{
    SECTION("Windows uses LOCALAPPDATA, never Documents")
    {
        ZipRootEnv env;
        env.local_app_data = "C:\\Users\\Ann\\AppData\\Local";
        env.user_profile   = "C:\\Users\\Ann";
        CHECK(zip_work_root_for(ZipRootPlatform::Windows, env, "C:\\Users\\Ann\\AppData\\Roaming\\EdgeSlicer") ==
              "C:\\Users\\Ann\\AppData\\Local\\EdgeSlicer\\Zip");
        env.local_app_data = "C:\\Users\\Ann\\AppData\\Local\\";
        CHECK(zip_work_root_for(ZipRootPlatform::Windows, env, "") == "C:\\Users\\Ann\\AppData\\Local\\EdgeSlicer\\Zip");
        // Missing or relative LOCALAPPDATA: USERPROFILE\AppData\Local.
        env.local_app_data = "AppData\\Local";
        CHECK(zip_work_root_for(ZipRootPlatform::Windows, env, "") == "C:\\Users\\Ann\\AppData\\Local\\EdgeSlicer\\Zip");
        // Non-ASCII user name stays UTF-8.
        env.local_app_data = "C:\\Users\\J\xC3\xBCrgen\\AppData\\Local";
        CHECK(zip_work_root_for(ZipRootPlatform::Windows, env, "") == "C:\\Users\\J\xC3\xBCrgen\\AppData\\Local\\EdgeSlicer\\Zip");
    }
    SECTION("macOS uses ~/Library/Caches")
    {
        ZipRootEnv env;
        env.home = "/Users/ann";
        CHECK(zip_work_root_for(ZipRootPlatform::MacOS, env, "/Users/ann/Library/Application Support/EdgeSlicer") ==
              "/Users/ann/Library/Caches/EdgeSlicer/Zip");
    }
    SECTION("Linux uses XDG_CACHE_HOME, then ~/.cache; Flatpak's cache dir comes through XDG_CACHE_HOME")
    {
        ZipRootEnv env;
        env.home           = "/home/ann";
        env.xdg_cache_home = "/home/ann/.var/app/com.example.EdgeSlicer/cache";
        CHECK(zip_work_root_for(ZipRootPlatform::Linux, env, "") == "/home/ann/.var/app/com.example.EdgeSlicer/cache/EdgeSlicer/Zip");
        env.xdg_cache_home = "relative/cache"; // ignored per the XDG spec
        CHECK(zip_work_root_for(ZipRootPlatform::Linux, env, "") == "/home/ann/.cache/EdgeSlicer/Zip");
        env.xdg_cache_home.clear();
        CHECK(zip_work_root_for(ZipRootPlatform::Linux, env, "") == "/home/ann/.cache/EdgeSlicer/Zip");
    }
    SECTION("no usable folder: the app data folder, else empty (never a relative path)")
    {
        const ZipRootEnv none;
        CHECK(zip_work_root_for(ZipRootPlatform::Linux, none, "/home/ann/.config/EdgeSlicer") == "/home/ann/.config/EdgeSlicer/Zip");
        CHECK(zip_work_root_for(ZipRootPlatform::Windows, none, "D:\\Data\\EdgeSlicer") == "D:\\Data\\EdgeSlicer\\Zip");
        CHECK(zip_work_root_for(ZipRootPlatform::MacOS, none, "").empty());
        CHECK(zip_work_root_for(ZipRootPlatform::Linux, none, "EdgeSlicer").empty());
    }
}

#ifndef ZIP_OPEN_STANDALONE
TEST_CASE("remote saves of a ZIP-opened project get a hub file name, never a dialog", "[ZipOpen][RemoteSave]")
{
    // RemoteAccess names the save into <datadir>/hub/saves with this (review B-2).
    CHECK(zip_remote_save_name("C:/Users/Ann/AppData/Local/EdgeSlicer/Zip/bundle_20261103-101500/Benchy.3mf", "20261103-120000") ==
          "Benchy_20261103-120000.3mf");
    CHECK(zip_remote_save_name("/home/ann/.cache/EdgeSlicer/Zip/s/part.3MF", "X") == "part_X.3mf");
    CHECK(zip_remote_save_name("C:\\Zip\\s\\job.gcode.3mf", "X") == "job.gcode_X.3mf");
    CHECK(zip_remote_save_name("", "X") == "Untitled_X.3mf");
    CHECK(zip_remote_save_name("/z/s/...3mf", "X") == "Untitled_X.3mf");
    CHECK(zip_remote_save_name("/z/s/a:b|c.3mf", "X") == "a_b_c_X.3mf");
}

TEST_CASE("zip_work_dir overrides the default root", "[ZipOpen][ZipRoot]")
{
    namespace fs = boost::filesystem;
    const fs::path app_data = fs::temp_directory_path() / "edge-zip-appdata-test";
    const fs::path def      = default_zip_work_root(app_data);
    REQUIRE_FALSE(def.empty());
    CHECK(def.is_absolute());
    CHECK(def.filename() == "Zip");
    CHECK(def.parent_path().filename() == "EdgeSlicer");
    CHECK(resolve_zip_work_root("", app_data) == def);
    CHECK(resolve_zip_work_root("   ", app_data) == def);
    const fs::path custom = fs::temp_directory_path() / "my-zip-folder";
#ifdef _WIN32
    CHECK(resolve_zip_work_root(boost::nowide::narrow(custom.wstring()), app_data) == custom);
#else
    CHECK(resolve_zip_work_root(custom.string(), app_data) == custom);
#endif
}

TEST_CASE("zip_session_entry_dest keeps archive-relative folders", "[ZipOpen]")
{
    namespace fs = boost::filesystem;
    const fs::path dest = zip_session_entry_dest(fs::path("sess"), "models/part.stl");
    CHECK(dest == fs::path("sess") / "models" / "part.stl");
}
#endif

#ifndef ZIP_OPEN_STANDALONE
TEST_CASE("AppConfig seeds zip work dir, auto-open and hidden caps", "[ZipOpen][AppConfig]")
{
    AppConfig config;
    CHECK(config.get(ZIP_WORK_DIR_KEY).empty());
    CHECK(config.get_bool(ZIP_OPEN_SINGLE_WITHOUT_ASKING_KEY));
    CHECK(config.get(ZIP_MAX_ENTRIES_SCANNED_KEY) == "10000");
    CHECK(config.get(ZIP_MAX_ENTRY_UNCOMPRESSED_KEY) == "2147483648");
    CHECK(config.get(ZIP_MAX_TOTAL_UNCOMPRESSED_KEY) == "4294967296");
    CHECK(config.get(ZIP_MAX_COMPRESSION_RATIO_KEY) == "200");

    const ZipCapValues caps = zip_extract_caps_from_values(config.get(ZIP_MAX_ENTRIES_SCANNED_KEY),
                                                          config.get(ZIP_MAX_ENTRY_UNCOMPRESSED_KEY),
                                                          config.get(ZIP_MAX_TOTAL_UNCOMPRESSED_KEY),
                                                          config.get(ZIP_MAX_COMPRESSION_RATIO_KEY));
    CHECK(caps.max_entries_scanned == 10000);
    CHECK(caps.max_entry_uncompressed == 2147483648ull);
    CHECK(caps.max_total_uncompressed == 4294967296ull);
    CHECK(caps.max_compression_ratio == 200);
}
#endif

TEST_CASE("zip_extract_caps_from_values falls back to defaults", "[ZipOpen]")
{
    const ZipCapValues caps = zip_extract_caps_from_values("", "not-a-number", "", "200");
    CHECK(caps.max_entries_scanned == ZipCapValues{}.max_entries_scanned);
    CHECK(caps.max_entry_uncompressed == ZipCapValues{}.max_entry_uncompressed);
    CHECK(caps.max_compression_ratio == 200);
}

TEST_CASE("zip_extract_caps_from_values raises values below the floors", "[ZipOpen]")
{
    const ZipCapValues zero = zip_extract_caps_from_values("0", "0", "0", "0");
    CHECK(zero.max_entries_scanned == ZIP_MIN_ENTRIES_SCANNED);
    CHECK(zero.max_entry_uncompressed == ZIP_MIN_ENTRY_UNCOMPRESSED);
    CHECK(zero.max_total_uncompressed == ZIP_MIN_TOTAL_UNCOMPRESSED);
    CHECK(zero.max_compression_ratio == ZIP_MIN_COMPRESSION_RATIO);
    const ZipCapValues ok = zip_extract_caps_from_values("20", "2097152", "3145728", "50");
    CHECK(ok.max_entries_scanned == 20);
    CHECK(ok.max_entry_uncompressed == 2097152ull);
    CHECK(ok.max_total_uncompressed == 3145728ull);
    CHECK(ok.max_compression_ratio == 50);
}

TEST_CASE("format_zip_size uses byte units", "[ZipOpen]")
{
    CHECK(format_zip_size(512) == "512 B");
    CHECK(format_zip_size(2048).find("KB") != std::string::npos);
}

#ifdef __APPLE__
TEST_CASE("Apple FT_MODEL extras are models inside a zip", "[ZipOpen]")
{
    CHECK(classify_zip_entry_name("a.usd", false) == ZipEntryKind::Model);
    CHECK(classify_zip_entry_name("a.ply", false) == ZipEntryKind::Model);
}
#endif
