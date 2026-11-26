#pragma once

// Whether Shift+LeftDown on the 3D canvas should start a rectangle selection
// while `current` is the active gizmo (a GLGizmosManager::EType value).
//
// Painter gizmos and Cut already consumed Shift for their own picking.
// OrcaSlicer #17533 also excluded Measure and Assembly: Shift-drag there
// started a rectangle that could close the gizmo on release. Edge has no
// PaintedModifier, so that enumerator is omitted.
//
// Header-only and free of wxWidgets/OpenGL so Catch2 can cover the list
// without constructing GLCanvas3D (tests/slic3rutils/gizmo_rectangle_selection_tests.cpp).

namespace Slic3r {
namespace GUI {

template<typename GizmosManager>
inline bool gizmo_allows_shift_rectangle_selection(typename GizmosManager::EType current)
{
    switch (current) {
    case GizmosManager::FdmSupports:
    case GizmosManager::Seam:
    case GizmosManager::Cut:
    case GizmosManager::MmSegmentation:
    case GizmosManager::FuzzySkin:
    case GizmosManager::Measure:
    case GizmosManager::Assembly:
        return false;
    default:
        return true;
    }
}

} // namespace GUI
} // namespace Slic3r
