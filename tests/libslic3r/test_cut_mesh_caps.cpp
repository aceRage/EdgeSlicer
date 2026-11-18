// cut_mesh() on planes that pass within its tolerance (1 micron) of vertices of the mesh. A vertex
// that close counted as on the plane to some of its tests and off it to others, so a facet next to
// it could go to the wrong half or to neither, and the cap was matched to a vertex the halves do not
// use: the halves came out with open edges (the Benchy below lost 310 mm3 of one side), and a plane a
// hair off a flat face gave a "half" that was that face alone.

#include <catch2/catch.hpp>

#include <cstdio>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

struct HalfStats
{
    size_t        open_edges = 0;
    size_t        faces      = 0;
    double        volume     = 0.;
    BoundingBoxf3 bbox;
};

HalfStats half_stats(const indexed_triangle_set& its)
{
    HalfStats s;
    s.open_edges = its_num_open_edges(its);
    s.faces      = its.indices.size();
    s.volume     = its_volume(its);
    if (!its.indices.empty())
        s.bbox = bounding_box(its);
    return s;
}

// Put the plane (point, normal) on z == 0 the way the cut tool does, then cut there.
void cut_by_plane(const indexed_triangle_set& src, const Vec3d& point, const Vec3d& normal, HalfStats& upper, HalfStats& lower)
{
    const Transform3d to_plane = Transform3d(Eigen::Quaterniond::FromTwoVectors(normal.normalized(), Vec3d::UnitZ())) *
                                 Geometry::translation_transform(-point);
    indexed_triangle_set its = src;
    its_transform(its, to_plane);
    indexed_triangle_set up, lo;
    cut_mesh(its, 0.f, &up, &lo);
    upper = half_stats(up);
    lower = half_stats(lo);
}

const indexed_triangle_set& benchy()
{
    static indexed_triangle_set cached;
    if (!cached.empty())
        return cached;
    const boost::filesystem::path path = boost::filesystem::path(TEST_DATA_DIR).parent_path().parent_path() / "resources" /
                                         "handy_models" / "3DBenchy.3mf";
    Model                     model;
    DynamicPrintConfig        config;
    ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::Disable};
    REQUIRE(load_3mf(path.string().c_str(), config, ctxt, &model, false));
    REQUIRE(!model.objects.empty());
    cached = model.objects.front()->volumes.front()->mesh().its;
    return cached;
}

} // namespace

// The Benchy lies along X here: its plane of symmetry is y == 0 of its bounding box. Planes a few
// tenths of a micron off it run along near-vertical edges of the hull.
TEST_CASE("cut_mesh: the Benchy cut next to its plane of symmetry gives closed halves", "[CutCaps]")
{
    const indexed_triangle_set& b = benchy();
    REQUIRE(its_num_open_edges(b) == 0);
    const Vec3d  c      = bounding_box(b).center();
    const double volume = its_volume(b);
    for (double dy : {-1e-4, 1e-4, 5e-4, 9e-4, 2e-3}) {
        DYNAMIC_SECTION("y = centre + " << dy)
        {
            HalfStats up, lo;
            cut_by_plane(b, c + Vec3d(0., dy, 0.), Vec3d::UnitY(), up, lo);
            CHECK(up.open_edges == 0);
            CHECK(lo.open_edges == 0);
            // Before the fix the lower half lost 310 mm3 at +5e-4 and +9e-4.
            CHECK_THAT(up.volume + lo.volume, WithinRel(volume, 1e-4));
        }
    }
}

