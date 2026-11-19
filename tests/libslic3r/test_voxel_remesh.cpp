#include <catch2/catch.hpp>

#include "libslic3r/MeshRepair.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;

static Vec3d box_size(const indexed_triangle_set &its)
{
    BoundingBoxf3 bb;
    for (const Vec3f &v : its.vertices)
        bb.merge(v.cast<double>());
    return bb.size();
}

// mesh_to_grid() drops every shell whose volume is below EPSILON. A mesh wound inside
// out has a negative volume, so all of it is dropped and the whole-mesh fallback
// builds the field. That fallback has to sample at the voxel size asked, like the
// shells do, or grid_to_mesh() (which divides by the stored voxel_scale) gives the
// part back shrunk by the voxel size.
TEST_CASE("Voxel remesh: an inside-out part keeps its size", "[MeshRemesh][OpenVDB]")
{
    indexed_triangle_set cube = its_make_cube(20., 20., 20.);
    REQUIRE(its_volume(cube) == Approx(8000.));

    indexed_triangle_set inside_out = cube;
    its_flip_triangles(inside_out);
    REQUIRE(its_volume(inside_out) < 0.);

    for (double voxel : {0.5, 0.25}) {
        INFO("voxel " << voxel);
        // The same cube wound outward, through the per-shell path, as the yardstick.
        const indexed_triangle_set ref = remesh_by_voxels(cube, voxel);
        REQUIRE_FALSE(ref.indices.empty());
        CHECK(box_size(ref).x() == Approx(20.).margin(voxel));

        const indexed_triangle_set out = remesh_by_voxels(inside_out, voxel);
        REQUIRE_FALSE(out.indices.empty());
        const Vec3d size = box_size(out);
        CHECK(size.x() == Approx(20.).margin(voxel));
        CHECK(size.y() == Approx(20.).margin(voxel));
        CHECK(size.z() == Approx(20.).margin(voxel));
        CHECK(std::abs(its_volume(out)) == Approx(std::abs(its_volume(ref))).epsilon(0.01));
    }
}
