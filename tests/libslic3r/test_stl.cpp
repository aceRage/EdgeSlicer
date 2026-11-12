#include <catch2/catch.hpp>

#include <algorithm>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/system/error_code.hpp>
#include <cstdint>
#include <string>

#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;

static inline std::string stl_path(const char* path)
{
	return std::string(TEST_DATA_DIR) + "/test_stl/" + path;
}

SCENARIO("Reading an STL file", "[stl]") {
	GIVEN("umlauts in the path of a binary STL file, Czech characters in the file name") {
        WHEN("STL file is read") {
			Slic3r::Model model;
			THEN("load should succeed") {
                REQUIRE(Slic3r::load_stl(stl_path("Geräte/20mmbox-čřšřěá.stl").c_str(), &model));
				REQUIRE(is_approx(model.objects.front()->volumes.front()->mesh().size(), Vec3d(20, 20, 20)));
            }
        }
    }
	GIVEN("in ASCII format") {
		WHEN("line endings LF") {
			Slic3r::Model model;
			THEN("load should succeed") {
				REQUIRE(Slic3r::load_stl(stl_path("ASCII/20mmbox-LF.stl").c_str(), &model));
				REQUIRE(is_approx(model.objects.front()->volumes.front()->mesh().size(), Vec3d(20, 20, 20)));
			}
		}
		WHEN("line endings CRLF") {
			Slic3r::Model model;
			THEN("load should succeed") {
				REQUIRE(Slic3r::load_stl(stl_path("ASCII/20mmbox-CRLF.stl").c_str(), &model));
				REQUIRE(is_approx(model.objects.front()->volumes.front()->mesh().size(), Vec3d(20, 20, 20)));
			}
		}
#if 0
		// ASCII STLs ending with just carriage returns are not supported. These were used by the old Macs, while the Unix based MacOS uses LFs as any other Unix.
		WHEN("line endings CR") {
			Slic3r::Model model;
			THEN("load should succeed") {
				REQUIRE(Slic3r::load_stl(stl_path("ASCII/20mmbox-CR.stl").c_str(), &model));
				REQUIRE(is_approx(model.objects.front()->volumes.front()->mesh().size(), Vec3d(20, 20, 20)));
			}
		}

#endif
		WHEN("nonstandard STL file (text after ending tags, invalid normals, for example infinities)") {
			Slic3r::Model model;
			THEN("load should succeed") {
				REQUIRE(Slic3r::load_stl(stl_path("ASCII/20mmbox-nonstandard.stl").c_str(), &model));
				REQUIRE(is_approx(model.objects.front()->volumes.front()->mesh().size(), Vec3d(20, 20, 20)));
			}
		}
	}
}

TEST_CASE("A binary STL whose facet bytes never exceed 127 is read as binary", "[stl]")
{
    const indexed_triangle_set cube = its_make_cube(10., 10., 10.);
    std::string                stl(80, '\0');
    const auto                 append = [&stl](const auto &value) {
        stl.append(reinterpret_cast<const char *>(&value), sizeof(value));
    };
    append(uint32_t(cube.indices.size()));
    const stl_normal zero_normal = stl_normal::Zero();
    for (const stl_triangle_vertex_indices &facet : cube.indices) {
        append(zero_normal);
        for (int i = 0; i < 3; ++i)
            append(cube.vertices[facet[i]]);
        append(uint16_t(0));
    }
    REQUIRE(std::none_of(stl.begin() + 84, stl.begin() + 84 + 128, [](unsigned char c) { return c > 127; }));

    const boost::filesystem::path path =
        boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("stl_lowbyte_%%%%.stl");
    {
        boost::nowide::ofstream ofs(path.string(), std::ios::binary);
        REQUIRE(ofs.good());
        ofs.write(stl.data(), std::streamsize(stl.size()));
        REQUIRE(ofs.good());
    }

    struct RemoveTemp
    {
        boost::filesystem::path p;
        ~RemoveTemp()
        {
            boost::system::error_code ec;
            boost::filesystem::remove(p, ec);
        }
    } guard{path};

    Model model;
    REQUIRE(load_stl(path.string().c_str(), &model));
    REQUIRE(is_approx(model.objects.front()->volumes.front()->mesh().size(), Vec3d(10, 10, 10)));
}

