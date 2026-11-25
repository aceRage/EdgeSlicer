#pragma once
// Paint Split (tests/design_paint_split.md): split a part by a painted or picked REGION. The user selects
// surface with a one-state mask (ENFORCER = "goes with the piece"); each connected component of the mask
// becomes one closed piece, cut out by a closed cutter solid (P1: the Plane style) through ONE Manifold
// Split, after which the keep-connected rule (design 4.2) decides which cut components really leave the body.
//
// No GUI dependencies; everything here is testable headless (tests/libslic3r/test_paint_split.cpp).
#include "BoundingBox.hpp"
#include "DrawCut.hpp"
#include "Point.hpp"
#include "TriangleSelector.hpp"
#include <admesh/stl.h>

#include <string>
#include <vector>

namespace Slic3r {

// Refusals and failures. paint_split_error_message() gives the (translatable) text; nothing here throws.
enum class PaintSplitError {
    None,
    OpenMesh,      // the part has open edges (design D16: refuse, the panel offers Repair)
    NonManifold,   // closed, but Manifold does not accept it (self-touching, non-manifold edges)
    EmptyMask,     // nothing selected
    NoBoundary,    // a component covers a whole closed shell: there is no boundary to cut along
    Band,          // two boundary loops that are both large: a painted ring (design D7: "split it as two cuts")
    MultiLoop,     // more than two boundary loops left after the hole fill
    BadComponent,  // the component index is out of range
    BooleanFailed, // Manifold failed during the split
    EmptyPiece,    // no cut component carries the selection (the cutter misses the region)
    EmptyBody,     // the piece would take the whole part
};
const char* paint_split_error_message(PaintSplitError err);

// Non-fatal findings, listed in the panel's Output Log.
enum class PaintSplitWarning {
    PlaneFarFromBoundary, // design 4.3b guard: the fitted plane is far from a twisted boundary loop
    VolumeMismatch,       // piece + body + loose != source within 1e-6 relative (kerf 0 only)
    ReunionSeam,          // the re-union left cut faces inside the body; the subtract fallback was used
    ComponentsReturned,   // some cut components carried no selection and went back to the body
    PiecesAttached,       // some body-side material hung only from the piece and was attached to it (D18)
    LoosePieces,          // ... or was broken off as loose parts
};
const char* paint_split_warning_message(PaintSplitWarning w);

// ---------------------------------------------------------------------------------------------------------
// Region and boundary (design 4.1)
// ---------------------------------------------------------------------------------------------------------

struct PaintSplitLoop
{
    std::vector<DrawCutSample> pts;   // closed: the last sample connects back to the first
    double                     length{ 0. };
    bool                       is_outer{ false };
};

struct PaintSplitRegionParams
{
    // Unselected islands inside a component (eyes inside a painted face) of at most this share of the
    // component's selected area are filled, i.e. join the selection (design D7: 10 %).
    double fill_holes_pct{ 10. };
};

struct PaintSplitRegion
{
    PaintSplitError       error{ PaintSplitError::None };
    // The strict retriangulation of the mask (the same one extract_color_patches() builds): the mask's
    // sub-facets as real triangles over the source surface, closed.
    indexed_triangle_set  surface;
    std::vector<int>      facet_comp;    // per surface facet: -1 = unselected, else the component id
    std::vector<std::vector<PaintSplitLoop>> loops; // per component, after the hole fill; [0] is the outer one
    std::vector<double>   area;          // per component, selected area after the hole fill
    std::vector<int>      holes_filled;  // per component
    std::vector<PaintSplitError> comp_error; // per component: None, NoBoundary, Band or MultiLoop

