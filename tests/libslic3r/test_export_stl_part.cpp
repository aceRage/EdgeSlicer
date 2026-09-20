#include <catch2/catch.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <Eigen/Geometry>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

// ModelObject::volume_mesh_in_world is the GUI-free core of Plater::export_stl_part:
// it must place the one volume where the user sees it (instance matrix * volume matrix)
// and must keep normals pointing outwards through a mirroring volume transform.

namespace {

// A cube of `size`, with its min corner at the origin in volume coordinates.
ModelVolume *add_cube(ModelObject &object, double size)
{
    return object.add_volume(make_cube(size, size, size), ModelVolumeType::MODEL_PART, false);
}

} // namespace

TEST_CASE("volume_mesh_in_world places the selected part in world coordinates", "[ExportStlPart]")
{
    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "assembly";

    // Volume 0: a 10 mm cube shifted +20 in X inside the object.
    ModelVolume *v0 = add_cube(*object, 10.);
    v0->name        = "base";
    v0->set_offset(Vec3d(20., 0., 0.));

    // Volume 1: a 4 mm cube, mirrored in X and lifted, so its transform has a
    // negative determinant - the case that needs the winding flip.
    ModelVolume *v1 = add_cube(*object, 4.);
    v1->name        = "mirrored pin";
    v1->set_mirror(Vec3d(-1., 1., 1.));
    v1->set_offset(Vec3d(0., 0., 30.));

    // Instance: translate + rotate 90 degrees about Z.
    ModelInstance *inst = object->add_instance();
    inst->set_offset(Vec3d(100., 5., 0.));
    inst->set_rotation(Vec3d(0., 0., M_PI / 2.));

    const Transform3d inst_trafo = inst->get_matrix();

    SECTION("a plain volume lands under instance matrix * volume matrix")
    {
        TriangleMesh mesh = object->volume_mesh_in_world(0, 0);
        REQUIRE_FALSE(mesh.empty());
        REQUIRE(mesh.its.vertices.size() == v0->mesh().its.vertices.size());

        const BoundingBoxf3 bb       = mesh.bounding_box();
        const Transform3d   expected = inst_trafo * v0->get_matrix();

        // The cube spans [0,10]^3 in volume coordinates; check its 8 corners survive the trip.
        BoundingBoxf3 expected_bb;
        for (int i = 0; i < 8; ++i)
            expected_bb.merge(expected * Vec3d((i & 1) ? 10. : 0., (i & 2) ? 10. : 0., (i & 4) ? 10. : 0.));

        CHECK_THAT(bb.min.x(), WithinAbs(expected_bb.min.x(), 1e-4));
        CHECK_THAT(bb.min.y(), WithinAbs(expected_bb.min.y(), 1e-4));
        CHECK_THAT(bb.min.z(), WithinAbs(expected_bb.min.z(), 1e-4));
        CHECK_THAT(bb.max.x(), WithinAbs(expected_bb.max.x(), 1e-4));
        CHECK_THAT(bb.max.y(), WithinAbs(expected_bb.max.y(), 1e-4));
        CHECK_THAT(bb.max.z(), WithinAbs(expected_bb.max.z(), 1e-4));

        // The rotation about Z moved the volume's +X offset onto +Y of the instance origin.
        CHECK_THAT(bb.min.z(), WithinAbs(0., 1e-4));
        CHECK_THAT(bb.min.y(), WithinAbs(5. + 20., 1e-4));

        // Every vertex is exactly the transformed source vertex.
        for (size_t i = 0; i < mesh.its.vertices.size(); ++i) {
            const Vec3d want = expected * v0->mesh().its.vertices[i].cast<double>();
            CHECK_THAT((mesh.its.vertices[i].cast<double>() - want).norm(), WithinAbs(0., 1e-4));
        }
    }

    SECTION("a mirrored volume keeps its normals pointing outwards")
    {
        const Transform3d full = inst_trafo * v1->get_matrix();
        REQUIRE(full.matrix().determinant() < 0.); // really is left handed

        TriangleMesh mesh = object->volume_mesh_in_world(0, 1);
        REQUIRE_FALSE(mesh.empty());

        // its_volume is positive only when the winding is consistent and outward facing.
        // Without the fix_left_handed flip this would come out as -64.
        CHECK_THAT(double(its_volume(mesh.its)), WithinAbs(4. * 4. * 4., 1e-2));

        // The mesh still sits where the mirrored part is drawn.
        const BoundingBoxf3 bb = mesh.bounding_box();
        CHECK_THAT(bb.min.z(), WithinAbs(30., 1e-4));
        CHECK_THAT(bb.max.z(), WithinAbs(34., 1e-4));
    }

    SECTION("an unmirrored volume is positive too, and the two agree in sign")
    {
        CHECK(its_volume(object->volume_mesh_in_world(0, 0).its) > 0.f);
        CHECK(its_volume(object->volume_mesh_in_world(0, 1).its) > 0.f);
    }

    SECTION("no instance selected still applies the volume matrix only")
    {
        TriangleMesh mesh = object->volume_mesh_in_world(-1, 0);
        REQUIRE_FALSE(mesh.empty());
        const BoundingBoxf3 bb = mesh.bounding_box();
        CHECK_THAT(bb.min.x(), WithinAbs(20., 1e-4));
        CHECK_THAT(bb.max.x(), WithinAbs(30., 1e-4));
        CHECK(its_volume(mesh.its) > 0.f);
    }

    SECTION("out of range indices give an empty mesh rather than crashing")
    {
        CHECK(object->volume_mesh_in_world(0, -1).empty());
        CHECK(object->volume_mesh_in_world(0, 7).empty());
        CHECK_FALSE(object->volume_mesh_in_world(9, 0).empty()); // bad instance -> volume space
    }
}
