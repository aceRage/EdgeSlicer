#include <catch2/catch.hpp>

#include "libslic3r/GCode/PostProcessor.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/UntrustedInput.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <boost/algorithm/string/predicate.hpp>
#include <iterator>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

using namespace Slic3r;

namespace {

struct TempDir
{
    boost::filesystem::path path;
    TempDir()
        : path(boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("edge_pp3mf_%%%%%%%%"))
    {
        boost::filesystem::create_directories(path);
    }
    ~TempDir()
    {
        boost::system::error_code ec;
        boost::filesystem::remove_all(path, ec);
    }
};

std::string write_file(const boost::filesystem::path &p, const std::string &contents)
{
    boost::nowide::ofstream out(p.string().c_str(), std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out << contents;
    out.close();
    return p.string();
}

std::string read_file(const std::string &p)
{
    boost::nowide::ifstream in(p.c_str(), std::ios::binary);
    REQUIRE(in.good());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

DynamicPrintConfig config_with_scripts(const std::vector<std::string> &scripts)
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("post_process", new ConfigOptionStrings(scripts));
    return cfg;
}

std::string write_append_script(const TempDir &dir)
{
#ifdef _WIN32
    const boost::filesystem::path script = dir.path / "append_marker.bat";
    // No leading ';': cmd.exe treats that as a statement separator.
    write_file(script, "@echo off\r\n>>\"%~1\" echo PP_MARKER\r\n");
    return script.string();
#else
    const boost::filesystem::path script = dir.path / "append_marker.sh";
    write_file(script, "#!/bin/sh\nprintf 'PP_MARKER\\n' >> \"$1\"\n");
    REQUIRE(::chmod(script.string().c_str(), 0755) == 0);
    return script.string();
#endif
}

std::string write_fail_script(const TempDir &dir)
{
#ifdef _WIN32
    const boost::filesystem::path script = dir.path / "fail.bat";
    write_file(script, "@echo off\r\nexit /b 2\r\n");
    return script.string();
#else
    const boost::filesystem::path script = dir.path / "fail.sh";
    write_file(script, "#!/bin/sh\nexit 2\n");
    REQUIRE(::chmod(script.string().c_str(), 0755) == 0);
    return script.string();
#endif
}

} // namespace

TEST_CASE("Packed 3mf G-code is post-processed only for a non-BBL FFF plate that packs G-code", "[PostProcessor][3mf]")
{
    // Export plate sliced file / send 3mf (WithGcode, not a backup).
    CHECK(should_post_process_packed_gcode(true, false, ptFFF, false));
    // BBL already post-processed the mapped temp file after slicing.
    CHECK_FALSE(should_post_process_packed_gcode(true, false, ptFFF, true));
    // Backups use SkipStatic; the writer does not pack G-code.
    CHECK_FALSE(should_post_process_packed_gcode(true, true, ptFFF, false));
    // Project save / config 3mf: no G-code packed.
    CHECK_FALSE(should_post_process_packed_gcode(false, false, ptFFF, false));
    CHECK_FALSE(should_post_process_packed_gcode(false, false, ptFFF, true));
    // SLA has no FFF post_process pipeline here.
    CHECK_FALSE(should_post_process_packed_gcode(true, false, ptSLA, false));
    CHECK_FALSE(should_post_process_packed_gcode(true, false, ptUnknown, false));
}

TEST_CASE("use_3mf upload does not run scripts: packing already did, or BBL already did", "[PostProcessor][3mf]")
{
    // The packing helper is the only 3mf site. prepare_upload(use_3mf) must skip.
    // Non-BBL + use_3mf: pack runs, upload does not.
    CHECK(should_post_process_packed_gcode(true, false, ptFFF, false));
    // BBL + use_3mf: pack skips, upload skips, after-slice in-place already ran.
    CHECK_FALSE(should_post_process_packed_gcode(true, false, ptFFF, true));
}

TEST_CASE("An empty post_process leaves the packed G-code path unchanged", "[PostProcessor][3mf]")
{
    TempDir dir;
    const std::string raw = write_file(dir.path / "plate.gcode", "; RAW\n");
    std::string       path = raw;
    DynamicPrintConfig cfg = config_with_scripts({});
    PackedGcodePlate plate;
    plate.gcode_file = &path;
    plate.config     = &cfg;
    std::vector<PackedGcodePlate> plates(1, plate);
    std::vector<std::string>      tmp;
    REQUIRE_NOTHROW(post_process_packed_gcodes(plates, (dir.path / "out.gcode.3mf").string(), tmp));
    CHECK(path == raw);
    CHECK(tmp.empty());
    CHECK(read_file(raw) == "; RAW\n");
}

TEST_CASE("A packed-G-code script runs on a copy and leaves the original mapped file alone", "[PostProcessor][3mf]")
{
    TempDir dir;
    const std::string raw = write_file(dir.path / "plate.gcode", "; RAW\n");
    std::string       path = raw;
    DynamicPrintConfig cfg = config_with_scripts({write_append_script(dir)});
    PackedGcodePlate plate;
    plate.gcode_file = &path;
    plate.config     = &cfg;
    std::vector<PackedGcodePlate> plates(1, plate);
    std::vector<std::string>      tmp;
    REQUIRE_NOTHROW(post_process_packed_gcodes(plates, (dir.path / "out.gcode.3mf").string(), tmp));
    REQUIRE(tmp.size() == 1);
    CHECK(path == tmp.front());
    CHECK(path != raw);
    CHECK(read_file(raw) == "; RAW\n");
    const std::string processed = read_file(path);
    CHECK(processed.find("; RAW") != std::string::npos);
    CHECK(processed.find("PP_MARKER") != std::string::npos);
    boost::system::error_code ec;
    boost::filesystem::remove(path, ec);
}

TEST_CASE("A failing packed-G-code script throws and does not keep a copy", "[PostProcessor][3mf]")
{
    TempDir dir;
    const std::string raw = write_file(dir.path / "plate.gcode", "; RAW\n");
    std::string       path = raw;
    DynamicPrintConfig cfg = config_with_scripts({write_fail_script(dir)});
    PackedGcodePlate plate;
    plate.gcode_file = &path;
    plate.config     = &cfg;
    std::vector<PackedGcodePlate> plates(1, plate);
    std::vector<std::string>      tmp;
    REQUIRE_THROWS_AS(post_process_packed_gcodes(plates, (dir.path / "out.gcode.3mf").string(), tmp), Slic3r::RuntimeError);
    CHECK(path == raw);
    CHECK(tmp.empty());
    CHECK(read_file(raw) == "; RAW\n");
    CHECK_FALSE(boost::filesystem::exists(raw + ".pp"));
}

TEST_CASE("post_process stripped from an untrusted 3MF does not run when packing G-code", "[PostProcessor][3mf][Untrusted]")
{
    TempDir dir;
    const std::string raw = write_file(dir.path / "plate.gcode", "; RAW\n");
    std::string       path = raw;
    DynamicPrintConfig cfg = config_with_scripts({write_append_script(dir)});
    untrusted::TrustedValues none;
    const auto found = untrusted::find_untrusted_settings(cfg, "project settings", none);
    REQUIRE_FALSE(found.empty());
    untrusted::neutralize_settings(cfg, found, nullptr);
    CHECK(untrusted::joined_post_process(cfg).empty());

    PackedGcodePlate plate;
    plate.gcode_file = &path;
    plate.config     = &cfg;
    std::vector<PackedGcodePlate> plates(1, plate);
    std::vector<std::string>      tmp;
    REQUIRE_NOTHROW(post_process_packed_gcodes(plates, (dir.path / "out.gcode.3mf").string(), tmp));
    CHECK(path == raw);
    CHECK(tmp.empty());
    CHECK(read_file(raw) == "; RAW\n");
}

TEST_CASE("A plate with no G-code file or no config is skipped when packing", "[PostProcessor][3mf]")
{
    std::string empty;
    DynamicPrintConfig cfg = config_with_scripts({"this-script-must-not-run"});
    PackedGcodePlate no_file;
    no_file.gcode_file = &empty;
    no_file.config     = &cfg;
    std::string raw = "plate.gcode";
    PackedGcodePlate no_cfg;
    no_cfg.gcode_file = &raw;
    no_cfg.config     = nullptr;
    std::vector<PackedGcodePlate> plates;
    plates.push_back(no_file);
    plates.push_back(no_cfg);
    std::vector<std::string> tmp;
    REQUIRE_NOTHROW(post_process_packed_gcodes(plates, "out.gcode.3mf", tmp));
    CHECK(empty.empty());
    CHECK(raw == "plate.gcode");
    CHECK(tmp.empty());
}

TEST_CASE("Two sliced plates each get their own post-processed copy", "[PostProcessor][3mf]")
{
    TempDir dir;
    std::string a = write_file(dir.path / "plate_0.gcode", "; A\n");
    std::string b = write_file(dir.path / "plate_1.gcode", "; B\n");
    const std::string a_raw = a;
    const std::string b_raw = b;
    DynamicPrintConfig cfg = config_with_scripts({write_append_script(dir)});
    PackedGcodePlate pa;
    pa.gcode_file = &a;
    pa.config     = &cfg;
    PackedGcodePlate pb;
    pb.gcode_file = &b;
    pb.config     = &cfg;
    std::vector<PackedGcodePlate> plates;
    plates.push_back(pa);
    plates.push_back(pb);
    std::vector<std::string> tmp;
    REQUIRE_NOTHROW(post_process_packed_gcodes(plates, (dir.path / "out.gcode.3mf").string(), tmp));
    REQUIRE(tmp.size() == 2);
    CHECK(a != a_raw);
    CHECK(b != b_raw);
    CHECK(read_file(a_raw) == "; A\n");
    CHECK(read_file(b_raw) == "; B\n");
    CHECK(read_file(a).find("PP_MARKER") != std::string::npos);
    CHECK(read_file(b).find("PP_MARKER") != std::string::npos);
    boost::system::error_code ec;
    boost::filesystem::remove(a, ec);
    boost::filesystem::remove(b, ec);
}

// ---------------------------------------------------------------------------------------------
// End to end through the real 3MF writer: what Plater::export_3mf does after
// post_process_packed_gcodes() (pack pd->gcode_file with SaveStrategy::WithGcode).
// ---------------------------------------------------------------------------------------------
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/miniz_extension.hpp"

#include <map>

namespace {

// All entries of a zip: name -> content.
std::map<std::string, std::string> read_zip_entries(const std::string &zip_path)
{
    std::map<std::string, std::string> out;
    mz_zip_archive                     zip;
    mz_zip_zero_struct(&zip);
    REQUIRE(open_zip_reader(&zip, zip_path));
    const mz_uint n = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st;
        REQUIRE(mz_zip_reader_file_stat(&zip, i, &st));
        size_t size = 0;
        void  *data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
        REQUIRE((data != nullptr || st.m_uncomp_size == 0));
        out[st.m_filename] = data ? std::string(static_cast<const char *>(data), size) : std::string();
        mz_free(data);
    }
    close_zip_reader(&zip);
    return out;
}

// Packs one sliced plate into a .gcode.3mf exactly like Plater::export_3mf does for Export plate sliced file.
void pack_gcode_3mf(const std::string &out_path, const std::string &gcode_file)
{
    boost::filesystem::create_directories(boost::filesystem::temp_directory_path() / "snorca_tests");
    Slic3r::set_temporary_dir((boost::filesystem::temp_directory_path() / "snorca_tests").string());

    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "pp_cube";
    object->add_volume(make_cube(10., 10., 10.))->name = "cube";
    object->add_instance();
    object->ensure_on_bed();

    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    PlateData         *plate = new PlateData();
    plate->plate_index      = 0;
    plate->is_sliced_valid  = true;
    plate->gcode_file       = gcode_file;
    plate->objects_and_instances.emplace_back(0, 0);

    StoreParams sp;
    sp.path            = out_path.c_str();
    sp.model           = &model;
    sp.config          = &cfg;
    sp.strategy        = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SplitModel | SaveStrategy::WithGcode |
                         SaveStrategy::SkipModel;
    sp.plate_data_list.push_back(plate);
    REQUIRE(store_bbs_3mf(sp));
    release_PlateData_list(sp.plate_data_list);
}

} // namespace

