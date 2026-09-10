#ifndef slic3r_CurvedCut_hpp_
#define slic3r_CurvedCut_hpp_

#include "Point.hpp"
#include "TriangleMesh.hpp"
#include "BoundingBox.hpp"

#include <vector>

namespace Slic3r {

// ---------------------------------------------------------------------------
// Curved cut, phase 1: a height field z = f(u,v) over the base cut plane.
//
// The cutting surface is NOT a free mesh. It is a single-valued function of the
// two in-plane coordinates, sampled from a coarse control grid. That is what
// makes it self-intersection-free by construction and what makes the zero
// displacement case degrade EXACTLY to the flat plane cut (see
// CurvedCutSheet::is_flat and Cut::perform_with_curved_sheet, which routes a
// flat sheet straight back into perform_with_plane so the output is identical).
//
// Coordinates: the sheet lives in the cut plane's own frame, the same frame
// cut_mesh() slices at z == 0. (u,v) run over [0,1]^2 and map linearly onto
// [-half_size_u, +half_size_u] x [-half_size_v, +half_size_v] in local X and Y;
// f is in millimetres along local Z.
// ---------------------------------------------------------------------------

class CurvedCutSheet
{
public:
    // Control grid resolution limits. The gizmo exposes 3..15; the maths works
    // for any n >= 2 but a 2x2 grid cannot bend, only tilt.
    static const int MinResolution = 3;
    static const int MaxResolution = 15;
    static const int DefaultResolution = 5;
    // Dense sample resolution for the PREVIEW sheet. 64x64 is plenty to look
    // right and cheap to rebuild every drag tick.
    static const int DefaultSamples = 64;
    // Dense sample resolution for the CUT. The sampled sheet is piecewise
    // linear, so it sits below the true surface by a chord sag that falls as
    // 1/N^2: for a 8 mm dome over an 80 mm span that is 0.040 mm at 64x64 but
    // 0.0098 mm at 128x128. The cut face has to hold 0.02 mm, the preview does
    // not, so the cut samples finer than the preview does.
    static const int CutSamples = 128;

    CurvedCutSheet() { reset(DefaultResolution); }
    explicit CurvedCutSheet(int resolution) { reset(resolution); }

    // Zero every displacement, optionally changing the grid resolution.
    void reset(int resolution = -1);

    int  resolution() const { return m_resolution; }
    // Change the control grid resolution, re-sampling the CURRENT surface onto
    // the new grid so the shape is preserved as closely as the new grid can
    // represent it (exactly, when the new grid is a refinement).
    void set_resolution(int resolution);

    // Half extent of the sheet's domain in the cut plane, in mm. The sheet must
    // cover the object's footprint under the cut plane.
    //
    // PHASE 2: the domain is a RECTANGLE, so u and v get their own half extents.
    // half_size() is kept as the square API (it reports the larger of the two,
    // and setting it makes the domain square again), so every caller that only
    // ever wanted "the sheet is this big" still compiles and still means what it
    // used to. Nothing about the height field changes: (u,v) still run over
    // [0,1]^2, they just map onto a rectangle now.
    double half_size() const { return std::max(m_half_size_u, m_half_size_v); }
    void   set_half_size(double hs) { set_half_size(hs, hs); }
    double half_size_u() const { return m_half_size_u; }
    double half_size_v() const { return m_half_size_v; }
    // Change the domain. `resample` re-samples the CURRENT surface onto the new
    // extent the way set_resolution() re-samples onto a new grid: each control
    // point takes the height the OLD surface had at the SAME local (x,y) in mm,
    // clamped at the old domain's border. The surface therefore stays put in the
    // cut plane while the rectangle around it grows or shrinks - which is what
    // "the shape survives a re-fit" has to mean, since a bend the user drew over
    // the part must not slide or scale when the plane is nudged.
    //
    // Without `resample` the control heights are left alone, so the surface is
    // STRETCHED onto the new rectangle - phase 1's set_half_size() behaviour,
    // kept for the callers (and tests) that want exactly that.
    //
    // PHASE 3: re-sampling is IDEMPOTENT. Re-sampling reads the surface through
    // Catmull-Rom and writes control values back, and that round trip is lossy
    // at a different grid phase - so re-sampling from the PREVIOUS re-sample
    // compounds the loss, and a plane drag (which produces a stream of re-fits)
    // used to walk the border away from where the user drew it. The sheet
    // therefore keeps a REFERENCE grid: the last surface an EDIT produced, with
    // the extent it was edited at. Every re-sample reads that reference, never
    // the previously re-sampled values, so N re-fits cost exactly what one costs
    // and returning to an earlier extent returns to that extent's values
    // bit-for-bit. Any edit (grab / smooth / reset / set_values / set_resolution)
    // republishes the reference.
    void set_half_size(double hs_u, double hs_v, bool resample = false);

