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

#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {

// Refusals and failures. paint_split_error_message() gives the (translatable) text; nothing here throws.
enum class PaintSplitError {
    None,
    OpenMesh,      // no longer returned: open parts are repaired while splitting (RepairFailed when that fails)
    NonManifold,   // closed, but Manifold does not accept it (self-touching, non-manifold edges)
    EmptyMask,     // nothing selected
    NoBoundary,    // a component covers a whole closed shell: there is no boundary to cut along
    Band,          // no longer returned: a band is cut as one cut per edge (owner 2026-11-25, D7 reversed)
    MultiLoop,     // no longer returned: a region with several edges is cut once per edge
    BadComponent,  // the component index is out of range
    BooleanFailed, // Manifold failed during the split
    EmptyPiece,    // no cut component carries the selection (the cutter misses the region)
    EmptyBody,     // the piece would take the whole part
    RepairFailed,  // the part has holes or slivers that the automatic repair could not close
    CutCrossesRegion, // one edge's cut would take most of the selected region away (PaintSplitPieces::bad_loop)
    CutterFailed,     // one edge's follow-paint cut surface could not be built (folds, crosses itself, too short)
    EdgeNotSeparated, // one edge's cut sheet does not part the region from what lies across that edge
    VoxelFailed,      // the nearest-skin field could not be meshed (no OpenVDB in this build, or a broken claim)
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
    Repaired,             // the part had holes or slivers; they were closed for the split (owner 2026-11-25)
};
const char* paint_split_warning_message(PaintSplitWarning w);

// ---------------------------------------------------------------------------------------------------------
// Region and boundary (design 4.1)
// ---------------------------------------------------------------------------------------------------------

// ---------------------------------------------------------------------------------------------------------
// Repair while splitting (owner hand test 2026-11-25: "Repair" first dropped the painting and blocked the
// split). An open part is closed here instead, for the split only: the volume's own mesh and painting are not
// touched, and the halves take the painting back by the Cut's spatial remap.
// ---------------------------------------------------------------------------------------------------------

struct PaintSplitRepairStats
{
    bool needed{ false };          // the input was not a closed, welded mesh
    bool ok{ true };               // the output is closed (every edge used by exactly two faces)
    int  welded{ 0 };              // vertices merged by the tolerance weld (float slivers)
    int  degenerate_removed{ 0 };  // faces dropped because the weld collapsed them
    int  holes_filled{ 0 };        // boundary loops closed with a fan
    int  faces_added{ 0 };
    int  nonmanifold_edges{ 0 };   // edges used by more than two faces after the repair (then ok == false)
};

// Welds exact and near-coincident vertices (float slivers), drops the faces that collapse, and closes every
// boundary loop with a fan around its centroid. The kept faces stay in their input order and the added ones
// come after them; `old_to_new` (input facet -> output facet, -1 = dropped) lets a mask follow. Returns
// stats.ok. A closed, welded input comes back unchanged with stats.needed == false.
bool paint_split_repair(const indexed_triangle_set& in, indexed_triangle_set& out, std::vector<int>* old_to_new,
                        PaintSplitRepairStats& stats);
// A one-state mask (or any painting) of the input, carried onto paint_split_repair()'s output: dropped facets
// lose their entries, added facets are unpainted.
TriangleSelector::TriangleSplittingData paint_split_remap_mask(const TriangleSelector::TriangleSplittingData& mask,
                                                               const std::vector<int>& old_to_new);

struct PaintSplitLoop
{
    std::vector<DrawCutSample> pts;   // closed: the last sample connects back to the first
    double                     length{ 0. };
    bool                       is_outer{ false };
    // From the unselected skin just across this edge to the selected skin just inside it (a few facet rings
    // on each side). It says which side of this edge's cut the piece is on, locally, which the whole region
    // cannot say for a band or a hole.
    Vec3d                      inward{ Vec3d::Zero() };
    // Centroids of the unselected facets just across the edge: the cut must part them from the piece.
    std::vector<Vec3d>         across;
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
    std::vector<PaintSplitError> comp_error; // per component: None or NoBoundary (bands and several edges are cut, one cut per edge)
    // The part was open: the region was taken on paint_split_repair()'s output, and the split repairs too.
    bool                  repaired{ false };
    PaintSplitRepairStats repair;

