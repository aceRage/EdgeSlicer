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
#include <exception>

#include <boost/log/trivial.hpp>

#include <glad/gl.h>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

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
    case SplitWarning::Kind::DowelCanTurn:
    case SplitWarning::Kind::AdjustRefused:
    case SplitWarning::Kind::PlaneInDetail:
    case SplitWarning::Kind::AdjustImpossible:
    case SplitWarning::Kind::SmallPart:
    case SplitWarning::Kind::NoFlatFace:
    case SplitWarning::Kind::NoLabel:
    case SplitWarning::Kind::ManualSiteRefused: return Severity::Warning;
    default: return Severity::Info;
    }
}

// Box of a plane (thin, across the model), of a cut face (its plane clipped to the face's cell),
// or of a part: what a click on a warning shows.
static BoundingBoxf3 plane_box(const SplitPlan &plan, int plane, const SplitCell *cell)
{
    BoundingBoxf3 out;
    if (plane < 0 || size_t(plane) >= plan.planes.size())
        return out;
    const SplitPlane &pl = plan.planes[size_t(plane)];
    Vec3d             lo = plan.model_bbox.min, hi = plan.model_bbox.max;
    if (cell != nullptr) {
        // The face's cell along the other axes.
        std::array<std::vector<double>, 3> pos;
        for (int a = 0; a < 3; ++a)
            pos[a] = plan.plane_positions(a);
        for (int a = 0; a < 3; ++a) {
            if (a == pl.axis)
                continue;
            const int c = (*cell)[a];
            if (c > 0 && size_t(c) - 1 < pos[a].size())
                lo[a] = pos[a][size_t(c) - 1];
            if (size_t(c) < pos[a].size())
                hi[a] = pos[a][size_t(c)];
        }
    }
    lo[pl.axis] = pl.pos - 0.5;
    hi[pl.axis] = pl.pos + 0.5;
    out.merge(lo);
    out.merge(hi);
    return out;
}

// P4: a plan that was asked to stop (the panel changed under it).
struct PlanCanceled : public std::exception
{
    const char *what() const noexcept override { return "Split to Fit plan canceled"; }
};

static wxString refusal_text(SplitManualRefusal why)
{
    switch (why) {
    case SplitManualRefusal::NotOnFace: return _L("click on the material of a cut face");
    case SplitManualRefusal::NearEdge: return _L("too close to the edge of the face");
    case SplitManualRefusal::NearOtherPlane: return _L("too close to another cut");
    case SplitManualRefusal::NearDowel: return _L("too close to another dowel (see Spacing)");
    case SplitManualRefusal::TooLittleMaterial: return _L("too little material behind it");
    }
    return wxString();
}

} // namespace split_fit_gizmo_detail

