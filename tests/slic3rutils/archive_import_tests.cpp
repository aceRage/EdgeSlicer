// Remote printing (A1): what a .gcode.3mf another PC sends must pass before it enters the G-code
// archive (ArchiveImport). The real files are the reprint fixtures; the bad ones are written here
// with miniz, into a temporary folder of the test's own.

#include <catch2/catch.hpp>

#include "slic3r/GUI/ArchiveImport.hpp"
#include "slic3r/GUI/HubClients.hpp"
#include "libslic3r/miniz_extension.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace fs = boost::filesystem;
using namespace Slic3r::GUI;

namespace {

std::string fixture(const char* name) { return (fs::path(TEST_DATA_DIR) / "bambu_reprint" / name).string(); }

struct TempDir
{
    fs::path dir;
    TempDir() : dir(fs::temp_directory_path() / fs::unique_path("archive_import_%%%%-%%%%")) { fs::create_directories(dir); }
    ~TempDir() { boost::system::error_code ig; fs::remove_all(dir, ig); }
};

// A ZIP with these entries (name, content), as a .gcode.3mf.
std::string make_zip(const TempDir& t, const char* file, const std::vector<std::pair<std::string, std::string>>& entries)
{
    const std::string path = (t.dir / file).string();
    mz_zip_archive    zip;
    std::memset(&zip, 0, sizeof(zip));
    REQUIRE(Slic3r::open_zip_writer(&zip, path));
    for (const auto& e : entries)
        REQUIRE(mz_zip_writer_add_mem(&zip, e.first.c_str(), e.second.data(), e.second.size(), MZ_DEFAULT_COMPRESSION));
    REQUIRE(mz_zip_writer_finalize_archive(&zip));
    Slic3r::close_zip_writer(&zip);
    return path;
}

} // namespace

TEST_CASE("clean_name keeps a .gcode.3mf name and refuses anything else", "[RemoteHub][remote-print]")
{
    REQUIRE(ArchiveImport::clean_name("plate_1.gcode.3mf") == "plate_1.gcode.3mf");
    REQUIRE(ArchiveImport::clean_name("Benchy.GCODE.3MF") == "Benchy.GCODE.3MF");
    REQUIRE(ArchiveImport::clean_name("../../evil.gcode.3mf") == "evil.gcode.3mf");
    REQUIRE(ArchiveImport::clean_name("model.3mf").empty());
    REQUIRE(ArchiveImport::clean_name("job.gcode").empty());
    REQUIRE(ArchiveImport::clean_name(".gcode.3mf").empty());
    REQUIRE(ArchiveImport::clean_name("").empty());
}

TEST_CASE("a sliced job the desktop exported passes, with its plate and picture", "[RemoteHub][remote-print]")
{
    for (const char* f : { "h2d_two_sided.gcode.3mf", "x1c_two_colour.gcode.3mf", "h2c_rack.gcode.3mf" }) {
        const ArchiveImport::Package p = ArchiveImport::check_package(fixture(f));
        INFO(f << ": " << p.error);
        REQUIRE(p.error.empty());
        REQUIRE(p.plate >= 1);
        REQUIRE(p.entries > 0);
    }
}

TEST_CASE("check_package refuses what is not a sliced job", "[RemoteHub][remote-print]")
{
    TempDir t;
    {
        const std::string path = (t.dir / "text.gcode.3mf").string();
        boost::nowide::ofstream text(path.c_str());
        text << "G28\nG1 X10\n";
        text.close();
        REQUIRE_FALSE(ArchiveImport::check_package(path).error.empty());
    }
    // A ZIP with no G-code at all (a plain project).
    REQUIRE_THAT(ArchiveImport::check_package(make_zip(t, "project.gcode.3mf", { { "3D/3dmodel.model", "<model/>" } })).error,
                 Catch::Matchers::Contains("no G-code"));
    // Two plates of G-code: an "all plates" export cannot be one print.
    REQUIRE_THAT(ArchiveImport::check_package(make_zip(t, "two.gcode.3mf", { { "Metadata/plate_1.gcode", "G28\n" },
                                                                             { "Metadata/plate_2.gcode", "G28\n" } })).error,
                 Catch::Matchers::Contains("several plates"));
    // An entry that climbs out of wherever it would be unpacked.
    REQUIRE_THAT(ArchiveImport::check_package(make_zip(t, "slip.gcode.3mf", { { "Metadata/plate_1.gcode", "G28\n" },
                                                                              { "../../evil.dll", "MZ" } })).error,
                 Catch::Matchers::Contains("unsafe name"));
    // One plate of G-code and a picture: fine, and the picture is kept.
    const std::string png = std::string("\x89PNG\r\n\x1a\n", 8) + std::string(32, '\0');
    const ArchiveImport::Package ok = ArchiveImport::check_package(make_zip(t, "one.gcode.3mf", { { "Metadata/plate_3.gcode", "G28\n" },
                                                                                                 { "Metadata/plate_3.png", png } }));
    REQUIRE(ok.error.empty());
    REQUIRE(ok.plate == 3);
    REQUIRE(ok.thumbnail_png == png);
    // A missing file.
    REQUIRE_FALSE(ArchiveImport::check_package((t.dir / "nothing.gcode.3mf").string()).error.empty());
}

TEST_CASE("remote printing routes need the print permission", "[RemoteHub][remote-print][clients]")
{
    REQUIRE(HubClients::route_scope("POST", "/api/archive/upload") == HubClients::SCOPE_PRINT);
    REQUIRE(HubClients::route_scope("POST", "/api/instances/ensure") == HubClients::SCOPE_PRINT);
    REQUIRE_FALSE(HubClients::allows(HubClients::SCOPE_STATUS | HubClients::SCOPE_CAMERA, HubClients::route_scope("POST", "/api/archive/upload")));
}