    size_t num_components() const { return area.size(); }
    bool   component_ok(int comp) const
    {
        return error == PaintSplitError::None && comp >= 0 && size_t(comp) < comp_error.size() &&
               comp_error[size_t(comp)] == PaintSplitError::None;
    }
};

// `mesh` is the part's own mesh; `mask` its one-state selection (any painted state counts as selected).
// An open mesh is repaired first (paint_split_repair) and the mask carried onto it; only a failed repair is
// refused (RepairFailed).
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
// The same for any one of the component's loops. With one loop the normal points at the side holding most of
// the selected area (as above); with several it points across that loop, from the unselected skin to the
// selected skin next to it (PaintSplitLoop::inward), so each cut of a band or around a hole faces the right way.
PaintSplitPlane paint_split_fit_loop_plane(const PaintSplitRegion& region, int comp, int loop, const PaintSplitPlaneParams& params = {});
// Every loop's plane of component `comp`, in loop order.
std::vector<PaintSplitPlane> paint_split_fit_planes(const PaintSplitRegion& region, int comp, const PaintSplitPlaneParams& params = {});
// The cut face a plane makes for one loop (the preview of the cut surface): the cross-section of the closed
// `mesh` (same frame as the region) by the plane, keeping only the islands the loop runs round. Triangles
// facing along the plane's normal; empty when the plane misses the part.
indexed_triangle_set paint_split_cap(const indexed_triangle_set& mesh, const PaintSplitPlane& plane, const PaintSplitLoop& loop);
// The closed half-space box on the normal's side of the plane, big enough to contain `bbox` (inflated 1 mm).
indexed_triangle_set paint_split_half_space(const PaintSplitPlane& plane, const BoundingBoxf3& bbox);
// The closed slab |distance| <= kerf / 2 around the plane, over the same extent.
indexed_triangle_set paint_split_slab(const PaintSplitPlane& plane, const BoundingBoxf3& bbox, double kerf);

// ---------------------------------------------------------------------------------------------------------
// Follow paint (design 3.3a / 4.3a; the default since the owner's hand test 2026-11-26): each boundary loop
// becomes a closed draw-cut stroke, and the draw cut's band + flat core is the cut surface. The skin line is the
// paint line; inside, the band runs Depth mm at Lip angle onto a flat core inside the loop's own footprint, and
// only that surface is cut with (as a thin sheet), so the cut never reaches past the loop. Under a flat patch the
// cut is a plug Depth deep instead.
// ---------------------------------------------------------------------------------------------------------

// The thickness of a follow-paint cut sheet, mm. The sheet's material goes to the rest of the part (the rest is
// the part less the piece), so the piece's cut face sits this much short of the rest's: invisible in print.
constexpr double PaintSplitSheetThickness = 0.02;

struct PaintSplitFollowParams
{
    double depth{ 3. };        // mm the band travels in before the flat core (capped so it cannot fold)
    double angle_deg{ 0. };    // the lip angle, -90..90 (0 = a flat shelf; see DrawCutParams::angle_deg)
    double extension{ 2. };    // mm the skirt reaches out past the skin line
    double smoothing{ 0.3 };   // 0..1 of the paint line before it becomes the stroke
    bool   square_to_bed{ false }; // the core forced level (DrawCutDirection::AxisZ)
};

struct PaintSplitFollowCut
{
    PaintSplitError      error{ PaintSplitError::None };
    indexed_triangle_set preview;  // band + skirt + core only: what the gizmo draws
    indexed_triangle_set sheet;    // that surface as a closed sheet PaintSplitSheetThickness thick: what Apply cuts with
    double               depth{ 0. };  // the depth actually used (lowered when the asked one folds)
    bool                 separates{ false }; // the cut parts the part across the loop (false: a plug under a flat patch)
    bool                 ignored{ false };   // a speck of an edge (under 3 mm round): no cut, no error
    int                  stage{ 0 };          // where a failure happened (0 ok, 1 stroke, 2 self-crossing, 3 folds, 4 cutter): diagnostics
};

