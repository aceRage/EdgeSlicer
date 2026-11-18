#include <catch2/catch.hpp>
#include <test_utils.hpp>

#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/MeshBoolean.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/CSGMesh/ModelToCSGMesh.hpp>
#include <libslic3r/CSGMesh/PerformCSGMeshBooleans.hpp>

#include <cstddef>
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

// ----------------------------------------------------------------------------
// Mesh Boolean gizmo: the result part jumped (up, down, left, right) after Union /
// Difference / Intersection.
//
// The gizmo booleans the two parts in OBJECT coordinates (each mesh transformed by its
// own ModelVolume::get_matrix()), then hands the result to
// ModelObject::replace_volume_with_object_mesh(). add_volume() centres that mesh and
// gives the volume the matching offset, which is correct - but the gizmo then
// overwrote the offset with the SOURCE part's offset, moving the result by
// (source offset - result bbox centre). That vector depends on where the tool part
// is relative to the source, hence "no obvious pattern".
//
// These tests run the gizmo's exact sequence and require the result to sit where
// the boolean geometry is, and every other part not to move.
// ----------------------------------------------------------------------------

namespace {

Transform3d make_trafo(const Vec3d &offset, const Vec3d &axis, double angle_deg, const Vec3d &scale)
{
    Transform3d t = Transform3d::Identity();
    t.translate(offset);
    t.rotate(Eigen::AngleAxisd(angle_deg * PI / 180., axis.normalized()));
    t.scale(scale);
    return t;
}

BoundingBoxf3 world_bbox(const ModelObject &mo, const ModelVolume &mv)
{
    return mv.mesh().transformed_bounding_box(mo.instances.front()->get_matrix() * mv.get_matrix());
}

void check_same_box(const BoundingBoxf3 &got, const BoundingBoxf3 &expected, double tol = 1e-3)
{
    INFO("got min " << got.min.transpose() << " max " << got.max.transpose());
    INFO("expected min " << expected.min.transpose() << " max " << expected.max.transpose());
    CHECK((got.min - expected.min).norm() < tol);
    CHECK((got.max - expected.max).norm() < tol);
}

// Object with a rotated + scaled + offset instance, and up to three parts:
//   [0] A - rotated, non-uniformly scaled (optionally mirrored), off-centre
//   [1] B - overlaps A, rotated differently
//   [2] C - well away from both, never touched (only when with_bystander)
ModelObject *make_boolean_scene(Model &model, bool mirror_a, bool with_bystander)
{
    ModelObject *mo = model.add_object();
    mo->name = "boolean scene";
    ModelInstance *inst = mo->add_instance();
    inst->set_transformation(Geometry::Transformation(
        make_trafo(Vec3d(92., 117., 6.), Vec3d(0., 0., 1.), 25., Vec3d(1.1, 1.1, 1.1))));

    ModelVolume *a = mo->add_volume(TriangleMesh(its_make_cube(20., 20., 20.)));
    a->name = "A";
    a->set_transformation(Geometry::Transformation(
        make_trafo(Vec3d(-12., 5., 9.), Vec3d(1., 0., 2.), 35., Vec3d(mirror_a ? -1.2 : 1.2, 0.8, 1.0))));

    ModelVolume *b = mo->add_volume(TriangleMesh(its_make_cube(15., 15., 15.)));
    b->name = "B";
    b->set_transformation(Geometry::Transformation(
        make_trafo(Vec3d(-1., 11., 16.), Vec3d(0., 1., 0.), 15., Vec3d(1., 1., 1.))));

    if (with_bystander) {
        ModelVolume *c = mo->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
        c->name = "C";
        c->set_transformation(Geometry::Transformation(
            make_trafo(Vec3d(45., -30., 0.), Vec3d(0., 0., 1.), 10., Vec3d(1., 1., 1.))));
    }
    return mo;
}

// The gizmo's apply path, minus the GUI: boolean in object coordinates, replace the
// source volume, optionally delete the tool. Returns the new volume and fills
// `expected` with the world bbox of the boolean geometry.
ModelVolume *run_gizmo_boolean(ModelObject *mo, size_t src_idx, size_t tool_idx, const std::string &op,
                               const std::string &suffix, bool delete_tool, BoundingBoxf3 &expected, double &expected_volume)
{
    TriangleMesh src = mo->volumes[src_idx]->mesh();
    src.transform(mo->volumes[src_idx]->get_matrix(), true);
    TriangleMesh tool = mo->volumes[tool_idx]->mesh();
    tool.transform(mo->volumes[tool_idx]->get_matrix(), true);

    std::vector<TriangleMesh> out;
    REQUIRE(MeshBoolean::mfd::make_boolean(src, tool, out, op));
    REQUIRE(!out.empty());
    REQUIRE(!out.front().empty());

    expected        = out.front().transformed_bounding_box(mo->instances.front()->get_matrix());
    expected_volume = std::abs(out.front().volume());

    ModelVolume *nv = mo->replace_volume_with_object_mesh(src_idx, TriangleMesh(out.front()), suffix);
    if (delete_tool)
        mo->delete_volume(tool_idx);
    return nv;
}

} // namespace

