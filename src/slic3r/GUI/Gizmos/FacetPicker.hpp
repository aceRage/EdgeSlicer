#ifndef slic3r_FacetPicker_hpp_
#define slic3r_FacetPicker_hpp_

#include "slic3r/GUI/GLModel.hpp"
#include "libslic3r/Point.hpp"

#include <memory>
#include <vector>

namespace Slic3r {
class ModelVolume;
class TriangleMesh;
class GLVolume;

namespace GUI {
class CommonGizmosDataPool;
class Selection;
struct Camera; // declared as a struct in Camera.hpp; matching it avoids MSVC ABI linker errors

// Picks a single triangular facet of the selected object under the mouse and draws it
// highlighted, or a set of facets around it that the caller chose (set_highlight()).
//
// It deliberately knows nothing about what a picked facet is *for* - the caller decides
// that - so it can serve any gizmo that needs "which triangle is under the cursor".
// GLGizmoAdvancedCut uses it to place the cut plane; GLGizmoFlatten's "Face under cursor"
// mode uses it (through update_from_scene()) to find the face to lay the object on.
class FacetPicker
{
public:
    struct Hit
    {
        int                facet        = -1;
        const ModelVolume *volume       = nullptr;
        Transform3d        trafo        = Transform3d::Identity(); // volume -> world
        Vec3d              world_pos    = Vec3d::Zero();
        Vec3d              world_normal = Vec3d::Zero();
        // Set by update_from_scene() only: the GLVolume that was hit, its ModelVolume index, and the mesh its
        // raycaster casts against, which is the one `facet` indexes (the ModelVolume's own, shared).
        const GLVolume                     *gl_volume  = nullptr;
        int                                 volume_idx = -1;
        std::shared_ptr<const TriangleMesh> mesh;

        bool valid() const { return facet >= 0 && volume != nullptr; }
        // The triangles `facet` indexes.
        const indexed_triangle_set &its() const;
    };

    bool is_active() const { return m_active; }
    void set_active(bool active);

    // Drops the cached hit and the highlight geometry.
    void reset();

    // Recomputes the cached hit from the mouse position. Returns true when the ray hits a
    // model part; on a miss the cache is left invalid.
    bool update(const Vec2d &mouse_position, const CommonGizmosDataPool *pool, const Selection &selection, const Camera &camera);

    // Like update(), but casts against the selected GLVolumes' own scene raycasters instead of the
    // CommonGizmosDataPool's, which copies every part mesh and builds a new tree when a gizmo opens.
    // Only the model parts in the selection are tested, so with one part selected only it can be hit.
    // The nearest hit of all parts wins. A part whose ray crosses it an odd number of times (an open or
    // non-manifold mesh, which unproject_on_mesh() rejects) still gives its nearest hit above the bed.
    bool update_from_scene(const Vec2d &mouse_position, const Selection &selection, const Camera &camera);

    // Draws `facets` of the hit's mesh in `color` instead of the hit facet, until the next call. The facets
    // belong to the mesh hit when this is called, and are drawn only while hits stay on that mesh. Passing
    // an empty list goes back to the hit facet. At most `max_facets` are drawn.
    void set_highlight(std::vector<int> facets, const ColorRGBA &color, size_t max_facets = 200000);

    // Draws the cached facet, or the facets given to set_highlight(). No-op when there is no valid hit.
    void render(const Camera &camera);

    const Hit &hit() const { return m_hit; }

    // World-space unit normal of a facet whose mesh-local normal is mesh_normal, for a
    // volume placed by trafo. Returns zero for a degenerate facet.
    //
    // Uses the inverse-transpose of the linear part, not the linear part itself: on a
    // non-uniformly scaled instance the latter yields a normal that is visibly not
    // perpendicular to the facet.
    static Vec3d world_normal(const Transform3d &trafo, const Vec3f &mesh_normal);

private:
    bool    m_active{false};
    Hit     m_hit;
    int     m_rendered_facet{-1}; // so the highlight is rebuilt only when the facet changes
    GLModel m_highlight;

    // set_highlight(): the facets, which mesh they index, their colour, and whether the model needs rebuilding.
    std::vector<int>            m_region;
    const indexed_triangle_set *m_region_its{nullptr};
    ColorRGBA                   m_region_color;
    bool                        m_region_dirty{false};
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_FacetPicker_hpp_
