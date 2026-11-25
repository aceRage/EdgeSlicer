#include "GLGizmoPaintSplit.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosCommon.hpp"
#include "slic3r/GUI/Jobs/PaintSplitJob.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include <cereal/types/vector.hpp>

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <glad/gl.h>

namespace Slic3r::GUI {

namespace {
// The mask over the part's own colours: a warm orange, see-through so the paint under it shows.
const ColorRGBA PS_MASK_COLOR(1.f, 0.54f, 0.24f, 0.70f);
const ColorRGBA PS_TINT_COLOR(0.f, 0.59f, 0.53f, 0.35f);
const ColorRGBA PS_PLANE_COLOR(0.f, 0.59f, 0.53f, 0.22f);
const char     *PS_KEEP_AS_PARTS_KEY = "paint_split_keep_as_parts";
} // namespace

GLGizmoPaintSplit::GLGizmoPaintSplit(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id)
    : GLGizmoPainterBase(parent, icon_filename, sprite_id)
{
}

bool GLGizmoPaintSplit::on_init()
{
    const wxString shift = _L("Shift+");
    m_desc["select"]        = _L("Select");
    m_desc["brush"]         = _L("Brush");
    m_desc["bucket"]        = _L("Bucket");
    m_desc["smart_fill"]    = _L("Smart fill");
    m_desc["pick_colour"]   = _L("Pick colour");
    m_desc["pick_segment"]  = _L("Pick segment");
    m_desc["cursor_size"]   = _L("Brush size");
    m_desc["fill_angle"]    = _L("Angle");
    m_desc["fill_holes"]    = _L("Fill holes up to");
    m_desc["clear"]         = _L("Clear selection");
    m_desc["style"]         = _L("Cut style");
    m_desc["auto"]          = _L("Auto");
    m_desc["plane"]         = _L("Plane");
    m_desc["offset"]        = _L("Offset");
    m_desc["tilt_u"]        = _L("Tilt 1");
    m_desc["tilt_v"]        = _L("Tilt 2");
    m_desc["snap"]          = _L("Square to bed");
    m_desc["result"]        = _L("Result");
    m_desc["keep_as_parts"] = _L("Keep as parts");
    m_desc["only_painted"]  = _L("Cut only the painted part");
    m_desc["apply"]         = _L("Apply");
    m_desc["erase"]         = shift + _L("Left mouse button") + ": " + _L("Erase");
    return true;
}

std::string GLGizmoPaintSplit::on_get_name() const { return _u8L("Paint Split"); }

wxString GLGizmoPaintSplit::handle_snapshot_action_name(bool shift_down, Button /*button_down*/) const
{
    return shift_down ? _L("Erase Paint Split selection") : _L("Paint Split selection");
}

// ------------------------------------------------------------------------------------------- state

void GLGizmoPaintSplit::on_opening()
{
    m_keep_as_parts   = wxGetApp().app_config->get_bool(PS_KEEP_AS_PARTS_KEY);
    m_close_confirmed = false;
    m_preview_dirty   = true;
}

void GLGizmoPaintSplit::on_shutdown()
{
    m_paint_views.clear();
    m_plane_fill.reset();
    m_plane_outline.reset();
    m_tint.reset();
    m_planes.clear();
    m_region = PaintSplitRegion();
    m_parent.toggle_model_objects_visibility(true);
}

bool GLGizmoPaintSplit::has_selection() const
{
    for (const auto &sel : m_triangle_selectors)
        if (sel && sel->has_facets(EnforcerBlockerType::ENFORCER))
            return true;
    return false;
}

void GLGizmoPaintSplit::on_set_state()
{
    // Never refuses to close: programmatic closes (undo, object replaced or deleted, a repair) must
    // happen at once. The user's own closes (toolbar, Esc) ask first through defer_close().
    if (get_state() == On && !m_was_on) {
        // A fresh session: an undo that re-opens the gizmo loads its mask right after this (on_load).
        m_committed.clear();
        m_picked_filament.clear();
        m_committed_object = 0;
    }
    m_was_on = get_state() == On;
    GLGizmoPainterBase::on_set_state();
    if (get_state() == Off)
        m_close_confirmed = false;
}

bool GLGizmoPaintSplit::defer_close(std::function<void()> close)
{
    if (get_state() != On || !has_selection())
        return false;
    if (m_close_prompt_pending)
        return true; // one question at a time
    m_close_prompt_pending = true;
    wxGetApp().CallAfter([this, close]() {
        MessageDialog dlg(wxGetApp().mainframe, _L("Discard the Paint Split selection?"), _L("Paint Split"),
                          wxICON_QUESTION | wxYES | wxNO);
        const bool yes         = dlg.ShowModal() == wxID_YES;
        m_close_prompt_pending = false;
        // The gizmo may have been closed meanwhile (undo, the object removed): then there is nothing to do.
        if (yes && get_state() == On)
            close();
    });
    return true;
}

void GLGizmoPaintSplit::on_save(cereal::BinaryOutputArchive &ar) const
{
    const int      version = 1;
    const uint64_t object  = uint64_t(m_committed_object);
    ar(version, object, m_committed, m_picked_filament, int(m_mode), int(m_style), m_fill_holes_pct, m_offset, m_tilt_u, m_tilt_v,
       m_snap_to_bed, m_keep_as_parts);
}

void GLGizmoPaintSplit::on_load(cereal::BinaryInputArchive &ar)
{
    int      version = 0, mode = 0, style = 0;
    uint64_t object  = 0;
    ar(version, object, m_loaded, m_picked_filament, mode, style, m_fill_holes_pct, m_offset, m_tilt_u, m_tilt_v, m_snap_to_bed,
       m_keep_as_parts);
    m_loaded_object = size_t(object);
    m_loaded_valid  = true;
    m_mode          = Mode(std::clamp(mode, 0, int(Mode::PickSegment)));
    m_style         = Style(std::clamp(style, 0, int(Style::Plane)));
    m_preview_dirty = true;
    // The base schedules update_from_model_object(), which takes the selectors from m_loaded.
    GLGizmoPainterBase::on_load(ar);
}

bool GLGizmoPaintSplit::paint_differs_from_model() const
{
    if (m_committed.size() != m_triangle_selectors.size())
        return true;
    for (size_t i = 0; i < m_triangle_selectors.size(); ++i)
        if (m_triangle_selectors[i]->serialize() != m_committed[i])
            return true;
    return false;
}

void GLGizmoPaintSplit::update_model_object()
{
    // Nothing goes into the model: the mask is committed to the gizmo's own archive.
    const ModelObject *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    m_committed.resize(m_triangle_selectors.size());
    for (size_t i = 0; i < m_triangle_selectors.size(); ++i)
        m_committed[i] = m_triangle_selectors[i]->serialize();
    m_committed_object = mo != nullptr ? mo->id().id : 0;
    m_picked_filament.resize(m_triangle_selectors.size(), 0);
    for (size_t i = 0; i < m_triangle_selectors.size(); ++i)
        if (!m_triangle_selectors[i]->has_facets(EnforcerBlockerType::ENFORCER))
            m_picked_filament[i] = 0;
    m_preview_dirty = true;
    m_parent.set_as_dirty();
}

void GLGizmoPaintSplit::update_from_model_object(bool /*first_update*/)
{
    wxBusyCursor wait;

    const ModelObject *mo = m_c->selection_info()->model_object();
    m_triangle_selectors.clear();
    m_paint_views.clear();
    if (mo == nullptr)
        return;

    size_t parts = 0;
    for (const ModelVolume *mv : mo->volumes)
        parts += mv->is_model_part();

    // The mask to show: what an undo just loaded, else what this session committed, else nothing.
    std::vector<TriangleSelector::TriangleSplittingData> source;
    if (m_loaded_valid && m_loaded_object == mo->id().id && m_loaded.size() == parts)
        source = m_loaded;
    else if (m_committed_object == mo->id().id && m_committed.size() == parts)
        source = m_committed;
    else
        m_picked_filament.clear();
    m_loaded_valid = false;
    m_loaded.clear();
    source.resize(parts);
    m_picked_filament.resize(parts, 0);

    std::vector<ColorRGBA> extruder_colors = get_extruders_colors();
    if (extruder_colors.empty())
        extruder_colors.push_back(GLVolume::NEUTRAL_COLOR);

    size_t idx = 0;
    for (const ModelVolume *mv : mo->volumes) {
        if (!mv->is_model_part())
            continue;
        const TriangleMesh *mesh = &mv->mesh();
        // The model's own colours, drawn first.
        int extruder_idx = mv->extruder_id() > 0 ? mv->extruder_id() - 1 : 0;
        extruder_idx     = std::min(extruder_idx, int(extruder_colors.size()) - 1);
        std::vector<ColorRGBA> ebt_colors{ extruder_colors[size_t(extruder_idx)] };
        ebt_colors.insert(ebt_colors.end(), extruder_colors.begin(), extruder_colors.end());
        auto view = std::make_unique<TriangleSelectorPatch>(*mesh, ebt_colors, 0.2f);
        const EnforcerBlockerType max_ebt = EnforcerBlockerType(std::min(extruder_colors.size(), size_t(EnforcerBlockerType::ExtruderMax)));
        view->deserialize(mv->mmu_segmentation_facets.get_data(), false, max_ebt);
        view->request_update_render_data();
        m_paint_views.emplace_back(std::move(view));

        // The mask: one state, drawn over it.
        m_triangle_selectors.emplace_back(std::make_unique<TriangleSelectorGUI>(*mesh));
        m_triangle_selectors.back()->deserialize(source[idx], false, EnforcerBlockerType::ENFORCER);
        m_triangle_selectors.back()->request_update_render_data();
        ++idx;
    }
    m_committed        = std::move(source);
    m_committed_object = mo->id().id;
    m_preview_dirty    = true;
}

// ------------------------------------------------------------------------------------------- picking

bool GLGizmoPaintSplit::raycast(const Vec2d &mouse_position, int &mesh_id, size_t &facet, Vec3f &hit) const
{
    mesh_id               = -1;
    const ModelObject *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (mo == nullptr || m_c->raycaster() == nullptr)
        return false;
    const Camera        &camera = wxGetApp().plater()->get_camera();
    const ModelInstance *mi     = mo->instances[m_parent.get_selection().get_instance_idx()];
    const auto           rcs    = m_c->raycaster()->raycasters();
    double               best   = std::numeric_limits<double>::max();
    int                  part   = -1;
    for (const ModelVolume *mv : mo->volumes) {
        if (!mv->is_model_part())
            continue;
        ++part;
        if (size_t(part) >= rcs.size())
            break;
        const Transform3d trafo = mi->get_transformation().get_matrix() * mv->get_matrix();
        Vec3f             h, n;
        size_t            f = 0;
        if (rcs[size_t(part)]->unproject_on_mesh(mouse_position, trafo, camera, h, n, m_c->object_clipper()->get_clipping_plane(), &f)) {
            const double d = (camera.get_position() - trafo * h.cast<double>()).squaredNorm();
            if (d < best) {
                best    = d;
                mesh_id = part;
                facet   = f;
                hit     = h;
            }
        }
    }
    return mesh_id >= 0;
}

void GLGizmoPaintSplit::pick(int mesh_id, size_t facet, const Vec3f &hit, bool clear)
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr || mesh_id < 0 || size_t(mesh_id) >= m_triangle_selectors.size())
        return;
    const ModelVolume *mv   = nullptr;
    int                part = -1;
    for (const ModelVolume *v : mo->volumes)
        if (v->is_model_part() && ++part == mesh_id) {
            mv = v;
            break;
        }
    if (mv == nullptr)
        return;

    const bool colour = m_mode == Mode::PickColour;
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), clear ? _u8L("Clear Paint Split selection") : colour ? _u8L("Pick colour") : _u8L("Pick segment"),
                                  UndoRedo::SnapshotType::GizmoAction);
    TriangleSelectorGUI &sel = *m_triangle_selectors[size_t(mesh_id)];
    m_picked_filament.resize(m_triangle_selectors.size(), 0);
    if (clear) {
        sel.reset();
        m_picked_filament[size_t(mesh_id)] = 0;
    } else {
        const TriangleSelector::TriangleSplittingData &paint = mv->mmu_segmentation_facets.get_data();
        const EnforcerBlockerType state = paint_split_state_at(mv->mesh(), paint, int(facet), hit);
        const TriangleSelector::TriangleSplittingData mask =
            paint_split_mask_from_paint(mv->mesh(), paint, state, colour ? -1 : int(facet), &hit);
        sel.deserialize(mask, true, EnforcerBlockerType::ENFORCER);
        // D6: the piece takes the picked filament; the unpainted state is the part's own.
        int filament = int(state);
        if (state == EnforcerBlockerType::NONE)
            filament = mv->extruder_id() > 0 ? mv->extruder_id() : 0;
        m_picked_filament[size_t(mesh_id)] = filament;
    }
    sel.request_update_render_data(true);
    update_model_object();
}

