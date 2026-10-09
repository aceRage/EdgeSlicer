#ifndef slic3r_SvgSplit_hpp_
#define slic3r_SvgSplit_hpp_

// "SVG (Split)": import an SVG as one part per painted shape, the way Bambu Studio imports an SVG
// into a new object (one volume per fill and one per stroke, in the order of the file).
//
// Bambu Studio only stacks the shapes: every part keeps its whole outline, so the parts overlap and
// a slice of them is the union again. An SVG is painted in order (later shapes cover earlier ones),
// so here every part loses the area covered by the shapes painted after it. The parts then do not
// overlap and each one is what is visible of that shape in a browser. Shapes which are completely
// covered produce no part.
//
// Every part becomes its own SVG volume: its outline is written into a small generated SVG (whole
// image sized, so the parts stay aligned) and the volume is moved by `offset`, because an SVG volume
// is centered by its own bounding box.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "BoundingBox.hpp"
#include "ExPolygon.hpp"
#include "Point.hpp"

struct NSVGimage;

namespace Slic3r {

struct NSVGLineParams;

struct SvgSplitPart
{
    // Visible area of the shape, scaled mm, Y up (as create_shape_with_ids() without centering)
    ExPolygons shape;
    // Colour of the paint (fill or stroke), the first stop of a gradient
    std::array<uint8_t, 3> color{0, 0, 0};
    // Index of the shape in the SVG (painter's order)
    size_t shape_index = 0;
    // The part is the outline of a stroke, otherwise of a fill
    bool is_stroke = false;
    // Center of the bounding box of the part relative to the center of the bounding box of the whole
    // drawing [in mm], Y up. The SVG volume of the part has to be moved by it in its local XY plane.
    Vec2d offset = Vec2d::Zero();
    // Content of the generated SVG file of the part
    std::string svg;
};

struct SvgSplitResult
{
    // In painter's order
    std::vector<SvgSplitPart> parts;
    // Count of painted fills and strokes (before covered ones were removed)
    size_t painted = 0;
    // Fills and strokes completely covered by later shapes (or left with only specks)
    size_t covered = 0;
    // Size of the whole drawing [in mm]
    double width  = 0.;
    double height = 0.;
    // The SVG is over the complexity limits (NSVGLineParams::max_flat_points or untrusted::SVG_*)
    bool too_complex = false;
    // Empty on success
    std::string error;

    bool is_valid() const { return error.empty() && !parts.empty(); }
};

/// <summary>
/// Split a parsed SVG into parts, one per visible fill / stroke, later shapes win over earlier ones.
/// </summary>
/// <param name="image">Parsed SVG</param>
/// <param name="params">Conversion of curves to lines, same as for the whole SVG volume</param>
/// <param name="min_area">Islands and holes smaller than this are dropped [in mm^2]</param>
/// <param name="write_svg">False skips the SVG text of the parts (tests, statistics)</param>
SvgSplitResult split_svg_by_shapes(const NSVGimage &image, const NSVGLineParams &params, double min_area = 0.01, bool write_svg = true);

/// <summary>
/// SVG text of one part: one evenodd path in micrometers, sized as the whole drawing
/// </summary>
/// <param name="shape">Part, scaled mm, Y up</param>
/// <param name="drawing">Bounding box of the whole drawing, scaled mm, Y up</param>
/// <param name="color">Fill colour</param>
std::string svg_split_part_svg(const ExPolygons &shape, const BoundingBox &drawing, const std::array<uint8_t, 3> &color);

/// <summary>
/// Path of the generated SVG of a part inside of .3mf
/// </summary>
std::string svg_split_path_in_3mf(const std::string &group_id, size_t index);

/// <summary>
/// "#rrggbb"
/// </summary>
std::string svg_split_color_to_hex(const std::array<uint8_t, 3> &color);

} // namespace Slic3r
#endif // slic3r_SvgSplit_hpp_