// `mesh` is the part in the region's frame (closed); `bbox` its bounding box. A wavy edge whose stroke crosses
// itself is retried smoother and coarser. Plugs (a flat patch) go straight in by Depth, with Lip angle as their
// wall's draft; elsewhere Lip angle is the band's slope.
PaintSplitFollowCut paint_split_follow_cut(const indexed_triangle_set& mesh, const PaintSplitLoop& loop,
                                           const PaintSplitFollowParams& params, const BoundingBoxf3& bbox);

// ---------------------------------------------------------------------------------------------------------
// Nearest skin (the owner's choice, 2026-11-26): the piece is the material nearer to the region's selected skin
// than to any other skin of the part, optionally no deeper than Depth. On the skin the cut meets the paint line
// itself (to about a voxel); inside it runs where the two skins are equally near, so it stays local, and bands,
// holes and fused contacts (legs against a sack) need nothing special. The field is sampled on a voxel grid over
// the region's neighbourhood and its zero level set (the "claim") is cut out of the exact mesh.
// ---------------------------------------------------------------------------------------------------------

// R1 (design 8.2 E1, 8.3 S1/S2, D22, D23 a; PaintSplitField.cpp): the same field, sampled coarse to fine only
// near its zero set, meshed by marching tetrahedra with exact roots, refined near the skin and smoothed with the
// seam pinned. `dense` keeps the P2 evaluation (a dense grid meshed by OpenVDB) for comparison.
struct PaintSplitSkinParams
{
    double depth{ 0. };               // mm under the selected skin; 0 = unlimited (to where another skin is nearer)
    double voxel{ 0. };               // mm; 0 = automatic (dense: from max_samples over the claim's box; sparse: 0.2 mm)
    size_t max_samples{ 6000000 };    // dense: the voxel budget of the automatic size
    bool   dense{ false };            // the P2 engine (no smoothing, no near-skin refinement)
    double smoothing{ 0.5 };          // "Smooth cut face", 0..1: Taubin passes = 20 x this (D22)
    size_t max_cells{ 2000000 };      // sparse: the fine-cell budget; past it the voxel doubles
    int    skin_refine{ 2 };          // sparse: near-skin refinements, each halving the voxel within 2 voxels of the skin
    int    root_iterations{ 4 };      // sparse: regula falsi steps per edge root (0 = linear roots)
    bool   allow_complement{ true };  // sparse: mesh the complement when the selected skin is the larger side
};

// What evaluating one claim cost and gave (the stage timers, design 8.1 step 0).
struct PaintSplitClaimStats
{
    double voxel{ 0. }, near_skin_voxel{ 0. };
    bool   complement{ false };
    size_t cells{ 0 }, samples{ 0 }, queries{ 0 }, triangles{ 0 };
    double setup_s{ 0. }, field_s{ 0. }, mesh_s{ 0. }, smooth_s{ 0. };
};

struct PaintSplitClaim
{
    indexed_triangle_set mesh;                // closed, outward
    bool                 complement{ false }; // the mesh is the claim's complement near the part: piece = part - mesh
    PaintSplitClaimStats stats;
};

// The cutter of component `comp` (same frame as the region), empty on failure.
PaintSplitClaim paint_split_nearest_cutter(const indexed_triangle_set& mesh, const PaintSplitRegion& region, int comp,
                                           const PaintSplitSkinParams& params);
// The claim of component `comp` as a closed mesh (never the complement), empty on failure. `voxel_used`
// receives the voxel size.
indexed_triangle_set paint_split_nearest_claim(const indexed_triangle_set& mesh, const PaintSplitRegion& region, int comp,
                                               const PaintSplitSkinParams& params, double* voxel_used = nullptr);
// The part of the claim's surface that lies inside the part: the cut surface, for the preview.
indexed_triangle_set paint_split_claim_cut_surface(const indexed_triangle_set& mesh, const indexed_triangle_set& claim);

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
    // Several loops: the rest of the part falls into pieces. The one that becomes the body (the part the next
    // region is cut from) is the one holding most of these points (the other regions' selected skin), then the
    // biggest. Points on the source surface, same frame.
    std::vector<Vec3d>          body_hints;
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

