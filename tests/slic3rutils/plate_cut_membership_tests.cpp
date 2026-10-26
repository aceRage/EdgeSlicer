// OrcaSlicer #16881 / Edge: plates store (object, instance) by index. Deleting a whole object
// (cut, or wiping an object that is only connectors) must drop every remaining instance before
// update_object_index remaps higher object ids. Otherwise a second instance on another plate is
// renumbered onto the previous object, and Select All / prime-tower preview pick up the wrong plate.
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

ModelObject* add_cube_object(Model& model, int instances)
{
    ModelObject* object = model.add_object();
    object->add_volume(make_cube(10., 10., 10.));
    for (int i = 0; i < instances; ++i)
        object->add_instance();
    object->invalidate_bounding_box();
    return object;
}

} // namespace

TEST_CASE("Deleting a multi-instance object drops every instance from every plate", "[PartPlateList][PlateCut]")
{
    // Object 0 stays (one instance on plate 0). Object 1 is cut/deleted: instance 0 on plate 0,
    // instance 1 on plate 1. Before the fix, notify_instance_removed(1, -1) dropped only instance 0
    // and remapped (1, 1) to (0, 1) — a stale membership on the previous object.
    Model        model;
    add_cube_object(model, 1);
    add_cube_object(model, 2);

    GuiPlateList list(270, 270, 270, nullptr, &model, ptFFF);
    REQUIRE(list.create_plate(false) == 1);
    REQUIRE(list.get_plate_count() == 2);

    REQUIRE(list.get_plate(0)->add_instance(0, 0, false) == 0);
    REQUIRE(list.get_plate(0)->add_instance(1, 0, false) == 0);
    REQUIRE(list.get_plate(1)->add_instance(1, 1, false) == 0);

    REQUIRE(list.get_plate(0)->contain_instance(0, 0));
    REQUIRE(list.get_plate(0)->contain_instance(1, 0));
    REQUIRE(list.get_plate(1)->contain_instance(1, 1));

    model.delete_object(1);
    list.notify_instance_removed(1, -1);

    REQUIRE(list.get_plate(0)->contain_instance(0, 0));
    REQUIRE_FALSE(list.get_plate(0)->contain_instance(1, 0));
    REQUIRE_FALSE(list.get_plate(1)->contain_instance(1, 1));
    // The bug: leftover (1, 1) remapped onto object 0 as (0, 1).
    REQUIRE_FALSE(list.get_plate(0)->contain_instance(0, 1));
    REQUIRE_FALSE(list.get_plate(1)->contain_instance(0, 1));
    REQUIRE(list.get_plate(1)->empty());
}

TEST_CASE("Deleting an object remaps higher object indices on the plates", "[PartPlateList][PlateCut]")
{
    Model        model;
    add_cube_object(model, 1);
    add_cube_object(model, 1);

    GuiPlateList list(270, 270, 270, nullptr, &model, ptFFF);
    REQUIRE(list.create_plate(false) == 1);
    REQUIRE(list.get_plate(0)->add_instance(0, 0, false) == 0);
    REQUIRE(list.get_plate(1)->add_instance(1, 0, false) == 0);

    model.delete_object(0);
    list.notify_instance_removed(0, -1);

    REQUIRE(list.get_plate(0)->empty());
    REQUIRE(list.get_plate(1)->contain_instance(0, 0));
    REQUIRE_FALSE(list.get_plate(1)->contain_instance(1, 0));
}
