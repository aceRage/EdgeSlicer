#include <catch2/catch.hpp>
#include <algorithm>
#include <cstddef>
#include <vector>
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Geometry.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

ModelVolume *add_box(ModelObject *object, const Vec3d &size, const Vec3d &corner)
{
    TriangleMesh box = make_cube(size.x(), size.y(), size.z());
    box.translate(corner.cast<float>());
    return object->add_volume(std::move(box), ModelVolumeType::MODEL_PART, false);
}

// The vertices of every volume of every instance, in world coordinates, in a stable order.
std::vector<Vec3d> world_vertices(const ModelObject &object)
{
    std::vector<Vec3d> points;
    for (const ModelInstance *instance : object.instances)
        for (const ModelVolume *volume : object.volumes)
            for (const Vec3f &p : volume->mesh().its.vertices)
                points.push_back(instance->get_matrix() * volume->get_matrix() * p.cast<double>());
    return points;
}

double max_distance(const std::vector<Vec3d> &a, const std::vector<Vec3d> &b)
{
    REQUIRE(a.size() == b.size());
    double distance = 0.;
    for (size_t i = 0; i < a.size(); ++i)
        distance = std::max(distance, (a[i] - b[i]).norm());
    return distance;
}

Transform3d placed(const Vec3d &offset, const Vec3d &rotation, const Vec3d &scale)
{
    return Geometry::translation_transform(offset) * Geometry::rotation_transform(rotation) * Geometry::scale_transform(scale);
}

// How far the linear part is from the identity.
double off_identity(const Matrix3d &m) { return (m - Matrix3d::Identity()).cwiseAbs().maxCoeff(); }

// A two-part object with three instances: a tilted, unevenly scaled one, a copy of it, and a different one.
ModelObject *make_object(Model &model)
{
    ModelObject *object = model.add_object();
    add_box(object, {20., 10., 5.}, {-10., -5., 0.});
    ModelVolume *second = add_box(object, {4., 4., 8.}, {-2., -2., 0.});
    second->set_transformation(Geometry::Transformation(placed({12., 3., 5.}, {0.2, 0., 0.7}, {1., 2., 1.})));
    object->add_instance()->set_transformation(Geometry::Transformation(placed({50., 60., 3.}, {0.3, -0.4, 1.1}, {2., 1., 0.5})));
    object->add_instance()->set_transformation(Geometry::Transformation(placed({120., 60., 3.}, {0.3, -0.4, 1.1}, {2., 1., 0.5})));
    object->add_instance()->set_transformation(Geometry::Transformation(placed({50., 150., 0.}, {0., 0., 0.5}, {1., 1., 1.})));
    return object;
}

} // namespace

TEST_CASE("Applying an object's transform moves nothing", "[Model][ApplyTransform]")
{
    const int  what     = GENERATE(0, 1, 2);
    const bool rotation = what != 1;
    const bool scale    = what != 0;
    CAPTURE(rotation, scale);

    Model        model;
    ModelObject *object = make_object(model);
    const std::vector<Vec3d> before         = world_vertices(*object);
    const Vec3d              offset_before  = object->instances[0]->get_offset();
    const Matrix3d           part_before    = object->volumes[1]->get_matrix().linear();
    const Matrix3d           linear_before  = object->instances[0]->get_matrix().linear();

    REQUIRE(object->apply_instance_transformation(0, rotation, scale));

    CHECK_THAT(max_distance(world_vertices(*object), before), WithinAbs(0., 1e-3));
    CHECK_THAT((object->instances[0]->get_offset() - offset_before).norm(), WithinAbs(0., 1e-9));
    // The parts keep their own rotation and scale.
    CHECK_THAT((object->volumes[1]->get_matrix().linear() - part_before).cwiseAbs().maxCoeff(), WithinAbs(0., 1e-9));

    const Geometry::Transformation &now = object->instances[0]->get_transformation();
    if (rotation)
        CHECK_THAT(now.get_rotation().norm(), WithinAbs(0., 1e-6));
    if (scale)
        CHECK_THAT((now.get_scaling_factor() - Vec3d::Ones()).norm(), WithinAbs(0., 1e-6));
    if (rotation && scale)
        CHECK_THAT(off_identity(now.get_matrix().linear()), WithinAbs(0., 1e-9));
    if (!scale)
        CHECK_THAT((now.get_scaling_factor() - Vec3d(2., 1., 0.5)).norm(), WithinAbs(0., 1e-6));
    if (!rotation)
        // What stays is the same turn without the scale.
        CHECK_THAT((now.get_matrix().linear() - Geometry::rotation_transform({0.3, -0.4, 1.1}).linear()).cwiseAbs().maxCoeff(),
                   WithinAbs(0., 1e-6));

    // The copy ends up like the reference instance, as it started.
    CHECK_THAT((object->instances[1]->get_matrix().linear() - now.get_matrix().linear()).cwiseAbs().maxCoeff(), WithinAbs(0., 1e-9));
    // A second time there is nothing left to apply.
    CHECK_FALSE(object->apply_instance_transformation(0, rotation, scale));
    CHECK_THAT((object->instances[0]->get_matrix().linear() - now.get_matrix().linear()).cwiseAbs().maxCoeff(), WithinAbs(0., 1e-12));
    (void) linear_before;
}