void GLGizmoPaintSplit::clear_selection()
{
    if (!has_selection())
        return;
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Clear Paint Split selection"), UndoRedo::SnapshotType::GizmoAction);
    for (auto &sel : m_triangle_selectors) {
        sel->reset();
        sel->request_update_render_data(true);
    }
    std::fill(m_picked_filament.begin(), m_picked_filament.end(), 0);
    update_model_object();
}

bool GLGizmoPaintSplit::gizmo_event(SLAGizmoEventType action, const Vec2d &mouse_position, bool shift_down, bool alt_down, bool control_down)
{
    if (pick_mode()) {
        switch (action) {
        case SLAGizmoEventType::LeftDown: {
            int    mesh_id = -1;
            size_t facet   = 0;
            Vec3f  hit     = Vec3f::Zero();
            if (!raycast(mouse_position, mesh_id, facet, hit))
                return false; // a click beside the object turns the camera
            pick(mesh_id, facet, hit, shift_down);
            return true;
        }
        case SLAGizmoEventType::LeftUp:
        case SLAGizmoEventType::RightDown:
        case SLAGizmoEventType::RightUp:
        case SLAGizmoEventType::Dragging:
        case SLAGizmoEventType::Moving: return false;
        default: break;
        }
    }
    return GLGizmoPainterBase::gizmo_event(action, mouse_position, shift_down, alt_down, control_down);
}