    // Control point displacement along the plane normal, in mm.
    double  at(int i, int j) const { return m_z[size_t(j) * m_resolution + i]; }
    double& at(int i, int j)       { return m_z[size_t(j) * m_resolution + i]; }
    const std::vector<double>& values() const { return m_z; }
    void set_values(const std::vector<double>& z);

    // The reference extent the current control values were last EDITED at. Equal
    // to the live extent unless a re-sample has moved the domain since.
    double reference_half_size_u() const { return m_ref_half_size_u; }
    double reference_half_size_v() const { return m_ref_half_size_v; }
    // Adopt the current (re-sampled) values as the new reference, i.e. "this is
    // the shape now, forget where it came from". An edit does this implicitly.
    void   commit_reference();

    // (u,v) in [0,1]^2 of control point (i,j).
    double control_u(int i) const { return m_resolution < 2 ? 0.5 : double(i) / double(m_resolution - 1); }
    // Local (x,y) of control point (i,j), in the cut plane frame.
    Vec2d  control_xy(int i, int j) const;
    Vec3d  control_pos(int i, int j) const;

    // The height field. Catmull-Rom (not B-spline) on purpose: it INTERPOLATES
    // its control points, so a control point's own displacement is the surface
    // height there - which is what the sampling proof in the spec's test plan
    // measures, and what makes "all zero -> exactly flat" hold pointwise rather
    // than only approximately.
    double evaluate(double u, double v) const;
    // Same, from a point in the cut plane's local frame.
    double evaluate_local(double x, double y) const;

    // True when every control point is at zero, i.e. the surface is the plane
    // z == 0 and the cut must go through the plain flat path.
    bool is_flat() const;

    // Largest |displacement| over the control grid, in mm.
    double max_displacement() const;

    // --- editing -----------------------------------------------------------
    // Grab: move every control point within `radius` (measured in the plane,
    // in mm) by `delta` mm along the normal, weighted by the Sculpt gizmo's
    // falloff (MeshSculpt's falloff_weight) when `falloff` is set.
    void grab(const Vec2d& center_xy, double radius, double delta, bool falloff = true);
    // One Laplacian smoothing pass over the control grid. `strength` in [0,1]
    // blends between the original value and the 4-neighbour average; when
    // `radius` is positive only points within it (falloff-weighted) move.
    void smooth(double strength = 0.5, const Vec2d* center_xy = nullptr, double radius = 0.0, bool falloff = true);

    // --- sampling ----------------------------------------------------------
    // Dense sheet as a triangle grid in the cut plane's frame, `samples` x
    // `samples` vertices. Used both for the preview and (thickened) for the cut.
    indexed_triangle_set sample_sheet(int samples = DefaultSamples) const;

private:
    // The reference grid + extent every re-sample reads from (see set_half_size).
    // Kept in step with m_z by commit_reference(), which every editing entry
    // point calls.
    void publish_reference();