namespace {
// A folder of its own under the system temp folder, removed afterwards.
struct StlScratch
{
    boost::filesystem::path dir;
    StlScratch() : dir(boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("edge_stl_export_%%%%-%%%%"))
    {
        boost::filesystem::create_directories(dir);
    }
    ~StlScratch()
    {
        boost::system::error_code ec;
        boost::filesystem::remove_all(dir, ec);
    }
    std::string prefix() const { return (dir / "").string(); }
};
} // namespace

TEST_CASE("Export all objects as STLs names each file safely", "[stl][Regression]")
{
    StlScratch scratch;
    const std::string dir = scratch.prefix();

    // An object name with a path separator or a character Windows refuses used to name a file that
    // could not be written; the export said nothing.
    CHECK(stl_export_file_path(dir, "bracket / v2") == dir + "bracket _ v2.stl");
    CHECK(stl_export_file_path(dir, "a:b*c?d\"e<f>g|h\\i") == dir + "a_b_c_d_e_f_g_h_i.stl");
    CHECK(stl_export_file_path(dir, "  ") == dir + "object.stl");
    CHECK(stl_export_file_path(dir, "Cube") == dir + "Cube.stl");

    TriangleMesh mesh(its_make_cube(10., 10., 10.));
    const std::string first = stl_export_file_path(dir, "part/1");
    REQUIRE(store_stl(first.c_str(), &mesh, true));
    CHECK(boost::filesystem::exists(first));
    // A second object of the same name gets "(1)", as before.
    CHECK(stl_export_file_path(dir, "part/1") == dir + "part_1(1).stl");
}

TEST_CASE("store_stl reports a file it could not write and leaves nothing behind", "[stl][Regression]")
{
    StlScratch scratch;
    TriangleMesh mesh(its_make_cube(10., 10., 10.));

    SECTION("binary, into a folder that does not exist") {
        const boost::filesystem::path target = scratch.dir / "missing" / "cube.stl";
        CHECK_FALSE(store_stl(target.string().c_str(), &mesh, true));
        CHECK_FALSE(boost::filesystem::exists(target));
    }
    SECTION("ASCII, into a folder that does not exist") {
        const boost::filesystem::path target = scratch.dir / "missing" / "cube.stl";
        CHECK_FALSE(store_stl(target.string().c_str(), &mesh, false));
    }
    SECTION("a written file is complete and no temporary file is left") {
        const boost::filesystem::path target = scratch.dir / "cube.stl";
        REQUIRE(store_stl(target.string().c_str(), &mesh, true));
        CHECK(boost::filesystem::file_size(target) == 84 + 50 * mesh.facets_count());
        CHECK_FALSE(boost::filesystem::exists(scratch.dir / "cube.stl.tmp"));
        Model model;
        REQUIRE(load_stl(target.string().c_str(), &model));
        CHECK(model.objects.front()->volumes.front()->mesh().facets_count() == 12);
    }
    SECTION("a failed write keeps the file that was there") {
        const boost::filesystem::path target = scratch.dir / "cube.stl";
        REQUIRE(store_stl(target.string().c_str(), &mesh, true));
        const auto size_before = boost::filesystem::file_size(target);
        // The temporary file's name is taken by a folder, so the write fails.
        boost::filesystem::create_directory(scratch.dir / "cube.stl.tmp");
        TriangleMesh bigger(its_make_sphere(10., 0.3));
        CHECK_FALSE(store_stl(target.string().c_str(), &bigger, true));
        CHECK(boost::filesystem::file_size(target) == size_before);
    }
}
