#include <catch2/catch.hpp>
#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/BoundingBox.hpp"

using namespace Slic3r;

namespace {

// A box of the given size whose lowest corner is at `corner` in the file's coordinates. The mesh keeps them, unless
// `as_reader` centers it and keeps the same place in the volume's offset, as the file readers do.
ModelVolume *add_box(ModelObject *object, const Vec3d &size, const Vec3d &corner, bool as_reader = false)
{
    TriangleMesh box = make_cube(size.x(), size.y(), size.z());
    box.translate(corner.cast<float>());
    return object->add_volume(std::move(box), ModelVolumeType::MODEL_PART, as_reader);
}

// The vertices of every volume (or only of the model parts) of every instance of every object, in world coordinates,
// in a stable order.
std::vector<Vec3d> world_vertices(const Model &model, bool parts_only = false)
{
    std::vector<Vec3d> points;
    for (const ModelObject *object : model.objects)
        for (const ModelInstance *instance : object->instances)
            for (const ModelVolume *volume : object->volumes)
                if (!parts_only || volume->is_model_part())
                    for (const Vec3f &p : volume->mesh().its.vertices)
                        points.push_back(instance->get_matrix() * volume->get_matrix() * p.cast<double>());
    return points;
}

// The box around the given points.
BoundingBoxf3 points_box(const std::vector<Vec3d> &points)
{
    BoundingBoxf3 bb;
    for (const Vec3d &p : points)
        bb.merge(p);
    return bb;
}

// The larger distance between the matching corners of two boxes.
double box_distance(const BoundingBoxf3 &a, const BoundingBoxf3 &b) { return std::max((a.min - b.min).norm(), (a.max - b.max).norm()); }

// Largest distance between two lists of points of equal length.
double max_distance(const std::vector<Vec3d> &a, const std::vector<Vec3d> &b)
{
    REQUIRE(a.size() == b.size());
    double distance = 0.;
    for (size_t i = 0; i < a.size(); ++i)
        distance = std::max(distance, (a[i] - b[i]).norm());
    return distance;
}

// The points turned by `angle` around the vertical axis through `center`.
std::vector<Vec3d> turned(const std::vector<Vec3d> &points, double angle, const Vec2d &center)
{
    const Vec3d       center_3d(center.x(), center.y(), 0.);
    const Transform3d around = Geometry::translation_transform(center_3d) * Geometry::rotation_transform(angle * Vec3d::UnitZ()) *
                               Geometry::translation_transform(-center_3d);
    std::vector<Vec3d> out;
    for (const Vec3d &p : points)
        out.push_back(around * p);
    return out;
}

// The XY center of the box around the given points.
Vec2d layout_center(const std::vector<Vec3d> &points)
{
    const BoundingBoxf3 bb = points_box(points);
    return {bb.center().x(), bb.center().y()};
}

// The linear parts (rotation, scale, mirror) of the volumes and of the instances of an object.
std::vector<Matrix3d> linear_parts(const ModelObject &object)
{
    std::vector<Matrix3d> parts;
    for (const ModelVolume *volume : object.volumes)
        parts.push_back(volume->get_matrix().linear());
    for (const ModelInstance *instance : object.instances)
        parts.push_back(instance->get_matrix().linear());
    return parts;
}

} // namespace

TEST_CASE("Centering a placed object keeps it where it stands", "[Model][RotateCenter]")
{
    const bool as_reader = GENERATE(false, true);
    CAPTURE(as_reader);

    Model model;
    ModelObject *object = model.add_object();
    // A part far from its object's origin, placed by its instance, as an AMF constellation leaves it, with a
    // modifier reaching beyond it.
    add_box(object, {20., 10., 5.}, {100., 50., 0.}, as_reader);
    TriangleMesh modifier = make_cube(40., 10., 10.);
    modifier.translate(90.f, 50.f, 0.f);
    object->add_volume(std::move(modifier), ModelVolumeType::PARAMETER_MODIFIER, false);
    object->add_instance()->set_offset({5., 7., 0.});
    const std::vector<Vec3d> before = world_vertices(model);

    model.rotate_and_center_objects(0.);

    CHECK_THAT(max_distance(world_vertices(model), before), Catch::Matchers::WithinAbs(0., 1e-4));
    CHECK_THAT(object->full_raw_mesh_bounding_box().center().norm(), Catch::Matchers::WithinAbs(0., 1e-4));
    // The instance compensates the centering, so there is nothing for merging or export to undo.
    CHECK_THAT(object->origin_translation.norm(), Catch::Matchers::WithinAbs(0., 1e-9));
}