// ------------------------------------------------------------------------------------------- preview

int GLGizmoPaintSplit::masked_part(int *count) const
{
    int first = -1, n = 0;
    for (size_t i = 0; i < m_triangle_selectors.size(); ++i)
        if (m_triangle_selectors[i]->has_facets(EnforcerBlockerType::ENFORCER)) {
            if (first < 0)
                first = int(i);
            ++n;
        }
    if (count != nullptr)
        *count = n;
    return first;
}

Transform3d GLGizmoPaintSplit::part_to_cut_frame(int part) const
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    const ModelInstance *mi = mo->instances[m_parent.get_selection().get_instance_idx()];
    int idx = -1;
    for (const ModelVolume *mv : mo->volumes)
        if (mv->is_model_part() && ++idx == part)
            return mi->get_transformation().get_matrix_no_offset() * mv->get_matrix();
    return Transform3d::Identity();
}

void GLGizmoPaintSplit::refresh_preview()
{
    const ModelObject *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (mo == nullptr)
        return;
    const Transform3d trafo = mo->instances[m_parent.get_selection().get_instance_idx()]->get_transformation().get_matrix();
    if (!trafo.isApprox(m_preview_trafo)) {
        m_preview_trafo = trafo;
        m_preview_dirty = true;
    }
    if (m_preview_dirty)
        rebuild_preview();
}

