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

    // How the traced shapes were moved into the frame of the silhouette:
    // flipped (Y, and X when mirrored), scaled by the factor, moved by -center
    double factor   = 1.;
    Point  center   = Point(0, 0);
    bool   mirrored = false;

    bool empty() const { return shape.empty(); }
};

/// <summary>
/// Move a traced shape (e.g. one colour level) into the frame of the silhouette made from the
/// same trace, so it lies on the silhouette as it does in the image.
/// </summary>
ExPolygons to_silhouette_frame(const ExPolygons &traced, const Silhouette &silhouette);

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

// ---- box with a lid ----------------------------------------------------------------------------

// One printable object made of parts. Every part is a closed mesh in millimetres; the parts of
// an object share one frame and the object lies on z = 0 in its print orientation.
struct OutlinePart
{
    std::string          name;
    indexed_triangle_set mesh;
    // -1 .. body of the object, otherwise index of the colour level the part shows
    int level = -1;
};

struct OutlineObject
{
    std::string              name;
    std::vector<OutlinePart> parts;
};

struct OutlineBoxParams
{
    // Outer height of the base, the floor included [in mm]
    double height = 25.;
    double wall   = 1.6; // four lines of a 0.4 mm nozzle
    double floor  = 1.2;
    // Chamfer of the bottom edge of the base [in mm], it hides elephant foot. 0 = off
    double chamfer = 0.6;

    double lid_thickness = 2.;
    // The lid is larger than the base by this distance on every side [in mm], 0 = flush
    double lid_overhang = 1.5;
    // The lip under the lid goes into the base
    double lip_height    = 4.;
    double lip_thickness = 1.6;
    // Gap between the lip and the wall on every side [in mm]
    double clearance = 0.2;

    // The colour levels of the image are inlaid into the top face of the lid. The lid is printed
    // with its top face on the bed, so the picture is in the first layers.
    bool   decoration       = true;
    double decoration_depth = 0.6;
    // The picture keeps this distance from the edge of the base outline [in mm]
    double decoration_margin = 0.8;
};

// Cross-sections of the box in the frame of the silhouette (as the closed box is seen from above)
struct OutlineBoxSections
{
    ExPolygons wall;   // wall of the base
    ExPolygons cavity; // inside of the base, empty when the shape is too thin for one
    ExPolygons lid;    // plate of the lid
    ExPolygons lip;    // lip under the lid
    // Picture on the lid, one shape per colour level (can be empty), levels do not overlap
    std::vector<ExPolygons> decoration;
};

struct OutlineBoxResult
{
    OutlineObject base;
    // Print orientation: the top face lies on the bed and the lip points up, so the lid is
    // mirrored against the base
    OutlineObject lid;
    // Empty on success, translatable english text
    std::string              error;
    std::vector<std::string> warnings;

    bool is_valid() const { return error.empty() && !base.parts.empty() && !lid.parts.empty(); }
};

OutlineBoxParams sanitize(const OutlineBoxParams &params);

/// <param name="detail">Colour levels in the frame of the silhouette (to_silhouette_frame)</param>
OutlineBoxSections outline_box_sections(const ExPolygons &silhouette, const std::vector<ExPolygons> &detail, const OutlineBoxParams &params);

/// <summary>
/// Create a box in the shape of the silhouette and a lid for it.
/// </summary>
OutlineBoxResult make_outline_box(const ExPolygons &silhouette, const std::vector<ExPolygons> &detail, const OutlineBoxParams &params);

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