// Seconds per stage of one region's split (design 8.1 step 0). The engine fills repair .. cut; the Cut wrapper
// adds paint (exact carry plus finalize()) and wrapper (the rest of perform_with_cutter_solid), and total.
struct PaintSplitTimes
{
    double repair{ 0. }, field{ 0. }, mesh{ 0. }, smooth{ 0. }, cut{ 0. }, wrapper{ 0. }, paint{ 0. }, total{ 0. };
    double engine() const { return repair + field + mesh + smooth + cut; }
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
    int                               bad_loop{ -1 }; // CutCrossesRegion / EmptyPiece: the loop (0-based) whose cut failed
    int                               flipped_loops{ 0 }; // paint_split_assign_loops: cuts taken the other way round (the skin was unclear)
    // paint_split_assign_nearest: per facet of piece / body / each loose part, 1 when it comes from the source mesh
    // (an untouched or seam-crossed skin facet), 0 on the cut face. Exact paint carry (E10) reads it.
    std::vector<uint8_t>              piece_from_source, body_from_source;
    std::vector<std::vector<uint8_t>> loose_from_source;
    PaintSplitTimes                   times;
    PaintSplitClaimStats              claim;

    bool ok() const { return error == PaintSplitError::None; }
};

// Splits `mesh` (closed; same frame as `region` and `cutter`) by the closed `cutter`. The piece is
// mesh AND cutter (piece_inside) or mesh \ cutter, reduced to the components that carry component `comp` of
// `region`; everything else is the body (plus the loose parts, in Loose mode).
PaintSplitPieces paint_split_assign(const indexed_triangle_set& mesh, const PaintSplitRegion& region, int comp,
                                    const indexed_triangle_set& cutter, bool piece_inside,
                                    const PaintSplitAssignParams& params = {});

// A region with several loops (a band, a trim with holes; owner 2026-11-25): one cut per loop, `planes[i]`
// for loop i (each pointing at the piece, paint_split_fit_planes). Cut by cut, the piece keeps the parts on
// the region's side that carry the selection and the parts across other loops' cuts; it loses what lies across
// this loop's own edge. The rest of the part (mesh less the piece) falls into connected pieces: the body is the
// one body_hints picks, the others come back in `loose`. Refused with CutCrossesRegion (and bad_loop) when one
// cut would take most of the selection away. No kerf, no attach rule: everything not in the piece is the rest.
PaintSplitPieces paint_split_assign_loops(const indexed_triangle_set& mesh, const PaintSplitRegion& region, int comp,
                                          const std::vector<PaintSplitPlane>& planes, const PaintSplitAssignParams& params = {});

// Follow paint: one closed thin sheet per loop (PaintSplitFollowCut::sheet; an empty one is skipped). Each sheet
// is cut out of the piece; the parts that still carry the selection stay. A sheet that leaves the skin across its
// edge on the piece is refused (EdgeNotSeparated). Works for one loop too. The rest of the part (mesh less the
// piece, sheets included) falls into connected pieces as above.
PaintSplitPieces paint_split_assign_cutters(const indexed_triangle_set& mesh, const PaintSplitRegion& region, int comp,
                                            const std::vector<indexed_triangle_set>& cutters, const PaintSplitAssignParams& params = {});

// The nearest-skin split: piece = the part AND the claim (paint_split_nearest_claim), reduced to the components that
// carry the selection; the rest of the part falls into connected pieces as above.
PaintSplitPieces paint_split_assign_nearest(const indexed_triangle_set& mesh, const PaintSplitRegion& region, int comp,
                                            const PaintSplitSkinParams& skin, const PaintSplitAssignParams& params = {});

