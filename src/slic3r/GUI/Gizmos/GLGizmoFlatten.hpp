#ifndef slic3r_GLGizmoFlatten_hpp_
#define slic3r_GLGizmoFlatten_hpp_

#include "GLGizmoBase.hpp"
#include "FacetPicker.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "slic3r/GUI/MeshUtils.hpp"

#include <map>
#include <memory>

namespace Slic3r {

enum class ModelVolumeType : int;


namespace GUI {


class GLGizmoFlatten : public GLGizmoBase
{
// This gizmo does not use grabbers. The m_hover_id relates to polygon managed by the class itself.

public:
    // Convex hull: the faces the object can rest on, drawn as overlays to click (the original mode).
    // Face under cursor: any flat face the pointer is over, recessed ones included.
    enum class Mode { Hull, Face };

private:

    struct PlaneData {
        std::vector<Vec3d> vertices; // should be in fact local in update_planes()
        PickingModel vbo;
        Vec3d normal;
        float area;
        int picking_id{ -1 };
    };

    // This holds information to decide whether recalculation is necessary:
    std::vector<Transform3d> m_volumes_matrices;
    std::vector<ModelVolumeType> m_volumes_types;
    Vec3d m_first_instance_scale;
    Vec3d m_first_instance_mirror;

    std::vector<PlaneData> m_planes;
    // The planes have been worked out for the current selection, so an empty m_planes means the hull offers none.
    bool m_planes_computed{ false };
    // Printed size (bounding box diagonal, mm) of what the planes were computed for.
    double m_size_diagonal{ 0. };
    std::vector<std::shared_ptr<SceneRaycasterItem>> m_planes_casters;
    const ModelObject* m_old_model_object = nullptr;
    int                m_old_instance_id{ -1 };
    // The part the planes were computed for, or -1 when they belong to the whole object.
    int                m_old_volume_id{ -1 };

    // ---- modes ----
    Mode m_mode{ Mode::Hull };           // chosen in the panel and remembered in AppConfig
    bool m_alt_down{ false };            // Alt held: Face under cursor for as long as it is
    Mode m_casters_mode{ Mode::Hull };   // the mode the raycasters were last registered for

    // ---- Face under cursor ----
    // The flat face under the pointer, grown from the hit facet on the hit part's own mesh.
    struct FaceRegion {
        std::shared_ptr<const TriangleMesh> mesh;          // the mesh the facets index
        Transform3d      world{ Transform3d::Identity() }; // the part's world matrix when the face was grown
        std::vector<int> sorted_facets;                    // to tell cheaply whether a hover is still on it
        Vec3d            object_normal{ Vec3d::Zero() };   // outward, object coordinates: what flattening_rotate*() takes
        double           recess_depth{ 0. };               // mm inside the convex hull; see lay_on_face_recess_depth()
        bool             recessed{ false };

        bool valid() const { return mesh != nullptr && !sorted_facets.empty() && !object_normal.isZero(); }
    };
    FacetPicker m_picker;
    FaceRegion  m_region;
    // Facet adjacency per part mesh, worked out on the first hover over it. The shared pointer keeps the key alive.
    std::map<const TriangleMesh*, std::pair<std::shared_ptr<const TriangleMesh>, std::vector<Vec3i32>>> m_neighbors;
    // Convex hull vertices (object coordinates) of what is laid, for telling recessed faces apart.
    std::vector<Vec3d>  m_hull_points;
    const ModelObject  *m_hull_points_object{ nullptr };
    int                 m_hull_points_volume{ -2 };
    bool                m_face_pick_dirty{ false };   // pick again at the pointer on the next frame
    bool                m_left_down_consumed{ false };

    // The selected part when a single part of a multi-part object is selected, otherwise -1.
    int selected_part_idx() const;
    bool is_selection_supported() const;
    void update_planes();
    bool is_plane_update_necessary() const;

    // The mode in effect: Face under cursor when chosen, while Alt is held, or when the hull offers no faces.
    Mode active_mode() const;
    // True when the hull offers no faces, so Face under cursor is on although Convex hull is chosen.
    bool hull_has_no_faces() const { return m_planes_computed && m_planes.empty(); }
    void set_mode(Mode mode);
    // Re-registers the raycasters when the mode in effect changed: in face mode the plane overlays must not
    // catch the hover.
    void sync_mode();
    void update_face_pick(const Vec2d &mouse_position);
    void clear_face_pick();
    const std::vector<Vec3i32> &neighbors_of(const std::shared_ptr<const TriangleMesh> &mesh);
    const std::vector<Vec3d> &hull_points();
    // Turns the object (or the selected part) so `object_normal` points down, then drops it onto the bed.
    void lay_on(const Vec3d &object_normal);

public:
    GLGizmoFlatten(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);

    void set_flattening_data(const ModelObject* model_object, int instance_id, int volume_id = -1);

    /// <summary>
    /// Apply rotation on select plane
    /// </summary>
    /// <param name="mouse_event">Keep information about mouse click</param>
    /// <returns>Return True when use the information otherwise False.</returns>
    bool on_mouse(const wxMouseEvent &mouse_event) override;

    void data_changed(bool is_serializing) override;
protected:
    bool on_init() override;
    std::string on_get_name() const override;
    bool on_is_activable() const override;
    void on_render() override;
    void on_render_input_window(float x, float y, float bottom_limit) override;
    void on_register_raycasters_for_picking() override;
    void on_unregister_raycasters_for_picking() override;
    void on_set_state() override;
    CommonGizmosDataID on_get_requirements() const override;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoFlatten_hpp_
