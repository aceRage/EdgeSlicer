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
// [-half_size, +half_size] in local X and Y; f is in millimetres along local Z.
// ---------------------------------------------------------------------------

class CurvedCutSheet
{
public:
    // Control grid resolution limits. The gizmo exposes 3..9; the maths works
    // for any n >= 2 but a 2x2 grid cannot bend, only tilt.
    static const int MinResolution = 3;
    static const int MaxResolution = 9;
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

    // Half extent of the sheet's square domain in the cut plane, in mm. The
    // sheet must cover the object's footprint under the cut plane.
    double half_size() const { return m_half_size; }
    void   set_half_size(double hs) { m_half_size = std::max(hs, 1e-6); }

    // Control point displacement along the plane normal, in mm.
    double  at(int i, int j) const { return m_z[size_t(j) * m_resolution + i]; }
    double& at(int i, int j)       { return m_z[size_t(j) * m_resolution + i]; }
    const std::vector<double>& values() const { return m_z; }
    void set_values(const std::vector<double>& z);

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
    int                 m_resolution{DefaultResolution};
    double              m_half_size{50.0};
    std::vector<double> m_z;
};

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
indexed_triangle_set curved_cut_lower_slab(const CurvedCutSheet& sheet, const BoundingBoxf3& bbox, int samples = CurvedCutSheet::CutSamples, double extent = -1.0);

// Split `mesh` (already in the cut plane's frame) by the sheet. Returns false
// when both booleans failed. Either output pointer may be null.
// Manifold first, mcut as the fallback - the same chain the flexi joint cut and
// the Mesh Boolean gizmo use.
bool curved_cut_split(const indexed_triangle_set& mesh,
                      const CurvedCutSheet&       sheet,
                      indexed_triangle_set*       upper,
                      indexed_triangle_set*       lower,
                      int                         samples = CurvedCutSheet::CutSamples);

} // namespace Slic3r

#endif /* slic3r_CurvedCut_hpp_ */