// What Cut::perform_with_cutter_solid() needs besides the cutter (design 4.4).
struct PaintSplitKeep
{
    int                     mask_volume_idx{ -1 };    // the model part of the source object the mask is on
    const PaintSplitRegion* region{ nullptr };        // the mask's region, ALREADY in the cut frame
    int                     component{ 0 };           // which region component becomes the piece
    std::vector<PaintSplitPlane> planes;              // per loop of `component`, cut frame (Cut::perform_with_paint_split_planes)
    std::vector<indexed_triangle_set> cutters;        // or Follow paint's closed cutters per loop (these win when set)
    bool                    nearest_skin{ false };    // the nearest-skin split (wins over both)
    PaintSplitSkinParams    skin;
    PaintSplitAssignParams  assign;                   // kerf_solid, if any, also in the cut frame
    bool                    cut_only_painted_part{ false }; // D19 (c): other parts stay whole on the body
    // E10 / D30 (nearest skin only): paint the pieces by facet provenance (an untouched facet's painting copied bit
    // for bit, a seam-crossed one resampled, the cut face unpainted) and copy uncut parts' painting verbatim,
    // instead of the spatial remap in Cut::finalize(). Mirrored or airbrushed parts keep the remap.
    bool                    exact_paint{ true };
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
    int                            bad_loop{ -1 };     // see PaintSplitPieces::bad_loop
    PaintSplitRejoin               rejoin;
    std::vector<PaintSplitWarning> warnings;
    // Stage timers and what the paint carry did (design 8.1 step 0).
    PaintSplitTimes                times;
    PaintSplitClaimStats           claim;
    double                         piece_volume{ 0. }, body_volume{ 0. }, loose_volume{ 0. };
    bool                           exact_paint{ false };   // the mask part was painted by provenance
    size_t                         paint_copied{ 0 }, paint_resampled{ 0 }, paint_unpainted{ 0 }; // facets
};

// D19 (a): the share of `mesh`'s volume inside the closed `cutter` (0..1; -1 when the boolean fails). A part
// the mask is not on is not cut; it goes whole to the side holding most of its volume.
double paint_split_inside_fraction(const indexed_triangle_set& mesh, const indexed_triangle_set& cutter);
// ... inside all of `cutters` at once (a band's piece is the part between its cuts).
double paint_split_inside_fraction(const indexed_triangle_set& mesh, const std::vector<indexed_triangle_set>& cutters);

// ---------------------------------------------------------------------------------------------------------
// Helpers for the gizmo's selection tools
// ---------------------------------------------------------------------------------------------------------

// Pick colour: every facet (and sub-facet) of `state` in `paint` becomes selected in the returned mask.
// Pick segment: with `seed_facet` >= 0 only the region of that state connected (across edges) to the seed.
// The mask is a one-state TriangleSplittingData against the same mesh. `hit` (mesh coordinates, on the seed
// facet) picks the sub-facet the user clicked; without it the first sub-facet of `state` on the seed is used.
TriangleSelector::TriangleSplittingData paint_split_mask_from_paint(const TriangleMesh& mesh,
                                                                    const TriangleSelector::TriangleSplittingData& paint,
                                                                    EnforcerBlockerType state, int seed_facet = -1,
                                                                    const Vec3f* hit = nullptr);
// The painted state under `hit` (mesh coordinates) on `facet`: NONE where the facet is unpainted there.
EnforcerBlockerType paint_split_state_at(const TriangleMesh& mesh, const TriangleSelector::TriangleSplittingData& paint,
                                         int facet, const Vec3f& hit);
// Two one-state masks on the same mesh combined (Ctrl+click in the pick modes): Union adds `b` to `a`,
// Subtract takes `b` out of `a`. The result is resampled from both, so a brushed mask and a picked patch mix.
enum class PaintSplitMaskOp { Union, Subtract };
TriangleSelector::TriangleSplittingData paint_split_mask_combine(const TriangleMesh& mesh, const TriangleSelector::TriangleSplittingData& a,
                                                                 const TriangleSelector::TriangleSplittingData& b, PaintSplitMaskOp op);

// One click of Pick colour / Pick segment (owner 2026-11-25): the connected patch of the painted state under
// `hit` on `facet`. Without `add` it replaces `mask`; with `add` (Ctrl) it is added to `mask`, or taken out of
// it when the click lands on surface `mask` already selects. `picked` gets the state under the click.
TriangleSelector::TriangleSplittingData paint_split_pick(const TriangleMesh& mesh, const TriangleSelector::TriangleSplittingData& paint,
                                                         const TriangleSelector::TriangleSplittingData& mask, int facet,
                                                         const Vec3f& hit, bool add, EnforcerBlockerType* picked = nullptr);

// The selected area of a one-state mask on `mesh` (mesh units squared).
double paint_split_mask_area(const TriangleMesh& mesh, const TriangleSelector::TriangleSplittingData& mask);

} // namespace Slic3r
