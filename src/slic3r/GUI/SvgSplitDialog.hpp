#ifndef slic3r_GUI_SvgSplitDialog_hpp_
#define slic3r_GUI_SvgSplitDialog_hpp_

// Questions around "SVG (Split)": which import a dropped SVG gets, and which filament each colour
// of the split parts gets.

#include <vector>

#include <wx/string.h>

#include "libslic3r/SvgSplit.hpp"

class wxWindow;

namespace Slic3r {
enum class ModelVolumeType : int;
}

namespace Slic3r { namespace GUI {

enum class SvgDropAction { Cancel, Plain, Split };

/// <summary>
/// An .svg file is dropped onto the plate or imported (File > Import): import it as "SVG" (one
/// part, union of the shapes) or as "SVG (Split)" (one part per shape). Esc / Cancel skips the file.
/// </summary>
/// <param name="apply_to_all">When not null (more SVG files follow), a check box offers to use the
/// answer for the other SVG files too; receives its state</param>
SvgDropAction ask_svg_drop_action(wxWindow *parent, const wxString &file_name, bool *apply_to_all = nullptr);

/// <summary>
/// Is the file imported by the SVG gizmo (SVG / SVG (Split)) instead of the model loader?
/// </summary>
bool is_svg_file(const std::string &path);

// Files of one import split into SVG files (SVG gizmo) and the others (model loader), as indices
// into the input in their order
struct SvgImportPartition
{
    std::vector<size_t> svg;
    std::vector<size_t> other;
};
SvgImportPartition partition_svg_files(const std::vector<std::string> &paths);

/// <summary>
/// Does an imported SVG ask "SVG" / "SVG (Split)" for this target? A new object (INVALID) and an
/// object part ask; a negative volume or a modifier is always the plain SVG (split parts would act
/// exactly like their union).
/// </summary>
bool svg_import_asks(ModelVolumeType target);

/// <summary>
/// The one route of every file import of the GUI (File > Import menu and Ctrl+I, toolbar, Home,
/// recent files, drop of several files, downloads, files given to the running app, Add part >
/// Load...): every .svg is created by the SVG gizmo, after the SVG / SVG (Split) / Cancel question
/// (with "use this answer for the other SVG files"), the same code as Add Primitive.
/// </summary>
/// <param name="paths">Files of the import (UTF-8)</param>
/// <param name="target">INVALID: every SVG becomes a new object in the center of the view;
/// a volume type: the SVG is added to the selected object as that type</param>
/// <returns>The files which are not .svg (all files when the SVG gizmo is not available)</returns>
std::vector<std::string> import_svg_files_by_gizmo(const std::vector<std::string> &paths, ModelVolumeType target);

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