wxString split_warning_text(const SplitWarning &w, const SplitPlan &plan)
{
    // P5: a warning about a piece names the piece ("X1-Y2_b"), else the section.
    const std::string part  = w.piece >= 0 && size_t(w.piece) < plan.pieces.size() ? plan.pieces[size_t(w.piece)].name :
                              w.part >= 0 && size_t(w.part) < plan.parts.size()   ? plan.parts[size_t(w.part)].name :
                                                                                     std::string();
    const std::string plane = w.plane >= 0 && size_t(w.plane) < plan.planes.size() ? plan.planes[size_t(w.plane)].name() : std::string();
    // A face by the two pieces it joins ("X1-Y1 | X2-Y1_b"), else by its code or plane.
    std::string face = plane;
    if (w.mate >= 0 && size_t(w.mate) < plan.mates.size()) {
        const SplitMate &m = plan.mates[size_t(w.mate)];
        const std::string below = m.piece_below >= 0 ? plan.pieces[size_t(m.piece_below)].name : std::string();
        const std::string above = m.piece_above >= 0 ? plan.pieces[size_t(m.piece_above)].name : std::string();
        face = m.code + " (" + below + " | " + above + ")";
    } else if (w.face >= 0 && size_t(w.face) < plan.faces.size()) {
        const SplitFace &f = plan.faces[size_t(w.face)];
        face               = f.mate_label.empty() || f.mate_label == " | " ? f.mate_code : f.mate_label;
    }
    switch (w.kind) {
    case SplitWarning::Kind::NoUsableVolume:
        return _L("The margins leave no room on this printer. Make them smaller.");
    case SplitWarning::Kind::TooManyParts:
        return format_wxstr(_L("This would make %1% parts, more than the %2% Split to Fit makes. Check the margins, or scale the model down first."),
                            int(w.value), SPLIT_MAX_CELLS);
    case SplitWarning::Kind::PartDoesNotFit: {
        const Vec3d s = w.piece >= 0 && size_t(w.piece) < plan.pieces.size() ? plan.pieces[size_t(w.piece)].size() :
                        w.part >= 0 && size_t(w.part) < plan.parts.size()     ? plan.parts[size_t(w.part)].size() :
                                                                                 Vec3d::Zero();
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
    case SplitWarning::Kind::PlaneMoved: {
        const std::string by = format("%+.1f", w.value);
        switch (SplitAdjustReason(w.detail)) {
        case SplitAdjustReason::ThinPieces:
            return format_wxstr(_L("Plane %1% moved %2% mm to avoid thin pieces on the cut face."), plane, by);
        case SplitAdjustReason::DowelRoom:
            return format_wxstr(_L("Plane %1% moved %2% mm to leave more room for dowels."), plane, by);
        case SplitAdjustReason::FineDetail:
            return format_wxstr(_L("Plane %1% moved %2% mm to avoid fine detail."), plane, by);
        case SplitAdjustReason::RaggedOutline:
            return format_wxstr(_L("Plane %1% moved %2% mm to get a simpler cut face."), plane, by);
        case SplitAdjustReason::FlatFace:
            return format_wxstr(_L("Plane %1% moved %2% mm away from a flat face, so no paper-thin layer is left."), plane, by);
        case SplitAdjustReason::KeepFit:
            return format_wxstr(_L("Plane %1% moved %2% mm so the part next to it still fits the printer."), plane, by);
        default: return format_wxstr(_L("Plane %1% moved %2% mm to a better place to cut."), plane, by);
        }
    }
    case SplitWarning::Kind::AdjustRefused:
        return format_wxstr(_L("Plane %1% would cut better %2% mm away, but the part next to it would then not fit the printer."), plane,
                            format("%+.1f", w.value));
    case SplitWarning::Kind::PlaneInDetail:
        return format_wxstr(_L("Plane %1% cuts through fine detail or thin pieces, and there is no better place within %2% mm. "
                               "Drag it if that matters."),
                            plane, format("%.0f", w.value));
    case SplitWarning::Kind::AdjustImpossible: {
        static const char *axes[3] = { "X", "Y", "Z" };
        return format_wxstr(_L("The %1% planes were left as laid out: moving them would break the size rules."),
                            axes[std::clamp(int(w.value), 0, 2)]);
    }
    case SplitWarning::Kind::SmallPart:
        return format_wxstr(_L("Part %1% is very small (%2% cm3). Drag a plane to make it bigger, or take care when gluing it."), part,
                            format("%.2f", w.value / 1000.));
    case SplitWarning::Kind::NoFlatFace:
        return format_wxstr(_L("Part %1% has no flat face to stand on (%2% cm2 touches the bed): it needs supports, or turn it by hand."),
                            part, format("%.2f", w.value / 100.));
    case SplitWarning::Kind::DiscardedPieces:
        return format_wxstr(_L_PLURAL("%1% small piece (%2% cm3 in all) is left out: \"Discard pieces smaller than\" is on.",
                                      "%1% small pieces (%2% cm3 in all) are left out: \"Discard pieces smaller than\" is on.", int(w.value)),
                            int(w.value), format("%.2f", w.value2 / 1000.));
    case SplitWarning::Kind::LabelSmaller:
        return format_wxstr(_L("Face %1%: the label is smaller (%2% mm tall) to fit the face."), face, format("%.1f", w.value));
    case SplitWarning::Kind::LabelPattern:
        return format_wxstr(_L("Face %1%: no room for the text, so it gets the dot pattern instead."), face);
    case SplitWarning::Kind::NoLabel:
        return format_wxstr(_L("Face %1% gets no label: there is no room clear of the edges and the dowels. Mark it by hand."), face);
    case SplitWarning::Kind::ManualSiteRefused:
        return format_wxstr(_L("No dowel added there: %1%."), split_fit_gizmo_detail::refusal_text(SplitManualRefusal(int(w.value))));
    }
    return wxString();
}

GLGizmoSplitToFit::GLGizmoSplitToFit(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{}

GLGizmoSplitToFit::~GLGizmoSplitToFit()
{
    stop_planner();
    m_alive.reset();
}

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
        // P4: hand-made dowel edits belong to one opening, like the moved planes.
        m_options.manual_sites.clear();
        m_manual_note.clear();
        m_click_pending = false;
        m_hover_pick    = SplitPickTarget();
        m_mesh_object = ObjectID();
        m_plan_dirty  = true;
        m_drag_plane  = -1;
        m_highlight   = BoundingBoxf3();
    } else if (m_state == Off) {
        // P4: no plan is left running for a closed gizmo.
        stop_planner();
        save_settings();
        drop_grabbers();
        m_drag_plane = -1;
        m_highlight  = BoundingBoxf3();
        m_highlight_model.reset();
        m_mesh.reset();
        m_plan = SplitPlan();
        m_planes_fill.reset();
        m_pinned_fill.reset();
        m_planes_outline.reset();
        m_bad_parts.reset();
        m_sites_fill.reset();
        m_sites_axes.reset();
        m_sites_manual.reset();
        for (GLModel &m : m_piece_boxes)
            m.reset();
        m_labels.reset();
        m_mesh_neighbors.reset();
        m_click_pending = false;
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
    // P3: the adjustment's settings live for the session only (no app config key for them yet).
    const bool              adjust       = m_options.adjust_planes;
    const double            adjust_limit = m_options.adjust_limit;
    // P5: the labels and the discard option, for the session too.
    const SplitLabelOptions labels       = m_options.labels;
    const bool              discard      = m_options.discard_small;
    const double            tiny_volume  = m_options.small_piece_volume;
    // P4: what Apply does with the parts, for the session too.
    const bool              lay_flat     = m_options.lay_flat;
    const bool              loose_plates = m_options.loose_own_plates;
    reset_options();
    if (m_adjust_seen) {
        m_options.adjust_planes      = adjust;
        m_options.adjust_limit       = adjust_limit;
        m_options.labels             = labels;
        m_options.discard_small      = discard;
        m_options.small_piece_volume = tiny_volume;
        m_options.lay_flat           = lay_flat;
        m_options.loose_own_plates   = loose_plates;
    }
    m_adjust_seen                = true;
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
    // D3 dowels on (loose round Dowel, 6 x 20 mm, up to 3 per face), D4 plane adjustment on, 10 mm.
    m_options                = SplitOptions();
    m_options.dowels.enabled = true;
    m_options.adjust_planes  = true;
    m_options.adjust_limit   = 10.;
    // P5: labels on (code, up to 6 mm tall, 0.6 mm deep); small pieces kept (discard off, 1 cm3).
    m_options.labels.enabled = true;
    // P4: parts laid on their best cut face (lead's default: on); identical dowels as instances of one
    // object; loose pieces arranged with the main parts unless asked (lead's default: off).
    m_options.lay_flat         = true;
    m_options.instance_dowels  = true;
    m_options.loose_own_plates = false;
}

void GLGizmoSplitToFit::refresh_inputs()
{
    int          instance = 0;
    ModelObject *mo       = selected_object(&instance);
    if (mo == nullptr || size_t(instance) >= mo->instances.size()) {
        if (m_mesh && !m_mesh->indices.empty()) {
            m_mesh.reset();
            m_mesh_neighbors.reset();
            m_plan_dirty = true;
        }
        m_mesh_object = ObjectID();
        return;
    }
    const Transform3d trafo = mo->instances[size_t(instance)]->get_transformation().get_matrix();
    if (mo->id() != m_mesh_object || !trafo.isApprox(m_mesh_trafo) || mo->volumes.size() != m_mesh_volumes) {
        if (mo->id() != m_mesh_object) {
            m_options.pinned.clear();
            m_options.manual_sites.clear();
        }
        auto mesh      = std::make_shared<indexed_triangle_set>(split_object_mesh(*mo, instance));
        // P5: once per mesh, not per plan.
        m_mesh_neighbors = std::make_shared<const std::vector<Vec3i32>>(its_face_neighbors_par(*mesh));
        m_mesh           = std::move(mesh);
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

// ---- P4: planning off the UI thread ---------------------------------------------------------------------

void GLGizmoSplitToFit::request_plan()
{
    m_plan_dirty = false;
    ++m_plan_wanted;
    if (m_plan_running) {
        // The plan under way is out of date: stop it; plan_finished() starts the newest one.
        if (m_plan_cancel)
            m_plan_cancel->store(true);
        return;
    }
    start_plan();
}

void GLGizmoSplitToFit::start_plan()
{
    if (m_plan_thread.joinable())
        m_plan_thread.join();   // finished already (its callback has not run yet): returns at once
    const uint64_t gen = m_plan_wanted;
    if (!m_mesh || m_mesh->indices.empty()) {
        // Nothing to plan: an empty plan, at once.
        adopt_plan(SplitPlan());
        m_plan_adopted = gen;
        return;
    }
    m_plan_running     = true;
    m_plan_running_gen = gen;
    m_plan_cancel      = std::make_shared<std::atomic<bool>>(false);
    // Everything the planner reads is copied or shared here, on the UI thread.
    std::shared_ptr<const indexed_triangle_set> mesh      = m_mesh;
    std::shared_ptr<const std::vector<Vec3i32>> neighbors = m_mesh_neighbors;
    std::shared_ptr<std::atomic<bool>>          cancel    = m_plan_cancel;
    const SplitUsableVolume                     usable    = split_usable_volume(m_printer, m_options);
    const SplitOptions                          options   = m_options;
    m_plan_thread = std::thread([this, mesh, neighbors, cancel, usable, options, gen]() {
        std::unique_ptr<SplitPlan> result;
        try {
            result = std::make_unique<SplitPlan>(split_plan_grid(
                *mesh, usable, options,
                [cancel]() {
                    if (cancel->load())
                        throw split_fit_gizmo_detail::PlanCanceled();
                },
                neighbors.get()));
        } catch (const split_fit_gizmo_detail::PlanCanceled &) {
            result.reset();
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(error) << "Split to Fit: planning failed: " << e.what();
            // A cancel that came back wrapped (from a parallel loop) is still a cancel; a real failure
            // gives an empty plan, as before.
            result.reset();
            if (!cancel->load())
                result = std::make_unique<SplitPlan>();
        }
        {
            std::lock_guard<std::mutex> lk(m_plan_mutex);
            m_plan_result     = std::move(result);
            m_plan_result_gen = gen;
        }
        // Back on the UI thread, if the gizmo still exists then (plan_finished() checks that this plan
        // is still the one it waits for: the gizmo may have been closed and opened again).
        std::weak_ptr<int> alive = m_alive;
        wxGetApp().CallAfter([this, alive, gen]() {
            if (alive.lock())
                plan_finished(gen);
        });
    });
    m_parent.set_as_dirty();
}

void GLGizmoSplitToFit::plan_finished(uint64_t gen)
{
    // A callback of a thread that was stopped and joined already (the gizmo closed in between).
    if (!m_plan_running || gen != m_plan_running_gen)
        return;
    if (m_plan_thread.joinable())
        m_plan_thread.join();
    m_plan_running = false;
    std::unique_ptr<SplitPlan> result;
    {
        std::lock_guard<std::mutex> lk(m_plan_mutex);
        if (m_plan_result_gen == gen)
            result = std::move(m_plan_result);
        m_plan_result.reset();
    }
    if (result && gen == m_plan_wanted && !m_plan_dirty) {
        adopt_plan(std::move(*result));
        m_plan_adopted = gen;
    } else if (!m_plan_dirty)
        // The inputs changed while it ran: plan again for the newest ones (a change not yet asked for
        // is asked for by the next frame).
        start_plan();
    m_parent.set_as_dirty();
    m_parent.request_extra_frame();
}

void GLGizmoSplitToFit::stop_planner()
{
    if (m_plan_cancel)
        m_plan_cancel->store(true);
    if (m_plan_thread.joinable())
        m_plan_thread.join();
    m_plan_running = false;
    m_plan_cancel.reset();
    std::lock_guard<std::mutex> lk(m_plan_mutex);
    m_plan_result.reset();
}

void GLGizmoSplitToFit::adopt_plan(SplitPlan &&plan)
{
    m_plan          = std::move(plan);
    m_preview_dirty = true;

    // P4: a refused hand-made dowel says why once (in the panel) and is dropped; edits for planes that
    // have moved since are dropped too (they no longer apply).
    auto same = [](const SplitManualSite &l, const SplitManualSite &r) {
        return l.axis == r.axis && l.index == r.index && l.add == r.add && l.plane_pos == r.plane_pos && (l.pos - r.pos).norm() < 1e-9;
    };
    std::vector<SplitWarning> kept;
    int                       newest = -1;
    for (const SplitWarning &w : m_plan.warnings) {
        if (w.kind != SplitWarning::Kind::ManualSiteRefused) {
            kept.push_back(w);
            continue;
        }
        if (w.detail >= 0 && size_t(w.detail) < m_plan.manual_sites.size()) {
            const SplitManualSite &m = m_plan.manual_sites[size_t(w.detail)];
            auto it = std::find_if(m_options.manual_sites.begin(), m_options.manual_sites.end(),
                                   [&](const SplitManualSite &x) { return same(x, m); });
            if (it != m_options.manual_sites.end())
                m_options.manual_sites.erase(it);
            if (w.detail > newest) {
                newest        = w.detail;
                m_manual_note = into_u8(split_warning_text(w, m_plan));
            }
        }
    }
    m_plan.warnings = std::move(kept);
    m_options.manual_sites.erase(std::remove_if(m_options.manual_sites.begin(), m_options.manual_sites.end(),
                                                [this](const SplitManualSite &m) {
                                                    const int i = m_plan.plane_index(m.axis, m.index);
                                                    return i < 0 || std::abs(m_plan.planes[size_t(i)].pos - m.plane_pos) > 1e-6;
                                                }),
                                 m_options.manual_sites.end());

    // The old plan's planes and parts are gone: so is a drag in the panel, and the highlight.
    if (!m_dragging)
        m_drag_plane = -1;
    m_highlight = BoundingBoxf3();
    m_highlight_model.reset();
    m_hover_pick = SplitPickTarget();
    sync_grabbers();
}

// ---- P3: plane handles -----------------------------------------------------------------------------

void GLGizmoSplitToFit::drop_grabbers()
{
    for (Grabber &g : m_grabbers)
        if (g.picking_id >= 0)
            g.unregister_raycasters_for_picking();
    m_grabbers.clear();
    if (m_hover_id >= 0)
        m_hover_id = -1;
}

void GLGizmoSplitToFit::sync_grabbers()
{
    // Never under a running drag: the grabber being dragged must stay.
    if (m_dragging)
        return;
    drop_grabbers();
    if (m_state != On)
        return;
    m_grabbers.resize(m_plan.planes.size());
    for (size_t i = 0; i < m_plan.planes.size(); ++i) {
        const int a           = m_plan.planes[i].axis;
        Grabber  &g           = m_grabbers[i];
        g.color               = AXES_COLOR[size_t(a)];
        g.hover_color         = AXES_HOVER_COLOR[size_t(a)];
        // The arrows show the one direction the handle moves in.
        static const EGrabberExtension ext[3] = { EGrabberExtension(int(EGrabberExtension::PosX) | int(EGrabberExtension::NegX)),
                                                  EGrabberExtension(int(EGrabberExtension::PosY) | int(EGrabberExtension::NegY)),
                                                  EGrabberExtension(int(EGrabberExtension::PosZ) | int(EGrabberExtension::NegZ)) };
        g.extensions = ext[a];
    }
    register_grabbers_for_picking();
    update_grabbers();
}

double GLGizmoSplitToFit::shown_pos(size_t i) const
{
    return m_drag_plane >= 0 && size_t(m_drag_plane) == i ? m_drag_pos : m_plan.planes[i].pos;
}

void GLGizmoSplitToFit::update_grabbers()
{
    if (m_grabbers.size() != m_plan.planes.size())
        return;
    // Each handle sits on its plane's edge, just outside the model: on top for X and Y planes
    // (centred across), behind the model for Z planes.
    const BoundingBoxf3 &bb  = m_plan.model_bbox;
    const double         pad = std::max(5., 0.04 * bb.size().maxCoeff());
    for (size_t i = 0; i < m_plan.planes.size(); ++i) {
        const int a = m_plan.planes[i].axis;
        Vec3d     c = bb.center();
        c[a]        = shown_pos(i);
        if (a == 2)
            c.y() = bb.max.y() + pad;
        else
            c.z() = bb.max.z() + pad;
        m_grabbers[i].center = c;
        m_grabbers[i].matrix = Transform3d::Identity();
    }
}

void GLGizmoSplitToFit::on_register_raycasters_for_picking()
{
    // The handles are drawn on top of the scene; picking has to agree.
    m_parent.set_raycaster_gizmos_on_top(true);
}

void GLGizmoSplitToFit::on_unregister_raycasters_for_picking()
{
    m_parent.set_raycaster_gizmos_on_top(false);
}

bool GLGizmoSplitToFit::on_mouse(const wxMouseEvent &mouse_event)
{
    if (!m_grabbers.empty() && use_grabbers(mouse_event))
        return true;
    if (m_dragging)
        return false;

    // P4: a click on a dowel disc removes it, a click on a cut face adds one there. The click is taken
    // only over a disc or the material of a cut face, so a drag anywhere else still turns the view.
    const Vec2d mouse(double(mouse_event.GetX()), double(mouse_event.GetY()));
    const bool  can_edit = m_options.dowels.enabled && m_plan.valid && m_drag_plane < 0;
    if (mouse_event.Moving()) {
        const SplitPickTarget hover = can_edit && m_hover_id < 0 ? pick_at(mouse) : SplitPickTarget();
        if (hover.kind != m_hover_pick.kind || hover.site != m_hover_pick.site || hover.mate != m_hover_pick.mate) {
            m_hover_pick = hover;
            m_parent.set_as_dirty();
        }
        return false;
    }
    if (mouse_event.LeftDown()) {
        m_click_pending = false;
        if (!can_edit || m_hover_id >= 0 || mouse_event.ShiftDown() || mouse_event.ControlDown() || mouse_event.AltDown())
            return false;
        const SplitPickTarget target = pick_at(mouse);
        if (target.kind == SplitPickTarget::Kind::None)
            return false;
        m_click_pending = true;
        m_click_start   = mouse;
        m_click_target  = target;
        return true;
    }
    if (m_click_pending) {
        if (mouse_event.Dragging())
            return true;
        if (mouse_event.LeftUp()) {
            m_click_pending = false;
            // A click, not a drag: the button came up where it went down.
            if ((mouse - m_click_start).norm() <= 5. * double(m_parent.get_scale()))
                click_site(m_click_target);
            return true;
        }
    }
    return false;
}

SplitPickTarget GLGizmoSplitToFit::pick_at(const Vec2d &mouse) const
{
    const Linef3 ray = const_cast<GLCanvas3D &>(m_parent).mouse_ray(Point(coord_t(mouse.x()), coord_t(mouse.y())));
    return split_pick(m_plan, ray.a, ray.b - ray.a);
}

void GLGizmoSplitToFit::click_site(const SplitPickTarget &t)
{
    if (t.plane < 0 || size_t(t.plane) >= m_plan.planes.size())
        return;
    const SplitPlane &pl = m_plan.planes[size_t(t.plane)];
    m_manual_note.clear();
    if (t.kind == SplitPickTarget::Kind::Site && t.site >= 0 && size_t(t.site) < m_plan.sites.size()) {
        const SplitSite &site = m_plan.sites[size_t(t.site)];
        // A dowel the user added goes away with its edit; an automatic one is taken off by a new edit.
        auto it = std::find_if(m_options.manual_sites.begin(), m_options.manual_sites.end(), [&](const SplitManualSite &m) {
            return m.add && m.axis == pl.axis && m.index == pl.index && (m.pos - site.pos).norm() < 1e-6;
        });
        if (site.manual && it != m_options.manual_sites.end())
            m_options.manual_sites.erase(it);
        else
            m_options.manual_sites.push_back({ pl.axis, pl.index, pl.pos, site.pos, false });
    } else if (t.kind == SplitPickTarget::Kind::Face)
        m_options.manual_sites.push_back({ pl.axis, pl.index, pl.pos, t.pos, true });
    else
        return;
    m_hover_pick = SplitPickTarget();
    m_plan_dirty = true;
    m_parent.set_as_dirty();
    m_parent.request_extra_frame();
}

void GLGizmoSplitToFit::on_start_dragging()
{
    if (m_hover_id < 0 || size_t(m_hover_id) >= m_plan.planes.size()) {
        m_drag_plane = -1;
        return;
    }
    m_drag_plane     = m_hover_id;
    m_drag_pos       = m_plan.planes[size_t(m_hover_id)].pos;
    m_drag_start_pos = m_drag_pos;
    m_drag_have_t    = false;
    m_preview_dirty  = true;
}

void GLGizmoSplitToFit::on_dragging(const UpdateData &data)
{
    if (m_drag_plane < 0 || size_t(m_drag_plane) >= m_plan.planes.size())
        return;
    const int   a = m_plan.planes[size_t(m_drag_plane)].axis;
    Vec3d       e = Vec3d::Zero();
    e[a]          = 1.;
    // The point of the plane's axis (through the handle) nearest the mouse ray.
    const Vec3d anchor = m_grabbers[size_t(m_drag_plane)].center;
    const Vec3d d      = data.mouse_ray.unit_vector();
    const Vec3d w0     = anchor - data.mouse_ray.a;
    const double b     = e.dot(d);
    const double denom = 1. - b * b;
    if (denom < 1e-6)
        return;   // looking straight down the axis: no sensible move
    // Measured from the handle's own place, which follows the drag: add back where it already is.
    const double t = (b * d.dot(w0) - e.dot(w0)) / denom + (anchor[a] - m_drag_start_pos);
    if (!m_drag_have_t) {
        m_drag_start_t = t;
        m_drag_have_t  = true;
    }
    double pos = m_drag_start_pos + (t - m_drag_start_t);
    // Snap to 0.1 mm (Shift: 1 mm), inside the model.
    const double snap = wxGetKeyState(WXK_SHIFT) ? 1. : 0.1;
    pos               = snap * std::round(pos / snap);
    const double lo   = m_plan.model_bbox.min[a] + 0.1;
    const double hi   = m_plan.model_bbox.max[a] - 0.1;
    if (hi > lo)
        pos = std::clamp(pos, lo, hi);
    if (pos != m_drag_pos) {
        m_drag_pos      = pos;
        m_preview_dirty = true;
        update_grabbers();
    }
}

void GLGizmoSplitToFit::on_stop_dragging()
{
    commit_drag();
}

void GLGizmoSplitToFit::commit_drag()
{
    if (m_drag_plane < 0 || size_t(m_drag_plane) >= m_plan.planes.size()) {
        m_drag_plane = -1;
        return;
    }
    const SplitPlane &pl    = m_plan.planes[size_t(m_drag_plane)];
    const double      pos   = m_drag_pos;
    const bool        moved = std::abs(pos - pl.pos) > 1e-6;
    if (moved) {
        auto it = std::find_if(m_options.pinned.begin(), m_options.pinned.end(),
                               [&pl](const SplitPinnedPlane &p) { return p.axis == pl.axis && p.index == pl.index; });
        if (it == m_options.pinned.end())
            m_options.pinned.push_back({ pl.axis, pl.index, pos });
        else
            it->pos = pos;
        // The full plan (adjustment of the other planes, dowel sites) is made once, now (P4: on the
        // planner thread). Until it is in, the plane is shown where it was dropped.
        m_plan_dirty = true;
        m_plan.planes[size_t(m_drag_plane)].pos    = pos;
        m_plan.planes[size_t(m_drag_plane)].pinned = true;
    }
    m_drag_plane    = -1;
    m_preview_dirty = true;
    m_parent.set_as_dirty();
    m_parent.request_extra_frame();
}

std::string GLGizmoSplitToFit::drag_lengths(size_t i, double pos, bool *too_long) const
{
    const SplitPlane   &pl  = m_plan.planes[i];
    const int           a   = pl.axis;
    std::vector<double> xs  = m_plan.plane_positions(a);
    const double        prev = pl.index > 0 && size_t(pl.index) - 1 < xs.size() ? xs[size_t(pl.index) - 1] : m_plan.model_bbox.min[a];
    const double        next = size_t(pl.index) + 1 < xs.size() ? xs[size_t(pl.index) + 1] : m_plan.model_bbox.max[a];
    const double        l0 = pos - prev, l1 = next - pos;
    if (too_long != nullptr)
        *too_long = l0 > m_plan.usable.size[a] + 1e-6 || l1 > m_plan.usable.size[a] + 1e-6;
    return Slic3r::format("%.1f | %.1f mm", l0, l1);
}

std::string GLGizmoSplitToFit::get_tooltip() const
{
    const int i = m_drag_plane >= 0 ? m_drag_plane : m_hover_id;
    if (i < 0 || size_t(i) >= m_plan.planes.size()) {
        // P4: what a click does here.
        if (m_hover_pick.kind == SplitPickTarget::Kind::Site)
            return _u8L("Click to remove this dowel.");
        if (m_hover_pick.kind == SplitPickTarget::Kind::Face)
            return _u8L("Click to add a dowel here.");
        return std::string();
    }
    const double pos      = shown_pos(size_t(i));
    bool         too_long = false;
    std::string  out      = m_plan.planes[size_t(i)].name() + ": " + Slic3r::format("%.1f mm", pos) + "\n" +
                      _u8L("Parts either side") + ": " + drag_lengths(size_t(i), pos, &too_long);
    if (too_long)
        out += "\n" + _u8L("A part would be too long for the printer.");
    if (m_drag_plane < 0)
        out += "\n" + _u8L("Drag to move this plane. A moved plane stays where you put it.");
    return out;
}

void GLGizmoSplitToFit::show_warning(const SplitWarning &w)
{
    BoundingBoxf3 box;
    if (w.piece >= 0 && size_t(w.piece) < m_plan.pieces.size())
        box = m_plan.pieces[size_t(w.piece)].bbox;
    else if (w.part >= 0 && size_t(w.part) < m_plan.parts.size() && !m_plan.parts[size_t(w.part)].empty)
        box = m_plan.parts[size_t(w.part)].bbox;
    else if (w.face >= 0 && size_t(w.face) < m_plan.faces.size())
        box = split_fit_gizmo_detail::plane_box(m_plan, m_plan.faces[size_t(w.face)].plane, &m_plan.faces[size_t(w.face)].cell_below);
    else if (w.plane >= 0)
        box = split_fit_gizmo_detail::plane_box(m_plan, w.plane, nullptr);
    if (!box.defined)
        return;
    m_highlight = box;
    m_highlight_model.reset();
    // Zoom to it with some of the model round it.
    BoundingBoxf3 view = box;
    const Vec3d   grow = Vec3d::Constant(std::max(10., 0.25 * box.size().maxCoeff()));
    view.min -= grow;
    view.max += grow;
    wxGetApp().plater()->get_camera().zoom_to_box(view);
    m_parent.set_as_dirty();
    m_parent.request_extra_frame();
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
    m_sites_manual.reset();
    for (GLModel &m : m_piece_boxes)
        m.reset();
    m_labels.reset();
    if (m_plan.planes.empty() && m_plan.pieces_not_fitting() == 0)
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

    for (size_t pi = 0; pi < m_plan.planes.size(); ++pi) {
        const SplitPlane &pl = m_plan.planes[pi];
        const int a = pl.axis, u = (a + 1) % 3, v = (a + 2) % 3;
        Vec3d     c[4];
        for (int k = 0; k < 4; ++k) {
            // P3: a plane being dragged is drawn under the mouse (orange: it will be pinned).
            c[k][a] = shown_pos(pi);
            c[k][u] = (k == 1 || k == 2) ? hi[u] : lo[u];
            c[k][v] = (k >= 2) ? hi[v] : lo[v];
        }
        Geo         &g    = pl.pinned || int(pi) == m_drag_plane ? pinned : fill;
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

    static const int box_edges[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 },
                                          { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
    auto add_box = [](Geo &g, const BoundingBoxf3 &box) {
        const Vec3d   &mn = box.min, &mx = box.max;
        const unsigned b  = unsigned(g.vertices_count());
        for (int k = 0; k < 8; ++k)
            g.add_vertex(Vec3f(float((k & 1) ? mx.x() : mn.x()), float((k & 2) ? mx.y() : mn.y()), float((k & 4) ? mx.z() : mn.z())));
        for (const auto &e : box_edges)
            g.add_line(b + unsigned(e[0]), b + unsigned(e[1]));
    };

    // The boxes of the parts (P5: pieces) that would not fit.
    for (const SplitPiece &piece : m_plan.pieces)
        if (!piece.discarded && !piece.fits && piece.bbox.defined)
            add_box(bad, piece.bbox);

    // P5: the pieces of a section that falls apart, each in its own colour (discarded ones grey), so the
    // loose bits Apply will make into parts of their own show before Apply.
    {
        static const ColorRGBA palette[6] = { ColorRGBA(0.16f, 0.62f, 0.95f, 1.f), ColorRGBA(0.85f, 0.33f, 0.85f, 1.f),
                                              ColorRGBA(0.20f, 0.78f, 0.35f, 1.f), ColorRGBA(0.95f, 0.55f, 0.10f, 1.f),
                                              ColorRGBA(0.55f, 0.45f, 0.95f, 1.f), ColorRGBA(0.10f, 0.75f, 0.75f, 1.f) };
        std::vector<Geo> per_colour(7);
        for (Geo &g : per_colour)
            g.format = { Geo::EPrimitiveType::Lines, Geo::EVertexLayout::P3 };
        for (size_t s = 0; s < m_plan.parts.size(); ++s) {
            const std::vector<int> list = m_plan.pieces_of(int(s));
            if (list.size() < 2)
                continue;
            for (int i : list) {
                const SplitPiece &p = m_plan.pieces[size_t(i)];
                if (p.bbox.defined)
                    add_box(per_colour[p.discarded ? 6 : size_t(p.index) % 6], p.bbox);
            }
        }
        for (size_t c = 0; c < per_colour.size() && c < m_piece_boxes.size(); ++c)
            if (per_colour[c].vertices_count() > 0) {
                m_piece_boxes[c].init_from(std::move(per_colour[c]));
                m_piece_boxes[c].set_color(c < 6 ? palette[c] : ColorRGBA(0.55f, 0.55f, 0.55f, 1.f));
            }
    }

    // P5: the labels, as the outlines of their glyphs on the plane (one of each pair: both sit on the
    // same spot), drawn as they read from the piece below the plane.
    {
        Geo lbl;
        lbl.format = { Geo::EPrimitiveType::Lines, Geo::EVertexLayout::P3 };
        for (const SplitLabel &l : m_plan.labels) {
            if (!l.below)
                continue;
            if (l.mate >= 0 && size_t(l.mate) < m_plan.mates.size()) {
                const int face = m_plan.mates[size_t(l.mate)].face;
                if (face >= 0 && size_t(face) < m_plan.faces.size() && m_plan.faces[size_t(face)].plane == m_drag_plane)
                    continue;   // placed again when the plane is let go
            }
            const ExPolygons shapes = l.pattern ? split_label_pattern_shapes(l.number, l.stroke) :
                                                  split_label_text_shapes(l.text, l.height, l.stroke);
            auto add_poly = [&](const Polygon &poly) {
                const unsigned b = unsigned(lbl.vertices_count());
                for (const Point &pt : poly.points) {
                    const Vec2d q = unscale(pt);
                    lbl.add_vertex(Vec3f((l.center + q.x() * l.right + q.y() * l.up).cast<float>()));
                }
                const unsigned n = unsigned(poly.points.size());
                for (unsigned k = 0; k < n; ++k)
                    lbl.add_line(b + k, b + (k + 1) % n);
            };
            for (const ExPolygon &ex : shapes) {
                add_poly(ex.contour);
                for (const Polygon &h : ex.holes)
                    add_poly(h);
            }
        }
        if (lbl.vertices_count() > 0) {
            m_labels.init_from(std::move(lbl));
            m_labels.set_color(ColorRGBA(0.98f, 0.76f, 0.12f, 0.95f));
        }
    }

    // P2: each dowel site as a disc of its hole's size on its plane, and its length as a line
    // through it along the plane's normal.
    Geo sites, axes, manual;
    sites.format  = { Geo::EPrimitiveType::Triangles, Geo::EVertexLayout::P3 };
    axes.format   = { Geo::EPrimitiveType::Lines, Geo::EVertexLayout::P3 };
    manual.format = { Geo::EPrimitiveType::Triangles, Geo::EVertexLayout::P3 };
    static constexpr int DISC_SEGMENTS = 24;
    for (const SplitSite &s : m_plan.sites) {
        if (s.plane < 0 || size_t(s.plane) >= m_plan.planes.size())
            continue;
        // The dowels of a plane being dragged are placed again when it is let go.
        if (s.plane == m_drag_plane)
            continue;
        const int    a = m_plan.planes[size_t(s.plane)].axis, u = (a + 1) % 3, v = (a + 2) % 3;
        const double r = std::max(0.5, s.footprint_radius);
        // P4: a dowel added by hand is drawn in its own colour.
        Geo           &disc = s.manual ? manual : sites;
        const unsigned c    = unsigned(disc.vertices_count());
        disc.add_vertex(Vec3f(s.pos.cast<float>()));
        for (int k = 0; k < DISC_SEGMENTS; ++k) {
            const double t = 2. * PI * double(k) / double(DISC_SEGMENTS);
            Vec3d        p = s.pos;
            p[u] += r * std::cos(t);
            p[v] += r * std::sin(t);
            disc.add_vertex(Vec3f(p.cast<float>()));
        }
        for (int k = 0; k < DISC_SEGMENTS; ++k)
            disc.add_triangle(c, c + 1 + unsigned(k), c + 1 + unsigned((k + 1) % DISC_SEGMENTS));
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
    // A green that reads against the yellow discs, the teal planes and light and dark beds.
    if (manual.vertices_count() > 0) {
        m_sites_manual.init_from(std::move(manual));
        m_sites_manual.set_color(ColorRGBA(0.30f, 0.85f, 0.35f, 0.95f));
    }
}

void GLGizmoSplitToFit::on_render()
{
    refresh_inputs();
    if (m_plan_dirty)
        request_plan();
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
    if (m_sites_fill.is_initialized() || m_sites_axes.is_initialized() || m_sites_manual.is_initialized()) {
        glsafe(::glDisable(GL_DEPTH_TEST));
        if (m_sites_fill.is_initialized())
            m_sites_fill.render();
        if (m_sites_manual.is_initialized())
            m_sites_manual.render();
        glsafe(::glLineWidth(3.0f));
        if (m_sites_axes.is_initialized())
            m_sites_axes.render();
        glsafe(::glEnable(GL_DEPTH_TEST));
    }
    // P5: the pieces' boxes and the labels, over the model as the dowels are.
    const bool any_piece_box = std::any_of(m_piece_boxes.begin(), m_piece_boxes.end(), [](const GLModel &m) { return m.is_initialized(); });
    if (any_piece_box || m_labels.is_initialized()) {
        glsafe(::glDisable(GL_DEPTH_TEST));
        glsafe(::glLineWidth(2.0f));
        for (GLModel &m : m_piece_boxes)
            if (m.is_initialized())
                m.render();
        glsafe(::glLineWidth(1.5f));
        if (m_labels.is_initialized())
            m_labels.render();
        glsafe(::glEnable(GL_DEPTH_TEST));
    }

    // P3: what a click in the Output Log points at, outlined over everything.
    if (m_highlight.defined) {
        if (!m_highlight_model.is_initialized()) {
            GLModel::Geometry g;
            g.format       = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
            const Vec3d mn = m_highlight.min, mx = m_highlight.max;
            for (int k = 0; k < 8; ++k)
                g.add_vertex(Vec3f(float((k & 1) ? mx.x() : mn.x()), float((k & 2) ? mx.y() : mn.y()), float((k & 4) ? mx.z() : mn.z())));
            static const int edges[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 },
                                              { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
            for (const auto &e : edges)
                g.add_line(unsigned(e[0]), unsigned(e[1]));
            m_highlight_model.init_from(std::move(g));
            m_highlight_model.set_color(ColorRGBA::ORANGE());
        }
        glsafe(::glDisable(GL_DEPTH_TEST));
        glsafe(::glLineWidth(3.0f));
        m_highlight_model.render();
        glsafe(::glEnable(GL_DEPTH_TEST));
    }
    glsafe(::glLineWidth(1.0f));

    glsafe(::glEnable(GL_CULL_FACE));
    glsafe(::glDisable(GL_BLEND));
    shader->stop_using();

    // P3: the plane handles, on top of the scene (picking agrees, see on_register_raycasters_for_picking()).
    if (!m_grabbers.empty() && m_grabbers.size() == m_plan.planes.size()) {
        update_grabbers();
        glsafe(::glClear(GL_DEPTH_BUFFER_BIT));
        render_grabbers(0, m_grabbers.size() - 1, 0.f, false);
    }
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
        request_plan();

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

    // A collapsible section (the same header the Output Log uses). Its open / closed state is kept in the
    // app config as "split_fit_open_<id>" and read once; a missing key means the default given here.
    auto section = [&](const char *id, const wxString &title, bool default_open) -> bool {
        auto it = m_section_open.find(id);
        if (it == m_section_open.end()) {
            const std::string v = wxGetApp().app_config->get(std::string("split_fit_open_") + id);
            it = m_section_open.emplace(id, v.empty() ? default_open : v == "1").first;
        }
        ImGui::SetNextItemOpen(it->second, ImGuiCond_Always);
        // The Orca accent, not the toolbar style's orange: the tints the app derives from it for headers
        // (hover 0.50, active 0.75 as in ImGuiWrapper's style), with a lighter resting fill.
        auto orca = [](float alpha) { ImVec4 c = ImGuiWrapper::COL_ORCA; c.w = alpha; return c; };
        ImGui::PushStyleColor(ImGuiCol_Header, orca(0.30f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, orca(0.50f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, orca(0.75f));
        const bool open = ImGui::CollapsingHeader((into_u8(title) + "##split_sec_" + id).c_str());
        ImGui::PopStyleColor(3);
        if (open != it->second) {
            it->second = open;
            wxGetApp().app_config->set(std::string("split_fit_open_") + id, open ? "1" : "0");
        }
        return open;
    };

    // ---- the body: scrolls when the panel would be taller than the 3D view; the action row below it is
    // pinned. The body's height is the last frame's content height, held to what is left of the canvas
    // under the panel's top (and above its bottom limit) once the action row and a margin are taken off.
    // The first frame, before anything is measured, takes all of it; the next frame shrinks to fit.
    const ImGuiStyle &style = ImGui::GetStyle();
    float limit_y = float(m_parent.get_canvas_size().get_height());
    if (bottom_limit > 0.f)
        limit_y = std::min(limit_y, bottom_limit);
    const float footer_h = m_footer_h > 0.f ? m_footer_h : ImGui::GetFrameHeight() * 1.3f + style.ItemSpacing.y * 3.f;
    const float avail_h  = std::max(em * 6.f, limit_y - ImGui::GetCursorScreenPos().y - footer_h - style.WindowPadding.y - em * 0.75f);
    const bool  scrolls  = m_body_content_h <= 0.f || m_body_content_h > avail_h;
    const float body_h   = scrolls ? avail_h : m_body_content_h;
    const float body_w   = std::max(em * 27.f, m_table_w) + (scrolls ? style.ScrollbarSize : 0.f);
    ImGui::BeginChild("##split_body", ImVec2(body_w, body_h), false, ImGuiWindowFlags_NavFlattened);
    m_table_w = 0.f;

    // ---- the printer --------------------------------------------------------------------------------
    if (section("printer", _L("Printer area"), true)) {
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
    }

    // ---- the grid -----------------------------------------------------------------------------------
    if (section("grid", _L("Cut grid"), true)) {
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

    // ---- plane adjustment (P3) ------------------------------------------------------------------------
    if (m_imgui->bbl_checkbox(_L("Adjust planes automatically"), m_options.adjust_planes))
        changed = true;
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("Move each cut plane a little to a better place to cut: off thin necks and fine detail, clear of flat "
                            "faces, and where dowels fit. A plane never moves so far that a part stops fitting the printer, and "
                            "planes you moved yourself stay where you put them."),
                         wrap_w);
    if (m_options.adjust_planes)
        input_mm(_L("Up to"), "##split_adjust_limit", m_options.adjust_limit, 1., 50.,
                 _L("The furthest a plane may move from its even place."));

    // Planes: drag a handle in the 3D view, or a value here; a moved plane is kept where it was put
    // (orange). The plan is made again when the drag ends, not on every step of it.
    if (!m_plan.planes.empty() && section("planes", _L("Plane positions"), false)) {
        for (size_t pi = 0; pi < m_plan.planes.size(); ++pi) {
            const SplitPlane &pl  = m_plan.planes[pi];
            float             pos = float(shown_pos(pi));
            label(from_u8(pl.name()));
            ImGui::PushItemWidth(field_w * 1.6f);
            const std::string id = "##split_plane_" + pl.name();
            if (ImGui::DragFloat(id.c_str(), &pos, 0.5f, float(m_plan.model_bbox.min[pl.axis]), float(m_plan.model_bbox.max[pl.axis]), "%.1f mm")) {
                m_drag_plane    = int(pi);
                m_drag_pos      = double(pos);
                m_preview_dirty = true;
                m_parent.set_as_dirty();
            }
            // Let go (or typed in and confirmed): the plane is pinned and the plan made again.
            const bool released = ImGui::IsItemDeactivated();
            ImGui::PopItemWidth();
            if (ImGui::IsItemHovered())
                m_imgui->tooltip(_L("Drag to move this plane (or drag its handle in the 3D view). A moved plane stays where you put it; "
                                    "a part that becomes too long turns red."),
                                 wrap_w);
            const ImVec4 grey = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
            if (int(pi) == m_drag_plane) {
                bool              too_long = false;
                const std::string lengths  = drag_lengths(pi, m_drag_pos, &too_long);
                ImGui::SameLine();
                ImGuiWrapper::text_colored(too_long ? red : grey, from_u8(lengths));
            } else if (pl.pinned) {
                ImGui::SameLine();
                ImGuiWrapper::text_colored(grey, _L("(yours)"));
            } else if (pl.adjusted) {
                ImGui::SameLine();
                ImGuiWrapper::text_colored(grey, format_wxstr(_L("(moved %1% mm)"), Slic3r::format("%+.1f", pl.pos - pl.nominal)));
            }
            if (released && int(pi) == m_drag_plane && !m_dragging)
                commit_drag();
        }
        if (!m_options.pinned.empty() && m_imgui->button(_L("Put planes back"))) {
            m_options.pinned.clear();
            changed = true;
        }
    }
    }

    // ---- dowels (P2) --------------------------------------------------------------------------------
    SplitDowelOptions &dw = m_options.dowels;
    if (section("dowels", _L("Dowels"), true)) {
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
        // P4: click-to-add / remove in the 3D view.
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_w);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", into_u8(_L("Click a dowel in the view to remove it, or a cut face to add one.")).c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        if (!m_options.manual_sites.empty()) {
            size_t added = 0, removed = 0;
            for (const SplitManualSite &m : m_options.manual_sites)
                (m.add ? added : removed) += 1;
            m_imgui->text(format_wxstr(_L("By hand: %1% added, %2% removed"), added, removed));
            ImGui::SameLine();
            if (m_imgui->button(_L("Undo dowel edits"))) {
                m_options.manual_sites.clear();
                m_manual_note.clear();
                changed = true;
            }
            if (ImGui::IsItemHovered())
                m_imgui->tooltip(_L("Back to the automatic dowels. Edits are also dropped when the planes move."), wrap_w);
        }
        if (!m_manual_note.empty()) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_w);
            ImGui::PushStyleColor(ImGuiCol_Text, amber);
            ImGui::TextWrapped("%s", m_manual_note.c_str());
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
        }
    }
    }

    // ---- labels (P5) --------------------------------------------------------------------------------
    SplitLabelOptions &lb = m_options.labels;
    if (section("labels", _L("Labels and small pieces"), false)) {
    if (m_imgui->bbl_checkbox(_L("Label the cut faces"), lb.enabled))
        changed = true;
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("Recess the same code into both faces of every pair of parts that fit together, so you can tell which "
                            "part goes where. The codes are shown on the planes and listed in the Output Log."),
                         wrap_w);
    if (lb.enabled) {
        label(_L("Style"));
        ImGui::PushItemWidth(field_w * 1.8f);
        const std::vector<wxString> styles = { _L("Code"), _L("Number"), _L("Dots") };
        const int                   sel    = std::clamp(int(lb.style), 0, 2);
        if (ImGui::BeginCombo("##split_label_style", into_u8(styles[size_t(sel)]).c_str())) {
            for (int i = 0; i < 3; ++i)
                if (ImGui::Selectable(into_u8(styles[size_t(i)]).c_str(), i == sel)) {
                    lb.style = SplitLabelStyle(i);
                    changed  = true;
                }
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_L("Code: the plane and face, like X1.2. Number: a running number for each pair. Dots: a small pattern "
                                "of dots for the pair's number (the top-left dot is always there). Faces too small for text get the dots."),
                             wrap_w);
        input_mm(_L("Text height"), "##split_label_h", lb.height, 4., 30.,
                 _L("The tallest the text gets. Smaller faces get smaller text, never under 4 mm; below that the dots."));
        input_mm(_L("Depth"), "##split_label_d", lb.depth, 0.2, 3.,
                 _L("How deep the label is recessed into each face. 0.6 mm is three 0.2 mm layers."));
    }
    // Small pieces: one threshold names them "_tiny", warns about them and, if asked, leaves them out.
    {
        bool  discard = m_options.discard_small;
        if (m_imgui->bbl_checkbox(_L("Discard pieces smaller than"), discard)) {
            m_options.discard_small = discard;
            changed                 = true;
        }
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_L("Leave out crumbs smaller than this that the cuts break off. Off: they are kept as parts of their own, "
                                "with \"_tiny\" at the end of their names so you can find and delete them by hand."),
                             wrap_w);
        ImGui::SameLine();
        ImGui::PushItemWidth(field_w);
        double cm3 = m_options.small_piece_volume / 1000.;
        if (ImGui::InputDouble("##split_small_piece", &cm3, 0.0, 0.0, "%.2f") && std::isfinite(cm3)) {
            m_options.small_piece_volume = std::clamp(cm3, 0.01, 1000.) * 1000.;
            changed                      = true;
        }
        ImGui::PopItemWidth();
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_L("Pieces below this volume are marked \"_tiny\" and warned about, and left out when the box is ticked."),
                             wrap_w);
        ImGui::SameLine();
        m_imgui->text(_L("cm3"));
    }
    }

    // ---- what Apply does with the parts (P4): no new plan needed, read at Apply -----------------------
    if (section("after", _L("After splitting"), false)) {
    if (m_imgui->bbl_checkbox(_L("Lay parts on a cut face"), m_options.lay_flat))
        m_parent.set_as_dirty();
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("Turn each part to stand on the cut face that leaves the least overhang (45 degrees), if it still fits "
                            "the printer that way. A part already standing on a flat base keeps it unless a cut face is better. "
                            "A label on that face prints as a dent in the bottom."),
                         wrap_w);
    if (m_imgui->bbl_checkbox(_L("Loose parts on their own plates"), m_options.loose_own_plates))
        m_parent.set_as_dirty();
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("Arrange the pieces that broke off a section (the parts ending in _b, _c, ...) on plates of their own "
                            "instead of in the gaps between the main parts. Dowels always get plates of their own."),
                         wrap_w);
    }

    ImGui::Separator();

    // ---- the result ---------------------------------------------------------------------------------
    // P3 (owner, 2026-11-16): the parts table and the warnings fold into one "Output Log", collapsed by
    // default. The one-line result stays in view, with a count of the warnings, so nothing goes unseen.
    const bool single_copy = mo->instances.size() == 1;
    size_t     n_problems = 0, n_errors = 0, n_notes = 0;
    for (const SplitWarning &w : m_plan.warnings) {
        const Severity s = severity(w.kind);
        if (s == Severity::Info)
            ++n_notes;
        else {
            ++n_problems;
            if (s == Severity::Error)
                ++n_errors;
        }
    }
    const ImVec4 grey = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    auto         count_badge = [&]() {
        if (n_problems > 0) {
            ImGui::SameLine();
            ImGuiWrapper::text_colored(n_errors > 0 ? red : amber,
                                       format_wxstr(_L_PLURAL("%1% warning", "%1% warnings", n_problems), n_problems));
        } else if (n_notes > 0) {
            ImGui::SameLine();
            ImGuiWrapper::text_colored(grey, format_wxstr(_L_PLURAL("%1% note", "%1% notes", n_notes), n_notes));
        }
    };
    if (m_plan.valid) {
        // A rough plate count: the parts' footprints over the usable area, with room between them.
        // P5: every piece is a part (and takes room on a plate); discarded ones are not.
        double footprint = 0.;
        for (const SplitPiece &p : m_plan.pieces)
            if (!p.discarded)
                footprint += p.size().x() * p.size().y();
        const double bed    = std::max(1., usable.size.x() * usable.size.y());
        const int    plates = std::max(1, int(std::ceil(footprint * 1.3 / bed)));
        const bool   pins   = m_plan.dowel_options.type == SplitConnectorType::Pin;
        label(_L("Result"));
        // P4: the plan is being made again for the panel's new values (the old one is shown meanwhile).
        if (!plan_current()) {
            ImGuiWrapper::text_colored(amber, _L("Updating..."));
            ImGui::SameLine();
        }
        if (m_plan.dowel_options.enabled)
            m_imgui->text(format_wxstr(pins ? _L("%1% parts, %2% pins, about %3% plates") : _L("%1% parts, %2% dowels, about %3% plates"),
                                       m_plan.piece_count(), m_plan.dowel_count(), plates));
        else
            m_imgui->text(format_wxstr(_L("%1% parts, about %2% plates"), m_plan.piece_count(), plates));
        count_badge();
        // P5: where the extra parts come from, and what is left out.
        const size_t extra = m_plan.piece_count() > m_plan.part_count() ? m_plan.piece_count() - m_plan.part_count() : 0;
        if (extra > 0)
            ImGuiWrapper::text_colored(grey, format_wxstr(_L_PLURAL("(%1% loose piece is a part of its own)",
                                                                    "(%1% loose pieces are parts of their own)", extra), extra));
        if (const size_t gone = m_plan.discarded_count(); gone > 0)
            ImGuiWrapper::text_colored(amber, format_wxstr(_L_PLURAL("%1% small piece left out (%2% cm3)", "%1% small pieces left out (%2% cm3)", gone),
                                                           gone, Slic3r::format("%.2f", m_plan.discarded_volume() / 1000.)));
        if (m_plan.planes.empty())
            m_imgui->text_wrapped(_L("The model already fits the printer: there is nothing to split."), wrap_w);
    } else if (!plan_current()) {
        label(_L("Result"));
        ImGuiWrapper::text_colored(amber, _L("Updating..."));
    } else {
        // No plan: say why here, not only inside the folded log.
        for (const SplitWarning &w : m_plan.warnings)
            if (severity(w.kind) == Severity::Error) {
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_w);
                ImGui::PushStyleColor(ImGuiCol_Text, red);
                ImGui::TextWrapped("! %s", into_u8(split_warning_text(w, m_plan)).c_str());
                ImGui::PopStyleColor();
                ImGui::PopTextWrapPos();
            }
    }
    if (!single_copy)
        ImGuiWrapper::text_colored(red, format_wxstr(_L("This object has %1% copies. Split to Fit works on an object with one copy: "
                                                        "delete the other copies first."), mo->instances.size()));

    // ---- the Output Log: the parts table and the warnings, collapsed by default --------------------
    const bool log_has_content = m_plan.valid || !m_plan.warnings.empty();
    if (log_has_content) {
        m_output_log_open = section("log", _L("Output Log"), false);
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_L("Every part with its size, and every warning. Click a warning to see where it is."), wrap_w);
    }
    if (log_has_content && m_output_log_open && m_plan.valid) {
        // P5: one row per piece (the parts Apply makes), with the codes of its faces: an assembly list.
        // A piece's name column is as wide as the longest name ("X1-Y2-Z1_b_tiny").
        const float row_h  = ImGui::GetTextLineHeightWithSpacing();
        const float list_h = std::min(row_h * float(m_plan.pieces.size() + 1), em * 12.f);
        float       name_w = em * 6.f;
        for (const SplitPiece &p : m_plan.pieces)
            name_w = std::max(name_w, ImGui::CalcTextSize(p.name.c_str()).x + em);
        const float size_x = name_w, fit_x = size_x + em * 9.f, holes_x = fit_x + em * 5.5f;
        const float faces_x = m_plan.dowel_options.enabled ? holes_x + em * 4.5f : holes_x;
        // The body is at least as wide as this table (measured now, used by the next frame).
        const float table_w = std::max(em * 26.f, faces_x + em * 10.f);
        m_table_w = table_w + em;
        ImGui::BeginChild("##split_parts", ImVec2(table_w, list_h), true, ImGuiWindowFlags_HorizontalScrollbar);
        for (const SplitPiece &p : m_plan.pieces) {
            const ImVec4 text_col = p.discarded ? grey : ImGui::GetStyleColorVec4(ImGuiCol_Text);
            ImGuiWrapper::text_colored(text_col, from_u8(p.name.empty() ? mo->name : p.name));
            ImGui::SameLine(size_x);
            ImGuiWrapper::text_colored(text_col, from_u8(mm_size(p.size())));
            ImGui::SameLine(fit_x);
            if (p.discarded)
                ImGuiWrapper::text_colored(grey, _L("left out"));
            else if (p.fits)
                m_imgui->text(_L("fits"));
            else
                ImGuiWrapper::text_colored(red, _L("does not fit"));
            if (p.discarded)
                continue;
            if (m_plan.dowel_options.enabled) {
                // The holes this piece gets: one per dowel of each pair it is in.
                size_t holes = 0;
                for (int mi : p.mates)
                    holes += m_plan.mates[size_t(mi)].sites.size();
                ImGui::SameLine(holes_x);
                m_imgui->text(format_wxstr(_L("%1% holes"), holes));
            }
            // The codes of the faces it mates on (labelled or not): find the part with the same code.
            std::string codes;
            for (int mi : p.mates) {
                if (!codes.empty())
                    codes += ", ";
                codes += m_plan.mates[size_t(mi)].code;
            }
            if (!codes.empty()) {
                ImGui::SameLine(faces_x);
                m_imgui->text(from_u8(codes));
            }
        }
        ImGui::EndChild();
    }

    // ---- warnings (inside the Output Log) -----------------------------------------------------------
    const SplitWarning *clicked = nullptr;
    if (log_has_content && m_output_log_open && !m_plan.warnings.empty()) {
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
            // A warning about a plane, a face or a part is shown in the 3D view when clicked.
            if ((w.plane >= 0 || w.part >= 0 || w.face >= 0) && ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    clicked = &w;
            }
        }
        if (scroll)
            ImGui::EndChild();
    }
    if (clicked != nullptr)
        show_warning(*clicked);

    // The content height of the body, for the next frame (the same measure ImGui sizes a window by).
    {
        const ImGuiWindow *bw = ImGui::GetCurrentWindow();
        m_body_content_h      = std::max(0.f, bw->DC.CursorMaxPos.y - bw->DC.CursorStartPos.y) + 2.f;
    }
    ImGui::EndChild();

    ImGui::Separator();

    // ---- actions: pinned under the scrolling body ------------------------------------------------
    const float footer_y0 = ImGui::GetCursorPosY();
    // Both buttons are 1.3 x the usual frame height (font relative, so DPI follows), as wide as their text
    // needs, never under 5.5 em.
    const float btn_h = ImGui::GetFrameHeight() * 1.3f;
    auto        btn_w = [&](const wxString &text) {
        return std::max(em * 5.5f, (ImGui::CalcTextSize(into_u8(text).c_str()).x + style.FramePadding.x * 2.f) * 1.3f);
    };
    if (m_imgui->button(_L("Reset"), ImVec2(btn_w(_L("Reset")), btn_h), true)) {
        reset_options();
        changed = true;
    }
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("Back to the default margins, axes, layout and dowels"), wrap_w);
    ImGui::SameLine();
    // P4: only the plan of the panel's current values may be applied.
    const bool can_apply = m_plan.valid && !m_plan.planes.empty() && single_copy && plan_current();
    // Apply is the primary button: the app's confirm style (the Orca accent fill), the disabled style when
    // there is nothing to apply.
    if (can_apply)
        ImGuiWrapper::push_confirm_button_style();
    else
        ImGuiWrapper::push_button_disable_style();
    const bool apply_now = m_imgui->button(_L("Apply"), ImVec2(btn_w(_L("Apply")), btn_h), can_apply);
    if (can_apply)
        ImGuiWrapper::pop_confirm_button_style();
    else
        ImGuiWrapper::pop_button_disable_style();
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("Cut the object into these parts and arrange them over the plates"), wrap_w);
    m_footer_h = ImGui::GetCursorPosY() - footer_y0 + ImGui::GetStyle().ItemSpacing.y + 1.f;

    GizmoImguiEnd();
    ImGuiWrapper::pop_toolbar_style();

    if (changed) {
        m_plan_dirty = true;
        m_parent.set_as_dirty();
        m_parent.request_extra_frame();
    }
    // P4: keep frames coming while a plan is being made, so "Updating" and the result show at once.
    if (!plan_current())
        m_parent.request_extra_frame();
    if (apply_now && can_apply)
        apply();
}

