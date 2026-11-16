#include "GLGizmoSplitToFit.hpp"

#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosCommon.hpp"
#include "slic3r/GUI/Jobs/SplitToFitJob.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <algorithm>
#include <cmath>

#include <boost/log/trivial.hpp>

#include <glad/gl.h>
#include <imgui/imgui.h>

namespace Slic3r {
namespace GUI {

namespace split_fit_gizmo_detail {

// AppConfig keys: the panel's values are remembered per user, not per project (design 4.4).
static const char *CFG_MARGIN_XY = "split_fit_margin_xy";
static const char *CFG_MARGIN_Z  = "split_fit_margin_z";
static const char *CFG_LAYOUT    = "split_fit_layout";
static const char *CFG_MIN_T     = "split_fit_min_part_thickness";
static const char *CFG_REACH     = "split_fit_nozzle_reach";
// P2: dowels.
static const char *CFG_DOWELS    = "split_fit_dowels";
static const char *CFG_D_TYPE    = "split_fit_dowel_type";
static const char *CFG_D_SHAPE   = "split_fit_dowel_shape";
static const char *CFG_D_DIAM    = "split_fit_dowel_diameter";
static const char *CFG_D_LENGTH  = "split_fit_dowel_length";
static const char *CFG_D_MAX     = "split_fit_dowel_max_per_face";
static const char *CFG_D_WALL    = "split_fit_dowel_wall_margin";
static const char *CFG_D_SPACING = "split_fit_dowel_min_spacing";
static const char *CFG_D_TOL     = "split_fit_dowel_tolerance";
static const char *CFG_D_SCALE   = "split_fit_dowel_scale_down";

static double read_double(const std::string &key, double fallback, double lo, double hi)
{
    const std::string s = wxGetApp().app_config->get(key);
    if (s.empty())
        return fallback;
    try {
        const double v = std::stod(s);
        return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
    } catch (...) {
        return fallback;
    }
}

static std::string mm_size(const Vec3d &s)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "%.0f x %.0f x %.0f mm", s.x(), s.y(), s.z());
    return buf;
}

static bool same_printer(const SplitPrinterVolume &a, const SplitPrinterVolume &b)
{
    if (a.printable_area != b.printable_area || a.printable_height != b.printable_height || a.extruder_areas != b.extruder_areas ||
        a.extruder_heights != b.extruder_heights || a.exclude_boxes.size() != b.exclude_boxes.size())
        return false;
    for (size_t i = 0; i < a.exclude_boxes.size(); ++i)
        if (a.exclude_boxes[i].min != b.exclude_boxes[i].min || a.exclude_boxes[i].max != b.exclude_boxes[i].max)
            return false;
    return true;
}

enum class Severity { Info, Warning, Error };

static Severity severity(SplitWarning::Kind k)
{
    switch (k) {
    case SplitWarning::Kind::NoUsableVolume:
    case SplitWarning::Kind::TooManyParts:
    case SplitWarning::Kind::PartDoesNotFit: return Severity::Error;
    case SplitWarning::Kind::ModelNotWatertight:
    case SplitWarning::Kind::HollowModel:
    case SplitWarning::Kind::LoosePieces:
    case SplitWarning::Kind::OpenEdges:
    case SplitWarning::Kind::NoDowelOnFace:
    case SplitWarning::Kind::IslandsWithoutDowel:
    case SplitWarning::Kind::DowelCanTurn: return Severity::Warning;
    default: return Severity::Info;
    }
}

} // namespace split_fit_gizmo_detail

