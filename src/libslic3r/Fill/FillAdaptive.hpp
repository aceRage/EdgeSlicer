// Adaptive cubic infill was inspired by the work of @mboerwinkle
// as implemented for Cura.
// https://github.com/Ultimaker/CuraEngine/issues/381
// https://github.com/Ultimaker/CuraEngine/pull/401
//
// Our implementation is more accurate (discretizes a bit less cubes than Cura's)
// by splitting only such cubes which contain a triangle. 
// Our line extraction is time optimal instead of O(n^2) when connecting extracted lines,
// and we also implemented adaptivity for supporting internal overhangs only.

#ifndef slic3r_FillAdaptive_hpp_
#define slic3r_FillAdaptive_hpp_

#include "FillBase.hpp"

#include <vector>

struct indexed_triangle_set;

namespace Slic3r {

class PrintObject;

namespace FillAdaptive
{

struct Octree;
// To keep the definition of Octree opaque, we have to define a custom deleter.
struct OctreeDeleter { void operator()(Octree *p); };
using  OctreePtr = std::unique_ptr<Octree, OctreeDeleter>;

inline bool is_octree_infill_pattern(InfillPattern p) { return p == ipAdaptiveCubic || p == ipSupportCubic; }

// Orca #16295 (Edge: without per-body octrees): one octree per line spacing the regions fill with.
struct RegionOctrees
{
    std::vector<OctreePtr> sets;       // one per distinct (spacing, support cubic)
    std::vector<int>       region_set; // per printing region, -1 = no adaptive/support cubic infill

    Octree *region(size_t region_id) const
    {
        return region_id < region_set.size() && region_set[region_id] >= 0 ? sets[region_set[region_id]].get() : nullptr;
    }
};

// Line spacing of the adaptive or support cubic infill of each region of the object,
// zero for a region that generates no such infill.
std::vector<double>             adaptive_fill_line_spacing(const PrintObject &print_object);

// Rotation of the octree to stand on one of its corners.
Eigen::Quaterniond              transform_to_world();
// Inverse roation of the above.
Eigen::Quaterniond              transform_to_octree();

FillAdaptive::OctreePtr         build_octree(
    // Mesh is rotated to the coordinate system of the octree.
    const indexed_triangle_set  &triangle_mesh,
    // Overhang triangles extracted from fill surfaces with stInternalBridge type, 
    // rotated to the coordinate system of the octree.
    const std::vector<Vec3d>    &overhang_triangles, 
    coordf_t                     line_spacing, 
    // If true, octree is densified below internal overhangs only.
    bool                         support_overhangs_only);

//
// Some of the algorithms used by class FillAdaptive were inspired by
// Cura Engine's class SubDivCube
// https://github.com/Ultimaker/CuraEngine/blob/master/src/infill/SubDivCube.h
//
class Filler : public Slic3r::Fill
{
public:
    ~Filler() override {}

protected:
    Fill* clone() const override { return new Filler(*this); }
	void _fill_surface_single(
	    const FillParams                &params,
	    unsigned int                     thickness_layers,
	    const std::pair<float, Point>   &direction,
	    ExPolygon                        expolygon,
	    Polylines                       &polylines_out) override;
    // Let the G-code export reoder the infill lines.
    //FIXME letting the G-code exporter to reorder infill lines of Adaptive Cubic Infill
    // may not be optimal as the internal infill lines may get extruded before the long infill
    // lines to which the short infill lines are supposed to anchor.
	bool no_sort() const override { return false; }
    bool is_self_crossing() override { return true; }
};

} // namespace FillAdaptive
} // namespace Slic3r

#endif // slic3r_FillAdaptive_hpp_