TEST_CASE("Mesh Boolean gizmo: the result stays where the boolean geometry is", "[MeshBoolean]")
{
    struct Case { const char *op; const char *suffix; bool delete_tool; };
    const Case cases[] = {
        {"UNION",        "union",        true },
        {"A_NOT_B",      "difference",   false},
        {"A_NOT_B",      "difference",   true },
        {"INTERSECTION", "intersection", false},
        {"INTERSECTION", "intersection", true },
    };

    for (bool mirror_a : {false, true})
        for (const Case &c : cases) {
            DYNAMIC_SECTION(c.op << (c.delete_tool ? " delete tool" : " keep tool") << (mirror_a ? " mirrored source" : "")) {
                Model        model;
                ModelObject *mo = make_boolean_scene(model, mirror_a, true);
                const BoundingBoxf3 tool_before      = world_bbox(*mo, *mo->volumes[1]);
                const BoundingBoxf3 bystander_before = world_bbox(*mo, *mo->volumes[2]);

                BoundingBoxf3 expected;
                double        expected_volume = 0.;
                ModelVolume  *nv = run_gizmo_boolean(mo, 0, 1, c.op, c.suffix, c.delete_tool, expected, expected_volume);

                // The result replaced the source in slot 0.
                REQUIRE(mo->volumes.front() == nv);
                CHECK(nv->name == std::string("A - ") + c.suffix);
                // Stored centred, like every other volume.
                CHECK(nv->mesh().bounding_box().center().norm() < 1e-3);
                CHECK(std::abs(its_volume(nv->mesh().its)) * std::abs(nv->get_matrix().matrix().block<3, 3>(0, 0).determinant()) ==
                      Approx(expected_volume).epsilon(1e-4));

                // The regression: the result moved by (source offset - result centre).
                check_same_box(world_bbox(*mo, *nv), expected);

                // Untouched parts do not move.
                for (const ModelVolume *v : mo->volumes) {
                    if (v->name == "C")
                        check_same_box(world_bbox(*mo, *v), bystander_before);
                    else if (v->name == "B")
                        check_same_box(world_bbox(*mo, *v), tool_before);
                }
                CHECK(mo->volumes.size() == (c.delete_tool ? 2u : 3u));
            }
        }
}

TEST_CASE("Mesh Boolean gizmo: union of a two-part object, the result is the only part left", "[MeshBoolean]")
{
    // Deleting the tool leaves one volume, and ModelObject::delete_volume() then folds
    // the volume transform into the instance. The world position must survive that too.
    for (bool mirror_a : {false, true}) {
        Model        model;
        ModelObject *mo = make_boolean_scene(model, mirror_a, false);
        BoundingBoxf3 expected;
        double        expected_volume = 0.;
        ModelVolume  *nv = run_gizmo_boolean(mo, 0, 1, "UNION", "union", true, expected, expected_volume);
        REQUIRE(mo->volumes.size() == 1);
        REQUIRE(mo->volumes.front() == nv);
        check_same_box(world_bbox(*mo, *nv), expected);
    }
}

TEST_CASE("Mesh Boolean gizmo: the second part as the source", "[MeshBoolean]")
{
    // Source in a later slot than the tool: the result takes the source's slot and the
    // other parts keep theirs.
    Model        model;
    ModelObject *mo = make_boolean_scene(model, false, true);
    const BoundingBoxf3 a_before = world_bbox(*mo, *mo->volumes[0]);
    const BoundingBoxf3 c_before = world_bbox(*mo, *mo->volumes[2]);
    BoundingBoxf3 expected;
    double        expected_volume = 0.;
    ModelVolume  *nv = run_gizmo_boolean(mo, 1, 0, "A_NOT_B", "difference", false, expected, expected_volume);
    REQUIRE(mo->volumes[1] == nv);
    CHECK(mo->volumes[0]->name == "A");
    CHECK(mo->volumes[2]->name == "C");
    check_same_box(world_bbox(*mo, *nv), expected);
    check_same_box(world_bbox(*mo, *mo->volumes[0]), a_before);
    check_same_box(world_bbox(*mo, *mo->volumes[2]), c_before);
}