TEST_CASE("Post-processed G-code lands in the .gcode.3mf with a matching md5; without a script it is untouched", "[PostProcessor][3mf][Pack]")
{
    TempDir           dir;
    const std::string body = "; generated\nG1 X1 Y1 E1\n";
    const std::string raw  = write_file(dir.path / "plate_1.gcode", body);

    // Baseline: no script, G-code packed as is.
    const std::string plain_3mf = (dir.path / "plain.gcode.3mf").string();
    pack_gcode_3mf(plain_3mf, raw);
    const auto plain = read_zip_entries(plain_3mf);
    REQUIRE(plain.count("Metadata/plate_1.gcode") == 1);
    CHECK(plain.at("Metadata/plate_1.gcode") == body);
    REQUIRE(plain.count("Metadata/plate_1.gcode.md5") == 1);

    // With a script: pack the post-processed copy instead.
    std::string        path = raw;
    DynamicPrintConfig cfg  = config_with_scripts({write_append_script(dir)});
    PackedGcodePlate   item;
    item.gcode_file = &path;
    item.config     = &cfg;
    std::vector<PackedGcodePlate> plates(1, item);
    std::vector<std::string>      tmp;
    const std::string             pp_3mf = (dir.path / "pp.gcode.3mf").string();
    REQUIRE_NOTHROW(post_process_packed_gcodes(plates, pp_3mf, tmp));
    REQUIRE(tmp.size() == 1);
    pack_gcode_3mf(pp_3mf, path);
    const auto pp = read_zip_entries(pp_3mf);

    // (a) marker inside the packed G-code, once, after the original content
    const std::string packed = pp.at("Metadata/plate_1.gcode");
    CHECK(packed.compare(0, body.size(), body) == 0);
    const size_t first = packed.find("PP_MARKER");
    REQUIRE(first != std::string::npos);
    CHECK(packed.find("PP_MARKER", first + 1) == std::string::npos);

    // (b) the md5 entry matches the modified G-code (and differs from the unmodified one)
    const std::string packed_file = write_file(dir.path / "extracted.gcode", packed);
    std::string       md5;
    REQUIRE(bbl_calc_md5(packed_file, md5));
    REQUIRE(pp.count("Metadata/plate_1.gcode.md5") == 1);
    CHECK(boost::iequals(pp.at("Metadata/plate_1.gcode.md5"), md5));
    CHECK_FALSE(boost::iequals(pp.at("Metadata/plate_1.gcode.md5"), plain.at("Metadata/plate_1.gcode.md5")));

    // (c) still a readable zip with every original entry
    for (const auto &kv : plain)
        CHECK(pp.count(kv.first) == 1);
    CHECK(pp.size() == plain.size());

    // The viewer's original file is untouched.
    CHECK(read_file(raw) == body);

    boost::system::error_code ec;
    boost::filesystem::remove(path, ec);
}

TEST_CASE("A failing script aborts before a .gcode.3mf is written", "[PostProcessor][3mf][Pack]")
{
    TempDir           dir;
    const std::string raw      = write_file(dir.path / "plate_1.gcode", "; RAW\n");
    const std::string out_3mf  = (dir.path / "never.gcode.3mf").string();
    std::string        path    = raw;
    DynamicPrintConfig cfg     = config_with_scripts({write_fail_script(dir)});
    PackedGcodePlate   item;
    item.gcode_file = &path;
    item.config     = &cfg;
    std::vector<PackedGcodePlate> plates(1, item);
    std::vector<std::string>      tmp;
    // Plater::export_3mf catches this, shows the error and returns -1 before opening the zip.
    CHECK_THROWS_AS(post_process_packed_gcodes(plates, out_3mf, tmp), Slic3r::RuntimeError);
    CHECK_FALSE(boost::filesystem::exists(out_3mf));
    CHECK(tmp.empty());
    CHECK(path == raw);
}
