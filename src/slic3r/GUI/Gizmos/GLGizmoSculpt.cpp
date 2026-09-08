#include "GLGizmoSculpt.hpp"

#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosCommon.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/MeshUtils.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include "libslic3r/Geometry.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <glad/gl.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace Slic3r::GUI {

// The cursor indicator, shared with nothing - the paint gizmos keep their own.
static std::shared_ptr<GLModel> s_cursor_sphere;

// Throttle for the fallback path: when the vertex buffer cannot be patched in
// place we rebuild the whole GLModel, at most this often.
static constexpr int64_t RenderRebuildIntervalMs = 30;

GLGizmoSculpt::GLGizmoSculpt(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{}

GLGizmoSculpt::~GLGizmoSculpt()
{
    if (s_cursor_sphere != nullptr)
        s_cursor_sphere.reset();
}

bool GLGizmoSculpt::on_init()
{
    m_shortcut_key = 0;

    const wxString ctrl = _L("Ctrl+");
    const wxString shift = _L("Shift+");

    m_desc["brush"]              = _L("Brush");
    m_desc["grab"]               = _L("Grab");
    m_desc["inflate"]            = _L("Inflate");
    m_desc["deflate"]            = _L("Deflate");
    m_desc["smooth"]             = _L("Smooth");
    m_desc["radius"]             = _L("Brush size");
    m_desc["radius_caption"]     = ctrl + _L("Mouse wheel");
    m_desc["strength"]           = _L("Strength");
    m_desc["falloff"]            = _L("Smooth falloff");
    m_desc["taubin"]             = _L("Preserve volume (Taubin)");
    m_desc["sculpt_caption"]     = _L("Left mouse button");
    m_desc["sculpt"]             = _L("Sculpt");
    m_desc["invert_caption"]     = shift + _L("Left mouse button");
    m_desc["invert"]             = _L("Invert the brush");
    m_desc["subdivide"]          = _L("Subdivide");
    m_desc["subdivide_hint"]     = _L("The mesh here is too coarse for this brush size.");
    m_desc["subdivide_warning"]  = _L("Subdividing changes the triangles, so painted supports, seams, colours and fuzzy skin on this part are cleared.");
    m_desc["no_part"]            = _L("Select a single part to sculpt it.");
    m_desc["paint_kept"]         = _L("Sculpting keeps painted supports, seams, colours and fuzzy skin.");

    return true;
}

std::string GLGizmoSculpt::on_get_name() const { return _u8L("Sculpt"); }

std::string GLGizmoSculpt::get_gizmo_entering_text() const { return _u8L("Entering Sculpt gizmo"); }
std::string GLGizmoSculpt::get_gizmo_leaving_text() const { return _u8L("Leaving Sculpt gizmo"); }

CommonGizmosDataID GLGizmoSculpt::on_get_requirements() const
{
    return CommonGizmosDataID(int(CommonGizmosDataID::SelectionInfo) | int(CommonGizmosDataID::Raycaster));
}

bool GLGizmoSculpt::on_is_activable() const
{
    const Selection &selection = m_parent.get_selection();
    if (m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView)
        return false;
    // One part at a time in v1.
    return selection.get_volume_idxs().size() == 1;
}

// ----------------------------------------------------------------------------
// selection / session
// ----------------------------------------------------------------------------

ModelVolume *GLGizmoSculpt::selected_volume(int &object_idx, int &volume_idx, int &mesh_id) const
{
    object_idx = volume_idx = mesh_id = -1;

    const Selection &selection = m_parent.get_selection();
    const Selection::IndicesList &idxs = selection.get_volume_idxs();
    if (idxs.size() != 1)
        return nullptr;
    const GLVolume *gl_volume = selection.get_volume(*idxs.begin());
    if (gl_volume == nullptr)
        return nullptr;

    const GLVolume::CompositeID &cid = gl_volume->composite_id;
    const ModelObjectPtrs &objects = wxGetApp().model().objects;
    if (cid.object_id < 0 || objects.size() <= size_t(cid.object_id))
        return nullptr;
    ModelObject *object = objects[cid.object_id];
    if (cid.volume_id < 0 || object->volumes.size() <= size_t(cid.volume_id))
        return nullptr;
    ModelVolume *mv = object->volumes[cid.volume_id];
    if (!mv->is_model_part())
        return nullptr;

    // The shared Raycaster indexes only the model-part volumes, in order.
    int counter = -1;
    for (int i = 0; i <= cid.volume_id; ++i)
        if (object->volumes[i]->is_model_part())
            ++counter;

    object_idx = cid.object_id;
    volume_idx = cid.volume_id;
    mesh_id    = counter;
    return mv;
}

void GLGizmoSculpt::attach_to_selection()
{
    int object_idx = -1, volume_idx = -1, mesh_id = -1;
    ModelVolume *mv = selected_volume(object_idx, volume_idx, mesh_id);
    if (mv == nullptr) {
        detach();
        return;
    }
    if (m_volume == mv && m_session && m_volume_id == mv->id())
        return;

    m_volume     = mv;
    m_volume_id  = mv->id();
    m_object_idx = object_idx;
    m_volume_idx = volume_idx;
    m_mesh_id    = mesh_id;
    m_session    = std::make_unique<Sculpt::SculptSession>(mv->mesh().its);
    m_pending_commit = false;
    m_stroke_dirty_triangles.clear();
}

void GLGizmoSculpt::detach()
{
    m_volume     = nullptr;
    m_object_idx = m_volume_idx = m_mesh_id = -1;
    m_session.reset();
    m_stroke_active  = false;
    m_pending_commit = false;
    m_stroke_dirty_triangles.clear();
    m_hit_valid   = false;
    m_cursor      = Sculpt::CursorState{};
}

void GLGizmoSculpt::data_changed(bool /* is_serializing */)
{
    if (m_state != On)
        return;
    attach_to_selection();
}

void GLGizmoSculpt::on_set_state()
{
    if (m_state == On) {
        attach_to_selection();
    } else {
        if (m_stroke_active)
            end_stroke();
        detach();
    }
}

Transform3d GLGizmoSculpt::volume_trafo() const
{
    const Selection &selection = m_parent.get_selection();
    const ModelObject *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (mo == nullptr || m_volume == nullptr)
        return Transform3d::Identity();
    const int instance_idx = selection.get_instance_idx();
    if (instance_idx < 0 || size_t(instance_idx) >= mo->instances.size())
        return Transform3d::Identity();
    return mo->instances[instance_idx]->get_transformation().get_matrix() * m_volume->get_matrix();
}

double GLGizmoSculpt::mesh_scale() const
{
    // The brush radius is a world-space length; the mesh lives in the volume's
    // own space. v1 assumes a roughly uniform scale and uses the mean of the
    // three scaling factors (a strongly non-uniform scale makes the brush
    // elliptical, which is a v2 problem).
    const Vec3d s = Geometry::Transformation(volume_trafo()).get_scaling_factor();
    const double mean = (std::abs(s.x()) + std::abs(s.y()) + std::abs(s.z())) / 3.;
    return mean > EPSILON ? mean : 1.;
}

// ----------------------------------------------------------------------------
// stroke
// ----------------------------------------------------------------------------

bool GLGizmoSculpt::raycast(const Vec2d &mouse_position, Vec3f &hit) const
{
    if (m_volume == nullptr || m_mesh_id < 0 || m_c == nullptr || m_c->raycaster() == nullptr)
        return false;
    const std::vector<const MeshRaycaster *> raycasters = m_c->raycaster()->raycasters();
    if (size_t(m_mesh_id) >= raycasters.size())
        return false;

    const Camera &camera = wxGetApp().plater()->get_camera();
    Vec3f normal = Vec3f::Zero();
    size_t facet = 0;
    // No clipping plane: the Sculpt gizmo does not request the ObjectClipper resource.
    return raycasters[m_mesh_id]->unproject_on_mesh(mouse_position, volume_trafo(), camera, hit, normal, nullptr, &facet);
}

bool GLGizmoSculpt::project_on_drag_plane(const Vec2d &mouse_position, Vec3d &out) const
{
    const Camera &camera = wxGetApp().plater()->get_camera();
    const Linef3 ray = m_parent.mouse_ray(Point(int(mouse_position.x()), int(mouse_position.y())));
    const Vec3d dir = ray.b - ray.a;
    const Vec3d n = camera.get_dir_forward();
    const double denom = n.dot(dir);
    if (std::abs(denom) < EPSILON)
        return false;
    const double t = n.dot(m_stroke_anchor_world - ray.a) / denom;
    out = ray.a + t * dir;
    return true;
}

Sculpt::BrushParams GLGizmoSculpt::make_brush(const Vec3f &center_mesh, const Vec3f &displacement_mesh, bool shift_down) const
{
    Sculpt::BrushParams p;
    p.center   = center_mesh;
    p.radius   = float(double(m_cursor_radius) / mesh_scale());
    p.strength = m_strength;
    p.falloff  = m_falloff;

    switch (m_brush) {
    case Brush::Grab:
        p.type         = Sculpt::BrushType::Grab;
        p.displacement = displacement_mesh;
        break;
    case Brush::Inflate:
    case Brush::Deflate:
        p.type    = Sculpt::BrushType::Inflate;
        // A tick moves the surface by a fraction of the brush radius, so the
        // brush feels the same at any size.
        p.amount  = 0.06f * p.radius;
        p.deflate = (m_brush == Brush::Deflate);
        // Shift inverts the brush, the way the paint gizmos use it to erase.
        if (shift_down)
            p.deflate = !p.deflate;
        break;
    case Brush::Smooth:
        p.type       = Sculpt::BrushType::Smooth;
        p.iterations = 1;
        p.taubin     = m_taubin;
        break;
    }
    return p;
}

bool GLGizmoSculpt::start_stroke(const Vec2d &mouse_position, bool shift_down)
{
    if (m_volume == nullptr || !m_session)
        return false;

    Vec3f hit = Vec3f::Zero();
    if (!raycast(mouse_position, hit))
        return false;

    m_stroke_active       = true;
    m_stroke_center_mesh  = hit;
    m_stroke_anchor_world = volume_trafo() * hit.cast<double>();
    m_drag_prev_world     = m_stroke_anchor_world;
    m_stroke_dirty_triangles.clear();
    m_last_render_refresh = 0;

    // Grab needs a drag before it does anything; the other brushes act on the
    // click itself.
    if (m_brush != Brush::Grab)
        continue_stroke(mouse_position, shift_down);
    return true;
}

void GLGizmoSculpt::continue_stroke(const Vec2d &mouse_position, bool shift_down)
{
    if (!m_stroke_active || !m_session || m_volume == nullptr)
        return;

    Vec3f center_mesh = m_stroke_center_mesh;
    Vec3f displacement_mesh = Vec3f::Zero();

    if (m_brush == Brush::Grab) {
        Vec3d world = Vec3d::Zero();
        if (!project_on_drag_plane(mouse_position, world))
            return;
        const Vec3d delta_world = world - m_drag_prev_world;
        m_drag_prev_world = world;
        if (delta_world.squaredNorm() < 1e-12)
            return;
        // World delta -> volume space. The brush centre follows the drag too,
        // so a long pull keeps hold of the same patch.
        const Transform3d trafo = volume_trafo();
        displacement_mesh = (trafo.linear().inverse() * delta_world).cast<float>();
        center_mesh = m_stroke_center_mesh;
        m_stroke_center_mesh += displacement_mesh;
    } else {
        // Inflate / Smooth re-pick the surface under the cursor each tick.
        Vec3f hit = Vec3f::Zero();
        if (raycast(mouse_position, hit))
            center_mesh = hit;
        m_stroke_center_mesh = center_mesh;
    }

    m_session->apply(make_brush(center_mesh, displacement_mesh, shift_down), m_stroke_step);
    if (m_stroke_step.empty())
        return;

    m_pending_commit = true;
    refresh_render_volumes(m_stroke_step.dirty_triangles, false);
}

void GLGizmoSculpt::end_stroke()
{
    m_stroke_active = false;
    if (!m_pending_commit || !m_session || m_volume == nullptr) {
        m_pending_commit = false;
        return;
    }

    // One undo step per stroke: the snapshot is taken here, before the volume is
    // touched, so it records the pre-stroke mesh. This is the same discipline the
    // paint gizmos use (snapshot on button-up, around update_model_object()).
    Plater *plater = wxGetApp().plater();
    Plater::TakeSnapshot snapshot(plater, _u8L("Sculpt"), UndoRedo::SnapshotType::GizmoAction);

    indexed_triangle_set sculpted = m_session->mesh();
    // Deliberately no plater->clear_before_change_mesh(): the indices are
    // untouched, so every painted annotation stays valid.
    const bool committed = Sculpt::commit_sculpted_mesh(*m_volume, std::move(sculpted), /* ensure_on_bed */ true);
    assert(committed);
    (void) committed;

    plater->changed_mesh(m_object_idx);
    wxGetApp().obj_list()->update_item_error_icon(m_object_idx, -1);

    m_pending_commit = false;
    m_stroke_dirty_triangles.clear();

    // The AABB tree and the shared raycaster are both stale now.
    m_session->rebuild_tree();
    if (m_c != nullptr)
        m_c->update(on_get_requirements());
    // set_new_unique_id() bumped the volume id; keep the cache in step instead of
    // throwing the (just rebuilt) session away.
    m_volume_id = m_volume->id();
}

void GLGizmoSculpt::cancel_stroke()
{
    m_stroke_active  = false;
    m_pending_commit = false;
    if (m_volume != nullptr)
        m_session = std::make_unique<Sculpt::SculptSession>(m_volume->mesh().its);
}

// The cursor sphere is repositioned here, from BOTH the hover path and every
// drag tick. Doing it only on wxMouseEvent::Moving() (which wx fires only while
// no button is held) was the v1 bug: once a drag started, wx sent Dragging()
// instead and the sphere stayed frozen at the click point.
void GLGizmoSculpt::update_cursor(const Vec2d &mouse_position)
{
    m_last_mouse = mouse_position;

    Vec3f hit = Vec3f::Zero();
    m_hit_valid = raycast(mouse_position, hit);
    if (m_hit_valid)
        m_hit = hit;

    Sculpt::CursorInput in;
    in.hit_valid = m_hit_valid;
    in.hit       = hit;
    if (m_stroke_active)
        in.mode = (m_brush == Brush::Grab) ? Sculpt::CursorMode::StrokeGrab : Sculpt::CursorMode::StrokeHit;
    // For Grab the stroke centre has already been advanced by the drag, so it is
    // exactly "the anchor moved by the accumulated drag" the cursor should ride.
    in.grab_anchor = m_stroke_center_mesh;

    m_cursor = Sculpt::next_cursor_state(m_cursor, in);
}

bool GLGizmoSculpt::on_mouse(const wxMouseEvent &mouse_event)
{
    const Vec2d mouse_pos(double(mouse_event.GetX()), double(mouse_event.GetY()));

    if (mouse_event.Moving()) {
        update_cursor(mouse_pos);
        m_parent.set_as_dirty();
        return false;
    }

    const bool control_down = mouse_event.CmdDown();

    if (mouse_event.LeftDown()) {
        if (control_down || get_hover_id() != -1)
            return false;
        if (start_stroke(mouse_pos, mouse_event.ShiftDown())) {
            update_cursor(mouse_pos);
            m_parent.set_as_dirty();
            return true;
        }
        update_cursor(mouse_pos);
        return false;
    }
    if (mouse_event.Dragging()) {
        if (!m_stroke_active)
            return false;
        if (control_down) {
            // Ctrl mid-drag ends the stroke, matching the paint gizmos.
            end_stroke();
            return false;
        }
        continue_stroke(mouse_pos, mouse_event.ShiftDown());
        // After the stroke has advanced, so a Grab cursor rides the moved anchor.
        update_cursor(mouse_pos);
        m_parent.set_as_dirty();
        return true;
    }
    if (mouse_event.LeftUp()) {
        if (!m_stroke_active)
            return false;
        end_stroke();
        // The tree was just rebuilt, so this raycast lands on the sculpted
        // surface and the cursor settles onto what the stroke actually made.
        update_cursor(mouse_pos);
        m_parent.set_as_dirty();
        return true;
    }
    if (mouse_event.Leaving()) {
        m_hit_valid = false;
        m_cursor.visible = false;
    }
    return false;
}

bool GLGizmoSculpt::gizmo_event(SLAGizmoEventType action, const Vec2d & /* mouse_position */, bool /* shift_down */, bool /* alt_down */, bool control_down)
{
    if (action == SLAGizmoEventType::MouseWheelUp || action == SLAGizmoEventType::MouseWheelDown) {
        if (!control_down)
            return false;
        m_cursor_radius = (action == SLAGizmoEventType::MouseWheelDown)
                              ? std::max(m_cursor_radius - CursorRadiusStep, CursorRadiusMin)
                              : std::min(m_cursor_radius + CursorRadiusStep, CursorRadiusMax);
        // The panel reads m_cursor_radius straight out of the member every frame,
        // so the slider and its input box follow the wheel with no extra plumbing;
        // this redraw is what makes both the panel and the sphere show it at once.
        m_parent.set_as_dirty();
        return true;
    }
    if (action == SLAGizmoEventType::Escape && m_stroke_active) {
        cancel_stroke();
        m_parent.set_as_dirty();
        return true;
    }
    return false;
}

// ----------------------------------------------------------------------------
// rendering
// ----------------------------------------------------------------------------

void GLGizmoSculpt::refresh_render_volumes(const std::vector<uint32_t> &dirty_triangles, bool force)
{
    if (m_object_idx < 0 || m_volume_idx < 0 || !m_session)
        return;

    // Accumulate for the fallback path, which redraws everything it was told
    // about since the last rebuild.
    m_stroke_dirty_triangles.insert(m_stroke_dirty_triangles.end(), dirty_triangles.begin(), dirty_triangles.end());

    GLVolumeCollection &volumes = m_parent.get_volumes();
    bool partial_ok = true;
    for (GLVolume *v : volumes.volumes) {
        if (v == nullptr || v->composite_id.object_id != m_object_idx || v->composite_id.volume_id != m_volume_idx)
            continue;
        if (!v->model.update_triangles_in_place(m_session->mesh(), dirty_triangles))
            partial_ok = false;
    }
    m_partial_gpu_update = partial_ok;

    if (partial_ok) {
        m_stroke_dirty_triangles.clear();
        return;
    }

    // Fallback: rebuild the whole GLModel, throttled.
    const int64_t now = m_parent.timestamp_now();
    if (!force && now < m_last_render_refresh + RenderRebuildIntervalMs)
        return;
    m_last_render_refresh = now;
    for (GLVolume *v : volumes.volumes) {
        if (v == nullptr || v->composite_id.object_id != m_object_idx || v->composite_id.volume_id != m_volume_idx)
            continue;
        v->model.reset();
        v->model.init_from(m_session->mesh());
    }
    m_stroke_dirty_triangles.clear();
}

void GLGizmoSculpt::render_cursor_sphere() const
{
    if (!m_cursor.visible || m_volume == nullptr)
        return;

    if (s_cursor_sphere == nullptr) {
        s_cursor_sphere = std::make_shared<GLModel>();
        s_cursor_sphere->init_from(its_make_sphere(1.0, double(PI) / 12.0));
    }

    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    const Transform3d trafo = volume_trafo();
    const Transform3d scaling_inverse = Geometry::Transformation(trafo).get_scaling_factor_matrix().inverse();

    // The paint gizmos' cursor colours (GLGizmoPainterBase::get_cursor_hover_color
    // and get_cursor_sphere_left_button_color), so the brush reads the same here.
    ColorRGBA color = m_stroke_active ? ColorRGBA(0.0f, 0.0f, 1.0f, 0.25f) : ColorRGBA(0.0f, 0.0f, 0.0f, 0.25f);

    shader->start_using();
    const Camera &camera = wxGetApp().plater()->get_camera();
    const Transform3d view_model_matrix = camera.get_view_matrix() * trafo *
                                          Geometry::assemble_transform(m_cursor.position.cast<double>()) * scaling_inverse *
                                          Geometry::assemble_transform(Vec3d::Zero(), Vec3d::Zero(), m_cursor_radius * Vec3d::Ones());
    shader->set_uniform("view_model_matrix", view_model_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    const bool is_left_handed = Geometry::Transformation(view_model_matrix).is_left_handed();
    if (is_left_handed)
        glsafe(::glFrontFace(GL_CW));

    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    s_cursor_sphere->set_color(color);
    s_cursor_sphere->render();
    glsafe(::glDisable(GL_BLEND));

    if (is_left_handed)
        glsafe(::glFrontFace(GL_CCW));

    shader->stop_using();
}

void GLGizmoSculpt::on_render()
{
    if (m_volume == nullptr)
        return;
    glsafe(::glEnable(GL_DEPTH_TEST));
    render_cursor_sphere();
}

// ----------------------------------------------------------------------------
// subdivision
// ----------------------------------------------------------------------------

size_t GLGizmoSculpt::subdivided_triangle_count() const
{
    return m_session ? 4 * m_session->triangles_count() : 0;
}

bool GLGizmoSculpt::needs_subdivision()
{
    if (!m_session || !m_hit_valid)
        return false;
    const float radius_mesh = float(double(m_cursor_radius) / mesh_scale());
    const float edge = m_session->local_edge_length(m_hit, radius_mesh);
    if (edge <= 0.f)
        return false;
    return edge > radius_mesh * m_subdivide_ratio;
}

void GLGizmoSculpt::do_subdivide()
{
    if (!m_session || m_volume == nullptr)
        return;
    if (subdivided_triangle_count() > MaxTrianglesAfterSubdivision)
        return;

    Plater *plater = wxGetApp().plater();
    Plater::TakeSnapshot snapshot(plater, _u8L("Subdivide for sculpting"), UndoRedo::SnapshotType::GizmoAction);
    // A subdivision renumbers every facet, so the painted annotations have to go,
    // exactly the way the Simplify gizmo and "Repair by remeshing" handle it.
    plater->clear_before_change_mesh(m_object_idx);

    indexed_triangle_set subdivided = Sculpt::its_subdivide_midpoint(m_session->mesh());
    m_volume->set_mesh(std::move(subdivided));
    m_volume->calculate_convex_hull();
    m_volume->invalidate_convex_hull_2d();
    m_volume->set_new_unique_id();
    m_volume->get_object()->invalidate_bounding_box();
    m_volume->get_object()->ensure_on_bed();

    plater->changed_mesh(m_object_idx);
    wxGetApp().obj_list()->update_item_error_icon(m_object_idx, -1);

    if (m_c != nullptr)
        m_c->update(on_get_requirements());
    m_session.reset();
    attach_to_selection();
}

// ----------------------------------------------------------------------------
// the panel
// ----------------------------------------------------------------------------

void GLGizmoSculpt::on_render_input_window(float x, float y, float bottom_limit)
{
    if (!m_c->selection_info() || !m_c->selection_info()->model_object())
        return;

    const float approx_height = m_imgui->scaled(20.f);
    y = std::min(y, bottom_limit - approx_height);
#if BBS_TOOLBAR_ON_TOP
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
#else
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 1.0f, 0.0f);
#endif

    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiBegin(get_name(), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    if (m_volume == nullptr || !m_session) {
        m_imgui->text(m_desc.at("no_part"));
        GizmoImguiEnd();
        ImGuiWrapper::pop_toolbar_style();
        return;
    }

    // Lay the two sliders out the way the paint gizmos do: the label column is as
    // wide as the widest label plus a gap, the slider sits at a fixed offset, and
    // a small drag-input box for typing an exact value goes at the end of the row.
    // scaled(1.5f) after the text is the gap that keeps the labels off the track -
    // without it the panel reads as cramped, which is what the v1 panel did.
    const float space_size     = m_imgui->get_style_scaling() * 8;
    const float radius_label   = m_imgui->calc_text_size(m_desc.at("radius")).x + m_imgui->scaled(1.5f);
    const float strength_label = m_imgui->calc_text_size(m_desc.at("strength")).x + m_imgui->scaled(1.5f);
    const float sliders_left   = std::max(radius_label, strength_label);
    const float sliders_width  = m_imgui->scaled(7.0f);
    const float slider_icon_width = m_imgui->get_slider_icon_size().x;
    const float drag_left      = ImGui::GetStyle().WindowPadding.x + sliders_left + sliders_width - space_size;

    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc.at("brush"));

    const std::array<std::pair<Brush, const char *>, 4> brushes = {
        std::make_pair(Brush::Grab, "grab"),
        std::make_pair(Brush::Inflate, "inflate"),
        std::make_pair(Brush::Deflate, "deflate"),
        std::make_pair(Brush::Smooth, "smooth")};
    for (size_t i = 0; i < brushes.size(); ++i) {
        if (i != 0)
            ImGui::SameLine();
        if (m_imgui->radio_button(m_desc.at(brushes[i].second), m_brush == brushes[i].first))
            m_brush = brushes[i].first;
    }

    ImGui::Separator();

    // Brush size, in world millimetres - the slider prints the unit, the box at
    // the end takes an exact figure.
    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc.at("radius"));
    ImGui::SameLine(sliders_left);
    ImGui::PushItemWidth(sliders_width);
    m_imgui->bbl_slider_float_style("##sculpt_radius", &m_cursor_radius, CursorRadiusMin, CursorRadiusMax, "%.2f mm", 1.0f, true);
    ImGui::SameLine(drag_left);
    ImGui::PushItemWidth(1.5f * slider_icon_width);
    ImGui::BBLDragFloat("##sculpt_radius_input", &m_cursor_radius, 0.05f, 0.0f, 0.0f, "%.2f");
    // BBLDragFloat does not clamp, so a typed figure has to be brought back into
    // the range the brush and the Ctrl+wheel step both honour.
    m_cursor_radius = std::clamp(m_cursor_radius, CursorRadiusMin, CursorRadiusMax);

    // Strength is the dimensionless 0.05-1 multiplier from BrushParams, shown as
    // a percentage because that is what it reads as to a user.
    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc.at("strength"));
    ImGui::SameLine(sliders_left);
    ImGui::PushItemWidth(sliders_width);
    float strength_pct = m_strength * 100.f;
    if (m_imgui->bbl_slider_float_style("##sculpt_strength", &strength_pct, StrengthMin * 100.f, 100.f, "%.0f%%", 1.0f, true))
        m_strength = strength_pct / 100.f;
    ImGui::SameLine(drag_left);
    ImGui::PushItemWidth(1.5f * slider_icon_width);
    if (ImGui::BBLDragFloat("##sculpt_strength_input", &strength_pct, 1.0f, 0.0f, 0.0f, "%.0f"))
        m_strength = strength_pct / 100.f;
    m_strength = std::clamp(m_strength, StrengthMin, 1.f);

    m_imgui->bbl_checkbox(m_desc.at("falloff"), m_falloff);
    if (m_brush == Brush::Smooth)
        m_imgui->bbl_checkbox(m_desc.at("taubin"), m_taubin);

    ImGui::Separator();

    m_imgui->text(GUI::format_wxstr(_L("%1% triangles"), m_session->triangles_count()));

    if (needs_subdivision()) {
        m_imgui->text(m_desc.at("subdivide_hint"));
        if (subdivided_triangle_count() > MaxTrianglesAfterSubdivision) {
            m_imgui->text(GUI::format_wxstr(_L("Subdividing would exceed %1% triangles."), MaxTrianglesAfterSubdivision));
        } else {
            m_imgui->text(m_desc.at("subdivide_warning"));
            if (m_imgui->button(GUI::format_wxstr(_L("Subdivide to %1% triangles"), subdivided_triangle_count())))
                wxGetApp().CallAfter([this]() { do_subdivide(); });
        }
    } else {
        m_imgui->text(m_desc.at("paint_kept"));
    }

    GizmoImguiEnd();
    ImGuiWrapper::pop_toolbar_style();
}

} // namespace Slic3r::GUI
