#ifndef slic3r_GUI_DialogWorkArea_hpp_
#define slic3r_GUI_DialogWorkArea_hpp_

#include <algorithm>

namespace Slic3r {
namespace GUI {

// wxDisplay::GetFromWindow returns wxNOT_FOUND (-1) when the window is not on a
// display. The IP dialog then uses the primary display (index 0), matching
// GUI_App::window_pos_sanitize and PresetManagerDialog.
inline int work_area_display_index(int from_window)
{
    return from_window < 0 ? 0 : from_window;
}

// Keep a dialog's top-left on the work area. std::max runs last so a dialog
// larger than the work area still keeps its title bar on screen (OrcaSlicer
// #17763). Parentheses around min/max keep this MSVC-safe against windows.h.
inline void clamp_dialog_to_work_area(int &x, int &y, int w, int h, int work_x, int work_y, int work_w, int work_h)
{
    x = (std::max)(work_x, (std::min)(x, work_x + work_w - w));
    y = (std::max)(work_y, (std::min)(y, work_y + work_h - h));
}

} // namespace GUI
} // namespace Slic3r

#endif
