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
    CHECK(zip_entry_is_selectable(ZipEntryKind::Model, false, false));
    CHECK(zip_entry_is_selectable(ZipEntryKind::GCode, false, false));
    CHECK_FALSE(zip_entry_is_selectable(ZipEntryKind::Model, true, false));
    CHECK_FALSE(zip_entry_is_selectable(ZipEntryKind::GCode, false, true));
    CHECK_FALSE(zip_entry_is_selectable(ZipEntryKind::NestedZip, false, false));
    CHECK_FALSE(zip_entry_is_selectable(ZipEntryKind::Unsupported, false, false));
    CHECK_FALSE(zip_entry_is_selectable(ZipEntryKind::Ignored, false, false));
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

TEST_CASE("zip_obj_sidecar_indices pulls the matching mtl and same-folder images", "[ZipOpen]")
{
    ZipListedEntry obj;
    obj.index        = 0;
    obj.decoded_name = "models/part.obj";
    ZipListedEntry mtl;
    mtl.index        = 1;
    mtl.decoded_name = "models/part.mtl";
    ZipListedEntry png;
    png.index        = 2;
    png.decoded_name = "models/tex.png";
    ZipListedEntry other_mtl;
    other_mtl.index        = 3;
    other_mtl.decoded_name = "models/other.mtl";
    ZipListedEntry nested;
    nested.index        = 4;
    nested.decoded_name = "models/maps/tex.png";
    ZipListedEntry jpg;
    jpg.index        = 5;
    jpg.decoded_name = "models/photo.jpg";

    const std::vector<ZipListedEntry> all   = {obj, mtl, png, other_mtl, nested, jpg};
    const std::vector<std::uint32_t>  extra = zip_obj_sidecar_indices(obj, all);
    REQUIRE(extra.size() == 3);
    CHECK(extra[0] == 1);
    CHECK(extra[1] == 2);
    CHECK(extra[2] == 5);
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