wxString split_warning_text(const SplitWarning &w, const SplitPlan &plan)
{
    const std::string part  = w.part >= 0 && size_t(w.part) < plan.parts.size() ? plan.parts[size_t(w.part)].name : std::string();
    const std::string plane = w.plane >= 0 && size_t(w.plane) < plan.planes.size() ? plan.planes[size_t(w.plane)].name() : std::string();
    switch (w.kind) {
    case SplitWarning::Kind::NoUsableVolume:
        return _L("The margins leave no room on this printer. Make them smaller.");
    case SplitWarning::Kind::TooManyParts:
        return format_wxstr(_L("This would make %1% parts, more than the %2% Split to Fit makes. Check the margins, or scale the model down first."),
                            int(w.value), SPLIT_MAX_CELLS);
    case SplitWarning::Kind::PartDoesNotFit: {
        const Vec3d s = w.part >= 0 && size_t(w.part) < plan.parts.size() ? plan.parts[size_t(w.part)].size() : Vec3d::Zero();
        return format_wxstr(_L("Part %1% (%2%) does not fit the printer."), part.empty() ? std::string("1") : part,
                            split_fit_gizmo_detail::mm_size(s));
    }
    case SplitWarning::Kind::BedNotRectangular:
        return _L("The bed is not rectangular: the parts are sized for a rectangle inside it.");
    case SplitWarning::Kind::BedExclusion:
        return _L("The bed has excluded areas: the parts are sized for the rest of the bed.");
    case SplitWarning::Kind::NozzleReach:
        return _L("The parts are sized for the area both nozzles reach.");
    case SplitWarning::Kind::ModelNotWatertight:
        return format_wxstr(_L("The model has %1% open edges, so the parts may have holes. Repair the model first if you can."), int(w.value));
    case SplitWarning::Kind::HollowModel:
        return format_wxstr(_L("The model looks hollow (walls about %1% mm at plane %2%): the cut faces are thin rings with little to glue."),
                            format("%.1f", w.value), plane);
    case SplitWarning::Kind::LoosePieces:
        return format_wxstr(_L("Part %1% has %2% separate pieces. They are kept together in that part."), part, int(w.value));
    case SplitWarning::Kind::OpenEdges:
        return format_wxstr(_L("Part %1% has %2% open edges after the cut (the model was not watertight). Use Fix model on it."), part,
                            int(w.value));
    case SplitWarning::Kind::NoDowelOnFace:
    case SplitWarning::Kind::IslandsWithoutDowel:
    case SplitWarning::Kind::DowelCanTurn:
    case SplitWarning::Kind::DowelShortened:
    case SplitWarning::Kind::DowelScaledDown: {
        // The face by the two parts it joins ("X1-Y1 | X2-Y1"), or by its plane.
        std::string face = plane;
        if (w.face >= 0 && size_t(w.face) < plan.faces.size()) {
            const SplitFace &f = plan.faces[size_t(w.face)];
            face               = f.mate_label.empty() || f.mate_label == " | " ? f.mate_code : f.mate_label;
        }
        switch (w.kind) {
        case SplitWarning::Kind::NoDowelOnFace:
            return w.value == SPLIT_NO_DOWEL_TOO_THIN ?
                       format_wxstr(_L("Face %1% gets no dowel: there is too little solid material behind it. Glue that face."), face) :
                       format_wxstr(_L("Face %1% gets no dowel: it is too small or too thin for one. Glue that face."), face);
        case SplitWarning::Kind::IslandsWithoutDowel:
            return format_wxstr(_L("Face %1%: %2% of its separate pieces get no dowel (the limit per face was reached, or they are too small)."),
                                face, int(w.value));
        case SplitWarning::Kind::DowelCanTurn:
            return format_wxstr(_L("Face %1% has room for only one round dowel, so the parts can turn on it. Glue it, or use a square dowel."),
                                face);
        case SplitWarning::Kind::DowelShortened:
            return format_wxstr(_L("Face %1%: dowels shortened to %2% mm to stay inside the material."), face, format("%.1f", w.value));
        case SplitWarning::Kind::DowelScaledDown:
            return format_wxstr(_L("Face %1%: smaller dowels (%2% mm) to fit the face."), face, format("%.1f", w.value));
        default: break;
        }
        break;
    }
    }
    return wxString();
}