    size_t num_components() const { return area.size(); }
    bool   component_ok(int comp) const
    {
        return error == PaintSplitError::None && comp >= 0 && size_t(comp) < comp_error.size() &&
               comp_error[size_t(comp)] == PaintSplitError::None;
    }
};

// `mesh` is the part's own mesh; `mask` its one-state selection (any painted state counts as selected).
// Welds and refuses an open mesh exactly as the colour split does.
PaintSplitRegion paint_split_region(const indexed_triangle_set& mesh, const TriangleSelector::TriangleSplittingData& mask,
                                    const PaintSplitRegionParams& params = {});
// Carries a region into another frame (the cut frame): surface vertices and loop samples.
void paint_split_transform(PaintSplitRegion& region, const Transform3d& trafo);

// ---------------------------------------------------------------------------------------------------------
// Plane style (design 3.3b / 4.3b)
// ---------------------------------------------------------------------------------------------------------

struct PaintSplitPlaneParams
{
    double offset{ 0. };      // mm along the normal (towards the piece)
    double tilt_u_deg{ 0. };  // about the plane's first in-plane axis
    double tilt_v_deg{ 0. };  // about the second
    bool   snap_to_bed{ false }; // force the normal to +-Z
};

struct PaintSplitPlane
{
    bool   valid{ false };
    Vec3d  normal{ Vec3d::UnitZ() };  // points to the piece's side (where most of the selected area lies)
    Vec3d  point{ Vec3d::Zero() };    // on the plane, after Offset
    Vec3d  u{ Vec3d::UnitX() }, v{ Vec3d::UnitY() }; // in-plane basis, u x v = normal
    double max_loop_distance{ 0. };   // of the outer loop's samples from the fitted plane (before Offset/Tilt)
    bool   far_from_boundary{ false };
};

// Fits the plane of component `comp`'s outer loop (PCA of the length-weighted samples), points it at the
// selection and applies Offset / Tilt / Snap.
PaintSplitPlane paint_split_fit_plane(const PaintSplitRegion& region, int comp, const PaintSplitPlaneParams& params = {});
// The closed half-space box on the normal's side of the plane, big enough to contain `bbox` (inflated 1 mm).
indexed_triangle_set paint_split_half_space(const PaintSplitPlane& plane, const BoundingBoxf3& bbox);
// The closed slab |distance| <= kerf / 2 around the plane, over the same extent.
indexed_triangle_set paint_split_slab(const PaintSplitPlane& plane, const BoundingBoxf3& bbox, double kerf);

// ---------------------------------------------------------------------------------------------------------
// The keep-connected rule (design 4.2)
// ---------------------------------------------------------------------------------------------------------

enum class PaintSplitLooseMode {
    AttachToPiece, // D18 default: body-side material hanging only from the piece goes with the piece
    Loose          // ... or it becomes loose parts of its own
};

struct PaintSplitAssignParams
{
    PaintSplitLooseMode loose_mode{ PaintSplitLooseMode::AttachToPiece };
    // A cut component carries the selection when the selected area on its surface is at least this share
    // of the region component's selected area (design 4.2 A2).
    double carry_frac{ 0.01 };
    // Kerf: a closed solid (paint_split_slab) removed from both sides, but only within the dilated hull of
    // the carrying piece, so a returned component never gets a slot through it (design 4.2 C). May be null.
    const indexed_triangle_set* kerf_solid{ nullptr };
    double                      kerf{ 0. };
};

// The re-union measurement (design 4.2 A3, the P1 open question). The cut faces are the faces whose
// provenance is the cutter. When the re-union welds the returned components back seam-free, the body keeps
// exactly the cut faces the piece has (they mate); any excess is a membrane left inside the body.
struct PaintSplitRejoin
{
    int    returned{ 0 };              // piece-side components given back to the body
    int    body_components_before{ 0 }; // body-side components right after the Split
    int    body_components_after{ 0 };  // after the re-union
    double piece_cut_area{ 0. };       // area of cutter faces on the carrying piece
    double body_cut_area_reunion{ 0. }; // ... on the body after the re-union
    double membrane_area{ 0. };        // body_cut_area_reunion - piece_cut_area
    double reunion_volume_delta{ 0. }; // vol(body after union) - vol(body before) - sum vol(returned)
    int    reunion_genus{ 0 };         // genus of the re-unioned body (Manifold::Genus)
    bool   clean{ true };              // membrane_area within tolerance
    bool   fallback_used{ false };     // body = source - piece instead
};

struct PaintSplitPieces
{
    PaintSplitError                   error{ PaintSplitError::None };
    indexed_triangle_set              piece;
    indexed_triangle_set              body;
    std::vector<indexed_triangle_set> loose;
    int                               returned_components{ 0 };
    int                               attached_components{ 0 };
    double                            piece_volume{ 0. }, body_volume{ 0. }, loose_volume{ 0. }, source_volume{ 0. };
    PaintSplitRejoin                  rejoin;
    std::vector<PaintSplitWarning>    warnings;

    bool ok() const { return error == PaintSplitError::None; }
};

// Splits `mesh` (closed; same frame as `region` and `cutter`) by the closed `cutter`. The piece is
// mesh AND cutter (piece_inside) or mesh \ cutter, reduced to the components that carry component `comp` of
// `region`; everything else is the body (plus the loose parts, in Loose mode).
PaintSplitPieces paint_split_assign(const indexed_triangle_set& mesh, const PaintSplitRegion& region, int comp,
                                    const indexed_triangle_set& cutter, bool piece_inside,
                                    const PaintSplitAssignParams& params = {});

// What Cut::perform_with_cutter_solid() needs besides the cutter (design 4.4).
struct PaintSplitKeep
{
    int                     mask_volume_idx{ -1 };    // the model part of the source object the mask is on
    const PaintSplitRegion* region{ nullptr };        // the mask's region, ALREADY in the cut frame
    int                     component{ 0 };           // which region component becomes the piece
    PaintSplitAssignParams  assign;                   // kerf_solid, if any, also in the cut frame
    bool                    cut_only_painted_part{ false }; // D19 (c): other parts stay whole on the body
};

// What the cut did, for the panel's Output Log.
struct PaintSplitReport
{
    PaintSplitError                error{ PaintSplitError::None };
    int                            returned_components{ 0 };
    int                            attached_components{ 0 };
    int                            loose_parts{ 0 };
    int                            other_parts_to_piece{ 0 };
    int                            other_parts_to_body{ 0 };
    PaintSplitRejoin               rejoin;
    std::vector<PaintSplitWarning> warnings;
};

// D19 (a): the share of `mesh`'s volume inside the closed `cutter` (0..1; -1 when the boolean fails). A part
// the mask is not on is not cut; it goes whole to the side holding most of its volume.
double paint_split_inside_fraction(const indexed_triangle_set& mesh, const indexed_triangle_set& cutter);

// ---------------------------------------------------------------------------------------------------------
// Helpers for the gizmo's selection tools
// ---------------------------------------------------------------------------------------------------------

// Pick colour: every facet (and sub-facet) of `state` in `paint` becomes selected in the returned mask.
// Pick segment: with `seed_facet` >= 0 only the region of that state connected (across edges) to the seed.
// The mask is a one-state TriangleSplittingData against the same mesh.
TriangleSelector::TriangleSplittingData paint_split_mask_from_paint(const TriangleMesh& mesh,
                                                                    const TriangleSelector::TriangleSplittingData& paint,
                                                                    EnforcerBlockerType state, int seed_facet = -1);
// The selected area of a one-state mask on `mesh` (mesh units squared).
double paint_split_mask_area(const TriangleMesh& mesh, const TriangleSelector::TriangleSplittingData& mask);

} // namespace Slic3r
