#ifndef slic3r_GUI_GizmoSelectionObject_hpp_
#define slic3r_GUI_GizmoSelectionObject_hpp_

#include <cstddef>

namespace Slic3r {
namespace GUI {

// The object a gizmo works on, given the model's object list and Selection::get_object_idx().
//
// get_object_idx() is -1 whenever the selection does not resolve to exactly one object - for
// example after Ctrl/Shift-clicking a second object in the object list while a painting, cut,
// text, boolean or brim-ears gizmo is open. SelectionInfo::on_update used to index the object
// list with that -1 for any non-empty selection, read the heap header in front of the vector as
// a ModelObject* and crashed in Raycaster::on_update (2026-09-23 22:30 crash, build bea215ec7a).
// Anything outside the list yields nullptr, which every CommonGizmosData consumer already
// treats as "no object"; the gizmo is then closed by refresh_on_off_state().
template<class ObjectPtrs>
typename ObjectPtrs::value_type gizmo_selection_object(const ObjectPtrs& objects, int object_idx)
{
    if (object_idx < 0 || static_cast<std::size_t>(object_idx) >= objects.size())
        return nullptr;
    return objects[static_cast<std::size_t>(object_idx)];
}

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_GizmoSelectionObject_hpp_