TEST_CASE("Turning placed objects of different heights keeps them on the bed", "[Model][RotateCenter]")
{
    const double angle  = GENERATE(0., 0.25 * PI);
    const double raised = GENERATE(0., 1.);
    CAPTURE(angle, raised);

    Model model;
    ModelObject *low  = model.add_object();
    ModelObject *flat = model.add_object();
    add_box(low, {20., 10., 5.}, {0., 0., 0.});
    add_box(flat, {30., 30., 2.}, {0., 0., raised});
    low->add_instance()->set_offset({110., 110., 0.});
    flat->add_instance()->set_offset({150., 125., 0.});
    // Fill the bounding box caches first, so a stale cache would show.
    REQUIRE_THAT(low->min_z(), Catch::Matchers::WithinAbs(0., 1e-6));
    REQUIRE_THAT(flat->min_z(), Catch::Matchers::WithinAbs(raised, 1e-6));

    model.rotate_and_center_objects(angle);

    CHECK_THAT(low->min_z(), Catch::Matchers::WithinAbs(0., 1e-4));
    CHECK_THAT(flat->min_z(), Catch::Matchers::WithinAbs(raised, 1e-4));
    CHECK(model.looks_like_multipart_object() == (raised != 0.));
}

TEST_CASE("Turning placed objects turns their whole layout around its center", "[Model][RotateCenter]")
{
    Model model;
    ModelObject *bracket = model.add_object();
    ModelObject *plate   = model.add_object();
    add_box(bracket, {20., 10., 5.}, {0., 0., 0.});
    // A part at an offset inside its object turns with it.
    add_box(bracket, {4., 4., 8.}, {25., 0., 0.});
    add_box(plate, {30., 30., 2.}, {40., 40., 0.});
    bracket->add_instance()->set_offset({110., 110., 0.});
    bracket->add_instance()->set_offset({110., 140., 0.});
    // A turned and scaled copy still takes the rotation in its volumes.
    ModelInstance *plate_instance = plate->add_instance();
    plate_instance->set_offset({150., 125., 0.});
    plate_instance->set_rotation({0., 0., 0.3});
    plate_instance->set_scaling_factor({1.5, 1.5, 1.5});
    // A modifier reaching far to one side turns with its object but does not move the layout's center.
    TriangleMesh modifier = make_cube(60., 5., 5.);
    modifier.translate(200.f, 0.f, 0.f);
    bracket->add_volume(std::move(modifier), ModelVolumeType::PARAMETER_MODIFIER, false);
    // An object with nothing in it takes no part in the layout.
    model.add_object()->add_instance();
    // Fill the bounding box caches first, so a stale cache would show.
    for (const ModelObject *object : model.objects) {
        object->bounding_box_exact();
        object->raw_mesh_bounding_box();
    }

    const double                angle        = 0.5 * PI;
    const std::vector<Vec3d>    before       = world_vertices(model);
    const Vec2d                 center       = layout_center(world_vertices(model, true));
    const std::vector<Matrix3d> plate_before = linear_parts(*plate);
    // The modifier must reach outside the parts, or counting it would not move the center.
    REQUIRE((layout_center(before) - center).norm() > 1.);

    model.rotate_and_center_objects(angle);

    CHECK_THAT(max_distance(world_vertices(model), turned(before, angle, center)), Catch::Matchers::WithinAbs(0., 1e-3));
    // Instances that commute with the turn keep their own rotation and scale; the volumes take the turn.
    const Matrix3d              turn        = Geometry::rotation_transform(angle * Vec3d::UnitZ()).linear();
    const std::vector<Matrix3d> plate_after = linear_parts(*plate);
    CHECK(plate_after[0].isApprox(turn * plate_before[0], 1e-9));
    CHECK(plate_after[1].isApprox(plate_before[1], 1e-9));
    // The caches follow the change.
    for (const ModelObject *object : model.objects) {
        if (object->volumes.empty())
            continue;
        BoundingBoxf3 raw;
        for (const ModelVolume *volume : object->volumes)
            if (volume->is_model_part())
                raw.merge(volume->mesh().transformed_bounding_box(volume->get_matrix()));
        CHECK_THAT(box_distance(object->raw_mesh_bounding_box(), raw), Catch::Matchers::WithinAbs(0., 1e-4));
        BoundingBoxf3 exact;
        for (size_t i = 0; i < object->instances.size(); ++i)
            exact.merge(object->instance_bounding_box(i));
        CHECK_THAT(box_distance(object->bounding_box_exact(), exact), Catch::Matchers::WithinAbs(0., 1e-4));
    }
}

