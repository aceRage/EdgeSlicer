#ifndef slic3r_MeshRepair_hpp_
#define slic3r_MeshRepair_hpp_

#include <admesh/stl.h>
#include "TriangleMesh.hpp"

namespace Slic3r {

// Ultra: rebuild the mesh as the zero level-set of its signed distance field
// (OpenVDB). The output is guaranteed watertight and manifold regardless of how
// broken the input is; detail smaller than voxel_size (in mm) is lost.
// Returns an empty set on failure. Implemented in OpenVDBUtils.cpp; this header
// stays free of OpenVDB includes so GUI code can use it cheaply.
indexed_triangle_set remesh_by_voxels(const indexed_triangle_set &mesh, double voxel_size);

// Ultra: "Round all edges" - a whole-mesh fillet of radius `radius` (mm) by the
// morphological round trip on the same distance field: OPEN (erode r, dilate r)
// rounds every convex edge, and when `round_concave` is set CLOSE (dilate r,
// erode r) rounds every concave one as well. See MeshRound.hpp for the why; this
// is only the OpenVDB half, kept here so the header stays OpenVDB-free.
//
// Returns an empty set on failure. Like remesh_by_voxels() it re-extracts the
// surface from a lattice, so indices are renumbered and detail below voxel_size
// is lost.
indexed_triangle_set round_by_voxels(const indexed_triangle_set &mesh,
                                     double                      radius,
                                     double                      voxel_size,
                                     bool                        round_concave);

// Ultra: the LOCALIZED round - the bevel's fallback for a chain the geometric
// construction cannot build (MeshEdit.hpp, BevelStatus::CurvedSurface).
//
// Same rolling-ball fillet as round_by_voxels(), but confined to a BAND around
// `band_points` - in practice the vertices of the selected edge chain. The signed
// distance field is built once, rounded, and then blended back toward the
// ORIGINAL field as the distance from the band's points passes `band_radius`, so:
//
//   * inside band_radius the surface is the rounded one - the fillet the user asked
//     for, on the chain they selected;
//   * beyond band_radius + a feather it is the original surface, to within the
//     voxel size - the rest of the part is not reshaped, which is the whole reason
//     to prefer this over rounding everything;
//   * and it is ONE level set throughout, so the result is watertight and manifold
//     by construction. There is no seam to stitch and no boolean to fail: blending
//     the FIELD rather than cutting and sewing the MESH is what buys that.
//
// The feather is one band_radius wide. A hard switch between two fields leaves a
// step wherever they disagree, which is a crease exactly where the user was
// promised smoothness; a linear ramp over a radius costs nothing and removes it.
//
// `band_points` are in the mesh's own frame. Returns an empty set on failure, and
// on a build without OpenVDB (the stub), which is what makes the refusal path in
// the gizmo still reachable.
indexed_triangle_set round_band_by_voxels(const indexed_triangle_set &mesh,
                                          const std::vector<Vec3f>   &band_points,
                                          double                      band_radius,
                                          double                      radius,
                                          double                      voxel_size);

// True when this build has the OpenVDB target, i.e. when remesh_by_voxels() and
// round_by_voxels() do anything at all. The menu hides the "Round all edges"
// entry when it is false rather than offering something that can only ever fail.
bool voxel_ops_available();

} // namespace Slic3r

#endif // slic3r_MeshRepair_hpp_