TEST_CASE("cut_mesh: planes a hair off edges and rim vertices give closed halves", "[CutCaps]")
{
    SECTION("cube, plane through two edges moved 0.5 micron")
    {
        HalfStats up, lo;
        cut_by_plane(its_make_cube(20., 20., 20.), Vec3d(10. + 5e-4, 10., 10.), Vec3d(1., -1., 0.), up, lo);
        CHECK(up.open_edges == 0);
        CHECK(lo.open_edges == 0);
        CHECK_THAT(up.volume, WithinAbs(4000., 0.5));
        CHECK_THAT(lo.volume, WithinAbs(4000., 0.5));
    }
    SECTION("cylinder, plane through its axis moved 0.5 micron")
    {
        HalfStats up, lo;
        cut_by_plane(its_make_cylinder(10., 20., 2. * PI / 64.), Vec3d(5e-4, 0., 10.), Vec3d::UnitX(), up, lo);
        CHECK(up.open_edges == 0);
        CHECK(lo.open_edges == 0);
        CHECK_THAT(up.volume + lo.volume, WithinRel(double(its_volume(its_make_cylinder(10., 20., 2. * PI / 64.))), 1e-5));
    }
}

TEST_CASE("cut_mesh: a plane a hair off a flat face leaves nothing on the thin side", "[CutCaps]")
{
    for (double dz : {1e-5, 1e-4, 5e-4}) {
        DYNAMIC_SECTION("cube bottom + " << dz)
        {
            HalfStats up, lo;
            cut_by_plane(its_make_cube(20., 20., 20.), Vec3d(10., 10., dz), Vec3d::UnitZ(), up, lo);
            CHECK(up.open_edges == 0);
            CHECK_THAT(up.volume, WithinAbs(8000., 0.5));
            // Before the fix: the bottom face alone, 2 facets and 4 open edges.
            CHECK(lo.faces == 0);
        }
        DYNAMIC_SECTION("cylinder bottom + " << dz)
        {
            HalfStats up, lo;
            cut_by_plane(its_make_cylinder(10., 20., 2. * PI / 64.), Vec3d(0., 0., dz), Vec3d::UnitZ(), up, lo);
            CHECK(up.open_edges == 0);
            CHECK(lo.faces == 0);
        }
    }
}