void GLGizmoPaintSplit::rebuild_preview()
{
    m_preview_dirty = false;
    m_plane_fill.reset();
    m_plane_outline.reset();
    m_tint.reset();
    m_planes.clear();
    m_region       = PaintSplitRegion();
    m_preview_part = -1;

    const ModelObject *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (mo == nullptr)
        return;
    const int part = masked_part(&m_masked_parts);
    if (part < 0)
        return;
    const ModelVolume *mv  = nullptr;
    int                idx = -1;
    for (const ModelVolume *v : mo->volumes)
        if (v->is_model_part() && ++idx == part) {
            mv = v;
            break;
        }
    if (mv == nullptr)
        return;
    m_preview_part = part;

    const TriangleSelector::TriangleSplittingData mask = m_triangle_selectors[size_t(part)]->serialize();
    PaintSplitRegionParams                        rp;
    rp.fill_holes_pct = m_fill_holes_pct;
    m_region          = paint_split_region(mv->mesh().its, mask, rp);
    if (m_region.error != PaintSplitError::None)
        return;
    const Transform3d to_cut = part_to_cut_frame(part);
    paint_split_transform(m_region, to_cut);
    m_world_offset = mo->instances[m_parent.get_selection().get_instance_idx()]->get_offset();

    PaintSplitPlaneParams pp;
    pp.offset      = m_offset;
    pp.tilt_u_deg  = m_tilt_u;
    pp.tilt_v_deg  = m_tilt_v;
    pp.snap_to_bed = m_snap_to_bed;
    for (size_t c = 0; c < m_region.num_components(); ++c)
        m_planes.push_back(m_region.component_ok(int(c)) ? paint_split_fit_plane(m_region, int(c), pp) : PaintSplitPlane());

    // The plane discs, a little larger than each boundary loop.
    using Geo = GLModel::Geometry;
    Geo fill, outline;
    fill.format    = { Geo::EPrimitiveType::Triangles, Geo::EVertexLayout::P3 };
    outline.format = { Geo::EPrimitiveType::Lines, Geo::EVertexLayout::P3 };
    const int SEG  = 64;
    for (size_t c = 0; c < m_planes.size(); ++c) {
        const PaintSplitPlane &pl = m_planes[c];
        if (!pl.valid)
            continue;
        double r = 0.;
        for (const DrawCutSample &s : m_region.loops[c].front().pts) {
            const Vec3d d = s.pos - pl.point;
            r             = std::max(r, (d - pl.normal * pl.normal.dot(d)).norm());
        }
        r                  = 1.25 * r + 2.;
        const unsigned c0  = unsigned(fill.vertices_count());
        fill.add_vertex(Vec3f(pl.point.cast<float>()));
        for (int k = 0; k < SEG; ++k) {
            const double t = 2. * PI * double(k) / double(SEG);
            fill.add_vertex(Vec3f((pl.point + r * (std::cos(t) * pl.u + std::sin(t) * pl.v)).cast<float>()));
        }
        for (int k = 0; k < SEG; ++k)
            fill.add_triangle(c0, c0 + 1 + unsigned(k), c0 + 1 + unsigned((k + 1) % SEG));
        const unsigned o0 = unsigned(outline.vertices_count());
        for (int k = 0; k < SEG; ++k) {
            const double t = 2. * PI * double(k) / double(SEG);
            outline.add_vertex(Vec3f((pl.point + r * (std::cos(t) * pl.u + std::sin(t) * pl.v)).cast<float>()));
        }
        for (int k = 0; k < SEG; ++k)
            outline.add_line(o0 + unsigned(k), o0 + unsigned((k + 1) % SEG));
    }
    if (fill.vertices_count() > 0) {
        m_plane_fill.init_from(std::move(fill));
        m_plane_fill.set_color(PS_PLANE_COLOR);
    }
    if (outline.vertices_count() > 0) {
        m_plane_outline.init_from(std::move(outline));
        m_plane_outline.set_color(ColorRGBA::ORCA());
    }

    // The tint: the skin that leaves with the piece. A flood over the surface from the selected facets that
    // never crosses a plane - the keep-connected rule as the surface sees it (material joined only through
    // the inside, like a ponytail below the neck line, is not tinted).
    TriangleMesh mesh(mv->mesh());
    mesh.transform(to_cut, true);
    const indexed_triangle_set &its = mesh.its;
    TriangleSelector            scratch(mv->mesh());
    scratch.deserialize(mask, true);
    std::vector<int> src;
    scratch.get_facets_strict(EnforcerBlockerType::ENFORCER, &src);
    std::vector<char> seed(its.indices.size(), 0);
    for (int f : src)
        if (f >= 0 && size_t(f) < seed.size())
            seed[size_t(f)] = 1;
    const std::vector<Vec3i32> neighbors = its_face_neighbors(its);
    std::vector<char>          tinted(its.indices.size(), 0);
    for (const PaintSplitPlane &pl : m_planes) {
        if (!pl.valid)
            continue;
        auto on_piece = [&](size_t f) {
            const Vec3i32 &t = its.indices[f];
            const Vec3d    g = (its.vertices[t[0]] + its.vertices[t[1]] + its.vertices[t[2]]).cast<double>() / 3.;
            return pl.normal.dot(g - pl.point) > 0.;
        };
        std::vector<char>   seen(its.indices.size(), 0);
        std::vector<size_t> queue;
        for (size_t f = 0; f < its.indices.size(); ++f)
            if (seed[f] && on_piece(f)) {
                seen[f] = 1;
                queue.push_back(f);
            }
        while (!queue.empty()) {
            const size_t f = queue.back();
            queue.pop_back();
            tinted[f] = 1;
            for (int k = 0; k < 3; ++k) {
                const int g = neighbors[f][k];
                if (g >= 0 && !seen[size_t(g)] && on_piece(size_t(g))) {
                    seen[size_t(g)] = 1;
                    queue.push_back(size_t(g));
                }
            }
        }
    }
    Geo tint;
    tint.format = { Geo::EPrimitiveType::Triangles, Geo::EVertexLayout::P3 };
    for (size_t f = 0; f < its.indices.size(); ++f)
        if (tinted[f]) {
            const unsigned b = unsigned(tint.vertices_count());
            for (int k = 0; k < 3; ++k)
                tint.add_vertex(its.vertices[its.indices[f][k]]);
            tint.add_triangle(b, b + 1, b + 2);
        }
    if (tint.vertices_count() > 0) {
        m_tint.init_from(std::move(tint));
        m_tint.set_color(PS_TINT_COLOR);
    }
}

