#include "GLGizmoFlatten.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosCommon.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/LayOnFace.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <algorithm>
#include <numeric>

#include <glad/gl.h>
#include <imgui/imgui.h>
#include <wx/utils.h>

namespace Slic3r {
namespace GUI {

namespace {

// Face under cursor: neighbouring facets within this angle of the hit facet are one face.
constexpr double FLATTEN_FACE_MAX_ANGLE_DEG = 0.5;
// Overlay cap for a huge flat face; the face's normal still uses all of it.
constexpr size_t FLATTEN_FACE_MAX_DRAWN_FACETS = 200000;
// AppConfig key of the chosen mode.
constexpr const char *FLATTEN_MODE_CONFIG_KEY = "lay_on_face_mode";

// A recessed face is drawn in the panel's warning colour.
ColorRGBA flatten_recessed_color()
{
    const ColorRGB warning = ColorRGB::WARNING();
    return { warning.r(), warning.g(), warning.b(), 0.75f };
}

// A hint that wraps at `wrap_width`, so a long one does not widen the panel.
void flatten_hint(const wxString &text, float wrap_width, bool warning = false)
{
    if (warning)
        ImGui::PushStyleColor(ImGuiCol_Text, ImGuiWrapper::to_ImVec4(ColorRGB::WARNING()));
    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + wrap_width);
    ImGui::TextUnformatted(into_u8(text).c_str());
    ImGui::PopTextWrapPos();
    if (warning)
        ImGui::PopStyleColor();
}

} // namespace

GLGizmoFlatten::GLGizmoFlatten(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{}

bool GLGizmoFlatten::on_mouse(const wxMouseEvent &mouse_event)
{
    if (mouse_event.Moving() || mouse_event.Dragging() || mouse_event.LeftDown() || mouse_event.LeftUp()) {
        m_alt_down = mouse_event.AltDown();
        sync_mode();
    }

    if (active_mode() == Mode::Face) {
        const Vec2d mouse_pos(double(mouse_event.GetX()), double(mouse_event.GetY()));
        if (mouse_event.Moving())
            update_face_pick(mouse_pos);
        else if (mouse_event.Leaving())
            clear_face_pick();
        else if (mouse_event.LeftDown()) {
            // Pick again exactly where the click is, then lay on that face. The click is ours either way when it is
            // on the object, so that Alt+click does not select a part instead.
            update_face_pick(mouse_pos);
            if (m_picker.hit().valid()) {
                if (m_region.valid() && is_selection_supported())
                    lay_on(m_region.object_normal);
                m_left_down_consumed = true;
                return true;
            }
        }
        else if (mouse_event.LeftUp()) {
            const bool consumed  = m_left_down_consumed;
            m_left_down_consumed = false;
            return consumed;
        }
        return false;
    }

    if (mouse_event.LeftDown()) {
        if (m_hover_id != -1) {
            if (is_selection_supported())
                lay_on(m_planes[m_hover_id].normal);
            m_left_down_consumed = true;
            return true;
        }
    }
    else if (mouse_event.LeftUp()) {
        const bool consumed  = m_left_down_consumed || m_hover_id != -1;
        m_left_down_consumed = false;
        return consumed;
    }

    return false;
}

void GLGizmoFlatten::lay_on(const Vec3d &object_normal)
{
    Selection &selection = m_parent.get_selection();
    // Rotate the object, or only the selected part, so the normal points downward.
    // do_rotate() then drops the object back onto the bed and takes the undo snapshot.
    if (selected_part_idx() >= 0)
        selection.flattening_rotate_part(object_normal);
    else
        selection.flattening_rotate(object_normal);
    m_parent.do_rotate(L("Gizmo-Place on Face"));
    wxGetApp().obj_manipul()->set_dirty();

    // The object has turned: what is under the pointer now is another face.
    clear_face_pick();
    m_hull_points.clear();
    m_hull_points_object = nullptr;
    m_face_pick_dirty    = true;
}

void GLGizmoFlatten::data_changed(bool is_serializing)
{
    const Selection &  selection    = m_parent.get_selection();
    const ModelObject *model_object = nullptr;
    int                instance_id = -1;
    if (selection.is_single_full_instance() ||
        selection.is_from_single_object() ) {
        model_object = selection.get_model()->objects[selection.get_object_idx()];
        instance_id = selection.get_instance_idx();
    }
    set_flattening_data(model_object, instance_id, selected_part_idx());

    // This is called for this gizmo even when it is closed, so do nothing that needs it open, and no GL work
    // (the GL context need not be current here).
    if (get_state() == On) {
        // The face may belong to a part that moved, changed or went away (undo/redo, reload, another selection).
        // Forgetting the region is enough: the pick on the next frame grows a new one and replaces the highlight.
        m_region = FaceRegion();
        m_hull_points.clear();
        m_hull_points_object = nullptr;
        m_face_pick_dirty    = true;
        // Drop the adjacency of meshes nothing else holds any more.
        for (auto it = m_neighbors.begin(); it != m_neighbors.end();)
            it = it->second.first.use_count() <= 1 ? m_neighbors.erase(it) : std::next(it);
    }
}

int GLGizmoFlatten::selected_part_idx() const
{
    // A single model part of a multi-part object. Modifiers and negative volumes are not selectable here:
    // they select as SingleModifier.
    const Selection& selection = m_parent.get_selection();
    if (!selection.is_single_volume())
        return -1;
    const GLVolume*    v  = selection.get_first_volume();
    const ModelObject* mo = selection.get_model()->objects[v->object_idx()];
    const int          idx = v->volume_idx();
    return idx >= 0 && idx < (int)mo->volumes.size() && mo->volumes[idx]->is_model_part() ? idx : -1;
}

bool GLGizmoFlatten::is_selection_supported() const
{
    return m_parent.get_selection().is_single_full_instance() || selected_part_idx() >= 0;
}

bool GLGizmoFlatten::on_init()
{
    m_shortcut_key = WXK_CONTROL_F;
    return true;
}

void GLGizmoFlatten::on_set_state()
{
    if (m_state == On) {
        m_mode            = wxGetApp().app_config->get(FLATTEN_MODE_CONFIG_KEY) == "face" ? Mode::Face : Mode::Hull;
        m_alt_down        = false;
        m_face_pick_dirty = true;
    } else {
        // Nothing of the face mode outlives the gizmo: the adjacency can be large on big meshes.
        m_alt_down = false;
        clear_face_pick();
        m_neighbors.clear();
        m_hull_points.clear();
        m_hull_points_object = nullptr;
        m_left_down_consumed = false;
    }
}

CommonGizmosDataID GLGizmoFlatten::on_get_requirements() const
{
    // Face under cursor casts against the scene's own raycasters, so nothing else is needed (the Raycaster
    // data would copy every part mesh when the gizmo opens).
    return CommonGizmosDataID::SelectionInfo;
}

std::string GLGizmoFlatten::on_get_name() const
{
    return _u8L("Lay on face");
}

bool GLGizmoFlatten::on_is_activable() const
{
    // This is assumed in GLCanvas3D::do_rotate, do not change this
    // without updating that function too.
    return is_selection_supported();
}

GLGizmoFlatten::Mode GLGizmoFlatten::active_mode() const
{
    return (m_mode == Mode::Face || m_alt_down || hull_has_no_faces()) ? Mode::Face : Mode::Hull;
}

void GLGizmoFlatten::set_mode(Mode mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;
    wxGetApp().app_config->set(FLATTEN_MODE_CONFIG_KEY, mode == Mode::Face ? "face" : "hull");
    sync_mode();
}

void GLGizmoFlatten::sync_mode()
{
    const Mode mode = active_mode();
    if (mode == m_casters_mode)
        return;
    if (get_state() == On) {
        // Registers the plane overlays for the hull mode only.
        on_unregister_raycasters_for_picking();
        on_register_raycasters_for_picking();
    }
    m_casters_mode = mode;
    if (mode == Mode::Face)
        m_face_pick_dirty = true;
    else
        clear_face_pick();
    m_parent.request_extra_frame();
}

void GLGizmoFlatten::clear_face_pick()
{
    m_picker.reset();
    m_region = FaceRegion();
}

const std::vector<Vec3i32> &GLGizmoFlatten::neighbors_of(const std::shared_ptr<const TriangleMesh> &mesh)
{
    auto it = m_neighbors.find(mesh.get());
    if (it == m_neighbors.end())
        // Once per part mesh, on the first hover over it; parallel, as it is the one O(mesh) step.
        it = m_neighbors.emplace(mesh.get(), std::make_pair(mesh, its_face_neighbors_par(mesh->its))).first;
    return it->second.second;
}

const std::vector<Vec3d> &GLGizmoFlatten::hull_points()
{
    const ModelObject *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    const int          volume_id = selected_part_idx();
    if (mo != m_hull_points_object || volume_id != m_hull_points_volume) {
        // The part's own hull in part mode, as the part is what turns and drops; the object's otherwise.
        m_hull_points        = mo ? lay_on_face_hull_points(*mo, volume_id) : std::vector<Vec3d>();
        m_hull_points_object = mo;
        m_hull_points_volume = volume_id;
    }
    return m_hull_points;
}

void GLGizmoFlatten::update_face_pick(const Vec2d &mouse_position)
{
    const Camera &camera = wxGetApp().plater()->get_camera();
    if (!is_selection_supported() || !m_picker.update_from_scene(mouse_position, m_parent.get_selection(), camera)) {
        m_parent.request_extra_frame();
        return;
    }
    const FacetPicker::Hit &hit = m_picker.hit();

    // Still on the same face of the same, unmoved part: nothing to do, so hovering inside a face costs one ray.
    if (m_region.valid() && hit.mesh == m_region.mesh && hit.trafo.isApprox(m_region.world) &&
        std::binary_search(m_region.sorted_facets.begin(), m_region.sorted_facets.end(), hit.facet)) {
        m_parent.request_extra_frame();
        return;
    }

    FaceRegion region;
    region.mesh  = hit.mesh;
    region.world = hit.trafo;
    const indexed_triangle_set &its = hit.mesh->its;

    // The flat face around the hit facet, grown on the part's mesh: an affine map keeps a plane a plane, so this
    // holds under any scale or mirror of the part and the instance.
    std::vector<int> facets = grow_flat_face(its, neighbors_of(hit.mesh), hit.facet, FLATTEN_FACE_MAX_ANGLE_DEG);
    Vec3d mesh_normal = flat_face_normal(its, facets);
    if (mesh_normal.isZero())
        mesh_normal = its_face_normal(its, hit.facet).cast<double>().normalized();
    // The user picks the side they see. A facet wound the wrong way (a broken STL, or the inside of an open one)
    // would otherwise lay the face up instead of down.
    const Vec3d world_normal = FacetPicker::world_normal(hit.trafo, mesh_normal.cast<float>());
    if (world_normal.dot(hit.world_pos - camera.get_position()) > 0.)
        mesh_normal = -mesh_normal;

    const GLVolume    &gl_volume     = *hit.gl_volume;
    const Transform3d  volume_matrix = gl_volume.get_volume_transformation().get_matrix();
    region.object_normal = mesh_normal_to_object(volume_matrix, mesh_normal);

    // Recessed: inside the convex hull, so laid on it the face points down but the object rests on its outer points.
    std::vector<Vec3d> face_points;
    face_points.reserve(3 * facets.size());
    for (int facet : facets)
        for (int i = 0; i < 3; ++i)
            face_points.emplace_back(volume_matrix * its.vertices[its.indices[facet][i]].cast<double>());
    region.recess_depth = lay_on_face_recess_depth(hull_points(), face_points, region.object_normal,
                                                   gl_volume.get_instance_transformation().get_matrix_no_offset());
    region.recessed = region.recess_depth > std::max(0.01, 1e-4 * m_size_diagonal);

    region.sorted_facets = facets;
    std::sort(region.sorted_facets.begin(), region.sorted_facets.end());
    m_picker.set_highlight(std::move(facets), region.recessed ? flatten_recessed_color() : GLGizmoBase::FLATTEN_HOVER_COLOR, FLATTEN_FACE_MAX_DRAWN_FACETS);
    m_region = std::move(region);
    m_parent.request_extra_frame();
}

void GLGizmoFlatten::on_render()
{
    // Alt pressed or released without moving the mouse still switches the mode on the next frame.
    if (const bool alt = wxGetKeyState(WXK_ALT); alt != m_alt_down) {
        m_alt_down = alt;
        sync_mode();
    }

    const Selection& selection = m_parent.get_selection();
    if (!is_selection_supported())
        return;

    // The planes are needed in both modes: when there are none, Face under cursor takes over.
    if (this->is_plane_update_necessary())
        update_planes();
    sync_mode();

    if (active_mode() == Mode::Face) {
        if (m_face_pick_dirty) {
            m_face_pick_dirty = false;
            update_face_pick(m_parent.get_local_mouse_position());
        }
        // Depth-tested against the model (lifted a little, and pulled forward), so parts in front of the face hide it.
        glsafe(::glEnable(GL_BLEND));
        glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
        glsafe(::glPolygonOffset(-1.f, -1.f));
        m_picker.render(wxGetApp().plater()->get_camera());
        glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
        glsafe(::glDisable(GL_BLEND));
        return;
    }

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    shader->start_using();
    glsafe(::glClear(GL_DEPTH_BUFFER_BIT));

    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glEnable(GL_BLEND));

    const Transform3d& inst_matrix = selection.get_first_volume()->get_instance_transformation().get_matrix();
    const Camera& camera = wxGetApp().plater()->get_camera();
    const Transform3d model_matrix = Geometry::translation_transform(selection.get_first_volume()->get_sla_shift_z() * Vec3d::UnitZ()) * inst_matrix;
    const Transform3d view_model_matrix = camera.get_view_matrix() * model_matrix;

    shader->set_uniform("view_model_matrix", view_model_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    for (int i = 0; i < (int)m_planes.size(); ++i) {
        m_planes[i].vbo.model.set_color(i == m_hover_id ? GLGizmoBase::FLATTEN_HOVER_COLOR : GLGizmoBase::FLATTEN_COLOR);
        m_planes[i].vbo.model.render();
    }

    glsafe(::glEnable(GL_CULL_FACE));
    glsafe(::glDisable(GL_BLEND));
    shader->stop_using();
}

void GLGizmoFlatten::on_render_input_window(float x, float y, float bottom_limit)
{
    if (!is_selection_supported())
        return;

    dock_setup_next_window(x, y, bottom_limit, 0.f, /*size_to_content=*/true);
    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiBegin(get_name(), dock_window_flags(ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse |
                                                  ImGuiWindowFlags_NoTitleBar));
    if (!dock_render_titlebar(get_name())) {
        GizmoImguiEnd();
        ImGuiWrapper::pop_toolbar_style();
        return;
    }

    const wxString modes_tooltip = _L("Convex hull: the faces the object can rest on.\n"
                                      "Face under cursor: any flat face under the pointer, recessed ones too.\n"
                                      "Hold Alt to use Face under cursor for a moment.");

    // Named locals: bbl_radio_button takes a const char*, so the string has to outlive the call.
    const std::string hull_label = _u8L("Convex hull");
    const std::string face_label = _u8L("Face under cursor");
    if (m_imgui->bbl_radio_button(hull_label.c_str(), m_mode == Mode::Hull))
        set_mode(Mode::Hull);
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(modes_tooltip, ImGui::GetFontSize() * 22.f);
    ImGui::SameLine();
    if (m_imgui->bbl_radio_button(face_label.c_str(), m_mode == Mode::Face))
        set_mode(Mode::Face);
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(modes_tooltip, ImGui::GetFontSize() * 22.f);

    const float wrap_width = std::max(ImGui::GetFontSize() * 18.f, ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x - ImGui::GetStyle().WindowPadding.x);
    ImGui::Separator();

    if (active_mode() == Mode::Hull) {
        flatten_hint(_L("Click a highlighted face to lay the object on it."), wrap_width);
        flatten_hint(_L("Hold Alt to pick any face under the pointer."), wrap_width);
    } else {
        if (m_mode == Mode::Hull && hull_has_no_faces())
            flatten_hint(_L("No resting faces found on the convex hull, so Face under cursor is on."), wrap_width, true);
        else if (m_mode == Mode::Hull)
            flatten_hint(_L("Alt held: Face under cursor. Release Alt to go back."), wrap_width);
        if (m_picker.hit().valid() && m_region.valid() && m_region.recessed)
            flatten_hint(_L("Recessed face: it will face the bed, and the object rests on its outer points."), wrap_width, true);
        else
            flatten_hint(_L("Click a face to lay the object on it."), wrap_width);
    }

    GizmoImguiEnd();
    ImGuiWrapper::pop_toolbar_style();
}

void GLGizmoFlatten::on_register_raycasters_for_picking()
{
    // the gizmo grabbers are rendered on top of the scene, so the raytraced picker should take it into account
    m_parent.set_raycaster_gizmos_on_top(true);

    assert(m_planes_casters.empty());

    // Face under cursor picks the model itself, so the plane overlays must not catch the hover then.
    m_casters_mode = active_mode();
    if (!m_planes.empty() && m_casters_mode == Mode::Hull) {
        const Selection& selection = m_parent.get_selection();
        const Transform3d matrix = Geometry::translation_transform(selection.get_first_volume()->get_sla_shift_z() * Vec3d::UnitZ()) *
            selection.get_first_volume()->get_instance_transformation().get_matrix();

        for (int i = 0; i < (int)m_planes.size(); ++i) {
            m_planes_casters.emplace_back(m_parent.add_raycaster_for_picking(SceneRaycaster::EType::Gizmo, i, *m_planes[i].vbo.mesh_raycaster, matrix));
        }
    }
}

void GLGizmoFlatten::on_unregister_raycasters_for_picking()
{
    m_parent.remove_raycasters_for_picking(SceneRaycaster::EType::Gizmo);
    m_parent.set_raycaster_gizmos_on_top(false);
    m_planes_casters.clear();
}

void GLGizmoFlatten::set_flattening_data(const ModelObject* model_object, int instance_id, int volume_id)
{
    if (model_object != m_old_model_object || instance_id != m_old_instance_id || volume_id != m_old_volume_id) {
        m_planes.clear();
        m_planes_computed = false;
        if (get_state() == On) { // Only touch the raycasters if it's current
            on_unregister_raycasters_for_picking();
        }
    }
}

void GLGizmoFlatten::update_planes()
{
    const ModelObject* mo = m_c->selection_info()->model_object();
    const Transform3d &inst_matrix = mo->instances.front()->get_matrix_no_offset();
    // The candidate faces are shared with the CLI --ground-* options, the rest only prepares them for rendering.
    // With a single part selected, only that part's faces are offered. The gizmo's limits are looser than the
    // CLI's and scale with the printed size, so tiny and scaled-down parts still get faces to click.
    const int volume_id = selected_part_idx();
    m_size_diagonal = lay_on_face_size_diagonal(*mo, volume_id, inst_matrix);
    const LayOnFaceFilter filter = LayOnFaceFilter::for_gizmo(m_size_diagonal);
    std::vector<LayOnFacePlane> planes = volume_id >= 0 ? lay_on_face_planes(*mo->volumes[volume_id], inst_matrix, filter) :
                                                          lay_on_face_planes(*mo, inst_matrix, filter);
    m_planes.clear();
    on_unregister_raycasters_for_picking();

    // Every plane is offered: the scene raycaster has no limit on how many (the old cap of 254 came from colour picking).
    for (LayOnFacePlane& plane : planes) {
        // The outline is convex and lies in the plane frame, where the plane is horizontal.
        Pointf3s& polygon = plane.outline;

        // We will shrink the polygon a little bit so it does not touch the object edges:
        Vec3d centroid = std::accumulate(polygon.begin(), polygon.end(), Vec3d(0.0, 0.0, 0.0));
        centroid /= (double)polygon.size();
        for (auto& vertex : polygon)
            vertex = 0.9f*vertex + 0.1f*centroid;

        // Polygon is now simple and convex, we'll round the corners to make them look nicer.
        // The algorithm takes a vertex, calculates middles of respective sides and moves the vertex
        // towards their average (controlled by 'aggressivity'). This is repeated k times.
        // In next iterations, the neighbours are not always taken at the middle (to increase the
        // rounding effect at the corners, where we need it most).
        const unsigned int k = 10; // number of iterations
        const float aggressivity = 0.2f;  // agressivity
        const unsigned int N = polygon.size();
        std::vector<std::pair<unsigned int, unsigned int>> neighbours;
        if (k != 0) {
            Pointf3s points_out(2*k*N); // vector long enough to store the future vertices
            for (unsigned int j=0; j<N; ++j) {
                points_out[j*2*k] = polygon[j];
                neighbours.push_back(std::make_pair((int)(j*2*k-k) < 0 ? (N-1)*2*k+k : j*2*k-k, j*2*k+k));
            }

            for (unsigned int i=0; i<k; ++i) {
                // Calculate middle of each edge so that neighbours points to something useful:
                for (unsigned int j=0; j<N; ++j)
                    if (i==0)
                        points_out[j*2*k+k] = 0.5f * (points_out[j*2*k] + points_out[j==N-1 ? 0 : (j+1)*2*k]);
                    else {
                        float r = 0.2+0.3/(k-1)*i; // the neighbours are not always taken in the middle
                        points_out[neighbours[j].first] = r*points_out[j*2*k] + (1-r) * points_out[neighbours[j].first-1];
                        points_out[neighbours[j].second] = r*points_out[j*2*k] + (1-r) * points_out[neighbours[j].second+1];
                    }
                // Now we have a triangle and valid neighbours, we can do an iteration:
                for (unsigned int j=0; j<N; ++j)
                    points_out[2*k*j] = (1-aggressivity) * points_out[2*k*j] +
                                        aggressivity*0.5f*(points_out[neighbours[j].first] + points_out[neighbours[j].second]);

                for (auto& n : neighbours) {
                    ++n.first;
                    --n.second;
                }
            }
            polygon = points_out; // replace the coarse polygon with the smooth one that we just created
        }


        // Raise a bit above the object surface to avoid flickering. A tenth of a millimetre stands out on a tiny
        // part, so the lift shrinks with the part.
        const double lift = std::min(0.1, 0.01 * m_size_diagonal);
        for (auto& b : polygon)
            b(2) += lift;

        // Transform back to 3D (and also back to mesh coordinates)
        m_planes.emplace_back();
        m_planes.back().normal = plane.normal;
        m_planes.back().area = plane.area;
        m_planes.back().vertices = transform(polygon, inst_matrix.inverse() * plane.to_plane_frame.inverse());
    }

    // Planes are finished - let's save what we calculated it from:
    m_volumes_matrices.clear();
    m_volumes_types.clear();
    for (const ModelVolume* vol : mo->volumes) {
        m_volumes_matrices.push_back(vol->get_matrix());
        m_volumes_types.push_back(vol->type());
    }
    m_first_instance_scale = mo->instances.front()->get_scaling_factor();
    m_first_instance_mirror = mo->instances.front()->get_mirror();
    m_old_model_object = mo;
    m_old_instance_id = m_c->selection_info()->get_active_instance();
    m_old_volume_id = volume_id;
    m_planes_computed = true;
    // The part matrices may have changed, and with them the hull a face is measured against.
    m_hull_points_object = nullptr;

    // And finally create respective VBOs. The polygon is convex with
    // the vertices in order, so triangulation is trivial.
    for (auto& plane : m_planes) {
        indexed_triangle_set its;
        its.vertices.reserve(plane.vertices.size());
        its.indices.reserve(plane.vertices.size() / 3);
        for (size_t i = 0; i < plane.vertices.size(); ++i) {
            its.vertices.emplace_back((Vec3f)plane.vertices[i].cast<float>());
        }
        for (size_t i = 1; i < plane.vertices.size() - 1; ++i) {
            its.indices.emplace_back(0, i, i + 1); // triangle fan
        }

        plane.vbo.model.init_from(its);
        if (Geometry::Transformation(inst_matrix).is_left_handed()) {
            // we need to swap face normals in case the object is mirrored
            // for the raycaster to work properly
            for (stl_triangle_vertex_indices& face : its.indices) {
                if (its_face_normal(its, face).cast<double>().dot(plane.normal) < 0.0)
                    std::swap(face[1], face[2]);
            }
        }
        plane.vbo.mesh_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(std::move(its)));
        // vertices are no more needed, clear memory
        plane.vertices = std::vector<Vec3d>();
    }

    on_register_raycasters_for_picking();
}

bool GLGizmoFlatten::is_plane_update_necessary() const
{
    const ModelObject* mo = m_c->selection_info()->model_object();
    if (m_state != On || ! mo || mo->instances.empty())
        return false;

    // An object without faces is not worked out again every frame: m_planes_computed tells it from "not yet".
    if (! m_planes_computed || mo != m_old_model_object
        || mo->volumes.size() != m_volumes_matrices.size()
        || selected_part_idx() != m_old_volume_id)
        return true;

    // We want to recalculate when the scale changes - some planes could (dis)appear.
    if (! mo->instances.front()->get_scaling_factor().isApprox(m_first_instance_scale)
     || ! mo->instances.front()->get_mirror().isApprox(m_first_instance_mirror))
        return true;

    for (unsigned int i=0; i < mo->volumes.size(); ++i)
        if (! mo->volumes[i]->get_matrix().isApprox(m_volumes_matrices[i])
         || mo->volumes[i]->type() != m_volumes_types[i])
            return true;

    return false;
}

} // namespace GUI
} // namespace Slic3r