TEST_CASE("Applying an object's transform keeps a mirror as a transform", "[Model][ApplyTransform]")
{
    Model        model;
    ModelObject *object = model.add_object();
    add_box(object, {20., 10., 5.}, {-10., -5., 0.});
    ModelInstance *instance = object->add_instance();
    instance->set_transformation(Geometry::Transformation(placed({50., 60., 0.}, {0., 0., 0.8}, {1.5, 1.5, 3.})));
    instance->set_mirror(Y, -1.);
    const std::vector<Vec3d>                       before = world_vertices(*object);
    const std::vector<stl_triangle_vertex_indices> faces  = object->volumes[0]->mesh().its.indices;

    REQUIRE(object->apply_instance_transformation(0, true, true));

    CHECK_THAT(max_distance(world_vertices(*object), before), WithinAbs(0., 1e-3));
    CHECK(instance->get_transformation().is_left_handed());
    CHECK_THAT(instance->get_rotation().norm(), WithinAbs(0., 1e-6));
    CHECK_THAT((instance->get_scaling_factor() - Vec3d::Ones()).norm(), WithinAbs(0., 1e-6));
    // The triangles keep their vertex order, which painting depends on.
    CHECK(object->volumes[0]->mesh().its.indices == faces);
    TriangleMesh baked = object->volumes[0]->mesh();
    CHECK(baked.volume() > 0.f);
}

TEST_CASE("Applying an object's transform leaves a text's mesh alone", "[Model][ApplyTransform]")
{
    Model        model;
    ModelObject *object = make_object(model);
    ModelVolume *text   = object->volumes[1];
    text->emboss_shape  = EmbossShape();
    const std::vector<Vec3d>         before   = world_vertices(*object);
    const std::vector<stl_vertex>    vertices = text->mesh().its.vertices;

    REQUIRE(object->apply_instance_transformation(0, true, true));

    CHECK_THAT(max_distance(world_vertices(*object), before), WithinAbs(0., 1e-3));
    CHECK(text->mesh().its.vertices == vertices);
    CHECK_FALSE(ModelObject::can_apply_volume_transformation(*text));
    CHECK_FALSE(object->apply_volume_transformation(1, true, true, true));
}

TEST_CASE("Applying a part's transform moves nothing", "[Model][ApplyTransform]")
{
    const int  what     = GENERATE(0, 1, 2, 3, 4);
    const bool position = what == 3 || what == 4;
    const bool rotation = what == 0 || what == 2 || what == 4;
    const bool scale    = what == 1 || what == 2 || what == 4;
    CAPTURE(position, rotation, scale);

    Model        model;
    ModelObject *object = make_object(model);
    const std::vector<Vec3d>       before       = world_vertices(*object);
    const Transform3d              first_before = object->volumes[0]->get_matrix();
    const Geometry::Transformation part_before  = object->volumes[1]->get_transformation();
    const std::vector<Matrix3d>    instances_before = {object->instances[0]->get_matrix().linear(), object->instances[2]->get_matrix().linear()};

    REQUIRE(object->apply_volume_transformation(1, position, rotation, scale));

    CHECK_THAT(max_distance(world_vertices(*object), before), WithinAbs(0., 1e-3));
    // Only that part changes.
    CHECK(object->volumes[0]->get_matrix().isApprox(first_before));
    CHECK_THAT((object->instances[0]->get_matrix().linear() - instances_before[0]).cwiseAbs().maxCoeff(), WithinAbs(0., 1e-12));
    CHECK_THAT((object->instances[2]->get_matrix().linear() - instances_before[1]).cwiseAbs().maxCoeff(), WithinAbs(0., 1e-12));

    const Geometry::Transformation &now = object->volumes[1]->get_transformation();
    if (rotation)
        CHECK_THAT(now.get_rotation().norm(), WithinAbs(0., 1e-6));
    else
        CHECK_THAT((now.get_rotation() - part_before.get_rotation()).norm(), WithinAbs(0., 1e-6));
    if (scale)
        CHECK_THAT((now.get_scaling_factor() - Vec3d::Ones()).norm(), WithinAbs(0., 1e-6));
    else
        CHECK_THAT((now.get_scaling_factor() - part_before.get_scaling_factor()).norm(), WithinAbs(0., 1e-6));
    if (position)
        CHECK_THAT(now.get_offset().norm(), WithinAbs(0., 1e-9));
    else
        CHECK_THAT((now.get_offset() - part_before.get_offset()).norm(), WithinAbs(0., 1e-9));

    CHECK_FALSE(object->apply_volume_transformation(1, position, rotation, scale));
}

TEST_CASE("Nothing to apply on an untouched object", "[Model][ApplyTransform]")
{
    Model        model;
    ModelObject *object = model.add_object();
    add_box(object, {20., 10., 5.}, {-10., -5., 0.});
    object->add_instance()->set_offset({30., 40., 0.});
    const ObjectID mesh_id = object->volumes[0]->id();

    CHECK_FALSE(object->apply_instance_transformation(0, true, true));
    CHECK_FALSE(object->apply_volume_transformation(0, true, true, true));
    CHECK_FALSE(object->apply_instance_transformation(5, true, true));
    CHECK(object->volumes[0]->id() == mesh_id);
}