// A table of open edges, volume and bounds of both halves for a set of planes through a cube, a
// cylinder and the Benchy; run it by its tag to compare two builds.
TEST_CASE("cut cap survey", "[.CutCapSurvey]")
{
    struct Plane { std::string name; Vec3d point; Vec3d normal; };
    struct Subject { std::string name; indexed_triangle_set its; std::vector<Plane> planes; };
    std::vector<Subject> subjects;

    {
        Subject s{"cube20", its_make_cube(20., 20., 20.), {}};
        s.planes = {
            {"z=10", {10, 10, 10}, {0, 0, 1}},
            {"z=1e-4", {10, 10, 1e-4}, {0, 0, 1}},
            {"z=1e-5", {10, 10, 1e-5}, {0, 0, 1}},
            {"z=20-1e-4", {10, 10, 20 - 1e-4}, {0, 0, 1}},
            {"diag x=y (edges)", {10, 10, 10}, {1, -1, 0}},
            {"diag x+z (edges)", {10, 10, 10}, {1, 0, 1}},
            {"x+y+z=20 (3 verts)", {20, 0, 0}, {1, 1, 1}},
            {"x+y+z=30", {10, 10, 10}, {1, 1, 1}},
            {"x+y+z=20+1e-4", {20 + 1e-4, 0, 0}, {1, 1, 1}},
            {"tilt 1deg", {10, 10, 10}, {0.01745, 0, 1}},
            {"x=10", {10, 10, 10}, {1, 0, 0}},
            {"z=0 (face)", {10, 10, 0}, {0, 0, 1}},
            {"z=5e-4", {10, 10, 5e-4}, {0, 0, 1}},
            {"z=2e-3", {10, 10, 2e-3}, {0, 0, 1}},
            {"x=y+5e-4 (near edges)", {10 + 5e-4, 10, 10}, {1, -1, 0}},
        };
        subjects.emplace_back(std::move(s));
    }
    {
        indexed_triangle_set cyl = its_make_cylinder(10., 20., 2. * PI / 64.);
        Subject s{"cyl r10 h20 64", std::move(cyl), {}};
        s.planes = {
            {"z=10", {0, 0, 10}, {0, 0, 1}},
            {"x=0 (verts, axis)", {0, 0, 10}, {1, 0, 0}},
            {"y=0 (verts, axis)", {0, 0, 10}, {0, 1, 0}},
            {"x=1e-4", {1e-4, 0, 10}, {1, 0, 0}},
            {"x=1e-5", {1e-5, 0, 10}, {1, 0, 0}},
            {"tilt 45 through rim", {10, 0, 20}, {1, 0, 1}},
            {"tilt 30", {0, 0, 10}, {0.5, 0, 0.866}},
            {"z=1e-4", {0, 0, 1e-4}, {0, 0, 1}},
            {"x=5", {5, 0, 10}, {1, 0, 0}},
            {"x=5e-4", {5e-4, 0, 10}, {1, 0, 0}},
            {"x=9.9518471 (rim verts)", {9.9518471, 0, 10}, {1, 0, 0}},
            {"z=20 (face)", {0, 0, 20}, {0, 0, 1}},
        };
        subjects.emplace_back(std::move(s));
    }
    {
        indexed_triangle_set b = benchy();
        BoundingBoxf3        bb = bounding_box(b);
        Vec3d                c  = bb.center();
        std::printf("benchy bbox min %g %g %g max %g %g %g open %zu\n", bb.min.x(), bb.min.y(), bb.min.z(), bb.max.x(), bb.max.y(),
                    bb.max.z(), its_num_open_edges(b));
        Subject s{"benchy", std::move(b), {}};
        s.planes = {
            {"y=c (symmetry?)", c, {0, 1, 0}},
            {"x=c", c, {1, 0, 0}},
            {"z=c", c, {0, 0, 1}},
            {"z=10", {c.x(), c.y(), bb.min.z() + 10}, {0, 0, 1}},
            {"z=0.2", {c.x(), c.y(), bb.min.z() + 0.2}, {0, 0, 1}},
            {"tilt", c, {0.3, 0.2, 1}},
            {"y=c+1e-4", c + Vec3d(0, 1e-4, 0), {0, 1, 0}},
            {"x=c+1e-4", c + Vec3d(1e-4, 0, 0), {1, 0, 0}},
            {"y=c-1e-4", c - Vec3d(0, 1e-4, 0), {0, 1, 0}},
            {"y=c+5e-4", c + Vec3d(0, 5e-4, 0), {0, 1, 0}},
            {"y=c+9e-4", c + Vec3d(0, 9e-4, 0), {0, 1, 0}},
            {"y=c+2e-3", c + Vec3d(0, 2e-3, 0), {0, 1, 0}},
            {"y=c+0.01", c + Vec3d(0, 0.01, 0), {0, 1, 0}},
            {"y=c+0.5", c + Vec3d(0, 0.5, 0), {0, 1, 0}},
            {"y=c+3", c + Vec3d(0, 3, 0), {0, 1, 0}},
            {"z=c+1e-4", c + Vec3d(0, 0, 1e-4), {0, 0, 1}},
            {"x=c+5e-4", c + Vec3d(5e-4, 0, 0), {1, 0, 0}},
            {"tilt2", c + Vec3d(1, -2, 3), {-0.2, 0.5, 1}},
            {"tilt3", c + Vec3d(-5, 0, -4), {0.7, 0, 0.7}},
        };
        subjects.emplace_back(std::move(s));
    }

    for (const Subject& s : subjects)
        for (const Plane& p : s.planes) {
            HalfStats up, lo;
            cut_by_plane(s.its, p.point, p.normal, up, lo);
            auto row = [&](const char* side, const HalfStats& h) {
                std::printf("%-16s | %-22s | %s | open %4zu | faces %6zu | vol %14.6f | bb %10.5f %10.5f %10.5f %10.5f %10.5f %10.5f\n",
                            s.name.c_str(), p.name.c_str(), side, h.open_edges, h.faces, h.volume, h.bbox.min.x(), h.bbox.min.y(),
                            h.bbox.min.z(), h.bbox.max.x(), h.bbox.max.y(), h.bbox.max.z());
            };
            row("U", up);
            row("L", lo);
        }
}