void GLGizmoPaintSplit::render_preview()
{
    if (!m_plane_fill.is_initialized() && !m_tint.is_initialized())
        return;
    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;
    const Camera &camera = wxGetApp().plater()->get_camera();
    shader->start_using();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix() * Geometry::translation_transform(m_world_offset));
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    glsafe(::glDisable(GL_CULL_FACE));
    if (m_tint.is_initialized()) {
        glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
        glsafe(::glPolygonOffset(-1.f, -1.f));
        m_tint.render();
        glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
    }
    glsafe(::glDepthMask(GL_FALSE));
    if (m_plane_fill.is_initialized())
        m_plane_fill.render();
    glsafe(::glDepthMask(GL_TRUE));
    if (m_plane_outline.is_initialized()) {
        glsafe(::glLineWidth(2.f));
        m_plane_outline.render();
    }
    shader->stop_using();
}

void GLGizmoPaintSplit::render_painter_gizmo()
{
    refresh_preview();
    const Selection &selection = m_parent.get_selection();

    glsafe(::glEnable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));

    // The part's own colours first, then the mask over them.
    if (m_paint_views.size() == m_triangle_selectors.size()) {
        std::swap(m_triangle_selectors, m_paint_views);
        render_triangles(selection);
        std::swap(m_triangle_selectors, m_paint_views);
    }
    render_preview();
    const ColorRGBA saved                = TriangleSelectorGUI::enforcers_color;
    TriangleSelectorGUI::enforcers_color = PS_MASK_COLOR;
    glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
    glsafe(::glPolygonOffset(-2.f, -2.f));
    render_triangles(selection);
    glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
    TriangleSelectorGUI::enforcers_color = saved;

    m_c->object_clipper()->render_cut();
    m_c->instances_hider()->render_cut();
    if (!pick_mode())
        render_cursor();

    glsafe(::glDisable(GL_BLEND));
}

