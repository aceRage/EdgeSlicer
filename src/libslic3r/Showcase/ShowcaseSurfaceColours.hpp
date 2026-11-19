#ifndef slic3r_Showcase_ShowcaseSurfaceColours_hpp_
#define slic3r_Showcase_ShowcaseSurfaceColours_hpp_

// Showcase: the colours a part's surface prints in, from more than its own filament and its paint.
//
// A modifier that gives its region another filament (a coloured SVG or text modifier on a jar, a
// box modifier on filament 2, a height range on another filament) changes how the print looks, so
// the presentation render shows it: the part's surface inside the modifier is split off (through
// the paint machinery, TriangleSelector, so the edge follows the modifier closely and there are no
// cracks) and drawn in the modifier's filament. Modifiers that only change settings stay invisible.
//
// The order follows the slicer: the object's filament, then height ranges, then modifiers in the
// object's volume order (a later one wins where they overlap), and painted colour on top - unless
// the modifier has "Override paint" on, then its filament replaces the paint inside it.
//
// GL- and wx-free so the unit tests can check it.

#include "../TriangleMesh.hpp"
#include "../TriangleSelector.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace Slic3r {

class ModelVolume;
class AABBMesh;

namespace Showcase {

// One region that colours the part, in the part's own mesh coordinates.
struct ColourRegion
{
    enum class Kind { Volume, HeightRange };
    Kind kind     = Kind::Volume;
    int  filament = 0;            // 1-based
    bool override_paint = false;  // the modifier's filament wins over painted colour
    // Kind::Volume: the modifier's closed mesh, transformed into the part's mesh coordinates.
    indexed_triangle_set mesh;
    // Kind::HeightRange: [z_min, z_max) in print height (mm above the bed), and the part's mesh to
    // the world (bed) coordinates, for the heights.
    double      z_min = 0., z_max = 0.;
    Transform3d mesh_to_world = Transform3d::Identity();
};

// The filament a volume's config gives the outside of its region: outer wall, wall, or the
// "extruder" key (in that order); 0 = none of its own.
int visible_filament(const ModelVolume &volume);

// The regions of the part's object that colour this part (an instance's matrix places height
// ranges). Modifiers and ranges with no filament of their own, with a filament outside
// 1..num_filaments, or not overlapping the part, are left out. Empty when nothing colours it.
std::vector<ColourRegion> colour_regions_for(const ModelVolume &part, const Transform3d &instance_matrix, size_t num_filaments);

// A value that changes whenever colour_regions_for() would give another result (cache key).
uint64_t colour_regions_key(const ModelVolume &part, const Transform3d &instance_matrix, size_t num_filaments);

// The part's surface split by colour, as TriangleSelector::get_facets(): facets_per_filament[s] are
// the triangles in filament s, s = 0 being the part's own filament. `paint` is its colour painting
// (may be null or empty). edge_limit (mesh units) is the shortest edge split along a region's border.
// T-joints are triangulated, so the pieces meet without cracks.
void surface_colour_facets(const TriangleMesh                             &part_mesh,
                           const TriangleSelector::TriangleSplittingData *paint,
                           const std::vector<ColourRegion>               &regions,
                           float                                          edge_limit,
                           std::vector<indexed_triangle_set>             &facets_per_filament);

// Edge (mesh units) giving about `world_mm` along a region's border on a part placed by `mesh_to_world`.
float surface_colour_edge_limit(const Transform3d &mesh_to_world, float world_mm = 0.1f);

// Is a point (the region's mesh coordinates) inside a closed mesh: the first hit of most of three
// rays is the inside of the mesh. Exposed for the tests.
bool point_inside_closed_mesh(const AABBMesh &mesh, const Vec3d &point);

// Cursor that selects the part of the surface inside one region.
class SurfaceColourCursor : public TriangleSelector::Cursor
{
public:
    explicit SurfaceColourCursor(const ColourRegion &region);
    ~SurfaceColourCursor() override;

    bool is_mesh_point_inside(const Vec3f &point) const override;
    bool is_pointer_in_triangle(const Vec3f &, const Vec3f &, const Vec3f &) const override { return false; }
    int  vertices_inside(const TriangleSelector::Triangle &tr, const std::vector<TriangleSelector::Vertex> &vertices) const override;
    bool is_edge_inside_cursor(const TriangleSelector::Triangle &tr, const std::vector<TriangleSelector::Vertex> &vertices) const override;
    bool is_facet_visible(int, const std::vector<Vec3f> &) const override { return true; }

private:
    struct Impl;
    const ColourRegion   &m_region;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Showcase
} // namespace Slic3r

#endif // slic3r_Showcase_ShowcaseSurfaceColours_hpp_
