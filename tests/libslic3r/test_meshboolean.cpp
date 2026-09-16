#include <catch2/catch.hpp>
#include <test_utils.hpp>

#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/MeshBoolean.hpp>

#include <map>
#include <vector>

using namespace Slic3r;

TEST_CASE("CGAL and TriangleMesh conversions", "[MeshBoolean]") {
    TriangleMesh sphere = make_sphere(1.);
    
    auto cgalmesh_ptr = MeshBoolean::cgal::triangle_mesh_to_cgal(sphere);
    
    REQUIRE(cgalmesh_ptr);
    REQUIRE(! MeshBoolean::cgal::does_self_intersect(*cgalmesh_ptr));
    
    TriangleMesh M = MeshBoolean::cgal::cgal_to_triangle_mesh(*cgalmesh_ptr);
    
    REQUIRE(M.its.vertices.size() == sphere.its.vertices.size());
    REQUIRE(M.its.indices.size() == sphere.its.indices.size());
    
    REQUIRE(M.volume() == Approx(sphere.volume()));
    
    REQUIRE(! MeshBoolean::cgal::does_self_intersect(M));
}

// ----------------------------------------------------------------------------
// Ultra: the Mesh Boolean gizmo's Union produced correct geometry with INVERTED
// normals, while the right-click Mesh boolean on the same parts was fine.
//
// Cause: Manifold validates topology, not orientation. A closed mesh wound the wrong
// way is a valid 2-manifold to it, so it is accepted, carried through the boolean
// unchanged and returned inside-out. The gizmo fed it such a mesh because it
// transformed each volume with TriangleMesh::transform(trafo) - fix_left_handed
// defaulting to FALSE - so a mirrored (negative-determinant) part arrived inverted.
// The right-click path passes true and so never did.
//
// mfd::make_boolean() now normalises both inputs (and re-checks its output) to
// outward-facing, so both paths agree regardless of what the caller hands it.
// ----------------------------------------------------------------------------

// Signed volume in double. Positive = outward-facing normals. Deliberately computed
// here rather than via its_volume() so the assertion does not share an implementation
// with the code under test.
static double signed_volume(const indexed_triangle_set &its)
{
    double v = 0.;
    for (const Vec3i32 &f : its.indices) {
        const Vec3d a = its.vertices[f(0)].cast<double>();
        const Vec3d b = its.vertices[f(1)].cast<double>();
        const Vec3d c = its.vertices[f(2)].cast<double>();
        v += a.dot(b.cross(c));
    }
    return v / 6.;
}

// Every undirected edge used by exactly two faces.
static bool is_closed(const indexed_triangle_set &its)
{
    std::map<std::pair<int, int>, int> uses;
    for (const Vec3i32 &f : its.indices)
        for (int e = 0; e < 3; ++e) {
            int a = f(e), b = f((e + 1) % 3);
            ++uses[{std::min(a, b), std::max(a, b)}];
        }
    for (const auto &kv : uses)
        if (kv.second != 2)
            return false;
    return !its.indices.empty();
}

TEST_CASE("Mesh boolean: mfd union of two overlapping cubes faces outward", "[MeshBoolean]")
{
    // Two 10 mm cubes overlapping by 5 mm on X. its_make_cube() builds from the
    // origin, so translating the second by 5 gives a 15 x 10 x 10 union.
    TriangleMesh a{its_make_cube(10., 10., 10.)};
    TriangleMesh b{its_make_cube(10., 10., 10.)};
    b.translate(5.f, 0.f, 0.f);

    std::vector<TriangleMesh> out;
    REQUIRE(MeshBoolean::mfd::make_boolean(a, b, out, "UNION"));
    REQUIRE(out.size() == 1);

    const indexed_triangle_set &r = out.front().its;
    REQUIRE(!r.indices.empty());
    // The regression: this was NEGATIVE before the fix - correct geometry, inverted
    // normals, which renders inside-out.
    CHECK(signed_volume(r) > 0.);
    CHECK(is_closed(r));
    // And it really is the union, not one of the operands.
    CHECK(signed_volume(r) == Approx(1500.).epsilon(0.02));
}

TEST_CASE("Mesh boolean: mfd union with a mirrored operand faces outward", "[MeshBoolean]")
{
    // The other half of the bug: a MIRRORED volume. The gizmo transforms by the
    // volume matrix, and a negative-determinant matrix inverts the winding. Feeding
    // that mesh in unmirrored is what the gizmo did.
    TriangleMesh a{its_make_cube(10., 10., 10.)};

    TriangleMesh b{its_make_cube(10., 10., 10.)};
    // Mirror in X about x = 5, then shift so it still overlaps `a`: determinant -1.
    Transform3d m = Transform3d::Identity();
    m.scale(Vec3d(-1., 1., 1.));
    m.pretranslate(Vec3d(15., 0., 0.));
    REQUIRE(m.matrix().block(0, 0, 3, 3).determinant() < 0.);
    // fix_left_handed deliberately FALSE - this reproduces exactly what the gizmo
    // handed the backend. The backend must cope on its own.
    b.transform(m, false);
    REQUIRE(signed_volume(b.its) < 0.);   // the operand really is inside-out

    std::vector<TriangleMesh> out;
    REQUIRE(MeshBoolean::mfd::make_boolean(a, b, out, "UNION"));
    REQUIRE(out.size() == 1);

    const indexed_triangle_set &r = out.front().its;
    REQUIRE(!r.indices.empty());
    CHECK(signed_volume(r) > 0.);
    CHECK(is_closed(r));
    // Mirrored cube spans x in [5, 15], so the union is again 15 x 10 x 10.
    CHECK(signed_volume(r) == Approx(1500.).epsilon(0.02));
}
