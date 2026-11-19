#include <catch2/catch.hpp>
#include <cmath>
#include <string>
#include <vector>
#include "libslic3r/Orient.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/BoundingBox.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using orientation::OrientMesh;
using orientation::OrientMeshs;

namespace {

orientation::OrientParams quiet_params()
{
    orientation::OrientParams params;
    params.progressind   = [](unsigned, std::string) {};
    params.stopcondition = [] { return false; };
    return params;
}

// Orients every instance of the object as the GUI's OrientJob does: one OrientMesh per
// instance, measured on orientation::instance_mesh(), its rotation applied to that instance.
void orient_each_instance(ModelObject *object)
{
    OrientMeshs meshes;
    for (ModelInstance *instance : object->instances) {
        OrientMesh om;
        om.mesh   = orientation::instance_mesh(*instance);
        om.setter = [instance](const OrientMesh &p) { instance->rotate(p.rotation_matrix); };
        meshes.emplace_back(std::move(om));
    }
    orientation::orient(meshes, {}, quiet_params());
    for (const OrientMesh &om : meshes)
        om.apply();
    object->invalidate_bounding_box();
}

double instance_height(const ModelObject *object, size_t instance_idx)
{
    return object->instance_bounding_box(instance_idx).size().z();
}

using Grid = std::vector<std::vector<bool>>;

} // namespace

TEST_CASE("instance_mesh is the object placed by that instance alone", "[Orient]")
{
    Model        model;
    ModelObject *object = model.add_object();
    object->add_volume(make_cube(10., 20., 40.));
    object->add_instance()->set_offset({0., 0., 0.});
    object->add_instance()->set_offset({100., 0., 0.});

    const TriangleMesh one = orientation::instance_mesh(*object->instances[1]);
    CHECK(one.facets_count() == object->raw_mesh().facets_count());
    const BoundingBoxf3 bb = one.bounding_box();
    CHECK_THAT(bb.size().x(), WithinAbs(10., 1e-3));
    CHECK_THAT(bb.center().x(), WithinAbs(object->instance_bounding_box(1).center().x(), 1e-3));
    // ModelObject::mesh() is both copies merged.
    CHECK(object->mesh().facets_count() == 2 * one.facets_count());
}

TEST_CASE("Each copy of an object is oriented by its own pose", "[Orient]")
{
    // A 10 x 20 x 40 box: it prints best lying on its 20 x 40 face, 10 high. One copy stands
    // on its end, the other lies on its 10 x 40 side; both should end up lying flat.
    Model        model;
    ModelObject *object = model.add_object();
    object->add_volume(make_cube(10., 20., 40.));
    object->add_instance()->set_offset({0., 0., 0.});
    ModelInstance *turned = object->add_instance();
    turned->set_offset({100., 0., 0.});
    turned->set_rotation({PI / 2., 0., 0.});
    object->invalidate_bounding_box();
    REQUIRE_THAT(instance_height(object, 0), WithinAbs(40., 1e-3));
    REQUIRE_THAT(instance_height(object, 1), WithinAbs(20., 1e-3));

    orient_each_instance(object);

    CHECK_THAT(instance_height(object, 0), WithinAbs(10., 1e-3));
    CHECK_THAT(instance_height(object, 1), WithinAbs(10., 1e-3));
}

TEST_CASE("Copies side by side are each oriented as a lone copy", "[Orient]")
{
    // 2x20x10.obj lying on the bed: alone, auto orient turns it. Measured as the union of
    // two copies 120 mm apart (ModelObject::mesh()), it was left as it stands.
    auto orientation_of_copies = [](int copies) {
        Model        model  = Model::read_from_file(std::string(TEST_DATA_DIR) + "/2x20x10.obj");
        ModelObject *object = model.objects.front();
        object->clear_instances();
        for (int k = 0; k < copies; ++k)
            object->add_instance()->set_offset({120. * k, 0., 0.});
        object->invalidate_bounding_box();
        OrientMeshs meshes;
        for (ModelInstance *instance : object->instances) {
            OrientMesh om;
            om.mesh = orientation::instance_mesh(*instance);
            meshes.emplace_back(std::move(om));
        }
        orientation::orient(meshes, {}, quiet_params());
        std::vector<Vec3d> out;
        for (const OrientMesh &om : meshes)
            out.push_back(om.orientation);
        return out;
    };
    const Vec3d lone = orientation_of_copies(1).front();
    REQUIRE(std::abs(lone.z()) < 0.5); // it is turned
    for (const Vec3d &copy : orientation_of_copies(2))
        CHECK((copy - lone).norm() < 1e-6);
}

TEST_CASE("Orienting one plate turns the instances on it and no other", "[Orient]")
{
    const Grid none_locked = {{false, false}, {false}};
    const std::vector<bool> printable = {true, true};

    SECTION("an object whose last copy is on another plate") {
        // Object 0: copy 0 on the plate, copy 1 elsewhere; object 1 elsewhere.
        const Grid asked = {{true, false}, {false}};
        CHECK(orientation::instances_to_orient(asked, none_locked, printable, true) == Grid{{true, false}, {false}});
    }
    SECTION("an object whose first copy is on another plate") {
        const Grid asked = {{false, true}, {true}};
        CHECK(orientation::instances_to_orient(asked, none_locked, printable, true) == Grid{{false, true}, {true}});
    }
    SECTION("a plate holding only unprintable objects turns nothing elsewhere") {
        const Grid asked = {{true, true}, {false}};
        CHECK(orientation::instances_to_orient(asked, none_locked, {false, true}, true) == Grid{{false, false}, {false}});
    }
    SECTION("a locked plate elsewhere does not matter on the one-plate path") {
        const Grid asked = {{true, false}, {false}};
        const Grid locked = {{true, true}, {true}};
        CHECK(orientation::instances_to_orient(asked, locked, printable, true) == Grid{{true, false}, {false}});
    }
}

TEST_CASE("Orienting the selection", "[Orient]")
{
    const std::vector<bool> printable = {true, true, false};

    SECTION("nothing selected orients every printable instance not on a locked plate") {
        const Grid asked  = {{false, false}, {false}, {false}};
        const Grid locked = {{false, true}, {false}, {false}};
        bool       asked_locked = true;
        CHECK(orientation::instances_to_orient(asked, locked, printable, false, &asked_locked) ==
              Grid{{true, false}, {true}, {false}});
        CHECK_FALSE(asked_locked);
    }
    SECTION("a selection all on locked plates orients nothing and says so") {
        const Grid asked  = {{true, true}, {false}, {false}};
        const Grid locked = {{true, true}, {false}, {false}};
        bool       asked_locked = false;
        CHECK(orientation::instances_to_orient(asked, locked, printable, false, &asked_locked) ==
              Grid{{false, false}, {false}, {false}});
        CHECK(asked_locked);
    }
    SECTION("a selected object is turned where it is not locked") {
        const Grid asked  = {{true, true}, {false}, {false}};
        const Grid locked = {{false, true}, {false}, {false}};
        bool       asked_locked = false;
        CHECK(orientation::instances_to_orient(asked, locked, printable, false, &asked_locked) ==
              Grid{{true, false}, {false}, {false}});
        CHECK(asked_locked);
    }
}