TEST_CASE("Turning keeps mirrored, tilted and skewed instances in the turned layout", "[Model][RotateCenter]")
{
    const int case_idx = GENERATE(0, 1, 2, 3, 4);
    CAPTURE(case_idx);
    Transform3d linear = Transform3d::Identity();
    switch (case_idx) {
    case 0: linear.linear() = Vec3d(-1., 1., 1.).asDiagonal(); break;                            // mirror X
    case 1: linear.linear() = Vec3d(-1., -1., 1.).asDiagonal(); break;                           // both horizontal mirrors
    case 2: linear.linear() = Eigen::AngleAxisd(0.5, Vec3d::UnitX()).toRotationMatrix(); break; // tilt about X
    case 3: linear.linear() << 1., 0.3, 0., 0., 1., 0., 0., 0., 1.; break;                      // skew in XY
    default: linear.linear() = Vec3d(2., 1., 1.).asDiagonal(); break;                            // uneven XY scale
    }

    Model model;
    ModelObject *object = model.add_object();
    add_box(object, {20., 10., 5.}, {100., 0., 0.});
    add_box(object, {4., 4., 8.}, {125., 0., 0.});
    // One plain copy and one transformed copy of the same object: both must stay right.
    object->add_instance()->set_offset({60., 60., 0.});
    ModelInstance *transformed = object->add_instance();
    transformed->set_transformation(Geometry::Transformation(Geometry::translation_transform({160., 90., 0.}) * linear));

    const double                angle         = -0.25 * PI;
    const std::vector<Vec3d>    before        = world_vertices(model);
    const Vec2d                 center        = layout_center(before);
    const std::vector<Matrix3d> linear_before = linear_parts(*object);

    model.rotate_and_center_objects(angle);

    CHECK_THAT(max_distance(world_vertices(model), turned(before, angle, center)), Catch::Matchers::WithinAbs(0., 1e-3));
    // Both horizontal mirrors commute with the turn, so the volumes take it; otherwise every instance of the object
    // takes it and the shared volumes keep theirs.
    const Matrix3d              turn         = Geometry::rotation_transform(angle * Vec3d::UnitZ()).linear();
    const std::vector<Matrix3d> linear_after = linear_parts(*object);
    const size_t                volumes      = object->volumes.size();
    for (size_t i = 0; i < linear_after.size(); ++i) {
        CAPTURE(i);
        const bool turned_here = (i < volumes) == (case_idx == 1);
        CHECK(linear_after[i].isApprox(turned_here ? Matrix3d(turn * linear_before[i]) : linear_before[i], 1e-9));
    }
}

