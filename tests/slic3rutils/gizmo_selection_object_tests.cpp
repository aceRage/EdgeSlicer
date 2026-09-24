#include <catch2/catch.hpp>

#include "libslic3r/Model.hpp"
#include "slic3r/GUI/Gizmos/GizmoSelectionObject.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;

// 2026-09-23 22:30 crash: with a gizmo open (painting / cut / text / boolean / brim ears),
// selecting a second object in the object list made the selection span two objects, so
// Selection::get_object_idx() returned -1. SelectionInfo::on_update then read objects[-1] and
// Raycaster::on_update dereferenced that garbage pointer. The gizmo's object must be "none"
// for any index that does not name exactly one object.

TEST_CASE("A selection spanning several objects gives the gizmo no object", "[GizmoSelection]")
{
    Model model;
    model.add_object();
    model.add_object();
    model.add_object();

    // -1 is what Selection::get_object_idx() returns for a multi-object selection.
    REQUIRE(gizmo_selection_object(model.objects, -1) == nullptr);
    REQUIRE(gizmo_selection_object(model.objects, -2) == nullptr);
}

TEST_CASE("An index past the end of the object list gives the gizmo no object", "[GizmoSelection]")
{
    Model model;
    model.add_object();
    model.add_object();

    REQUIRE(gizmo_selection_object(model.objects, 2) == nullptr);
    REQUIRE(gizmo_selection_object(model.objects, 100) == nullptr);

    Model empty;
    REQUIRE(gizmo_selection_object(empty.objects, 0) == nullptr);
}

TEST_CASE("A single-object selection gives the gizmo that object", "[GizmoSelection]")
{
    Model model;
    ModelObject* first  = model.add_object();
    ModelObject* second = model.add_object();

    REQUIRE(gizmo_selection_object(model.objects, 0) == first);
    REQUIRE(gizmo_selection_object(model.objects, 1) == second);
}