    int                 m_resolution{DefaultResolution};
    double              m_half_size_u{50.0};
    double              m_half_size_v{50.0};
    std::vector<double> m_z;
    double              m_ref_half_size_u{50.0};
    double              m_ref_half_size_v{50.0};
    std::vector<double> m_ref_z;
};

// ---------------------------------------------------------------------------
// Phase 2: fitting the sheet to the cut's own cross-section.
//
// Phase 1 sized the sheet from the object's bounding-box diagonal, so on
// anything that is not a cube most control points landed in empty space well
// outside the part and only a couple of them did anything. The fit below
// intersects the object with the cut plane and sizes the sheet to THAT outline
// instead, so the handles sit over the material being cut.
// ---------------------------------------------------------------------------

// Half extents (u,v) of `mesh`'s cross-section at z == 0 in the cut plane's own
// frame, plus a margin of max(`margin_rel` * extent, `margin_abs`) on each side.
// `mesh` must already be in the plane frame (the same frame cut_mesh() slices).
//
// Returns false when the plane misses the mesh entirely (no crossing edge), in
// which case the caller should keep the extent it has - an empty cross-section
// is not a reason to collapse the sheet to nothing.
bool curved_cut_fit_extent(const indexed_triangle_set& mesh,
                           double&                     half_size_u,
                           double&                     half_size_v,
                           double                      margin_rel = 0.15,
                           double                      margin_abs = 5.0);

// ---------------------------------------------------------------------------
// Phase 3: fitting the sheet to the WHOLE part, not to the cross-section.
//
// curved_cut_fit_extent() sizes the sheet to the plane's intersection with the
// object. That is the right place for the HANDLES but the wrong extent for the
// CUTTER: outside the sheet's own domain evaluate_local() clamps, so the slab
// extrudes the sheet's RIM height outwards, and a part that is wider above or
// below the plane than it is AT the plane gets sliced by that extruded rim
// rather than by the surface the user drew. Bend the sheet hard enough and the
// rim leaves the part entirely on one side, and that side comes back empty -
// the reported "it removes the smaller part".
//
// The fix is to fit the extent to the bounding box of the WHOLE mesh projected
// onto the plane's (u,v) axes, so no part of the object ever lies outside the
// sheet's own domain and the extruded rim never touches material. The handles
// stay usable because the projection is a superset of the cross-section, not a
// different place - see curved_cut_default_resolution() for keeping the spacing
// sane once the domain covers the whole part.
//
// Returns false only for an empty mesh.
bool curved_cut_fit_projection_extent(const indexed_triangle_set& mesh,
                                      double&                     half_size_u,
                                      double&                     half_size_v,
                                      double                      margin_rel = 0.15,
                                      double                      margin_abs = 5.0);

// The control-grid resolution to use for a domain of these half extents: enough
// points that the spacing lands near `target_spacing` mm, clamped into
// [min_res, MaxResolution]. Fitting to the whole projection makes the domain
// bigger than the cross-section fit did, so a fixed 5 x 5 would spread the
// handles too thin on a large part; this keeps them at a workable pitch.
int curved_cut_default_resolution(double half_size_u,
                                  double half_size_v,
                                  double target_spacing = 10.0,
                                  int    min_res        = 5);

// ---------------------------------------------------------------------------
// Phase 3: which side of the sheet is empty, cheaply.
//
// A pure sign test: for every vertex of `mesh` (in the plane frame), compare its
// local z against the sheet's height there. Any vertex strictly above makes the
// upper side non-empty, any vertex strictly below makes the lower one non-empty.
// This is what the gizmo warns from before the user commits to a cut - it costs
// one pass over the vertices, where the actual answer costs two booleans.
//
// It is a CONSERVATIVE test in the direction that matters: it can only claim a
// side is non-empty when a vertex is on that side, and a mesh with a vertex on
// one side always has material there. (The converse - a side with no vertex but
// with material, from a face crossing the sheet between its vertices - cannot
// happen for a closed mesh: the material on that side is bounded by faces, and
// those faces have vertices.)
//
// `thickness` is the kerf: with t > 0 the test is against the OFFSET faces, so a
// side counts as empty when the kerf has eaten everything that was on it.
// ---------------------------------------------------------------------------
// Phase 3: cut thickness ("kerf"), shared by the flat and the curved cut.
// ---------------------------------------------------------------------------

// The panel's range, in mm. 0 is "no kerf" and is the default; above 20 mm a
// "cut" is really "split into two parts with a big hole between them", which the
// user can get by cutting twice.
static constexpr double CutThicknessMin = 0.0;
static constexpr double CutThicknessMax = 20.0;

// Where the removed band sits relative to the cut surface.
enum class CutThicknessOffset {
    Centred,   // [surface - t/2, surface + t/2]   (the default)
    Above,     // [surface,       surface + t]     - the band is taken from the upper half
    Below      // [surface - t,   surface      ]   - ... from the lower half
};

// The two face offsets a thickness produces along the plane normal, in mm:
// `lo` is where the LOWER half's face sits, `hi` where the UPPER half's does.
// Always lo <= 0 <= hi and hi - lo == thickness (clamped at 0).
void curved_cut_thickness_faces(double thickness, CutThicknessOffset offset, double& lo, double& hi);

void curved_cut_empty_sides(const indexed_triangle_set& mesh,
                            const CurvedCutSheet&       sheet,
                            bool&                       upper_empty,
                            bool&                       lower_empty,
                            double                      thickness = 0.0,
                            CutThicknessOffset          offset    = CutThicknessOffset::Centred);

// Signed distance from `pos` (in the cut plane's frame) to the nearest surface
// of `mesh` (also in that frame) along the plane normal, i.e. along local Z.
// Both directions are tried and the NEARER hit wins; the sign is the local-Z
// offset of the hit from `pos`. This is the pure core of the gizmo's
// right-click "snap the handle onto the model" gesture.
//
// Returns false when the ray misses in both directions, in which case the
// caller must leave the control point where it is.
bool curved_cut_snap_distance(const indexed_triangle_set& mesh,
                              const Vec3d&                pos,
                              double&                     distance);

// Build the closed slab that everything below the sheet gets intersected with:
// the sheet, offset DOWN to a floor well below `bbox`, with a rim stitched
// round the boundary so the result is watertight. `bbox` is the object's
// bounding box in the cut plane's frame.
//
// The slab's side walls are vertical (along local Z), never along the sheet
// normal, so a steep sheet cannot make the slab self-intersect.
// `extent`, when positive, is the half extent the slab is BUILT at, which may be
// larger than the sheet's own domain so the slab reaches past the object. The
// sheet is never rescaled to fit: outside its domain evaluate_local() clamps and
// the boundary height is extruded outwards, so widening the slab moves no part of
// the surface that lies over the object.
// `extent` applies to BOTH axes; `extent_v`, when positive, overrides it for v so
// a rectangular sheet can be widened per axis. (A square `extent` still works and
// still means what it did in phase 1.)
// `offset`, in mm along local Z, raises or lowers the whole slab's TOP surface
// (the sheet) without touching the floor, which is what a cut thickness needs:
// the material to remove is the band between sheet - t/2 and sheet + t/2, i.e.
// the difference of two of these slabs. The rim and the floor are unchanged, so
// the result is watertight the same way.
indexed_triangle_set curved_cut_lower_slab(const CurvedCutSheet& sheet, const BoundingBoxf3& bbox, int samples = CurvedCutSheet::CutSamples, double extent = -1.0, double extent_v = -1.0, double offset = 0.0);

// Split `mesh` (already in the cut plane's frame) by the sheet. Returns false
// when both booleans failed. Either output pointer may be null.
// Manifold first, mcut as the fallback - the same chain the flexi joint cut and
// the Mesh Boolean gizmo use.
// `thickness`, in mm, is the KERF: a band of material centred on the sheet is
// removed, so the upper half keeps what is above sheet + t/2 and the lower half
// what is below sheet - t/2. thickness == 0 is the original two-boolean cut,
// bit-for-bit - the offset slabs are only built when t > 0.
bool curved_cut_split(const indexed_triangle_set& mesh,
                      const CurvedCutSheet&       sheet,
                      indexed_triangle_set*       upper,
                      indexed_triangle_set*       lower,
                      int                         samples   = CurvedCutSheet::CutSamples,
                      double                      thickness = 0.0,
                      CutThicknessOffset          offset    = CutThicknessOffset::Centred);

// ---------------------------------------------------------------------------
// Phase 2 fixes: side visibility, as a PURE contract.
//
// The gizmo lets each half of the preview be Visible, Ghost or Hidden, and the
// state reaches the fragment shader as one float per side. Nobody can look at a
// headless build's framebuffer, so the mapping and the draw-order predicate that
// depends on it live here, out of the GUI, where a test can pin them:
//
//   Visible -> 1.0   solid, drawn in the opaque pass with depth writes ON
//   Ghost   -> 0.25  blended, drawn in a SECOND pass with depth writes OFF
//   Hidden  -> < 0   discarded in the shader (a zero alpha would still write
//                    depth and go on occluding, which is the opposite of hiding)
//
// The two-pass split is the fix for "Ghost showed only the cut face": a single
// pass with depth writes off for the WHOLE volume left the solid half with no
// depth buffer either, so its own back faces blended over its front faces and
// all that survived was the cap. See GLVolumeCollection::render.
// ---------------------------------------------------------------------------

enum class CurvedCutSideVisibility { Visible, Ghost, Hidden };

// The per-side alpha uniform (color_clip_side_alpha_1 / _2 in gouraud.fs).
float curved_cut_side_alpha(CurvedCutSideVisibility v);

// True when `alpha` is a GHOST alpha: strictly between fully transparent and
// fully solid. Hidden (negative) is not a ghost - it is a discard - and Visible
// (1.0) is not one either.
bool curved_cut_side_is_ghost(float alpha);

// True when either side is ghosted, i.e. the draw needs the extra blended pass.
bool curved_cut_has_ghost_side(float alpha_1, float alpha_2);

} // namespace Slic3r

#endif /* slic3r_CurvedCut_hpp_ */