TEST_CASE("Merging turned objects into one keeps every part where it stands", "[Model][RotateCenter]")
{
    Model model;
    ModelObject *bracket = model.add_object();
    ModelObject *plate   = model.add_object();
    add_box(bracket, {20., 10., 5.}, {100., 50., 0.});
    add_box(plate, {30., 30., 2.}, {-40., 10., 3.});
    bracket->add_instance()->set_offset({10., 20., 0.});
    plate->add_instance()->set_offset({60., 20., 0.});

    model.rotate_and_center_objects(0.25 * PI);
    const std::vector<Vec3d> standing = world_vertices(model);
    model.convert_multipart_object(1);

    // The merged object has no instance yet: its parts are in world coordinates.
    REQUIRE(model.objects.size() == 1);
    std::vector<Vec3d> merged;
    for (const ModelVolume *volume : model.objects.front()->volumes)
        for (const Vec3f &p : volume->mesh().its.vertices)
            merged.push_back(volume->get_matrix() * p.cast<double>());
    CHECK_THAT(max_distance(merged, standing), Catch::Matchers::WithinAbs(0., 1e-3));
}

TEST_CASE("Unplaced objects turn around the file origin and keep their height", "[Model][RotateCenter]")
{
    Model model;
    ModelObject *bracket = model.add_object();
    ModelObject *plate   = model.add_object();
    add_box(bracket, {20., 10., 5.}, {100., 50., 0.});
    add_box(plate, {30., 30., 2.}, {-40., 10., 3.});
    // Unplaced objects lie in the file's coordinates; origin_translation records how far they were centered.
    auto file_points = [&model]() {
        std::vector<Vec3d> points;
        for (const ModelObject *object : model.objects)
            for (const ModelVolume *volume : object->volumes)
                for (const Vec3f &p : volume->mesh().its.vertices)
                    points.push_back(volume->get_matrix() * p.cast<double>() - object->origin_translation);
        return points;
    };
    const double             angle  = 0.5 * PI;
    const std::vector<Vec3d> before = file_points();

    model.rotate_and_center_objects(angle);

    CHECK_THAT(max_distance(file_points(), turned(before, angle, Vec2d::Zero())), Catch::Matchers::WithinAbs(0., 1e-3));
    for (const ModelObject *object : model.objects) {
        const BoundingBoxf3 bb = object->full_raw_mesh_bounding_box();
        CHECK_THAT(bb.center().x(), Catch::Matchers::WithinAbs(0., 1e-4));
        CHECK_THAT(bb.center().y(), Catch::Matchers::WithinAbs(0., 1e-4));
        CHECK_THAT(object->origin_translation.z(), Catch::Matchers::WithinAbs(0., 1e-9));
    }
    CHECK_THAT(bracket->full_raw_mesh_bounding_box().min.z(), Catch::Matchers::WithinAbs(0., 1e-4));
    CHECK_THAT(plate->full_raw_mesh_bounding_box().min.z(), Catch::Matchers::WithinAbs(3., 1e-4));
}

TEST_CASE("Unplaced objects scaled to millimetres still merge on a common base", "[Model][RotateCenter]")
{
    Model model;
    ModelObject *tall = model.add_object();
    ModelObject *flat = model.add_object();
    add_box(tall, {2., 1., 0.5}, {0., 0., 0.});
    add_box(flat, {3., 3., 0.2}, {5., 0., 0.});

    model.rotate_and_center_objects(0.);
    // The rest of the import as it runs for several files loaded as one object.
    model.convert_from_imperial_units(false);
    for (ModelObject *object : model.objects)
        object->center_around_origin(false);
    model.convert_multipart_object(1);

    REQUIRE(model.objects.size() == 1);
    REQUIRE(model.objects.front()->volumes.size() == 2);
    std::vector<double> bases;
    for (const ModelVolume *volume : model.objects.front()->volumes)
        bases.push_back(volume->mesh().transformed_bounding_box(volume->get_matrix()).min.z());
    CHECK_THAT(bases.front(), Catch::Matchers::WithinAbs(bases.back(), 1e-3));
}