TEST_CASE("CGAL volume checks ignore unreferenced vertices", "[MeshBoolean][Regression]") {
    indexed_triangle_set mesh = its_make_cube(10., 10., 10.);
    // Leave isolated vertices before, between, and after the vertices used by faces.
    mesh.vertices.insert(mesh.vertices.begin(), Vec3f(-1.f, -1.f, -1.f));
    mesh.vertices.insert(mesh.vertices.begin() + 4, Vec3f(5.f, 5.f, 5.f));
    mesh.vertices.emplace_back(20.f, 20.f, 20.f);
    for (Vec3i32 &face : mesh.indices)
        for (int i = 0; i < 3; ++i)
            face[i] += face[i] >= 3 ? 2 : 1;

    auto cgal_mesh = MeshBoolean::cgal::triangle_mesh_to_cgal(mesh);
    REQUIRE(cgal_mesh);
    REQUIRE(MeshBoolean::cgal::does_bound_a_volume(*cgal_mesh));
    REQUIRE_FALSE(MeshBoolean::cgal::does_self_intersect(*cgal_mesh));

    TriangleMesh result = MeshBoolean::cgal::cgal_to_triangle_mesh(*cgal_mesh);
    REQUIRE(result.its.indices.size() == mesh.indices.size());
    REQUIRE(result.its.vertices.size() == mesh.vertices.size() - 3);
    for (const Vec3i32 &face : result.its.indices)
        for (int i = 0; i < 3; ++i) {
            REQUIRE(face[i] >= 0);
            REQUIRE(static_cast<std::size_t>(face[i]) < result.its.vertices.size());
        }
    REQUIRE(result.volume() == Approx(1000.).epsilon(1e-6));
}

TEST_CASE("CGAL volume checks reject open meshes with unreferenced vertices", "[MeshBoolean][Regression]") {
    indexed_triangle_set mesh = its_make_cube(10., 10., 10.);
    mesh.indices.pop_back();
    mesh.vertices.emplace_back(20.f, 20.f, 20.f);

    auto cgal_mesh = MeshBoolean::cgal::triangle_mesh_to_cgal(mesh);
    REQUIRE(cgal_mesh);
    REQUIRE_FALSE(MeshBoolean::cgal::does_bound_a_volume(*cgal_mesh));
}

TEST_CASE("mcut difference handles source splits between cuts", "[MeshBoolean]")
{
    // Orca #16275: a multi-component tool (slab + two holes) used to skip cuts after
    // the first successful split. This is the export-with-negative-parts path
    // (Plater::combine_mesh_fff → perform_csgmesh_booleans_mcut).
    TriangleMesh body{its_make_cube(30., 10., 10.)};

    TriangleMesh slab{its_make_cube(2., 12., 20.)};
    slab.translate(14.f, -1.f, -5.f);
    TriangleMesh hole_a{its_make_cube(4., 4., 20.)};
    hole_a.translate(3.f, 3.f, -5.f);
    TriangleMesh hole_b{its_make_cube(4., 4., 20.)};
    hole_b.translate(21.f, 3.f, -5.f);
    indexed_triangle_set tool_its = slab.its;
    its_merge(tool_its, hole_a.its);
    its_merge(tool_its, hole_b.its);
    TriangleMesh tool{tool_its};

    std::vector<TriangleMesh> result;
    MeshBoolean::mcut::make_boolean(body, tool, result, "A_NOT_B");
    REQUIRE(result.size() == 1);
    REQUIRE(its_split(result.front().its).size() == 2);
    CHECK(result.front().volume() == Approx(2480.).epsilon(1e-3));

    std::vector<TriangleMesh> mfd_result;
    REQUIRE(MeshBoolean::mfd::make_boolean(body, tool, mfd_result, "A_NOT_B"));
    REQUIRE_FALSE(mfd_result.empty());
    CHECK(signed_volume(mfd_result.front().its) == Approx(2480.).epsilon(0.02));
}

// ----------------------------------------------------------------------------
// Export with negative parts (Plater::export_stl -> model_to_csgmesh with
// mpartsDoSplits -> check_csgmesh_booleans -> CGAL).
// ----------------------------------------------------------------------------

// A 20 mm cube with a 10 mm cubic void in its middle: two shells in one mesh, the
// inner one wound inwards (its normals point into the void), as a hollow part is.
static indexed_triangle_set hollow_cube_its()
{
    indexed_triangle_set outer = its_make_cube(20., 20., 20.);
    indexed_triangle_set inner = its_make_cube(10., 10., 10.);
    its_translate(inner, Vec3f(5.f, 5.f, 5.f));
    its_flip_triangles(inner);
    its_merge(outer, inner);
    return outer;
}

