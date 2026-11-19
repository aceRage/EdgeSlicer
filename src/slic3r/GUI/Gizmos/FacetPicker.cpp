#include "FacetPicker.hpp"

#include "GLGizmoBase.hpp"
#include "GLGizmosCommon.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MeshUtils.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <glad/gl.h>

#include <limits>

// Ported from BambuStudio #12048 (108e10863). Adapted to this fork's OrcaSlicer-derived
// GL API: shader start_using()/stop_using() (not bind_shader/unbind_shader) and
// GLModel::render() (not render_geometry()); everything else matches 1:1.

namespace Slic3r {
namespace GUI {

Vec3d FacetPicker::world_normal(const Transform3d &trafo, const Vec3f &mesh_normal)
{
    const Vec3d  n   = trafo.linear().inverse().transpose() * mesh_normal.cast<double>();
    const double len = n.norm();
    return (len < EPSILON) ? Vec3d::Zero() : Vec3d(n / len);
}

void FacetPicker::set_active(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    if (!m_active)
        reset();
}

const indexed_triangle_set &FacetPicker::Hit::its() const
{
    return mesh ? mesh->its : volume->mesh().its;
}

void FacetPicker::reset()
{
    m_hit            = Hit();
    m_rendered_facet = -1;
    m_highlight.reset();
    m_region.clear();
    m_region_its   = nullptr;
    m_region_dirty = false;
}

void FacetPicker::set_highlight(std::vector<int> facets, const ColorRGBA &color, size_t max_facets)
{
    if (facets.size() > max_facets)
        facets.resize(max_facets); // the face itself is still whatever the caller grew; only the overlay is capped
    m_region       = std::move(facets);
    m_region_its   = (m_region.empty() || !m_hit.valid()) ? nullptr : &m_hit.its();
    m_region_color = color;
    m_region_dirty = true;
    m_rendered_facet = -1; // back on the single facet, rebuild it
}

bool FacetPicker::update_from_scene(const Vec2d &mouse_position, const Selection &selection, const Camera &camera)
{
    m_hit = Hit();

    const Model *model = selection.get_model();
    if (model == nullptr)
        return false;

    const GLVolume *closest_volume = nullptr;
    Transform3d     closest_trafo  = Transform3d::Identity();
    Vec3f           closest_hit    = Vec3f::Zero(), closest_normal = Vec3f::Zero();
    size_t          closest_facet  = 0;
    double          closest_sq     = std::numeric_limits<double>::max();

    for (unsigned int idx : selection.get_volume_idxs()) {
        const GLVolume *v = selection.get_volume(idx);
        if (v == nullptr || v->is_modifier || v->is_wipe_tower || !v->mesh_raycaster)
            continue;
        const int object_idx = v->object_idx();
        const int volume_idx = v->volume_idx();
        if (object_idx < 0 || object_idx >= int(model->objects.size()))
            continue;
        const ModelObject *mo = model->objects[object_idx];
        if (volume_idx < 0 || volume_idx >= int(mo->volumes.size()) || !mo->volumes[volume_idx]->is_model_part())
            continue;

        // The same matrix the scene raycaster places this volume with.
        const Transform3d trafo = v->world_matrix();
        Vec3f  hit = Vec3f::Zero(), normal = Vec3f::Zero();
        size_t facet = 0;
        bool   ok    = v->mesh_raycaster->unproject_on_mesh(mouse_position, trafo, camera, hit, normal, nullptr, &facet);
        if (!ok) {
            // unproject_on_mesh() rejects an odd number of crossings as "the camera is inside", which is also what
            // an open or non-manifold mesh gives. Take the nearest crossing then, but not one below the bed, as
            // unproject_on_mesh() would not.
            ok = v->mesh_raycaster->closest_hit(mouse_position, trafo, camera, hit, normal, nullptr, &facet) &&
                 (trafo * hit.cast<double>()).z() >= SINKING_Z_THRESHOLD;
        }
        if (!ok)
            continue;
        const double sq = (camera.get_position() - trafo * hit.cast<double>()).squaredNorm();
        if (sq < closest_sq) {
            closest_sq     = sq;
            closest_volume = v;
            closest_trafo  = trafo;
            closest_hit    = hit;
            closest_normal = normal;
            closest_facet  = facet; // with the nearest hit, never after the loop (see update())
        }
    }

    if (closest_volume == nullptr)
        return false;

    std::shared_ptr<const TriangleMesh> mesh = closest_volume->mesh_raycaster->get_mesh();
    if (!mesh || closest_facet >= mesh->its.indices.size())
        return false;

    const Vec3d n = world_normal(closest_trafo, closest_normal);
    if (n.isZero())
        return false; // degenerate facet

    m_hit.facet        = int(closest_facet);
    m_hit.volume       = model->objects[closest_volume->object_idx()]->volumes[closest_volume->volume_idx()];
    m_hit.trafo        = closest_trafo;
    m_hit.world_pos    = closest_trafo * closest_hit.cast<double>();
    m_hit.world_normal = n;
    m_hit.gl_volume    = closest_volume;
    m_hit.volume_idx   = closest_volume->volume_idx();
    m_hit.mesh         = std::move(mesh);
    return true;
}

bool FacetPicker::update(const Vec2d &mouse_position, const CommonGizmosDataPool *pool, const Selection &selection, const Camera &camera)
{
    m_hit = Hit();

    if (pool == nullptr || pool->raycaster() == nullptr || pool->selection_info() == nullptr)
        return false;

    const ModelObject *mo           = pool->selection_info()->model_object();
    const int          instance_idx = selection.get_instance_idx();
    if (mo == nullptr || instance_idx < 0 || instance_idx >= int(mo->instances.size()))
        return false;

    // Raycaster builds one MeshRaycaster per model-part volume, in mo->volumes order, so
    // build the matching transform list the same way.
    const ModelInstance *mi        = mo->instances[instance_idx];
    const double         sla_shift = pool->selection_info()->get_sla_shift();

    std::vector<Transform3d>         trafo_matrices;
    std::vector<const ModelVolume *> part_volumes;
    for (const ModelVolume *mv : mo->volumes) {
        if (mv->is_model_part()) {
            trafo_matrices.emplace_back(Geometry::translation_transform(sla_shift * Vec3d::UnitZ()) *
                                        mi->get_transformation().get_matrix() * mv->get_matrix());
            part_volumes.emplace_back(mv);
        }
    }

    const std::vector<const MeshRaycaster *> raycasters = pool->raycaster()->raycasters();
    if (raycasters.size() != trafo_matrices.size())
        return false; // hollowed-mesh or stale-cache case; skip rather than mis-index

    Vec3f  hit = Vec3f::Zero(), normal = Vec3f::Zero();
    Vec3f  closest_hit = Vec3f::Zero(), closest_normal = Vec3f::Zero();
    double closest_sq    = std::numeric_limits<double>::max();
    int    closest_id    = -1;
    size_t closest_facet = 0;

    for (int mesh_id = 0; mesh_id < int(trafo_matrices.size()); ++mesh_id) {
        size_t facet = 0;
        if (raycasters[mesh_id]->unproject_on_mesh(mouse_position, trafo_matrices[mesh_id], camera, hit, normal, nullptr, &facet)) {
            const double sq = (camera.get_position() - trafo_matrices[mesh_id] * hit.cast<double>()).squaredNorm();
            if (sq < closest_sq) {
                closest_sq     = sq;
                closest_id     = mesh_id;
                closest_hit    = hit;
                closest_normal = normal;
                // Captured inside the nearest-hit branch on purpose. The equivalent loop in
                // GLGizmoFlatten::update_raycast_cache captures it after the loop, so on a
                // multi-volume object it can index the last-tested volume's facet into the
                // nearest volume's mesh.
                closest_facet  = facet;
            }
        }
    }

    if (closest_id < 0)
        return false;

    const ModelVolume *mv = part_volumes[closest_id];
    if (closest_facet >= mv->mesh().its.indices.size())
        return false;

    const Vec3d n = world_normal(trafo_matrices[closest_id], closest_normal);
    if (n.isZero())
        return false; // degenerate facet

    m_hit.facet        = int(closest_facet);
    m_hit.volume       = mv;
    m_hit.trafo        = trafo_matrices[closest_id];
    m_hit.world_pos    = trafo_matrices[closest_id] * closest_hit.cast<double>();
    m_hit.world_normal = n;
    return true;
}

void FacetPicker::render(const Camera &camera)
{
    if (!m_hit.valid())
        return;

    const bool region = !m_region.empty();
    if (region) {
        // The facets belong to the mesh they were grown on; a hit on another part waits for the caller's next region.
        if (m_region_its != &m_hit.its())
            return;
        if (m_region_dirty) {
            m_region_dirty = false;
            m_highlight.reset();

            const indexed_triangle_set &its  = *m_region_its;
            // Lift off the surface so the highlight wins the depth test against the model.
            const Vec3d                 bias = m_hit.world_normal * 0.05;

            indexed_triangle_set temp_its;
            temp_its.vertices.reserve(3 * m_region.size());
            temp_its.indices.reserve(m_region.size());
            for (int facet : m_region) {
                if (facet < 0 || size_t(facet) >= its.indices.size())
                    continue;
                const auto &tri = its.indices[facet];
                const int   first = int(temp_its.vertices.size());
                for (int i = 0; i < 3; ++i)
                    temp_its.vertices.push_back((m_hit.trafo * its.vertices[tri[i]].cast<double>() + bias).cast<float>());
                temp_its.indices.push_back({ first, first + 1, first + 2 });
            }
            if (!temp_its.indices.empty())
                m_highlight.init_from(temp_its);
        }
    }
    // Rebuild only when the hovered facet actually changes.
    else if (m_rendered_facet != m_hit.facet) {
        m_rendered_facet = m_hit.facet;
        m_highlight.reset();

        const indexed_triangle_set &its = m_hit.its();
        const auto                 &tri = its.indices[m_hit.facet];
        // Lift off the surface so the highlight wins the depth test against the model.
        const Vec3d bias = m_hit.world_normal * 0.05;

        indexed_triangle_set temp_its;
        for (int i = 0; i < 3; ++i) {
            const Vec3d v = m_hit.trafo * its.vertices[tri[i]].cast<double>() + bias;
            temp_its.vertices.push_back(v.cast<float>());
        }
        temp_its.indices.push_back({0, 1, 2});
        m_highlight.init_from(temp_its);
    }

    if (!m_highlight.is_initialized())
        return;

    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDisable(GL_CULL_FACE));
    shader->start_using();
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    // Vertices are already in world space, so the model part of the matrix is identity.
    shader->set_uniform("view_model_matrix", camera.get_view_matrix());
    m_highlight.set_color(region ? m_region_color : GLGizmoBase::FLATTEN_HOVER_COLOR);
    m_highlight.render();
    shader->stop_using();
    glsafe(::glEnable(GL_CULL_FACE));
}

} // namespace GUI
} // namespace Slic3r