GLGizmoSplitToFit::GLGizmoSplitToFit(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{}

bool GLGizmoSplitToFit::on_init()
{
    m_shortcut_key = 0;
    return true;
}

std::string GLGizmoSplitToFit::on_get_name() const { return _u8L("Split to Fit"); }

CommonGizmosDataID GLGizmoSplitToFit::on_get_requirements() const { return CommonGizmosDataID::SelectionInfo; }

bool GLGizmoSplitToFit::on_is_activable() const
{
    if (m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView)
        return false;
    const Selection &selection = m_parent.get_selection();
    if (selection.get_object_idx() < 0 || selection.is_wipe_tower())
        return false;
    return selection.is_single_full_instance() && !m_parent.is_layers_editing_enabled();
}

ModelObject *GLGizmoSplitToFit::selected_object(int *instance) const
{
    const Selection &selection = m_parent.get_selection();
    const int        idx       = selection.get_object_idx();
    Model           &model     = wxGetApp().plater()->model();
    if (idx < 0 || size_t(idx) >= model.objects.size())
        return nullptr;
    if (instance != nullptr)
        *instance = std::max(0, selection.get_instance_idx());
    return model.objects[size_t(idx)];
}

void GLGizmoSplitToFit::on_set_state()
{
    if (m_state == On) {
        load_settings();
        m_options.pinned.clear();
        m_mesh_object = ObjectID();
        m_plan_dirty  = true;
    } else if (m_state == Off) {
        save_settings();
        m_mesh = indexed_triangle_set();
        m_plan = SplitPlan();
        m_planes_fill.reset();
        m_pinned_fill.reset();
        m_planes_outline.reset();
        m_bad_parts.reset();
        m_sites_fill.reset();
        m_sites_axes.reset();
    }
}

void GLGizmoSplitToFit::data_changed(bool /*is_serializing*/)
{
    // Another object (or none) may be selected now; refresh_inputs() notices.
    m_plan_dirty = true;
}

void GLGizmoSplitToFit::load_settings()
{
    using namespace split_fit_gizmo_detail;
    reset_options();
    m_options.margin_xy          = read_double(CFG_MARGIN_XY, 5., 0., 100.);
    m_options.margin_z           = read_double(CFG_MARGIN_Z, 5., 0., 100.);
    m_options.min_part_thickness = read_double(CFG_MIN_T, 10., 1., 100.);
    m_options.layout             = wxGetApp().app_config->get(CFG_LAYOUT) == "full" ? SplitLayout::FullSizeFirst : SplitLayout::Even;
    m_options.nozzle_reach       = wxGetApp().app_config->get(CFG_REACH) == "1";

    AppConfig         *cfg = wxGetApp().app_config;
    SplitDowelOptions &d   = m_options.dowels;
    // D3: dowels on unless the user turned them off.
    d.enabled         = cfg->get(CFG_DOWELS) != "0";
    d.type            = cfg->get(CFG_D_TYPE) == "pin" ? SplitConnectorType::Pin : SplitConnectorType::Dowel;
    d.shape           = SplitDowelShape(int(std::lround(read_double(CFG_D_SHAPE, double(int(d.shape)), 0., 3.))));
    d.diameter        = read_double(CFG_D_DIAM, d.diameter, 2., 30.);
    d.length          = read_double(CFG_D_LENGTH, d.length, 4., 100.);
    d.max_per_face    = int(std::lround(read_double(CFG_D_MAX, double(d.max_per_face), 1., 20.)));
    d.wall_margin     = read_double(CFG_D_WALL, d.wall_margin, 0.5, 30.);
    d.min_spacing     = read_double(CFG_D_SPACING, d.min_spacing, 2., 200.);
    d.tolerance       = read_double(CFG_D_TOL, d.tolerance, 0., 1.);
    d.scale_down      = cfg->get(CFG_D_SCALE) != "0";
}

void GLGizmoSplitToFit::save_settings() const
{
    using namespace split_fit_gizmo_detail;
    AppConfig *cfg = wxGetApp().app_config;
    cfg->set(CFG_MARGIN_XY, Slic3r::format("%.2f", m_options.margin_xy));
    cfg->set(CFG_MARGIN_Z, Slic3r::format("%.2f", m_options.margin_z));
    cfg->set(CFG_MIN_T, Slic3r::format("%.2f", m_options.min_part_thickness));
    cfg->set(CFG_LAYOUT, m_options.layout == SplitLayout::FullSizeFirst ? "full" : "even");
    cfg->set(CFG_REACH, m_options.nozzle_reach ? "1" : "0");

    const SplitDowelOptions &d = m_options.dowels;
    cfg->set(CFG_DOWELS, d.enabled ? "1" : "0");
    cfg->set(CFG_D_TYPE, d.type == SplitConnectorType::Pin ? "pin" : "dowel");
    cfg->set(CFG_D_SHAPE, std::to_string(int(d.shape)));
    cfg->set(CFG_D_DIAM, Slic3r::format("%.2f", d.diameter));
    cfg->set(CFG_D_LENGTH, Slic3r::format("%.2f", d.length));
    cfg->set(CFG_D_MAX, std::to_string(d.max_per_face));
    cfg->set(CFG_D_WALL, Slic3r::format("%.2f", d.wall_margin));
    cfg->set(CFG_D_SPACING, Slic3r::format("%.2f", d.min_spacing));
    cfg->set(CFG_D_TOL, Slic3r::format("%.3f", d.tolerance));
    cfg->set(CFG_D_SCALE, d.scale_down ? "1" : "0");
}

void GLGizmoSplitToFit::reset_options()
{
    // The defaults the owner accepted: D2 margins, D5 even parts, D9 every axis, D14 reach off,
    // D3 dowels on (loose round Dowel, 6 x 20 mm, up to 3 per face).
    m_options                = SplitOptions();
    m_options.dowels.enabled = true;
}

void GLGizmoSplitToFit::refresh_inputs()
{
    int          instance = 0;
    ModelObject *mo       = selected_object(&instance);
    if (mo == nullptr || size_t(instance) >= mo->instances.size()) {
        if (!m_mesh.indices.empty()) {
            m_mesh       = indexed_triangle_set();
            m_plan_dirty = true;
        }
        m_mesh_object = ObjectID();
        return;
    }
    const Transform3d trafo = mo->instances[size_t(instance)]->get_transformation().get_matrix();
    if (mo->id() != m_mesh_object || !trafo.isApprox(m_mesh_trafo) || mo->volumes.size() != m_mesh_volumes) {
        if (mo->id() != m_mesh_object)
            m_options.pinned.clear();
        m_mesh         = split_object_mesh(*mo, instance);
        m_mesh_object  = mo->id();
        m_mesh_trafo   = trafo;
        m_mesh_volumes = mo->volumes.size();
        m_plan_dirty   = true;
    }

    // D8: the current printer only. Re-read every frame (cheap) so a printer switch shows at once.
    const SplitPrinterVolume printer = split_printer_volume_from_config(wxGetApp().preset_bundle->printers.get_edited_preset().config);
    if (!split_fit_gizmo_detail::same_printer(printer, m_printer)) {
        m_printer    = printer;
        m_plan_dirty = true;
    }
}

void GLGizmoSplitToFit::update_plan()
{
    m_plan_dirty    = false;
    m_preview_dirty = true;
    try {
        const SplitUsableVolume usable = split_usable_volume(m_printer, m_options);
        m_plan                         = split_plan_grid(m_mesh, usable, m_options);
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "Split to Fit: planning failed: " << e.what();
        m_plan = SplitPlan();
    }
}

void GLGizmoSplitToFit::rebuild_preview()
{
    m_preview_dirty = false;
    m_planes_fill.reset();
    m_pinned_fill.reset();
    m_planes_outline.reset();
    m_bad_parts.reset();
    m_sites_fill.reset();
    m_sites_axes.reset();
    if (m_plan.planes.empty() && m_plan.parts_not_fitting() == 0)
        return;

    const BoundingBoxf3 &bb  = m_plan.model_bbox;
    const Vec3d          pad = Vec3d::Constant(std::max(5., 0.04 * bb.size().maxCoeff()));
    const Vec3d          lo  = bb.min - pad;
    const Vec3d          hi  = bb.max + pad;

    using Geo = GLModel::Geometry;
    Geo fill, pinned, outline, bad;
    fill.format    = { Geo::EPrimitiveType::Triangles, Geo::EVertexLayout::P3 };
    pinned.format  = { Geo::EPrimitiveType::Triangles, Geo::EVertexLayout::P3 };
    outline.format = { Geo::EPrimitiveType::Lines, Geo::EVertexLayout::P3 };
    bad.format     = { Geo::EPrimitiveType::Lines, Geo::EVertexLayout::P3 };

    for (const SplitPlane &pl : m_plan.planes) {
        const int a = pl.axis, u = (a + 1) % 3, v = (a + 2) % 3;
        Vec3d     c[4];
        for (int k = 0; k < 4; ++k) {
            c[k][a] = pl.pos;
            c[k][u] = (k == 1 || k == 2) ? hi[u] : lo[u];
            c[k][v] = (k >= 2) ? hi[v] : lo[v];
        }
        Geo         &g    = pl.pinned ? pinned : fill;
        const unsigned base = unsigned(g.vertices_count());
        for (const Vec3d &p : c)
            g.add_vertex(Vec3f(p.cast<float>()));
        g.add_triangle(base, base + 1, base + 2);
        g.add_triangle(base, base + 2, base + 3);
        const unsigned ob = unsigned(outline.vertices_count());
        for (const Vec3d &p : c)
            outline.add_vertex(Vec3f(p.cast<float>()));
        for (unsigned k = 0; k < 4; ++k)
            outline.add_line(ob + k, ob + (k + 1) % 4);
    }

    // The boxes of the parts that would not fit.
    for (const SplitPart &part : m_plan.parts) {
        if (part.empty || part.fits)
            continue;
        const Vec3d   &mn = part.bbox.min, &mx = part.bbox.max;
        const unsigned b  = unsigned(bad.vertices_count());
        for (int k = 0; k < 8; ++k)
            bad.add_vertex(Vec3f(float((k & 1) ? mx.x() : mn.x()), float((k & 2) ? mx.y() : mn.y()), float((k & 4) ? mx.z() : mn.z())));
        static const int edges[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 },
                                          { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
        for (const auto &e : edges)
            bad.add_line(b + unsigned(e[0]), b + unsigned(e[1]));
    }

    // P2: each dowel site as a disc of its hole's size on its plane, and its length as a line
    // through it along the plane's normal.
    Geo sites, axes;
    sites.format = { Geo::EPrimitiveType::Triangles, Geo::EVertexLayout::P3 };
    axes.format  = { Geo::EPrimitiveType::Lines, Geo::EVertexLayout::P3 };
    static constexpr int DISC_SEGMENTS = 24;
    for (const SplitSite &s : m_plan.sites) {
        if (s.plane < 0 || size_t(s.plane) >= m_plan.planes.size())
            continue;
        const int    a = m_plan.planes[size_t(s.plane)].axis, u = (a + 1) % 3, v = (a + 2) % 3;
        const double r = std::max(0.5, s.footprint_radius);
        const unsigned c = unsigned(sites.vertices_count());
        sites.add_vertex(Vec3f(s.pos.cast<float>()));
        for (int k = 0; k < DISC_SEGMENTS; ++k) {
            const double t = 2. * PI * double(k) / double(DISC_SEGMENTS);
            Vec3d        p = s.pos;
            p[u] += r * std::cos(t);
            p[v] += r * std::sin(t);
            sites.add_vertex(Vec3f(p.cast<float>()));
        }
        for (int k = 0; k < DISC_SEGMENTS; ++k)
            sites.add_triangle(c, c + 1 + unsigned(k), c + 1 + unsigned((k + 1) % DISC_SEGMENTS));
        Vec3d p0 = s.pos, p1 = s.pos;
        p0[a] -= s.half_length;
        p1[a] += s.half_length;
        const unsigned l = unsigned(axes.vertices_count());
        axes.add_vertex(Vec3f(p0.cast<float>()));
        axes.add_vertex(Vec3f(p1.cast<float>()));
        axes.add_line(l, l + 1);
    }

    ColorRGBA teal = ColorRGBA::ORCA();
    teal.a(0.22f);
    ColorRGBA orange = ColorRGBA::ORANGE();
    orange.a(0.30f);
    if (fill.vertices_count() > 0) {
        m_planes_fill.init_from(std::move(fill));
        m_planes_fill.set_color(teal);
    }
    if (pinned.vertices_count() > 0) {
        m_pinned_fill.init_from(std::move(pinned));
        m_pinned_fill.set_color(orange);
    }
    if (outline.vertices_count() > 0) {
        m_planes_outline.init_from(std::move(outline));
        m_planes_outline.set_color(ColorRGBA::ORCA());
    }
    if (bad.vertices_count() > 0) {
        m_bad_parts.init_from(std::move(bad));
        m_bad_parts.set_color(ColorRGBA::RED());
    }
    // A warm yellow reads on the teal planes and on light and dark beds alike.
    const ColorRGBA site_color(0.98f, 0.76f, 0.12f, 0.95f);
    if (sites.vertices_count() > 0) {
        m_sites_fill.init_from(std::move(sites));
        m_sites_fill.set_color(site_color);
    }
    if (axes.vertices_count() > 0) {
        m_sites_axes.init_from(std::move(axes));
        m_sites_axes.set_color(site_color);
    }
}

void GLGizmoSplitToFit::on_render()
{
    refresh_inputs();
    if (m_plan_dirty)
        update_plan();
    if (m_preview_dirty)
        rebuild_preview();

    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;
    const Camera &camera = wxGetApp().plater()->get_camera();

    shader->start_using();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix());
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    glsafe(::glDisable(GL_CULL_FACE));
    // The translucent planes must not hide each other or the model behind them.
    glsafe(::glDepthMask(GL_FALSE));
    if (m_planes_fill.is_initialized())
        m_planes_fill.render();
    if (m_pinned_fill.is_initialized())
        m_pinned_fill.render();
    glsafe(::glDepthMask(GL_TRUE));

    glsafe(::glLineWidth(2.0f));
    if (m_planes_outline.is_initialized())
        m_planes_outline.render();
    if (m_bad_parts.is_initialized())
        m_bad_parts.render();
    // The dowel sites lie inside the model: draw them over it, so they show through.
    if (m_sites_fill.is_initialized() || m_sites_axes.is_initialized()) {
        glsafe(::glDisable(GL_DEPTH_TEST));
        if (m_sites_fill.is_initialized())
            m_sites_fill.render();
        glsafe(::glLineWidth(3.0f));
        if (m_sites_axes.is_initialized())
            m_sites_axes.render();
        glsafe(::glEnable(GL_DEPTH_TEST));
    }
    glsafe(::glLineWidth(1.0f));

    glsafe(::glEnable(GL_CULL_FACE));
    glsafe(::glDisable(GL_BLEND));
    shader->stop_using();
}

void GLGizmoSplitToFit::on_render_input_window(float x, float y, float bottom_limit)
{
    using namespace split_fit_gizmo_detail;

    int          instance = 0;
    ModelObject *mo       = selected_object(&instance);
    if (mo == nullptr)
        return;
    refresh_inputs();
    if (m_plan_dirty)
        update_plan();

    dock_setup_next_window(x, y, bottom_limit, 0.f, /*size_to_content=*/true);
    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiBegin(get_name(), dock_window_flags(ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse |
                                                  ImGuiWindowFlags_NoTitleBar));
    if (!dock_render_titlebar(get_name())) {
        GizmoImguiEnd();
        ImGuiWrapper::pop_toolbar_style();
        return;
    }

    const float em       = ImGui::GetFontSize();
    const float label_w  = em * 10.f;
    const float field_w  = em * 5.f;
    const float wrap_w   = em * 24.f;
    const ImVec4 red     = ImGuiWrapper::COL_RED;
    const ImVec4 amber   = ImGuiWrapper::to_ImVec4(ColorRGB::WARNING());

    const SplitUsableVolume &usable = m_plan.usable;
    bool                     changed = false;

    auto label = [&](const wxString &text) {
        m_imgui->text(text);
        ImGui::SameLine(label_w);
    };
    auto input_mm = [&](const wxString &text, const char *id, double &value, double lo, double hi, const wxString &tip) {
        label(text);
        ImGui::PushItemWidth(field_w);
        double v = value;
        if (ImGui::InputDouble(id, &v, 0.0, 0.0, "%.1f") && std::isfinite(v)) {
            value   = std::clamp(v, lo, hi);
            changed = true;
        }
        ImGui::PopItemWidth();
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(tip, wrap_w);
        ImGui::SameLine();
        m_imgui->text(_L("mm"));
    };

    // ---- the printer --------------------------------------------------------------------------------
    label(_L("Printer volume"));
    m_imgui->text(from_u8(mm_size(usable.printer_size)) + " (" + _L("current printer") + ")");
    input_mm(_L("Side margin"), "##split_margin_xy", m_options.margin_xy, 0., 100.,
             _L("Kept free on every side of each part, in X and Y. It leaves room for a brim or skirt "
                "and for the arrangement's own spacing: 5 mm covers a 5 mm brim."));
    input_mm(_L("Top margin"), "##split_margin_z", m_options.margin_z, 0., 100.,
             _L("Kept free above each part, below the printer's maximum height."));
    if (usable.has_reach_option) {
        if (m_imgui->bbl_checkbox(_L("Keep to the area both nozzles reach"), m_options.nozzle_reach))
            changed = true;
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_L("Size the parts for the part of the bed every nozzle reaches, so a part can be printed "
                                "with either nozzle. Most split parts use one filament and do not need this."),
                             wrap_w);
    }
    label(_L("Usable"));
    if (usable.valid)
        m_imgui->text(from_u8(mm_size(usable.size)));
    else
        ImGuiWrapper::text_colored(red, _L("none"));

    ImGui::Separator();

    // ---- the grid -----------------------------------------------------------------------------------
    label(_L("Cut along"));
    static const char *axis_names[3] = { "X", "Y", "Z" };
    for (int a = 0; a < 3; ++a) {
        bool allow = m_options.allow_axis[a];
        if (m_imgui->bbl_checkbox(wxString::FromUTF8(axis_names[a]), allow)) {
            m_options.allow_axis[a] = allow;
            m_options.pinned.clear();
            changed = true;
        }
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_L("Cut along this axis where the model is longer than the usable size. "
                                "Untick it to keep the model whole in this direction: parts that are too long turn red."),
                             wrap_w);
        ImGui::SameLine();
        m_imgui->text(format_wxstr(_L("(%1% parts)"), m_plan.counts[a]));
        if (a < 2)
            ImGui::SameLine();
    }

    label(_L("Layout"));
    const std::string even_label = _u8L("Even parts");
    const std::string full_label = _u8L("Full size first");
    if (m_imgui->bbl_radio_button(even_label.c_str(), m_options.layout == SplitLayout::Even)) {
        m_options.layout = SplitLayout::Even;
        m_options.pinned.clear();
        changed = true;
    }
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("Every part on an axis gets the same length."), wrap_w);
    ImGui::SameLine();
    if (m_imgui->bbl_radio_button(full_label.c_str(), m_options.layout == SplitLayout::FullSizeFirst)) {
        m_options.layout = SplitLayout::FullSizeFirst;
        m_options.pinned.clear();
        changed = true;
    }
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("Parts as long as the printer allows, the remainder last."), wrap_w);
    if (m_options.layout == SplitLayout::FullSizeFirst)
        input_mm(_L("Smallest part"), "##split_min_t", m_options.min_part_thickness, 1., 100.,
                 _L("The last part on an axis is never thinner than this: the plane before it moves in."));

    // Planes: drag a value to move a plane; a moved plane is kept where it was put (orange).
    if (!m_plan.planes.empty() && ImGui::CollapsingHeader(into_u8(_L("Plane positions")).c_str())) {
        for (const SplitPlane &pl : m_plan.planes) {
            float pos = float(pl.pos);
            label(from_u8(pl.name()));
            ImGui::PushItemWidth(field_w * 1.6f);
            const std::string id = "##split_plane_" + pl.name();
            if (ImGui::DragFloat(id.c_str(), &pos, 0.5f, float(m_plan.model_bbox.min[pl.axis]), float(m_plan.model_bbox.max[pl.axis]), "%.1f mm")) {
                auto it = std::find_if(m_options.pinned.begin(), m_options.pinned.end(),
                                       [&pl](const SplitPinnedPlane &p) { return p.axis == pl.axis && p.index == pl.index; });
                if (it == m_options.pinned.end())
                    m_options.pinned.push_back({ pl.axis, pl.index, double(pos) });
                else
                    it->pos = double(pos);
                changed = true;
            }
            ImGui::PopItemWidth();
            if (ImGui::IsItemHovered())
                m_imgui->tooltip(_L("Drag to move this plane. A moved plane stays where you put it; a part that becomes too long turns red."),
                                 wrap_w);
        }
        if (!m_options.pinned.empty() && m_imgui->button(_L("Put planes back"))) {
            m_options.pinned.clear();
            changed = true;
        }
    }

    ImGui::Separator();

    // ---- dowels (P2) --------------------------------------------------------------------------------
    SplitDowelOptions &dw = m_options.dowels;
    if (m_imgui->bbl_checkbox(_L("Add dowels"), dw.enabled))
        changed = true;
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("Put loose dowels across every cut face, with a hole for each in both parts, so the parts line up "
                            "when you glue them. The dowels are added as objects to print."),
                         wrap_w);
    if (dw.enabled) {
        auto combo = [&](const wxString &text, const char *id, const std::vector<wxString> &items, int &selection, const wxString &tip) {
            label(text);
            ImGui::PushItemWidth(field_w * 1.8f);
            const int         sel     = std::clamp(selection, 0, int(items.size()) - 1);
            const std::string preview = into_u8(items[size_t(sel)]);
            if (ImGui::BeginCombo(id, preview.c_str())) {
                for (int i = 0; i < int(items.size()); ++i)
                    if (ImGui::Selectable(into_u8(items[size_t(i)]).c_str(), i == sel)) {
                        selection = i;
                        changed   = true;
                    }
                ImGui::EndCombo();
            }
            ImGui::PopItemWidth();
            if (ImGui::IsItemHovered())
                m_imgui->tooltip(tip, wrap_w);
        };
        int type = int(dw.type);
        combo(_L("Type"), "##split_dowel_type", { _L("Dowel"), _L("Pin") }, type,
              _L("Dowel: a plain peg, the Cut tool's Dowel. Pin: the Cut tool's push-fit pin, with a slit and a lip so it "
                 "holds without glue."));
        dw.type = SplitConnectorType(type);
        if (dw.type == SplitConnectorType::Dowel) {
            int shape = int(dw.shape);
            combo(_L("Shape"), "##split_dowel_shape", { _L("Round"), _L("Square"), _L("Hexagon"), _L("Triangle") }, shape,
                  _L("A square, hexagon or triangle dowel cannot turn in its hole, so one is enough to hold a face."));
            dw.shape = SplitDowelShape(shape);
        }
        input_mm(_L("Diameter"), "##split_dowel_d", dw.diameter, 2., 30.,
                 _L("The dowel's diameter. Faces with no room for it get smaller dowels if \"Smaller on small faces\" is ticked."));
        input_mm(_L("Length"), "##split_dowel_l", dw.length, 4., 100.,
                 _L("The dowel's whole length; half of it goes into each part. It is shortened where a part is too thin."));
        label(_L("Up to"));
        ImGui::PushItemWidth(field_w);
        int max_per_face = dw.max_per_face;
        if (ImGui::InputInt("##split_dowel_max", &max_per_face, 1, 1)) {
            dw.max_per_face = std::clamp(max_per_face, 1, 20);
            changed         = true;
        }
        ImGui::PopItemWidth();
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_L("The most dowels on one cut face. Two or more stop the parts turning."), wrap_w);
        ImGui::SameLine();
        m_imgui->text(_L("per cut face"));
        input_mm(_L("Wall margin"), "##split_dowel_wall", dw.wall_margin, 0.5, 30.,
                 _L("Solid material kept between a hole and the outside of the part."));
        input_mm(_L("Spacing"), "##split_dowel_spacing", dw.min_spacing, 2., 200.,
                 _L("The smallest distance between two dowels on one face, centre to centre."));
        label(_L("Fit tolerance"));
        ImGui::PushItemWidth(field_w);
        double tol = dw.tolerance;
        if (ImGui::InputDouble("##split_dowel_tol", &tol, 0.0, 0.0, "%.2f") && std::isfinite(tol)) {
            dw.tolerance = std::clamp(tol, 0., 1.);
            changed      = true;
        }
        ImGui::PopItemWidth();
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_L("Extra room round the dowel in each hole, per side. 0.1 mm suits most printers; print one small "
                                "joint first to check the fit on yours."),
                             wrap_w);
        ImGui::SameLine();
        m_imgui->text(_L("mm"));
        if (m_imgui->bbl_checkbox(_L("Smaller on small faces"), dw.scale_down))
            changed = true;
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_L("A face with no room for the full size gets 3/4 or half size dowels (never under 2 mm)."), wrap_w);
    }

    ImGui::Separator();

    // ---- the result ---------------------------------------------------------------------------------
    const bool single_copy = mo->instances.size() == 1;
    if (m_plan.valid) {
        // A rough plate count: the parts' footprints over the usable area, with room between them.
        double footprint = 0.;
        for (const SplitPart &p : m_plan.parts)
            if (!p.empty)
                footprint += p.size().x() * p.size().y();
        const double bed    = std::max(1., usable.size.x() * usable.size.y());
        const int    plates = std::max(1, int(std::ceil(footprint * 1.3 / bed)));
        const bool   pins   = m_plan.dowel_options.type == SplitConnectorType::Pin;
        label(_L("Result"));
        if (m_plan.dowel_options.enabled)
            m_imgui->text(format_wxstr(pins ? _L("%1% parts, %2% pins, about %3% plates") : _L("%1% parts, %2% dowels, about %3% plates"),
                                       m_plan.part_count(), m_plan.dowel_count(), plates));
        else
            m_imgui->text(format_wxstr(_L("%1% parts, about %2% plates"), m_plan.part_count(), plates));

        const float row_h  = ImGui::GetTextLineHeightWithSpacing();
        const float list_h = std::min(row_h * float(m_plan.part_count() + 1), em * 9.f);
        ImGui::BeginChild("##split_parts", ImVec2(em * 26.f, list_h), true);
        for (const SplitPart &p : m_plan.parts) {
            if (p.empty)
                continue;
            m_imgui->text(from_u8(p.name.empty() ? mo->name : p.name));
            ImGui::SameLine(em * 7.f);
            m_imgui->text(from_u8(mm_size(p.size())));
            ImGui::SameLine(em * 16.f);
            if (p.fits)
                m_imgui->text(_L("fits"));
            else
                ImGuiWrapper::text_colored(red, _L("does not fit"));
            if (m_plan.dowel_options.enabled) {
                // The holes this part gets: one per dowel on each of its faces.
                size_t holes = 0;
                for (int f : p.faces)
                    if (f >= 0 && size_t(f) < m_plan.faces.size())
                        holes += m_plan.faces[size_t(f)].sites.size();
                ImGui::SameLine(em * 21.f);
                m_imgui->text(format_wxstr(_L("%1% holes"), holes));
            }
        }
        ImGui::EndChild();
        if (m_plan.planes.empty())
            m_imgui->text_wrapped(_L("The model already fits the printer: there is nothing to split."), wrap_w);
    }
    if (!single_copy)
        ImGuiWrapper::text_colored(red, format_wxstr(_L("This object has %1% copies. Split to Fit works on an object with one copy: "
                                                        "delete the other copies first."), mo->instances.size()));

    // ---- warnings -----------------------------------------------------------------------------------
    if (!m_plan.warnings.empty()) {
        m_imgui->text(format_wxstr(_L("Warnings (%1%)"), m_plan.warnings.size()));
        // A grid with dowels can warn about many faces: past a handful the list scrolls.
        const bool scroll = m_plan.warnings.size() > 6;
        if (scroll)
            ImGui::BeginChild("##split_warnings", ImVec2(wrap_w + em, em * 12.f), true);
        for (const SplitWarning &w : m_plan.warnings) {
            const Severity s    = severity(w.kind);
            const wxString text = split_warning_text(w, m_plan);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_w);
            if (s == Severity::Info)
                ImGui::TextWrapped("%s", into_u8(text).c_str());
            else {
                ImGui::PushStyleColor(ImGuiCol_Text, s == Severity::Error ? red : amber);
                ImGui::TextWrapped("! %s", into_u8(text).c_str());
                ImGui::PopStyleColor();
            }
            ImGui::PopTextWrapPos();
        }
        if (scroll)
            ImGui::EndChild();
    }

    ImGui::Separator();

    // ---- actions ------------------------------------------------------------------------------------
    if (m_imgui->button(_L("Reset"), _L("Back to the default margins, axes, layout and dowels"))) {
        reset_options();
        changed = true;
    }
    ImGui::SameLine();
    const bool can_apply = m_plan.valid && !m_plan.planes.empty() && single_copy;
    m_imgui->disabled_begin(!can_apply);
    const bool apply_now = m_imgui->button(_L("Apply"), _L("Cut the object into these parts and arrange them over the plates"));
    m_imgui->disabled_end();

    GizmoImguiEnd();
    ImGuiWrapper::pop_toolbar_style();

    if (changed) {
        m_plan_dirty = true;
        m_parent.set_as_dirty();
        m_parent.request_extra_frame();
    }
    if (apply_now && can_apply)
        apply();
}

