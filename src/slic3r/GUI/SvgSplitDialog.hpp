#ifndef slic3r_GUI_SvgSplitDialog_hpp_
#define slic3r_GUI_SvgSplitDialog_hpp_

// Questions around "SVG (Split)": which import a dropped SVG gets, and which filament each colour
// of the split parts gets.

#include <vector>

#include <wx/string.h>

#include "libslic3r/SvgSplit.hpp"

class wxWindow;

namespace Slic3r { namespace GUI {

enum class SvgDropAction { Cancel, Plain, Split };

/// <summary>
/// A single .svg file was dropped onto the plate: import it as "SVG" (one part, union of the
/// shapes) or as "SVG (Split)" (one part per shape). Esc / Cancel cancels the drop.
/// </summary>
SvgDropAction ask_svg_drop_action(wxWindow *parent, const wxString &file_name);

/// <summary>
/// Choose a filament for every colour of an SVG split into parts. Every colour starts on the
/// project filament with the nearest colour (CIEDE2000, a tie keeps the lowest filament). Colours
/// without a close filament can be added as new filaments, while the project has free slots.
/// Nothing is remembered between imports.
/// </summary>
/// <param name="file_name">Shown in the dialog</param>
/// <param name="colors">Colours of the parts (svg_split_colors)</param>
/// <param name="part_count">Count of all parts</param>
/// <param name="extruders">OUT: per colour, 1 based filament (0 .. default filament of the object)</param>
/// <returns>False when canceled: the parts keep the default filament</returns>
bool ask_svg_split_filaments(wxWindow *parent, const wxString &file_name, const std::vector<SvgSplitColor> &colors, size_t part_count,
                             std::vector<int> &extruders);

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_SvgSplitDialog_hpp_
