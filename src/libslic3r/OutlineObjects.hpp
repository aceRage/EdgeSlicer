#ifndef slic3r_OutlineObjects_hpp_
#define slic3r_OutlineObjects_hpp_

// Printable objects generated from the outline of an image ("Image to object").
//
// Design: tests/design_image_to_objects.md. This file holds the part without GUI:
//   traced colour levels (libslic3r/ImageTrace) -> silhouette -> mesh of the object.
//
// Every object is a stack of bands: a band is a cross-section (ExPolygons) extruded between two
// heights. The bands are joined into one closed mesh. Phase 1 creates a cookie cutter; an
// outline shaped box and a clicker follow the same construction.

#include <string>
#include <vector>

#include "ExPolygon.hpp"
#include "TriangleMesh.hpp"

namespace Slic3r {

// ---- silhouette --------------------------------------------------------------------------------

struct SilhouetteParams
{
    // Longest side of the bounding box of the silhouette [in mm]
    double longest_side = 80.;
    // Gaps narrower than this are closed, parts closer than this are joined [in mm], 0 = off
    double close_gaps = 0.3;
    // Parts of the shape narrower than this are removed [in mm], 0 = off
    double min_feature = 1.2;
    // Holes inside of the shape are filled (only the outer outline is used)
    bool fill_holes = true;
    // Keep only the island with the largest area
    bool largest_only = true;
    // Mirror left to right. A cookie cutter is turned over when used, so the outline is mirrored.
    bool mirror = false;
};

struct Silhouette
{
    // Shape in scaled millimetres, Y up, the center of its bounding box is in the origin
    ExPolygons shape;
    // What min_feature removed (thin parts), in the same frame as the shape. Only for preview.
    ExPolygons removed;
    // Islands found before largest_only was applied
    size_t islands = 0;
    // Size of the bounding box of the shape [in mm]
    double width  = 0.;
    double height = 0.;
    // Non fatal issues, translatable english text
    std::vector<std::string> warnings;

    bool empty() const { return shape.empty(); }
};

/// <summary>
/// Create the silhouette from traced shapes.
/// </summary>
/// <param name="levels">Shapes of the trace (ImageTraceLayer::shape of every level): scaled
/// millimetres, Y down. All the levels together are the silhouette.</param>
Silhouette make_silhouette(const std::vector<ExPolygons> &levels, const SilhouetteParams &params);

// ---- cookie cutter -----------------------------------------------------------------------------

enum class CutterBraces : int {
    // Braces are added when the longest side of the cutter is over COOKIE_CUTTER_AUTO_BRACE_SIDE
    Auto = 0,
    Always,
    Never
};

constexpr double COOKIE_CUTTER_AUTO_BRACE_SIDE = 90.; // mm

struct CookieCutterParams
{
    // Height of the blade measured from the bed, the handle included [in mm]
    double blade_height = 12.5;
    // Thickness of the blade wall [in mm], two perimeters of a 0.4 mm nozzle by default
    double blade_thickness = 0.8;
    // The handle is a flange around the blade, it lies on the bed [in mm]
    double handle_width  = 4.;
    double handle_height = 3.5;
    // The cutting edge gets thinner towards its end
    bool   sharp_edge = true;
    double tip_width  = 0.4; // thickness of the blade at the cutting edge [in mm]
    double tip_height = 2.;  // height of the tapered part [in mm]
    // Height of one step of the taper [in mm], layer height of the print is a good value
    double taper_step = 0.2;
    // Bars across the opening keep a large or thin cutter in shape
    CutterBraces braces       = CutterBraces::Auto;
    double       brace_width  = 3.;
    // Bars are spaced by at most this distance [in mm]
    double       brace_spacing = 60.;
};

// Cross-sections of the cutter, for 2D preview. Scaled millimetres, frame of the silhouette.
struct CookieCutterSections
{
    ExPolygons handle; // flange (+ braces), lies on the bed
    ExPolygons blade;  // wall of the blade
    ExPolygons tip;    // blade at the cutting edge, equal to blade without sharp_edge
    bool       braces = false;
};

struct OutlineObjectResult
{
    // Mesh in millimetres, lies on z = 0, printable as it is (no supports)
    indexed_triangle_set mesh;
    // The bands were joined into one closed mesh. When false the mesh is made of overlapping
    // closed shells, which still slices correctly.
    bool joined = false;
    // Empty on success, translatable english text
    std::string error;
    std::vector<std::string> warnings;

    bool is_valid() const { return error.empty() && !mesh.indices.empty(); }
};

/// <summary>
/// Limit the parameters to printable values (thin blade, handle lower than blade, ...)
/// </summary>
CookieCutterParams sanitize(const CookieCutterParams &params);

CookieCutterSections cookie_cutter_sections(const ExPolygons &silhouette, const CookieCutterParams &params);

/// <summary>
/// Create a cookie cutter: the inner side of the blade is the silhouette.
/// The cutter is printed with the handle on the bed and the cutting edge up.
/// </summary>
/// <param name="silhouette">Silhouette::shape</param>
OutlineObjectResult make_cookie_cutter(const ExPolygons &silhouette, const CookieCutterParams &params);

// ---- building blocks (also used by tests) ------------------------------------------------------

struct OutlineBand
{
    ExPolygons shape; // scaled millimetres
    double     z0 = 0.;
    double     z1 = 0.; // mm, z1 > z0
};

/// <summary>
/// Closed prism of the shape between z0 and z1 [in mm]. Empty for an empty shape.
/// </summary>
indexed_triangle_set outline_prism(const ExPolygons &shape, double z0, double z1);

/// <summary>
/// Join bands stacked on each other into one mesh.
/// </summary>
/// <param name="joined">True when the result is one boolean union, false when the prisms
/// were only merged (overlapping closed shells)</param>
indexed_triangle_set join_outline_bands(const std::vector<OutlineBand> &bands, bool *joined = nullptr);

} // namespace Slic3r

#endif // slic3r_OutlineObjects_hpp_