// ------------------------------------------------------------------------------------------- apply

void GLGizmoPaintSplit::apply()
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr)
        return;
    if (m_preview_dirty)
        rebuild_preview();
    if (m_preview_part < 0 || m_masked_parts != 1 || m_region.error != PaintSplitError::None)
        return;
    Plater *plater = wxGetApp().plater();
    Worker &worker = plater->get_ui_job_worker();
    if (!worker.is_idle()) {
        plater->get_notification_manager()->push_notification(_u8L("Another task is running. Try Paint Split again when it has finished."));
        return;
    }

    const int           instance = m_parent.get_selection().get_instance_idx();
    PaintSplitApplyPlan plan;
    plan.instance              = instance;
    plan.region                = m_region;
    plan.keep_as_parts         = m_keep_as_parts;
    plan.cut_only_painted_part = m_cut_only_painted_part;
    int part = -1;
    for (size_t i = 0; i < mo->volumes.size(); ++i)
        if (mo->volumes[i]->is_model_part() && ++part == m_preview_part) {
            plan.mask_volume_idx = int(i);
            break;
        }
    const BoundingBoxf3 bbox     = mo->instance_bounding_box(size_t(instance), true);
    const int           filament = size_t(m_preview_part) < m_picked_filament.size() ? m_picked_filament[size_t(m_preview_part)] : 0;
    int                 n        = 0;
    for (size_t c = 0; c < m_planes.size(); ++c) {
        if (!m_planes[c].valid)
            continue;
        PaintSplitApplyPiece piece;
        piece.component = int(c);
        piece.cutter    = paint_split_half_space(m_planes[c], bbox);
        piece.filament  = filament;
        piece.label     = filament > 0 ? GUI::format(_u8L("Filament %1%"), filament) : GUI::format(_u8L("Region %1%"), ++n);
        if (filament > 0 && m_planes.size() > 1)
            piece.label += " " + std::to_string(++n);
        plan.pieces.push_back(std::move(piece));
    }
    if (plan.pieces.empty() || plan.mask_volume_idx < 0)
        return;

    wxGetApp().app_config->set_bool(PS_KEEP_AS_PARTS_KEY, m_keep_as_parts);
    auto job = std::make_unique<PaintSplitJob>(plater, *mo, std::move(plan));
    // Close first, as the Cut does: the object the gizmo shows is about to be replaced. The selection is
    // spent, so there is nothing to ask.
    m_close_confirmed = true;
    m_parent.reset_all_gizmos();
    replace_job(worker, std::move(job));
}

