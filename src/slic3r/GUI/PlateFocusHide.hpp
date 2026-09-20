#ifndef slic3r_GUI_PlateFocusHide_hpp_
#define slic3r_GUI_PlateFocusHide_hpp_

#include <set>
#include <utility>

namespace Slic3r {
namespace GUI {

// Ultra: "Hide other plates while moving".
//
// The decision of *which* plates stay visible is pure data, so it lives here
// rather than inside GLCanvas3D's per-frame render code: the canvas resolves
// the instances it has (selected ones, and the one belonging to each volume it
// is about to draw) and asks these helpers, which keeps the rule testable.
//
// Rule (design section 3.4): hide every plate that has no selected content.
// The current plate is always exempt, and so is any plate holding part of the
// selection - otherwise a Ctrl-click selection spanning two plates would make
// half of what the user is about to drag disappear.

// An (object_idx, instance_idx) pair, matching PartPlate::obj_to_instance_set.
using PlateInstanceKey = std::pair<int, int>;

// The plate indices that must keep rendering. `current_plate` is the plate the
// user is working on; `selected_plates` are the plates that own at least one
// selected instance (may be empty, may include `current_plate`).
inline std::set<int> plate_focus_exempt_plates(int current_plate, const std::set<int> &selected_plates)
{
    std::set<int> exempt = selected_plates;
    if (current_plate >= 0)
        exempt.insert(current_plate);
    return exempt;
}

// True when the selection reaches more than one plate. Such a selection is
// never hidden - every plate it touches is exempt (design 3.4).
inline bool plate_focus_selection_spans_plates(const std::set<int> &selected_plates)
{
    return selected_plates.size() > 1;
}

// Whether a single plate should be hidden this frame.
inline bool plate_focus_should_hide_plate(int plate_index, int current_plate, const std::set<int> &selected_plates)
{
    if (plate_index < 0)
        return false;
    const std::set<int> exempt = plate_focus_exempt_plates(current_plate, selected_plates);
    return exempt.find(plate_index) == exempt.end();
}

// Whether a volume's instance should be hidden this frame. `owner_plate` is the
// plate the instance belongs to, or -1 when no plate claims it (an instance
// sitting off every bed): unclaimed instances are never hidden, since there is
// no plate for them to "belong elsewhere" to.
inline bool plate_focus_should_hide_instance(int owner_plate, int current_plate, const std::set<int> &selected_plates)
{
    if (owner_plate < 0)
        return false;
    return plate_focus_should_hide_plate(owner_plate, current_plate, selected_plates);
}

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_PlateFocusHide_hpp_
