// When the filament library (OrcaFilamentLibrary) is copied from resources into <data_dir>/system
// at startup. Upstream did it on every start; now only when something changed (see
// src/slic3r/Utils/VendorInstallStamp.hpp). These tests pin down every reason to reinstall:
// missing json or folder, a newer resource version, no or unreadable stamp, a changed resource
// tree (edited, added, deleted, mtime-only) and a changed installed tree; and that an unchanged
// pair is skipped. Every test works in its own temp directory.

#include <catch2/catch.hpp>

#include "slic3r/Utils/VendorInstallStamp.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <ctime>
#include <string>

using namespace Slic3r::Utils;
namespace bfs = boost::filesystem;

namespace {

struct TempDir
{
    bfs::path path;
    TempDir() : path(bfs::temp_directory_path() / bfs::unique_path("vendor_stamp_%%%%%%%%")) { bfs::create_directories(path); }
    ~TempDir()
    {
        boost::system::error_code ec;
        bfs::remove_all(path, ec);
    }
};

void write_text(const bfs::path &p, const std::string &text)
{
    bfs::create_directories(p.parent_path());
    boost::nowide::ofstream out(p.string(), std::ios::binary | std::ios::trunc);
    out << text;
}

void touch(const bfs::path &p, long seconds)
{
    const std::time_t t = bfs::last_write_time(p);
    bfs::last_write_time(p, t + seconds);
}

std::string vendor_json(const std::string &version) { return "{\"name\": \"OrcaFilamentLibrary\", \"version\": \"" + version + "\"}\n"; }

// A small bundle laid out like profiles/OrcaFilamentLibrary(.json).
void make_bundle(const bfs::path &root, const std::string &version)
{
    write_text(root / "OrcaFilamentLibrary.json", vendor_json(version));
    write_text(root / "OrcaFilamentLibrary" / "filament" / "fdm_filament_pla.json", "{\"name\": \"fdm_filament_pla\"}\n");
    write_text(root / "OrcaFilamentLibrary" / "filament" / "Generic PLA @System.json", "{\"name\": \"Generic PLA @System\"}\n");
    write_text(root / "OrcaFilamentLibrary" / "filament" / "base" / "fdm_filament_common.json", "{\"name\": \"fdm_filament_common\"}\n");
}

std::string digest(const bfs::path &root)
{
    return tree_stamp_digest((root / "OrcaFilamentLibrary").string(), (root / "OrcaFilamentLibrary.json").string());
}

// Inputs as PresetUpdater gathers them, for a resource tree and an installed tree.
VendorInstallInputs inputs(const bfs::path &rsrc, const bfs::path &inst, const std::string &stamp)
{
    VendorInstallInputs in;
    boost::system::error_code ec;
    in.installed_json_exists   = bfs::exists(inst / "OrcaFilamentLibrary.json", ec);
    in.installed_dir_has_files = bfs::is_directory(inst / "OrcaFilamentLibrary", ec) &&
                                 bfs::directory_iterator(inst / "OrcaFilamentLibrary", ec) != bfs::directory_iterator();
    in.resource_version        = "02.00.00.10";
    in.installed_version       = "02.00.00.10";
    in.stamp_record            = stamp;
    in.resource_digest         = digest(rsrc);
    in.installed_digest        = digest(inst);
    return in;
}

// Simulates an install: the installed side becomes a copy of the resource side, then the stamp.
std::string install(const bfs::path &rsrc, const bfs::path &inst)
{
    boost::system::error_code ec;
    bfs::remove_all(inst, ec);
    make_bundle(inst, "02.00.00.10");
    return make_vendor_install_stamp(digest(rsrc), digest(inst));
}

} // namespace

TEST_CASE("tree_stamp_digest sees every kind of change", "[VendorInstallStamp]")
{
    TempDir t;
    make_bundle(t.path, "02.00.00.10");
    const std::string d0 = digest(t.path);
    REQUIRE_FALSE(d0.empty());
    CHECK(digest(t.path) == d0); // stable

    SECTION("edited file, different size")
    {
        write_text(t.path / "OrcaFilamentLibrary" / "filament" / "fdm_filament_pla.json", "{\"name\": \"fdm_filament_pla\", \"x\": 1}\n");
        CHECK(digest(t.path) != d0);
    }
    SECTION("same bytes, only the time moved")
    {
        touch(t.path / "OrcaFilamentLibrary" / "filament" / "fdm_filament_pla.json", 120);
        CHECK(digest(t.path) != d0);
    }
    SECTION("file added in a subfolder")
    {
        write_text(t.path / "OrcaFilamentLibrary" / "filament" / "base" / "extra.json", "{}\n");
        CHECK(digest(t.path) != d0);
    }
    SECTION("file deleted")
    {
        bfs::remove(t.path / "OrcaFilamentLibrary" / "filament" / "Generic PLA @System.json");
        CHECK(digest(t.path) != d0);
    }
    SECTION("vendor json version bump")
    {
        write_text(t.path / "OrcaFilamentLibrary.json", vendor_json("02.00.00.11"));
        CHECK(digest(t.path) != d0);
    }
    SECTION("missing folder or missing vendor json: no digest")
    {
        CHECK(tree_stamp_digest((t.path / "nope").string()).empty());
        bfs::remove(t.path / "OrcaFilamentLibrary.json");
        CHECK(digest(t.path).empty());
    }
    SECTION("empty folder: no digest")
    {
        bfs::create_directories(t.path / "empty");
        CHECK(tree_stamp_digest((t.path / "empty").string()).empty());
    }
}

