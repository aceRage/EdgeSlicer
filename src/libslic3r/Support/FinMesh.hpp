#pragma once

// Support fins (Fins.hpp) as real geometry: the sections the live fins print, layer by layer, built as a
// closed triangle mesh any slicer can print (the fin bake, FinBake.hpp).
//
// Credit: the technique is Slant3D's "designed-in supports", as specified by Support Fins
// (gittrahan/support-fins, printfins.com, MIT); this is EdgeSlicer's own implementation, and no code was
// taken from that project or from the OrcaSlicer fin pull requests.
//
// Unlike the stabilizers (StabilizerMesh.hpp), a fin has no simple analytic shape: its inner edge follows
// the part's side at the fin gap, layer by layer, and its tines are exactly one object layer tall. So the
// mesh is the live sections extruded: every layer's section (the walls and the tines, unioned) becomes a
// closed prism over that layer's own Z band [print_z - height, print_z], runs of identical sections share
// one prism, and every prism reaches `cap` into the next band so neighbours overlap instead of touching.
// Sliced again at the same layer heights (mid-layer, as every slicer does), the mesh gives back the
// sections it was built from; at another layer height a tine is no longer one layer (the bake dialog
// says so).
//
// The prisms are unioned with Manifold (MeshBoolean::mfd); when that fails the overlapping closed prisms
// are returned as they are, which every slicer unions per layer anyway.
//
// Frame: the planner's - XY in the object's sliced (print) coordinates, Z the height above the bed.

#include "Fins.hpp"

#include "../ExPolygon.hpp"
#include "../TriangleMesh.hpp"

#include <vector>

namespace Slic3r { namespace fins {

// One band of the mesh: a section between two heights.
struct FinMeshLayer
{
    double     bottom_z = 0.;
    double     top_z    = 0.;
    ExPolygons sections;
};

struct FinMeshOptions
{
    // How far each prism reaches into the band above it, mm. Far below half a layer, so a slicing plane in
    // the middle of a layer never sees it.
    double cap          = 0.01;
    // Union the prisms into one solid with Manifold. Off: the overlapping prisms as they are.
    bool   union_shells = true;
};

struct FinMeshReport
{
    size_t bands     = 0;     // layers with a section
    size_t prisms    = 0;     // closed prisms built (runs of identical sections share one)
    size_t dropped   = 0;     // islands that could not be triangulated (left out)
    size_t triangles = 0;
    bool   unioned   = false; // the Manifold union succeeded
    bool   closed    = false; // no open edges
    double volume    = 0.;    // mm^3
};

// How the tines go into the mesh.
struct FinTineOptions
{
    // The tine's width, mm; 0 = the live line width (a one-bead tine, exactly what the live fins print).
    double width        = 0.;
    // Stop every tine at the part's outline instead of `tine_depth` inside it: for fins baked as a separate
    // object, which would otherwise print the overlap twice.
    bool   stop_at_part = false;
};

// The sections a bake builds: each fin layer's walls and tines, unioned, with the tines rebuilt from their
// paths at `tines.width` (and trimmed at the part when `tines.stop_at_part`). One entry per object layer.
std::vector<FinMeshLayer> fin_mesh_layers(const std::vector<FinLayer> &layers, const std::vector<FinSlice> &slices,
                                          const FinSettings &settings, const FinTineOptions &tines = {});

// The bands as one closed mesh: unioned when options.union_shells and the union succeeds, merged as
// separate overlapping prisms otherwise. Empty when no band has a section.
indexed_triangle_set fin_mesh(const std::vector<FinMeshLayer> &bands, const FinMeshOptions &options = {},
                              FinMeshReport *report = nullptr);

}} // namespace Slic3r::fins