void GLGizmoSplitToFit::apply()
{
    int          instance = 0;
    ModelObject *mo       = selected_object(&instance);
    if (mo == nullptr || !m_plan.valid || mo->instances.size() != 1)
        return;
    Plater *plater = wxGetApp().plater();

    // D13: a part that does not fit is allowed, after a question.
    if (const size_t bad = m_plan.parts_not_fitting(); bad > 0) {
        MessageDialog dlg(plater,
                          format_wxstr(_L("%1% of the %2% parts will not fit the printer. Split anyway? You can turn or cut those parts "
                                          "by hand afterwards."), bad, m_plan.part_count()),
                          _L("Split to Fit"), wxYES_NO | wxICON_WARNING);
        if (dlg.ShowModal() != wxID_YES)
            return;
    }

    Worker &worker = plater->get_ui_job_worker();
    if (!worker.is_idle()) {
        plater->get_notification_manager()->push_notification(_u8L("Another task is running. Try Split to Fit again when it has finished."));
        return;
    }
    save_settings();
    std::unique_ptr<SplitToFitJob> job = std::make_unique<SplitToFitJob>(plater, *mo, instance, m_plan);
    // Close the gizmo first, as the Cut does: the object it shows is about to be replaced.
    m_parent.reset_all_gizmos();
    replace_job(worker, std::move(job));
}

} // namespace GUI
} // namespace Slic3r
