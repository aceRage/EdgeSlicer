#ifndef slic3r_GUI_RightUpSelect_hpp_
#define slic3r_GUI_RightUpSelect_hpp_

namespace Slic3r {
namespace GUI {

// Whether RightUp over a hovered volume should add that volume to the selection.
//
// GLCanvas3D sets Mouse::ignore_right_up during a camera pan or rotate and
// clears it in mouse_up_cleanup() after the RightUp handler runs. The handler
// must consult the flag first; otherwise releasing the button over an object
// after a pan/rotate replaces a cherry-picked selection (OrcaSlicer #17432).
inline bool should_select_hovered_volume_on_right_up(bool ignore_right_up, bool hover_volume_empty)
{
    const bool camera_dragged = ignore_right_up;
    return !camera_dragged && !hover_volume_empty;
}

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_RightUpSelect_hpp_
