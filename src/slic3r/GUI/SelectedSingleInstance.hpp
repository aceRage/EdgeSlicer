#pragma once

#include <cstddef>

namespace Slic3r {
namespace GUI {

// The instance Selection::get_selected_single_intance() returns, given the object's instance list
// and Selection::get_instance_idx().
//
// get_instance_idx() is -1 whenever the selection does not resolve to exactly one instance — for
// example after selecting several instances of the same object (Ctrl+A, or two copies) and using
// the Move gizmo in Object coordinates. The unguarded `instances[get_instance_idx()]` then indexes
// with -1 (MSVC debug assertion / crash). Anything below 0, or past the end of the list, yields
// nullptr, which GLGizmoMove already treats as "use the first volume's instance transform".
// OrcaSlicer #17642.
template<class InstancePtrs>
typename InstancePtrs::value_type selected_single_instance(const InstancePtrs& instances, int instance_idx)
{
    if (instance_idx < 0 || static_cast<std::size_t>(instance_idx) >= instances.size())
        return nullptr;
    return instances[static_cast<std::size_t>(instance_idx)];
}

} // namespace GUI
} // namespace Slic3r
