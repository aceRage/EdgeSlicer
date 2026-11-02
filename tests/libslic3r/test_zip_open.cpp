#include <catch2/catch.hpp>

#ifndef ZIP_OPEN_STANDALONE
#include "libslic3r/AppConfig.hpp"
#include <boost/filesystem.hpp>
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
    CHECK(classify_zip_entry_name("a.bgcode", false) == ZipEntryKind::GCode);
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
    REQUIRE(libs.size() == 2);
    CHECK(libs[0] == "mats/My Part.mtl");
    CHECK(libs[1] == "b.mtl");

    const auto refs = parse_mtl_texture_refs("newmtl a\nKd 1 1 1\nmap_Kd tex/diffuse.png\n"
                                             "map_Bump -bm 0.5 normals/n.png\nbump -o 0 0 0 -s 1 1 1 b.png\n# map_Kd no.png\n");
    REQUIRE(refs.size() == 3);
    CHECK(refs[0] == "tex/diffuse.png");
    CHECK(refs[1] == "normals/n.png");
    CHECK(refs[2] == "b.png");
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
    const std::vector<std::uint32_t> extra = zip_obj_sidecar_indices(all[0], all, reader);
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

#ifndef ZIP_OPEN_STANDALONE
TEST_CASE("default zip work root is Documents/EdgeSlicer/Zip", "[ZipOpen]")
{
    namespace fs = boost::filesystem;
    const fs::path docs = fs::temp_directory_path() / "edge-zip-docs-test";
    boost::system::error_code ec;
    fs::create_directories(docs, ec);
    REQUIRE(fs::is_directory(docs));
    CHECK(default_zip_work_root(docs) == docs / "EdgeSlicer" / "Zip");
    fs::remove_all(docs, ec);
}

TEST_CASE("zip_session_entry_dest keeps archive-relative folders", "[ZipOpen]")
{
    namespace fs = boost::filesystem;
    const fs::path dest = zip_session_entry_dest(fs::path("sess"), "models/part.stl");
    CHECK(dest == fs::path("sess") / "models" / "part.stl");
}
#endif

#ifndef ZIP_OPEN_STANDALONE
TEST_CASE("AppConfig seeds zip work dir, auto-open, hidden caps and autoclean", "[ZipOpen][AppConfig]")
{
    AppConfig config;
    CHECK(config.get(ZIP_WORK_DIR_KEY).empty());
    CHECK(config.get_bool(ZIP_OPEN_SINGLE_WITHOUT_ASKING_KEY));
    CHECK(config.get(ZIP_MAX_ENTRIES_SCANNED_KEY) == "10000");
    CHECK(config.get(ZIP_MAX_ENTRY_UNCOMPRESSED_KEY) == "2147483648");
    CHECK(config.get(ZIP_MAX_TOTAL_UNCOMPRESSED_KEY) == "4294967296");
    CHECK(config.get(ZIP_MAX_COMPRESSION_RATIO_KEY) == "200");
    CHECK_FALSE(config.get_bool(ZIP_WORK_AUTOCLEAN_KEY));
    CHECK(config.get(ZIP_WORK_AUTOCLEAN_DAYS_KEY) == "30");

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