TEST_CASE("Library install decision", "[VendorInstallStamp]")
{
    TempDir rs, ds;
    const bfs::path rsrc = rs.path, inst = ds.path;
    make_bundle(rsrc, "02.00.00.10");
    const std::string stamp = install(rsrc, inst);
    REQUIRE_FALSE(stamp.empty());

    SECTION("unchanged since the last install: skipped")
    {
        const auto d = decide_vendor_install(inputs(rsrc, inst, stamp));
        CHECK_FALSE(d.install);
    }
    SECTION("installed vendor json missing")
    {
        bfs::remove(inst / "OrcaFilamentLibrary.json");
        CHECK(decide_vendor_install(inputs(rsrc, inst, stamp)).install);
    }
    SECTION("installed folder missing")
    {
        bfs::remove_all(inst / "OrcaFilamentLibrary");
        const auto d = decide_vendor_install(inputs(rsrc, inst, stamp));
        CHECK(d.install);
        CHECK(d.reason == "missing");
    }
    SECTION("installed folder empty")
    {
        bfs::remove_all(inst / "OrcaFilamentLibrary");
        bfs::create_directories(inst / "OrcaFilamentLibrary");
        CHECK(decide_vendor_install(inputs(rsrc, inst, stamp)).install);
    }
    SECTION("resource version is newer")
    {
        auto in              = inputs(rsrc, inst, stamp);
        in.resource_version  = "02.00.00.11";
        const auto d         = decide_vendor_install(in);
        CHECK(d.install);
        CHECK(d.reason.find("newer") != std::string::npos);
    }
    SECTION("resource version bump in the files is caught by the digest too")
    {
        write_text(rsrc / "OrcaFilamentLibrary.json", vendor_json("02.00.00.11"));
        CHECK(decide_vendor_install(inputs(rsrc, inst, stamp)).install);
    }
    SECTION("older resource version with other content (another build sharing the data dir)")
    {
        write_text(rsrc / "OrcaFilamentLibrary.json", vendor_json("02.00.00.09"));
        auto in             = inputs(rsrc, inst, stamp);
        in.resource_version = "02.00.00.09";
        CHECK(decide_vendor_install(in).install);
    }
    SECTION("edited library file in resources (dev tree or a new build)")
    {
        write_text(rsrc / "OrcaFilamentLibrary" / "filament" / "fdm_filament_pla.json", "{\"name\": \"fdm_filament_pla\", \"t\": 2}\n");
        const auto d = decide_vendor_install(inputs(rsrc, inst, stamp));
        CHECK(d.install);
        CHECK(d.reason == "resource bundle changed");
    }
    SECTION("edited installed library file")
    {
        write_text(inst / "OrcaFilamentLibrary" / "filament" / "fdm_filament_pla.json", "{\"name\": \"hand edit\"}\n");
        const auto d = decide_vendor_install(inputs(rsrc, inst, stamp));
        CHECK(d.install);
        CHECK(d.reason == "installed copy changed");
    }
    SECTION("installed file deleted")
    {
        bfs::remove(inst / "OrcaFilamentLibrary" / "filament" / "base" / "fdm_filament_common.json");
        CHECK(decide_vendor_install(inputs(rsrc, inst, stamp)).install);
    }
    SECTION("no stamp (first start after the update, or the cache folder was cleared)")
    {
        const auto d = decide_vendor_install(inputs(rsrc, inst, std::string()));
        CHECK(d.install);
        CHECK(d.reason == "no install stamp");
    }
    SECTION("unreadable stamp")
    {
        CHECK(decide_vendor_install(inputs(rsrc, inst, "garbage\nresource=x\n")).install);
    }
    SECTION("a digest that could not be taken counts as changed")
    {
        auto in             = inputs(rsrc, inst, stamp);
        in.installed_digest = std::string();
        CHECK(decide_vendor_install(in).install);
    }
    SECTION("after a reinstall the new stamp makes the next start skip again")
    {
        write_text(rsrc / "OrcaFilamentLibrary" / "filament" / "new.json", "{}\n");
        REQUIRE(decide_vendor_install(inputs(rsrc, inst, stamp)).install);
        const std::string stamp2 = make_vendor_install_stamp(digest(rsrc), digest(inst));
        CHECK_FALSE(decide_vendor_install(inputs(rsrc, inst, stamp2)).install);
    }
}

TEST_CASE("make_vendor_install_stamp refuses empty digests", "[VendorInstallStamp]")
{
    CHECK(make_vendor_install_stamp("", "abc").empty());
    CHECK(make_vendor_install_stamp("abc", "").empty());
    CHECK_FALSE(make_vendor_install_stamp("a", "b").empty());
}

TEST_CASE("compare_dotted_versions", "[VendorInstallStamp]")
{
    CHECK(compare_dotted_versions("02.00.00.11", "02.00.00.10") > 0);
    CHECK(compare_dotted_versions("02.00.00.10", "02.00.00.10") == 0);
    CHECK(compare_dotted_versions("2.0", "02.00.00.00") == 0);
    CHECK(compare_dotted_versions("1.9.9", "2.0") < 0);
}
