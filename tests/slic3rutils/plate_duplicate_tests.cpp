// PartPlateList::duplicate_plate ("Duplicate Current Plate"). Plates store (object, instance) by
// index. The new plate must get one copy of each instance on the source plate, at the same place
// relative to the plate. Before the fix it copied each object once per instance on the plate, with
// all of the object's instances: two copies on the plate came out as two new objects of two copies
// each, stacked, and a copy of the object that sat on another plate was copied and moved too.
//
// GUI_App.hpp first: Windows headers that PartPlate.hpp's boost::asio path needs.
#include "slic3r/GUI/GUI_App.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "slic3r/GUI/PartPlate.hpp"

#include <catch2/catch.hpp>

using namespace Slic3r;
// Slic3r::PartPlateList is also forward-declared (UndoRedo.hpp), so name the GUI one in full.
using GuiPlateList = Slic3r::GUI::PartPlateList;

namespace {

ModelObject* add_cube_object(Model& model, const std::vector<Vec3d>& offsets)
{
    ModelObject* object = model.add_object();
    object->add_volume(make_cube(10., 10., 10.));
    for (const Vec3d& offset : offsets)
        object->add_instance()->set_offset(offset);
    object->invalidate_bounding_box();
    return object;
}

bool same_offset(const ModelInstance* instance, const Vec3d& expected)
{
    return (instance->get_offset() - expected).norm() < EPSILON;
}

} // namespace

TEST_CASE("Duplicating a plate copies only the instances on it", "[PartPlateList][PlateDuplicate]")
{
    Model        model;
    GuiPlateList list(270, 270, 270, nullptr, &model, ptFFF);
    REQUIRE(list.create_plate(false) == 1);
    const Vec3d origin0 = list.get_plate(0)->get_origin();
    const Vec3d origin1 = list.get_plate(1)->get_origin();

    // Object 0: two copies, both on plate 0. Object 1: one copy on plate 0, one on plate 1.
    const Vec3d a0 = origin0 + Vec3d(60., 60., 5.);
    const Vec3d a1 = origin0 + Vec3d(160., 160., 5.);
    const Vec3d b0 = origin0 + Vec3d(100., 200., 5.);
    const Vec3d b1 = origin1 + Vec3d(120., 120., 5.);
    add_cube_object(model, {a0, a1});
    add_cube_object(model, {b0, b1});

    REQUIRE(list.get_plate(0)->add_instance(0, 0, false) == 0);
    REQUIRE(list.get_plate(0)->add_instance(0, 1, false) == 0);
    REQUIRE(list.get_plate(0)->add_instance(1, 0, false) == 0);
    REQUIRE(list.get_plate(1)->add_instance(1, 1, false) == 0);

    const int new_index = list.duplicate_plate(0);
    REQUIRE(new_index == 2);
    REQUIRE(list.get_plate_count() == 3);
    // Plates 0 and 1 kept their places (two columns either way).
    REQUIRE((list.get_plate(0)->get_origin() - origin0).norm() < EPSILON);
    REQUIRE((list.get_plate(1)->get_origin() - origin1).norm() < EPSILON);
    const Vec3d offset = list.get_plate(2)->get_origin() - origin0;

    // One new object per source object, holding only the copies that were on plate 0.
    CHECK(model.objects.size() == 4);
    REQUIRE(model.objects.size() >= 4);
    CHECK(model.objects[2]->instances.size() == 2);
    REQUIRE(model.objects[3]->instances.size() >= 1);
    CHECK(same_offset(model.objects[2]->instances[0], a0 + offset));
    CHECK(same_offset(model.objects[2]->instances[1], a1 + offset));
    CHECK(same_offset(model.objects[3]->instances[0], b0 + offset));

    Slic3r::GUI::PartPlate* new_plate = list.get_plate(2);
    CHECK(new_plate->contain_instance(2, 0));
    CHECK(new_plate->contain_instance(2, 1));
    CHECK(new_plate->contain_instance(3, 0));
    CHECK_FALSE(new_plate->contain_instance(3, 1));
    CHECK(new_plate->get_objects_on_this_plate().size() == 3);

    // Nothing that was already there moves or changes plate.
    REQUIRE(model.objects[0]->instances.size() == 2);
    REQUIRE(model.objects[1]->instances.size() == 2);
    CHECK(same_offset(model.objects[0]->instances[0], a0));
    CHECK(same_offset(model.objects[0]->instances[1], a1));
    CHECK(same_offset(model.objects[1]->instances[0], b0));
    CHECK(same_offset(model.objects[1]->instances[1], b1));
    CHECK(list.get_plate(0)->contain_instance(0, 0));
    CHECK(list.get_plate(0)->contain_instance(0, 1));
    CHECK(list.get_plate(0)->contain_instance(1, 0));
    CHECK(list.get_plate(1)->contain_instance(1, 1));
    CHECK(list.get_plate(0)->get_objects_on_this_plate().size() == 3);
    CHECK(list.get_plate(1)->get_objects_on_this_plate().size() == 1);
}

TEST_CASE("Duplicating an empty plate adds an empty plate", "[PartPlateList][PlateDuplicate]")
{
    Model        model;
    GuiPlateList list(270, 270, 270, nullptr, &model, ptFFF);
    add_cube_object(model, {list.get_plate(0)->get_origin() + Vec3d(60., 60., 5.)});
    REQUIRE(list.get_plate(0)->add_instance(0, 0, false) == 0);
    REQUIRE(list.create_plate(false) == 1);

    REQUIRE(list.duplicate_plate(1) == 2);
    REQUIRE(list.get_plate_count() == 3);
    CHECK(model.objects.size() == 1);
    CHECK(list.get_plate(2)->empty());
}

TEST_CASE("Duplicating a plate when the plate count is at its maximum does nothing", "[PartPlateList][PlateDuplicate]")
{
    // create_plate returns -1 at the maximum; duplicate_plate used to read through the null plate.
    Model        model;
    GuiPlateList list(270, 270, 270, nullptr, &model, ptFFF);
    while (list.get_plate_count() < int(GuiPlateList::MAX_PLATES_COUNT))
        REQUIRE(list.create_plate(false) >= 0);

    CHECK(list.duplicate_plate(0) == -1);
    CHECK(list.duplicate_plate(-1) == -1);
    CHECK(list.get_plate_count() == int(GuiPlateList::MAX_PLATES_COUNT));
}