TEST_CASE("Export booleans: a hollow part with a negative part is cut", "[MeshBoolean][CSGMesh][Regression]")
{
    Model        model;
    ModelObject *object = model.add_object();
    ModelVolume *hollow = object->add_volume(TriangleMesh(hollow_cube_its()), ModelVolumeType::MODEL_PART, false);
    hollow->name        = "hollow";
    // A 4 x 4 bar through the whole part, crossing the void.
    indexed_triangle_set bar = its_make_cube(4., 4., 30.);
    its_translate(bar, Vec3f(8.f, 8.f, -5.f));
    ModelVolume *negative = object->add_volume(TriangleMesh(bar), ModelVolumeType::NEGATIVE_VOLUME, false);
    negative->name        = "bar";
    object->add_instance();

    std::vector<csg::CSGPart> parts;
    csg::model_to_csgmesh(*object, Transform3d::Identity(), std::back_inserter(parts),
                          csg::mpartsPositive | csg::mpartsNegative | csg::mpartsDoSplits);
    // Each split piece carries its part's name, so a refusal can name it.
    for (const csg::CSGPart &part : parts)
        if (csg::get_mesh(part))
            CHECK((part.name == "hollow" || part.name == "bar"));
    auto csgrange = range(parts);
    const auto status = csg::check_csgmesh_booleans(csgrange);
    INFO("fail reason " << int(std::get<0>(status)) << " part \"" << std::get<1>(status) << "\"");
    REQUIRE(std::get<0>(status) == csg::BooleanFailReason::OK);
    REQUIRE(std::get<2>(status) == csgrange.end());

    auto         cgalm  = csg::perform_csgmesh_booleans(csgrange);
    TriangleMesh result = MeshBoolean::cgal::cgal_to_triangle_mesh(*cgalm);
    // 20^3 - 10^3 (the void) - 4 x 4 x 10 (the bar where it crosses solid).
    CHECK(result.volume() == Approx(8000. - 1000. - 160.).epsilon(1e-4));
}

TEST_CASE("Export booleans: the refused part is named in full and always the same one", "[MeshBoolean][CSGMesh][Regression]")
{
    // Many parts the check refuses, each with its own long name. The check runs them in
    // parallel; the reported name must be one part's whole name, the same every time.
    std::vector<indexed_triangle_set> empties(64);
    for (int run = 0; run < 100; ++run) {
        std::vector<csg::CSGPart> parts;
        TriangleMesh              cube{its_make_cube(10., 10., 10.)};
        parts.emplace_back(&cube.its, csg::CSGType::Union);
        parts.back().name = "body";
        for (size_t i = 0; i < empties.size(); ++i) {
            parts.emplace_back(&empties[i], csg::CSGType::Difference);
            parts.back().name = "negative part number " + std::to_string(i) + std::string(40, char('a' + i % 26));
        }
        const auto status = csg::check_csgmesh_booleans(range(parts));
        REQUIRE(std::get<0>(status) == csg::BooleanFailReason::MeshEmpty);
        REQUIRE(std::get<1>(status) == parts[1].name);
    }
}


TEST_CASE("Mesh boolean: an intersection of parts that do not meet is empty, not a failure", "[MeshBoolean][Regression]")
{
    // The Mesh Boolean gizmo said "Unable to perform boolean operation on selected parts" for an
    // intersection of two parts that do not touch: the boolean worked, there is just nothing left.
    TriangleMesh a{its_make_cube(10., 10., 10.)};
    TriangleMesh b{its_make_cube(10., 10., 10.)};
    b.translate(30.f, 0.f, 0.f);

    std::vector<TriangleMesh> dst;
    CHECK(MeshBoolean::make_boolean_with_fallback(a, b, "INTERSECTION", dst) == MeshBoolean::BooleanOutcome::Empty);
    CHECK(dst.empty());
    // A difference whose tool covers the whole source is empty too.
    TriangleMesh big{its_make_cube(30., 30., 30.)};
    big.translate(-10.f, -10.f, -10.f);
    CHECK(MeshBoolean::make_boolean_with_fallback(a, big, "A_NOT_B", dst) == MeshBoolean::BooleanOutcome::Empty);

    // Overlapping parts still give the overlap.
    TriangleMesh c{its_make_cube(10., 10., 10.)};
    c.translate(5.f, 0.f, 0.f);
    REQUIRE(MeshBoolean::make_boolean_with_fallback(a, c, "INTERSECTION", dst) == MeshBoolean::BooleanOutcome::Done);
    REQUIRE(dst.size() == 1);
    CHECK(signed_volume(dst.front().its) == Approx(500.).epsilon(1e-4));

}