void GLGizmoSplitToFit::apply()
{
    int          instance = 0;
    ModelObject *mo       = selected_object(&instance);
    if (mo == nullptr || !m_plan.valid || mo->instances.size() != 1 || !plan_current())
        return;
    Plater *plater = wxGetApp().plater();

    // D13: a part that does not fit is allowed, after a question.
    if (const size_t bad = m_plan.pieces_not_fitting(); bad > 0) {
        MessageDialog dlg(plater,
                          format_wxstr(_L("%1% of the %2% parts will not fit the printer. Split anyway? You can turn or cut those parts "
                                          "by hand afterwards."), bad, m_plan.piece_count()),
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
    // P4: what Apply does with the parts is read now (changing it needs no new plan).
    SplitPlan plan        = m_plan;
    plan.lay_flat         = m_options.lay_flat;
    plan.instance_dowels  = m_options.instance_dowels;
    plan.loose_own_plates = m_options.loose_own_plates;
    std::unique_ptr<SplitToFitJob> job = std::make_unique<SplitToFitJob>(plater, *mo, instance, plan);
    // Close the gizmo first, as the Cut does: the object it shows is about to be replaced.
    m_parent.reset_all_gizmos();
    replace_job(worker, std::move(job));
}

} // namespace GUI
} // namespace Slic3r