// ------------------------------------------------------------------------------------------- panel

void GLGizmoPaintSplit::on_render_input_window(float x, float y, float bottom_limit)
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr)
        return;
    refresh_preview();

    dock_setup_next_window(x, y, bottom_limit, 0.f, /*size_to_content=*/true);
    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiBegin(get_name(), dock_window_flags(ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse |
                                                  ImGuiWindowFlags_NoTitleBar));
    if (!dock_render_titlebar(get_name())) {
        GizmoImguiEnd();
        ImGuiWrapper::pop_toolbar_style();
        return;
    }

    const float label_w   = m_imgui->scaled(8.f);
    const float slider_w  = m_imgui->scaled(8.f);
    const float wrap_w    = ImGui::GetFontSize() * 22.f;
    bool        dirty     = false;
    auto        slider    = [&](const char *id, const wxString &label, float *v, float lo, float hi, const char *fmt) {
        ImGui::AlignTextToFramePadding();
        m_imgui->text(label);
        ImGui::SameLine(label_w);
        ImGui::PushItemWidth(slider_w);
        const bool changed = m_imgui->bbl_slider_float_style(id, v, lo, hi, fmt, 1.0f, true);
        ImGui::PopItemWidth();
        return changed;
    };

    // Select
    m_imgui->text(m_desc.at("select"));
    const std::array<std::pair<Mode, const char *>, 5> modes{ { { Mode::Brush, "brush" },
                                                                 { Mode::Bucket, "bucket" },
                                                                 { Mode::SmartFill, "smart_fill" },
                                                                 { Mode::PickColour, "pick_colour" },
                                                                 { Mode::PickSegment, "pick_segment" } } };
    for (size_t i = 0; i < modes.size(); ++i) {
        if (i != 0 && i != 3)
            ImGui::SameLine();
        if (ImGui::RadioButton(into_u8(m_desc.at(modes[i].second)).c_str(), m_mode == modes[i].first) && m_mode != modes[i].first) {
            m_mode = modes[i].first;
            for (auto &sel : m_triangle_selectors) {
                sel->seed_fill_unselect_all_triangles();
                sel->request_update_render_data();
            }
        }
    }
    switch (m_mode) {
    case Mode::Brush:
        m_tool_type   = ToolType::BRUSH;
        m_cursor_type = TriangleSelector::CursorType::SPHERE;
        slider("##ps_cursor", m_desc.at("cursor_size"), &m_cursor_radius, CursorRadiusMin, CursorRadiusMax, "%.1f");
        break;
    case Mode::Bucket:
    case Mode::SmartFill:
        m_tool_type = m_mode == Mode::Bucket ? ToolType::BUCKET_FILL : ToolType::SMART_FILL;
        slider("##ps_angle", m_desc.at("fill_angle"), &m_smart_fill_angle, SmartFillAngleMin, SmartFillAngleMax, "%.0f");
        break;
    case Mode::PickColour:
    case Mode::PickSegment:
        m_tool_type = ToolType::BRUSH;
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_w);
        m_imgui->text(m_mode == Mode::PickColour ? _L("Click a colour: all of it on that part is selected. Shift+click clears the part.") :
                                                   _L("Click a painted area: that connected area is selected. Shift+click clears the part."));
        ImGui::PopTextWrapPos();
        break;
    }
    if (!pick_mode())
        m_imgui->text(m_desc.at("erase"));
    if (slider("##ps_holes", m_desc.at("fill_holes"), &m_fill_holes_pct, 0.f, 50.f, "%.0f %%"))
        dirty = true;
    if (m_imgui->button(m_desc.at("clear")))
        clear_selection();

    // The selected regions.
    ImGui::Separator();
    bool can_apply = false;
    if (m_masked_parts > 1) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_w);
        m_imgui->warning_text(_L("Select on one part only: the selection is on several parts."));
        ImGui::PopTextWrapPos();
    } else if (m_preview_part < 0) {
        m_imgui->text(_L("Paint or pick the region to split off."));
    } else if (m_region.error != PaintSplitError::None) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_w);
        m_imgui->warning_text(_L(paint_split_error_message(m_region.error)));
        ImGui::PopTextWrapPos();
        // No Repair button: Fix model clears the painting before it repairs (Plater::clear_before_change_mesh),
        // and an open part is repaired by Apply itself. What is left here is a part that repair cannot close.
    } else {
        // Owner 2026-11-25: holes no longer block the split; Apply closes them and keeps the painting.
        if (m_region.repaired) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_w);
            m_imgui->text(_L("Holes will be repaired while splitting. The painting is kept."));
            ImGui::PopTextWrapPos();
        }
        for (size_t c = 0; c < m_region.num_components(); ++c) {
            const wxString line = format_wxstr(_L("Region %1%: %2% edge(s), %3% cm²"), c + 1, m_region.loops[c].size(),
                                               wxString::Format("%.1f", m_region.area[c] / 100.));
            m_imgui->text(line);
            if (m_region.comp_error[c] != PaintSplitError::None) {
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_w);
                m_imgui->warning_text(_L(paint_split_error_message(m_region.comp_error[c])));
                ImGui::PopTextWrapPos();
            } else if (c < m_planes.size() && m_planes[c].valid) {
                can_apply = true;
                if (m_planes[c].far_from_boundary) {
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_w);
                    m_imgui->warning_text(_L(paint_split_warning_message(PaintSplitWarning::PlaneFarFromBoundary)));
                    ImGui::PopTextWrapPos();
                }
            }
            if (m_region.holes_filled[c] > 0)
                m_imgui->text(format_wxstr(_L("%1% small hole(s) filled"), m_region.holes_filled[c]));
        }
    }

    // Cut style (P2: Plane; Auto resolves to it until Follow paint arrives).
    ImGui::Separator();
    m_imgui->text(m_desc.at("style"));
    if (ImGui::RadioButton(into_u8(m_desc.at("auto")).c_str(), m_style == Style::Auto))
        m_style = Style::Auto;
    ImGui::SameLine();
    if (ImGui::RadioButton(into_u8(m_desc.at("plane")).c_str(), m_style == Style::Plane))
        m_style = Style::Plane;
    dirty |= slider("##ps_offset", m_desc.at("offset"), &m_offset, -50.f, 50.f, "%.1f mm");
    dirty |= slider("##ps_tilt_u", m_desc.at("tilt_u"), &m_tilt_u, -45.f, 45.f, "%.0f°");
    dirty |= slider("##ps_tilt_v", m_desc.at("tilt_v"), &m_tilt_v, -45.f, 45.f, "%.0f°");
    dirty |= m_imgui->bbl_checkbox(m_desc.at("snap"), m_snap_to_bed);

    // Result
    ImGui::Separator();
    m_imgui->text(m_desc.at("result"));
    m_imgui->bbl_checkbox(m_desc.at("keep_as_parts"), m_keep_as_parts);
    if (mo->volumes.size() > 1)
        m_imgui->bbl_checkbox(m_desc.at("only_painted"), m_cut_only_painted_part);

    ImGui::Separator();
    if (m_imgui->button(m_desc.at("apply"), ImVec2(0.f, 0.f), can_apply && m_masked_parts == 1))
        wxGetApp().CallAfter([this]() { apply(); });

    if (dirty) {
        m_preview_dirty = true;
        m_parent.set_as_dirty();
    }
    GizmoImguiEnd();
    ImGuiWrapper::pop_toolbar_style();
}

} // namespace Slic3r::GUI
