// ImageMap Phase 5d: a textured OBJ (mtl map_Kd) is painted from its texture, the way a GLB is.
//
// Spec: docs/superpowers/specs/2026-10-16-imagemap-phase5-completion.md, criterion K.
#include <catch2/catch.hpp>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <array>
#include <set>
#include <string>
#include <vector>

#include "libslic3r/Format/OBJ.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleSelector.hpp"

using namespace Slic3r;

namespace {

// A 20 mm cube whose every face maps the WHOLE texture (0..1 in u and v), one material.
const char *kCubeObj =
    "mtllib cube.mtl\n"
    "v 0 0 0\nv 20 0 0\nv 20 20 0\nv 0 20 0\n"
    "v 0 0 20\nv 20 0 20\nv 20 20 20\nv 0 20 20\n"
    "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
    "usemtl tex\n"
    // Every other face starts its UVs one corner on, so its two triangles' centroids land on the
    // texture's other diagonal: across the part all four quadrants are some triangle's centroid
    // colour, and the colour dialog is offered all four.
    "f 1/1 4/4 3/3 2/2\n"   // bottom
    "f 5/2 6/3 7/4 8/1\n"   // top
    "f 1/1 2/2 6/3 5/4\n"   // -Y
    "f 2/2 3/3 7/4 6/1\n"   // +X
    "f 3/1 4/2 8/3 7/4\n"   // +Y
    "f 4/2 1/3 5/4 8/1\n";  // -X

void write_text(const boost::filesystem::path &p, const std::string &text)
{
    boost::nowide::ofstream out(p.string(), std::ios::binary);
    out << text;
}

// What the last "colour dialog" call was handed and answered, for the failure messages.
std::vector<RGBA>          g_seen_colors;
std::vector<unsigned char> g_answered_ids;

// Four filaments - red, green, blue, white - and a "colour dialog" that picks the nearest.
ObjImportColorFn nearest_of_four()
{
    return [](std::vector<RGBA> &colors, bool, std::vector<unsigned char> &ids, unsigned char &first) {
        const std::array<std::array<float, 3>, 4> f = {{{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {1.f, 1.f, 1.f}}};
        ids.clear();
        for (const RGBA &c : colors) {
            size_t best = 0;
            float  bd   = 1e9f;
            for (size_t i = 0; i < f.size(); ++i) {
                const float d = (c[0] - f[i][0]) * (c[0] - f[i][0]) + (c[1] - f[i][1]) * (c[1] - f[i][1]) +
                                (c[2] - f[i][2]) * (c[2] - f[i][2]);
                if (d < bd) { bd = d; best = i; }
            }
            ids.push_back((unsigned char) (best + 1));
        }
        g_seen_colors  = colors;
        g_answered_ids = ids;
        first = 1;
    };
}

boost::filesystem::path make_case_dir(const char *name, const char *map_kd)
{
    const boost::filesystem::path dir = boost::filesystem::temp_directory_path() / "snorca_tests" / name;
    boost::filesystem::create_directories(dir);
    write_text(dir / "cube.obj", kCubeObj);
    write_text(dir / "cube.mtl", std::string("newmtl tex\nKd 0.8 0.8 0.8\nmap_Kd ") + map_kd + "\n");
    boost::filesystem::remove(dir / "quad.png");
    boost::filesystem::copy_file(boost::filesystem::path(TEST_DATA_DIR) / "image_fill" / "quad_rgbw.png", dir / "quad.png");
    return dir;
}

// How many filaments the part's painting actually uses, read back through TriangleSelector.
size_t painted_states(const ModelVolume &v)
{
    TriangleSelector sel(v.mesh());
    sel.deserialize(v.mmu_segmentation_facets.get_data(), false);
    size_t n = 0;
    for (int s = 1; s <= 4; ++s)
        if (!sel.get_facets(EnforcerBlockerType(s)).indices.empty())
            ++n;
    return n;
}

} // namespace

TEST_CASE("OBJ 5d: a map_Kd texture paints the part from the picture", "[obj][imagefill][p5d]")
{
    const boost::filesystem::path dir = make_case_dir("obj_tex_p5d", "quad.png");
    std::string warning;
    Model model = Model::read_from_file((dir / "cube.obj").string(), nullptr, nullptr, LoadStrategy::AddDefaultInstances,
                                        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0,
                                        nearest_of_four(), &warning);
    boost::filesystem::remove_all(dir);
    REQUIRE(model.objects.size() == 1);
    REQUIRE(model.objects[0]->volumes.size() == 1);
    const ModelVolume &v = *model.objects[0]->volumes[0];
    std::set<int> answered(g_answered_ids.begin(), g_answered_ids.end());
    INFO("warning: " << warning << " dialog saw " << g_seen_colors.size() << " colours, answered " << answered.size()
                     << " distinct filaments");
    CHECK(warning.empty());
    CHECK(v.is_mm_painted());
    // The four quadrants land on every face, at sub-facet resolution: more than the one or two
    // colours a per-triangle (centroid) sample of a two-triangle face could give.
    CHECK(painted_states(v) >= 3);
}

TEST_CASE("OBJ 5d: a texture that cannot be read keeps the material colour, with a warning", "[obj][imagefill][p5d]")
{
    const boost::filesystem::path dir = make_case_dir("obj_tex_p5d_missing", "missing.png");
    std::string warning;
    Model model = Model::read_from_file((dir / "cube.obj").string(), nullptr, nullptr, LoadStrategy::AddDefaultInstances,
                                        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0,
                                        nearest_of_four(), &warning);
    boost::filesystem::remove_all(dir);
    REQUIRE(model.objects.size() == 1);
    CHECK_FALSE(warning.empty());
    CHECK(painted_states(*model.objects[0]->volumes[0]) <= 1);
}

TEST_CASE("OBJ 5d: a texture path outside the OBJ's folder is not read", "[obj][imagefill][p5d]")
{
    const boost::filesystem::path dir = make_case_dir("obj_tex_p5d_escape", "../quad.png");
    std::string warning;
    Model model = Model::read_from_file((dir / "cube.obj").string(), nullptr, nullptr, LoadStrategy::AddDefaultInstances,
                                        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0,
                                        nearest_of_four(), &warning);
    boost::filesystem::remove_all(dir);
    REQUIRE(model.objects.size() == 1);
    CHECK_FALSE(warning.empty());
}
