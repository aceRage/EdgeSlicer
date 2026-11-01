// Edge: the Airbrush tool of the colour-painting gizmo (Airbrush P2, tests/design_airbrush_paint.md
// section 5; D8-D10). The logic it drives lives in libslic3r/Airbrush/AirbrushSession.hpp.
//
// How a stroke flows:
//   * The press, the drag and a timer while the button is held lay dabs into a WORKING copy of the
//     part's colour field. The model is not touched, so nothing waits on the mouse.
//   * Cells changed since the last bake are drawn over the model as the field itself (the overlay),
//     in chunks so a dab only rebuilds what it touched.
//   * The release starts a background bake: the field becomes ordinary paint on a worker thread,
//     which also builds the triangle selector the gizmo will show.
//   * When the bake is done, the UI thread takes one undo snapshot and writes field and paint into
//     the volume together, so undo/redo always restores a field with the paint baked from it.
//   * An undo or redo while a bake is running drops it (the model it was made for is gone).

#include "GLGizmoMmuSegmentation.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GLShader.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosCommon.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Airbrush/AirbrushSession.hpp"

#include <glad/gl.h>
#include <imgui/imgui.h>
#include <wx/timer.h>
#include <wx/utils.h>

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <future>
#include <optional>
#include <random>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace Slic3r::GUI {

namespace {

// A one-shot timer that calls back on the GUI thread; the gizmo is not a wxEvtHandler.
class AirbrushTimer : public wxTimer
{
public:
    explicit AirbrushTimer(std::function<void()> cb) : m_cb(std::move(cb)) {}
    void Notify() override { m_cb(); }

private:
    std::function<void()> m_cb;
};

using Clock = std::chrono::steady_clock;

double now_s() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }
double ms_since(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

constexpr const char *AIRBRUSH_CONFIG_KEY = "airbrush_brush";

ModelVolume *model_part(ModelObject *mo, int idx)
{
    if (mo == nullptr || idx < 0)
        return nullptr;
    int i = -1;
    for (ModelVolume *mv : mo->volumes)
        if (mv->is_model_part() && ++i == idx)
            return mv;
    return nullptr;
}

// The volume with this id anywhere in the model, and its object.
ModelVolume *find_volume(const ObjectID &id, ModelObject **owner)
{
    for (ModelObject *mo : wxGetApp().model().objects)
        for (ModelVolume *mv : mo->volumes)
            if (mv->id() == id) {
                if (owner)
                    *owner = mo;
                return mv;
            }
    return nullptr;
}

int base_filament_of(const ModelVolume &mv) { return std::max(mv.extruder_id(), 1); }

// Mesh -> world for every model part, in the order of the triangle selectors.
std::vector<Transform3d> part_trafos(GLCanvas3D &canvas, const ModelObject &mo, int instance_idx)
{
    std::vector<Transform3d> out;
    if (instance_idx < 0 || instance_idx >= int(mo.instances.size()))
        return out;
    const ModelInstance *mi       = mo.instances[size_t(instance_idx)];
    const bool           assemble = canvas.get_canvas_type() == GLCanvas3D::CanvasAssembleView;
    const Transform3d    inst     = assemble ? mi->get_assemble_transformation().get_matrix() : mi->get_transformation().get_matrix();
    for (const ModelVolume *mv : mo.volumes)
        if (mv->is_model_part()) {
            Transform3d t = inst * mv->get_matrix();
            if (assemble)
                t.translate(mv->get_transformation().get_offset() * (GLVolume::explosion_ratio - 1.0) +
                            mi->get_offset_to_assembly() * (GLVolume::explosion_ratio - 1.0));
            out.emplace_back(t);
        }
    return out;
}

ColorRGBA mix_rgb(const ColorRGBA &a, const ColorRGBA &b, float t, float alpha)
{
    return ColorRGBA(a.r() + (b.r() - a.r()) * t, a.g() + (b.g() - a.g()) * t, a.b() + (b.b() - a.b()) * t, alpha);
}

std::string hours_minutes(double seconds)
{
    const long minutes = std::lround(seconds / 60.);
    if (minutes < 60)
        return into_u8(wxString::Format(_L("%ld min"), minutes));
    return into_u8(wxString::Format(_L("%ld h %02ld min"), minutes / 60, minutes % 60));
}

} // namespace

struct GLGizmoMmuSegmentation::AirbrushState
{
    struct Chunk
    {
        std::unordered_set<uint64_t>                                cells;
        std::vector<std::pair<ColorRGBA, std::unique_ptr<GLModel>>> models;
        bool                                                        dirty = true;
    };
    struct Part
    {
        ObjectID                                   volume_id;
        std::shared_ptr<const TriangleMesh>        mesh;
        // The working field: the model's field plus strokes still waiting for their bake.
        std::shared_ptr<Airbrush::AirbrushField>   field;
        std::unique_ptr<Airbrush::AirbrushSurface> surface;
        // Cells changed since the last bake -> the stroke that changed them last.
        std::unordered_map<uint64_t, uint32_t>     pending;
        std::unordered_map<uint64_t, Chunk>        chunks;
        uint32_t serial       = 0;   // strokes (and parameter changes) so far
        uint32_t baked_serial = 0;   // ... of which the model's paint has these
        bool     foreign        = false;   // the last bake stopped: paint edited elsewhere
        bool     stale          = false;   // ... the record belongs to another mesh
        bool     overwrite_next = false;   // the user said "re-bake over it"

        bool needs_bake(bool in_stroke) const { return field && !in_stroke && serial > baked_serial && !foreign && !stale; }
    };
    struct BakeOut
    {
        std::shared_ptr<const TriangleMesh>    mesh;   // keeps the selector's mesh alive
        Airbrush::BakeJobResult                result;
        std::unique_ptr<TriangleSelectorPatch> selector;
        double                                 selector_ms = 0.;
    };

    Airbrush::BrushSettings brush;
    bool                    params_dirty = false;
    std::vector<Part>       parts;   // index = model part index (the triangle selector's)
    uint64_t                generation = 1;

    // The stroke being held.
    int                  stroke_mesh   = -1;
    Airbrush::DabEmitter emitter;
    Transform3d          stroke_trafo  = Transform3d::Identity();
    float                stroke_radius = 1.f;   // mesh mm
    size_t               stroke_dabs   = 0;
    double               stroke_dab_ms = 0., stroke_dab_ms_max = 0., overlay_ms_max = 0.;
    Clock::time_point    stroke_start;

    // The bake running in the background (one at a time).
    std::future<BakeOut> bake;
    int                  bake_mesh       = -1;
    uint32_t             bake_serial     = 0;
    uint64_t             bake_generation = 0;
    ObjectID             bake_volume;
    ObjectBase::Timestamp bake_paint_ts  = 0;
    double               bake_copy_ms    = 0.;

    // What the panel shows about the last bake.
    Airbrush::AirbrushBakeStats last_stats;
    bool                        has_stats        = false;
    double                      last_bake_ms     = -1.;
    double                      last_selector_ms = -1.;
    double                      last_apply_ms    = -1.;
    std::string                 last_error;

    // The cost meter (D8).
    std::future<Airbrush::CostJobResult>   cost;
    bool                                   cost_dirty      = true;
    uint64_t                               cost_generation = 0;
    std::optional<Airbrush::CostJobResult> cost_result;
    bool                                   cost_swap_assumed = false;

    std::unique_ptr<wxTimer> timer;

    bool baking() const { return bake.valid(); }
    bool any_needs_bake() const
    {
        for (size_t i = 0; i < parts.size(); ++i)
            if (parts[i].needs_bake(int(i) == stroke_mesh))
                return true;
        return false;
    }
};

// Defined here, where AirbrushState is complete (unique_ptr member).
GLGizmoMmuSegmentation::GLGizmoMmuSegmentation(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoPainterBase(parent, icon_filename, sprite_id), m_current_tool(ImGui::CircleButtonIcon)
{
}

GLGizmoMmuSegmentation::~GLGizmoMmuSegmentation()
{
    if (m_airbrush) {
        if (m_airbrush->timer)
            m_airbrush->timer->Stop();
        // Futures from std::async wait for their thread here; the jobs touch only their own data.
        m_airbrush.reset();
    }
}

GLGizmoMmuSegmentation::AirbrushState &GLGizmoMmuSegmentation::airbrush()
{
    if (!m_airbrush) {
        m_airbrush = std::make_unique<AirbrushState>();
        if (const AppConfig *cfg = wxGetApp().app_config; cfg != nullptr && cfg->has(AIRBRUSH_CONFIG_KEY))
            m_airbrush->brush = Airbrush::BrushSettings::from_string(cfg->get(AIRBRUSH_CONFIG_KEY));
        m_airbrush->brush.sanitize();
    }
    return *m_airbrush;
}

bool GLGizmoMmuSegmentation::airbrush_owns_mouse() const
{
    return m_tool_type == ToolType::AIRBRUSH || (m_airbrush && m_airbrush->stroke_mesh >= 0);
}

float GLGizmoMmuSegmentation::airbrush_cursor_radius() const { return m_airbrush ? m_airbrush->brush.radius_mm : Airbrush::AIRBRUSH_RADIUS_DEFAULT; }

// ---------------------------------------------------------------------------------------------
// Parts
// ---------------------------------------------------------------------------------------------

// The part for model part `idx`, (re)initialised from the volume when the volume or its mesh is new.
static GLGizmoMmuSegmentation::AirbrushState::Part &ensure_part(GLGizmoMmuSegmentation::AirbrushState &ab, int idx, const ModelVolume &mv)
{
    if (int(ab.parts.size()) <= idx)
        ab.parts.resize(size_t(idx) + 1);
    auto &p = ab.parts[size_t(idx)];
    if (!p.field || p.volume_id != mv.id() || p.mesh != mv.mesh_ptr()) {
        p           = GLGizmoMmuSegmentation::AirbrushState::Part();
        p.volume_id = mv.id();
        p.mesh      = mv.mesh_ptr();
        p.field     = mv.airbrush_field ? std::make_shared<Airbrush::AirbrushField>(*mv.airbrush_field) : std::make_shared<Airbrush::AirbrushField>();
    }
    return p;
}

// Lay dabs into the working field and mark what changed for the overlay.
static void apply_dabs(GLGizmoMmuSegmentation::AirbrushState &ab, GLGizmoMmuSegmentation::AirbrushState::Part &p,
                       const std::vector<Airbrush::SurfaceHit> &hits)
{
    if (hits.empty() || !p.field || !p.surface)
        return;
    std::vector<uint64_t> changed;
    for (const Airbrush::SurfaceHit &h : hits) {
        const auto t0 = Clock::now();
        Airbrush::Dab d;
        d.center   = h.pos;
        d.radius   = ab.stroke_radius;
        d.hardness = ab.brush.hardness;
        d.flow     = ab.brush.flow;
        d.normal   = h.facing;
        changed.clear();
        p.field->dab(*p.surface, d, &changed);
        for (uint64_t key : changed) {
            p.pending[key] = p.serial;
            auto &chunk    = p.chunks[Airbrush::overlay_chunk_of(key)];
            chunk.cells.insert(key);
            chunk.dirty = true;
        }
        const double ms = ms_since(t0);
        ab.stroke_dab_ms += ms;
        ab.stroke_dab_ms_max = std::max(ab.stroke_dab_ms_max, ms);
        ++ab.stroke_dabs;
    }
}

// ---------------------------------------------------------------------------------------------
// Mouse
// ---------------------------------------------------------------------------------------------

bool GLGizmoMmuSegmentation::on_airbrush_event(SLAGizmoEventType action, const Vec2d &mouse_position, bool shift_down, bool control_down)
{
    AirbrushState &ab = airbrush();
    switch (action) {
    case SLAGizmoEventType::MouseWheelUp:
    case SLAGizmoEventType::MouseWheelDown: {
        if (!control_down || ab.stroke_mesh >= 0)
            return false;
        const float f      = action == SLAGizmoEventType::MouseWheelUp ? 1.1f : 1.f / 1.1f;
        ab.brush.radius_mm = std::clamp(ab.brush.radius_mm * f, Airbrush::AIRBRUSH_RADIUS_MIN, Airbrush::AIRBRUSH_RADIUS_MAX);
        if (AppConfig *cfg = wxGetApp().app_config)
            cfg->set(AIRBRUSH_CONFIG_KEY, ab.brush.to_string());
        m_parent.set_as_dirty();
        return true;
    }
    case SLAGizmoEventType::LeftDown:
        if (ab.stroke_mesh >= 0)
            return true;
        return airbrush_press(mouse_position, shift_down);
    case SLAGizmoEventType::Dragging:
        if (ab.stroke_mesh < 0)
            return false;
        airbrush_drag(mouse_position);
        m_parent.set_as_dirty();
        return true;
    case SLAGizmoEventType::LeftUp:
    case SLAGizmoEventType::RightUp:
        if (ab.stroke_mesh < 0) {
            if (m_button_down == Button::None)
                return false;
            m_button_down = Button::None;
            return true;
        }
        airbrush_release();
        return true;
    default:
        // Moving, right press: not ours (the canvas rotates, the cursor follows).
        return false;
    }
}

bool GLGizmoMmuSegmentation::airbrush_press(const Vec2d &mouse_position, bool erase)
{
    AirbrushState &ab = airbrush();
    ModelObject   *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (mo == nullptr || m_triangle_selectors.empty())
        return false;
    const Selection               &selection = m_parent.get_selection();
    const std::vector<Transform3d> trafos    = part_trafos(m_parent, *mo, selection.get_instance_idx());
    const Camera                  &camera    = wxGetApp().plater()->get_camera();
    update_raycast_cache(mouse_position, camera, trafos);
    const int idx = m_rr.mesh_id;
    if (idx < 0 || idx >= int(trafos.size()) || idx >= int(m_triangle_selectors.size()))
        return false;   // missed the model: the canvas may rotate the view
    ModelVolume *mv = model_part(mo, idx);
    if (mv == nullptr)
        return false;

    auto &p = ensure_part(ab, idx, *mv);
    if (!p.surface) {
        const auto t0 = Clock::now();
        p.surface     = std::make_unique<Airbrush::AirbrushSurface>(p.mesh->its);
        BOOST_LOG_TRIVIAL(info) << "Airbrush: surface for " << p.mesh->its.indices.size() << " facets built in " << ms_since(t0) << " ms";
    }

    int slot = -1;
    if (!erase) {
        const int filament = int(get_left_button_state_type());
        slot               = p.field->add_slot(filament);
        if (slot < 0) {
            wxGetApp().plater()->get_notification_manager()->push_notification(
                NotificationType::CustomNotification, NotificationManager::NotificationLevel::WarningNotificationLevel,
                GUI::format(_L("The airbrush sprays at most %1% different filaments per part. Erase one of them first, or pick a "
                               "filament this part already uses."),
                            Airbrush::AIRBRUSH_MAX_SLOTS));
            return true;
        }
    }
    if (p.field->bake_params().empty())
        p.field->set_bake_params(Airbrush::bake_params_from(ab.brush, base_filament_of(*mv)).to_string());

    Airbrush::StrokeParams sp;
    sp.slot = slot;
    sp.seed = ab.brush.seed;
    p.field->begin_stroke(sp);
    ++p.serial;

    ab.stroke_mesh       = idx;
    ab.stroke_trafo      = trafos[size_t(idx)];
    const double scale   = std::cbrt(std::abs(ab.stroke_trafo.linear().determinant()));
    ab.stroke_radius     = float(double(ab.brush.radius_mm) / std::max(scale, 1e-6));
    ab.emitter           = Airbrush::DabEmitter();
    ab.stroke_dabs       = 0;
    ab.stroke_dab_ms     = 0.;
    ab.stroke_dab_ms_max = 0.;
    ab.overlay_ms_max    = 0.;
    ab.stroke_start      = Clock::now();

    Airbrush::SurfaceHit hit;
    hit.pos               = m_rr.hit;
    const Vec3f eye       = (ab.stroke_trafo.inverse() * camera.get_position()).cast<float>();
    const Vec3f to_eye    = eye - m_rr.hit;
    hit.facing            = to_eye.squaredNorm() > 0.f ? Vec3f(to_eye.normalized()) : Vec3f::Zero();
    apply_dabs(ab, p, ab.emitter.press(hit, now_s(), ab.stroke_radius));

    m_button_down      = Button::Left;
    m_last_mouse_click = Vec2d::Zero();
    m_parent.set_as_dirty();
    m_parent.request_extra_frame();
    airbrush_schedule(16);
    return true;
}

void GLGizmoMmuSegmentation::airbrush_drag(const Vec2d &mouse_position)
{
    AirbrushState &ab = airbrush();
    ModelObject   *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (mo == nullptr || ab.stroke_mesh < 0 || ab.stroke_mesh >= int(ab.parts.size()))
        return;
    const std::vector<Transform3d> trafos = part_trafos(m_parent, *mo, m_parent.get_selection().get_instance_idx());
    const Camera                  &camera = wxGetApp().plater()->get_camera();
    update_raycast_cache(mouse_position, camera, trafos);
    if (m_rr.mesh_id != ab.stroke_mesh)
        return;   // off the part being sprayed: the stroke pauses there
    auto &p = ab.parts[size_t(ab.stroke_mesh)];
    Airbrush::SurfaceHit hit;
    hit.pos            = m_rr.hit;
    const Vec3f eye    = (ab.stroke_trafo.inverse() * camera.get_position()).cast<float>();
    const Vec3f to_eye = eye - m_rr.hit;
    hit.facing         = to_eye.squaredNorm() > 0.f ? Vec3f(to_eye.normalized()) : Vec3f::Zero();
    apply_dabs(ab, p, ab.emitter.move(hit, now_s(), ab.stroke_radius));
    m_parent.request_extra_frame();
}

void GLGizmoMmuSegmentation::airbrush_release()
{
    AirbrushState &ab = airbrush();
    if (ab.stroke_mesh >= 0 && ab.stroke_mesh < int(ab.parts.size())) {
        auto &p = ab.parts[size_t(ab.stroke_mesh)];
        if (p.field)
            p.field->end_stroke();
        BOOST_LOG_TRIVIAL(info) << "Airbrush stroke: " << ab.stroke_dabs << " dabs in " << ms_since(ab.stroke_start) << " ms, dab avg "
                                << (ab.stroke_dabs ? ab.stroke_dab_ms / double(ab.stroke_dabs) : 0.) << " ms max " << ab.stroke_dab_ms_max
                                << " ms, overlay frame max " << ab.overlay_ms_max << " ms, field " << (p.field ? p.field->size() : 0)
                                << " cells, pending " << p.pending.size();
    }
    ab.emitter.release();
    ab.stroke_mesh     = -1;
    m_button_down      = Button::None;
    m_last_mouse_click = Vec2d::Zero();
    airbrush_start_bake();
    airbrush_schedule(30);
    m_parent.set_as_dirty();
}

// ---------------------------------------------------------------------------------------------
// Timer, bake and cost jobs
// ---------------------------------------------------------------------------------------------

void GLGizmoMmuSegmentation::airbrush_schedule(int interval_ms)
{
    AirbrushState &ab = airbrush();
    if (!ab.timer)
        ab.timer = std::make_unique<AirbrushTimer>([this]() { this->airbrush_tick(); });
    ab.timer->StartOnce(std::max(1, interval_ms));
}

void GLGizmoMmuSegmentation::airbrush_tick()
{
    if (!m_airbrush)
        return;
    AirbrushState &ab   = *m_airbrush;
    int            next = -1;
    if (ab.stroke_mesh >= 0 && ab.stroke_mesh < int(ab.parts.size())) {
        if (m_state != On) {
            airbrush_release();
        } else {
            // Holding still builds up: dwell dabs at the cursor.
            const auto t0 = Clock::now();
            apply_dabs(ab, ab.parts[size_t(ab.stroke_mesh)], ab.emitter.hold(now_s()));
            m_parent.set_as_dirty();
            m_parent.request_extra_frame();
            next = int(std::clamp(1.5 * ms_since(t0), 16., 100.));
        }
    }
    airbrush_poll_bake(false);
    airbrush_poll_cost(false);
    if (!ab.baking())
        airbrush_start_bake();
    if (m_state == On && m_tool_type == ToolType::AIRBRUSH && ab.cost_dirty && !ab.cost.valid() && !ab.baking() && !ab.any_needs_bake())
        airbrush_start_cost();
    if (next < 0 && (ab.baking() || ab.cost.valid() || ab.any_needs_bake()))
        next = 50;
    if (next > 0)
        airbrush_schedule(next);
}

void GLGizmoMmuSegmentation::airbrush_start_bake()
{
    AirbrushState &ab = airbrush();
    if (ab.baking())
        return;
    ModelObject *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (mo == nullptr)
        return;
    for (int idx = 0; idx < int(ab.parts.size()); ++idx) {
        auto &p = ab.parts[size_t(idx)];
        if (!p.needs_bake(idx == ab.stroke_mesh))
            continue;
        ModelVolume *mv = model_part(mo, idx);
        if (mv == nullptr || mv->id() != p.volume_id || mv->mesh_ptr() != p.mesh) {
            p = AirbrushState::Part();
            continue;
        }
        Airbrush::BakeJob job;
        const auto        t0 = Clock::now();
        job.field            = std::make_shared<const Airbrush::AirbrushField>(*p.field);
        ab.bake_copy_ms      = ms_since(t0);
        job.mesh             = p.mesh;
        job.current          = mv->mmu_segmentation_facets.get_data();
        Airbrush::AirbrushBakeParams params;
        if (!Airbrush::AirbrushBakeParams::from_string(p.field->bake_params(), params))
            params = Airbrush::bake_params_from(ab.brush, base_filament_of(*mv));
        params.base_filament = base_filament_of(*mv);
        job.params           = params;
        const size_t num_physical = size_t(std::max(wxGetApp().filaments_cnt(), 0));
        if (params.mix_steps >= 2 && wxGetApp().preset_bundle != nullptr)
            job.ladder = Airbrush::ladder_from_mixed(wxGetApp().preset_bundle->mixed_filaments, num_physical);
        job.overwrite    = p.overwrite_next;
        p.overwrite_next = false;

        // The selector the gizmo will show is built on the worker too.
        int extruder_idx = std::clamp(mv->extruder_id() - 1, 0, std::max(int(m_extruders_colors.size()) - 1, 0));
        std::vector<ColorRGBA> ebt_colors;
        if (!m_extruders_colors.empty())
            ebt_colors.push_back(m_extruders_colors[size_t(extruder_idx)]);
        ebt_colors.insert(ebt_colors.end(), m_extruders_colors.begin(), m_extruders_colors.end());
        const EnforcerBlockerType max_ebt = EnforcerBlockerType(std::min(m_extruders_colors.size(), size_t(EnforcerBlockerType::ExtruderMax)));

        ab.bake_mesh       = idx;
        ab.bake_serial     = p.serial;
        ab.bake_generation = ab.generation;
        ab.bake_volume     = mv->id();
        ab.bake_paint_ts   = mv->mmu_segmentation_facets.timestamp();
        try {
            ab.bake = std::async(std::launch::async, [job = std::move(job), ebt_colors = std::move(ebt_colors), max_ebt]() {
                AirbrushState::BakeOut out;
                out.mesh   = job.mesh;
                out.result = Airbrush::run_bake_job(job);
                if (out.result.status == Airbrush::BakeJobResult::Status::Ok) {
                    const auto t1 = Clock::now();
                    out.selector  = std::make_unique<TriangleSelectorPatch>(*job.mesh, ebt_colors, 0.2f);
                    out.selector->deserialize(out.result.painting, false, max_ebt);
                    out.selector->set_wireframe_needed(true);
                    out.selector_ms = ms_since(t1);
                }
                return out;
            });
        } catch (const std::exception &e) {
            ab.last_error = e.what();
            BOOST_LOG_TRIVIAL(error) << "Airbrush: could not start the bake: " << e.what();
        }
        return;
    }
}

void GLGizmoMmuSegmentation::airbrush_poll_bake(bool wait)
{
    if (!m_airbrush || !m_airbrush->bake.valid())
        return;
    AirbrushState &ab = *m_airbrush;
    if (!wait) {
        if (ab.bake.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        // Another tool's stroke is open on a selector: apply when it is released.
        if (m_button_down != Button::None && ab.stroke_mesh < 0)
            return;
    }
    AirbrushState::BakeOut out = ab.bake.get();
    m_parent.set_as_dirty();
    if (ab.bake_generation != ab.generation)
        return;   // the model was reloaded (undo/redo) since the job started
    ModelObject *mo = nullptr;
    ModelVolume *mv = find_volume(ab.bake_volume, &mo);
    const int    idx = ab.bake_mesh;
    if (mv == nullptr || mo == nullptr || mv->mesh_ptr() != out.mesh || idx < 0 || idx >= int(ab.parts.size()) ||
        ab.parts[size_t(idx)].volume_id != mv->id())
        return;
    auto &p = ab.parts[size_t(idx)];
    if (mv->mmu_segmentation_facets.timestamp() != ab.bake_paint_ts)
        return;   // the paint changed while baking: the part still needs a bake and gets a new one

    switch (out.result.status) {
    case Airbrush::BakeJobResult::Status::Ok: {
        const auto t0 = Clock::now();
        {
            // One undo step: the field and the paint baked from it, together.
            Plater::TakeSnapshot snapshot(wxGetApp().plater(), into_u8(_L("Airbrush")), UndoRedo::SnapshotType::GizmoAction);
            Airbrush::commit_bake(*mv, out.result);
        }
        ModelObject *sel_mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
        if (sel_mo == mo && idx < int(m_triangle_selectors.size())) {
            if (out.selector) {
                out.selector->set_filter_state(m_current_tool == ImGui::GapFillIcon);
                m_triangle_selectors[size_t(idx)] = std::move(out.selector);
            } else {
                const EnforcerBlockerType max_ebt = EnforcerBlockerType(std::min(m_extruders_colors.size(), size_t(EnforcerBlockerType::ExtruderMax)));
                m_triangle_selectors[size_t(idx)]->deserialize(mv->mmu_segmentation_facets.get_data(), true, max_ebt);
            }
            m_triangle_selectors[size_t(idx)]->request_update_render_data(true);
        }
        p.field->record() = out.result.field->record();
        p.baked_serial    = ab.bake_serial;
        for (auto it = p.pending.begin(); it != p.pending.end();) {
            if (it->second <= ab.bake_serial) {
                auto c = p.chunks.find(Airbrush::overlay_chunk_of(it->first));
                if (c != p.chunks.end()) {
                    c->second.cells.erase(it->first);
                    c->second.dirty = true;
                    if (c->second.cells.empty())
                        p.chunks.erase(c);
                }
                it = p.pending.erase(it);
            } else
                ++it;
        }
        p.foreign = p.stale = false;
        ab.last_error.clear();
        ab.last_stats       = out.result.stats;
        ab.has_stats        = true;
        ab.last_bake_ms     = out.result.bake_ms;
        ab.last_selector_ms = out.selector_ms;
        ab.last_apply_ms    = ms_since(t0);
        ab.cost_dirty       = true;
        BOOST_LOG_TRIVIAL(info) << "Airbrush bake: " << mv->mesh().its.indices.size() << " facets, " << out.result.stats.leaves_evaluated
                                << " leaves evaluated, " << out.result.stats.leaves_airbrushed << " airbrushed; field copy " << ab.bake_copy_ms
                                << " ms, bake " << out.result.bake_ms << " ms, selector " << out.selector_ms << " ms (worker), apply "
                                << ab.last_apply_ms << " ms (UI)";
        notify_paint_changed(mo);
        break;
    }
    case Airbrush::BakeJobResult::Status::ForeignEdit: p.foreign = true; break;
    case Airbrush::BakeJobResult::Status::StaleRecord: p.stale = true; break;
    case Airbrush::BakeJobResult::Status::Failed:
    default:
        // Do not retry the same strokes forever; the next stroke tries again.
        p.baked_serial = ab.bake_serial;
        ab.last_error  = out.result.error;
        BOOST_LOG_TRIVIAL(error) << "Airbrush bake failed: " << out.result.error;
        break;
    }
}

void GLGizmoMmuSegmentation::airbrush_start_cost()
{
    AirbrushState &ab = airbrush();
    if (ab.cost.valid())
        return;
    ModelObject *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (mo == nullptr || wxGetApp().preset_bundle == nullptr)
        return;
    const int instance_idx = std::max(m_parent.get_selection().get_instance_idx(), 0);
    if (instance_idx >= int(mo->instances.size()))
        return;

    const DynamicPrintConfig cfg          = wxGetApp().preset_bundle->full_config();
    const size_t             num_physical = size_t(std::max(wxGetApp().filaments_cnt(), 0));
    Airbrush::CostJob        job;
    Airbrush::AirbrushCostParams &cp = job.params;
    auto get_float = [&](const char *key, double fallback) {
        const ConfigOption *o = mo->config.option(key);
        if (o == nullptr)
            o = cfg.option(key);
        if (o == nullptr)
            return fallback;
        if (const auto *f = dynamic_cast<const ConfigOptionFloat *>(o))
            return f->value;
        if (const auto *fs = dynamic_cast<const ConfigOptionFloats *>(o))
            return fs->values.empty() ? fallback : fs->values.front();
        if (const auto *fl = dynamic_cast<const ConfigOptionFloatOrPercent *>(o))
            return fl->value;
        return fallback;
    };
    cp.layer_height       = get_float("layer_height", 0.2);
    cp.first_layer_height = get_float("initial_layer_print_height", cp.layer_height);
    cp.flush_multiplier   = get_float("flush_multiplier", 1.);

    // The filaments involved: every part's own and every sprayed slot, as physical ids.
    const MixedFilamentManager &mixed = wxGetApp().preset_bundle->mixed_filaments;
    std::set<int>               involved;
    auto add_physical = [&](int id) {
        if (id >= 1 && size_t(id) <= num_physical)
            involved.insert(id);
        else if (const MixedFilament *mf = mixed.mixed_filament_from_id(unsigned(id), num_physical)) {
            involved.insert(int(mf->component_a));
            involved.insert(int(mf->component_b));
        }
    };
    int part_idx = -1;
    for (const ModelVolume *mv : mo->volumes) {
        if (!mv->is_model_part())
            continue;
        ++part_idx;
        Airbrush::CostJob::Part part;
        part.mesh          = mv->mesh_ptr();
        part.paint         = mv->mmu_segmentation_facets.get_data();
        part.trafo         = mo->instances[size_t(instance_idx)]->get_transformation().get_matrix() * mv->get_matrix();
        part.base_filament = base_filament_of(*mv);
        add_physical(part.base_filament);
        if (mv->airbrush_field)
            for (int s : mv->airbrush_field->slots())
                add_physical(s);
        job.parts.push_back(std::move(part));
    }
    // Purge: the mean flush between the filaments involved (first nozzle's matrix).
    cp.flush_mm3 = 280.;
    if (const auto *m = cfg.option<ConfigOptionFloats>("flush_volumes_matrix"); m != nullptr && num_physical > 0 &&
                                                                             m->values.size() >= num_physical * num_physical) {
        double sum = 0.;
        int    n   = 0;
        for (int a : involved)
            for (int b : involved)
                if (a != b) {
                    sum += m->values[size_t(a - 1) * num_physical + size_t(b - 1)];
                    ++n;
                }
        if (n > 0)
            cp.flush_mm3 = sum / double(n);
    }
    cp.density_g_cm3 = 1.24;
    if (const auto *d = cfg.option<ConfigOptionFloats>("filament_density"); d != nullptr && !involved.empty()) {
        double sum = 0.;
        int    n   = 0;
        for (int a : involved)
            if (size_t(a - 1) < d->values.size() && d->values[size_t(a - 1)] > 0.) {
                sum += d->values[size_t(a - 1)];
                ++n;
            }
        if (n > 0)
            cp.density_g_cm3 = sum / double(n);
    }
    const double swap        = get_float("machine_load_filament_time", 0.) + get_float("machine_unload_filament_time", 0.);
    ab.cost_swap_assumed     = !(swap > 0.);
    cp.swap_seconds          = swap > 0. ? swap : 60.;
    auto mixed_copy          = std::make_shared<const MixedFilamentManager>(mixed);
    cp.resolve = [mixed_copy, num_physical](int state, int layer) {
        return int(mixed_copy->resolve(unsigned(std::max(state, 0)), num_physical, std::max(layer, 0)));
    };

    ab.cost_dirty      = false;
    ab.cost_generation = ab.generation;
    try {
        ab.cost = std::async(std::launch::async, [job = std::move(job)]() { return Airbrush::run_cost_job(job); });
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "Airbrush: could not start the cost estimate: " << e.what();
    }
}

void GLGizmoMmuSegmentation::airbrush_poll_cost(bool wait)
{
    if (!m_airbrush || !m_airbrush->cost.valid())
        return;
    AirbrushState &ab = *m_airbrush;
    if (!wait && ab.cost.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    Airbrush::CostJobResult r = ab.cost.get();
    if (ab.cost_generation == ab.generation) {
        BOOST_LOG_TRIVIAL(info) << "Airbrush cost: " << r.cost.swaps << " swaps, " << r.cost.purge_g << " g, " << r.cost.swap_seconds
                                << " s, estimated in " << r.ms << " ms";
        ab.cost_result = std::move(r);
    }
    m_parent.set_as_dirty();
}

// ---------------------------------------------------------------------------------------------
// Model sync
// ---------------------------------------------------------------------------------------------

void GLGizmoMmuSegmentation::airbrush_reset()
{
    if (!m_airbrush)
        return;
    AirbrushState &ab = *m_airbrush;
    ++ab.generation;
    if (ab.stroke_mesh >= 0) {
        if (ab.stroke_mesh < int(ab.parts.size()) && ab.parts[size_t(ab.stroke_mesh)].field)
            ab.parts[size_t(ab.stroke_mesh)].field->end_stroke();
        ab.emitter.release();
        ab.stroke_mesh = -1;
        m_button_down  = Button::None;
    }
    ab.parts.clear();
    ab.cost_dirty = true;
    ab.cost_result.reset();
    ab.has_stats = false;
}

void GLGizmoMmuSegmentation::airbrush_sync_from_model()
{
    ModelObject *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (mo == nullptr)
        return;
    AirbrushState &ab = airbrush();
    // The panel shows the grain, mix steps and seed the object was baked with.
    for (const ModelVolume *mv : mo->volumes) {
        if (!mv->is_model_part() || !mv->airbrush_field)
            continue;
        Airbrush::AirbrushBakeParams bp;
        if (Airbrush::AirbrushBakeParams::from_string(mv->airbrush_field->bake_params(), bp)) {
            ab.brush.grain_mm  = bp.grain_mm;
            ab.brush.mix_steps = bp.mix_steps >= 2;
            ab.brush.seed      = bp.seed;
            ab.brush.sanitize();
            break;
        }
    }
    ab.cost_dirty = true;
}

void GLGizmoMmuSegmentation::airbrush_apply_params_to_object()
{
    ModelObject *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    AirbrushState &ab = airbrush();
    if (AppConfig *cfg = wxGetApp().app_config)
        cfg->set(AIRBRUSH_CONFIG_KEY, ab.brush.to_string());
    if (mo == nullptr)
        return;
    int idx = -1;
    for (ModelVolume *mv : mo->volumes) {
        if (!mv->is_model_part())
            continue;
        ++idx;
        const bool working = idx < int(ab.parts.size()) && ab.parts[size_t(idx)].field && !ab.parts[size_t(idx)].field->empty();
        if (!mv->airbrush_field && !working)
            continue;
        auto &p = ensure_part(ab, idx, *mv);
        p.field->set_bake_params(Airbrush::bake_params_from(ab.brush, base_filament_of(*mv)).to_string());
        ++p.serial;
    }
    airbrush_schedule(1);
}

void GLGizmoMmuSegmentation::airbrush_clear_fields(ModelObject *mo)
{
    if (mo != nullptr)
        for (ModelVolume *mv : mo->volumes)
            if (mv->is_model_part())
                mv->airbrush_field.reset();
    airbrush_reset();
}

void GLGizmoMmuSegmentation::airbrush_finish_pending()
{
    if (!m_airbrush)
        return;
    AirbrushState &ab = *m_airbrush;
    if (ab.stroke_mesh >= 0)
        airbrush_release();
    if (ab.baking() || ab.any_needs_bake()) {
        wxBusyCursor wait;
        // One bake per part at most; each pass waits for the running one and starts the next.
        for (size_t guard = 0; guard <= 2 * ab.parts.size() + 1; ++guard) {
            airbrush_poll_bake(true);
            airbrush_start_bake();
            if (!ab.baking())
                break;
        }
    }
    if (ab.timer)
        ab.timer->Stop();
    airbrush_reset();
}

// ---------------------------------------------------------------------------------------------
// Overlay
// ---------------------------------------------------------------------------------------------

void GLGizmoMmuSegmentation::airbrush_render_overlay()
{
    if (!m_airbrush)
        return;
    AirbrushState &ab = *m_airbrush;
    ModelObject   *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (mo == nullptr)
        return;
    bool any = false;
    for (const auto &p : ab.parts)
        any |= !p.pending.empty();
    if (!any)
        return;
    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;
    const auto                     t0     = Clock::now();
    const std::vector<Transform3d> trafos = part_trafos(m_parent, *mo, m_parent.get_selection().get_instance_idx());
    const Camera                  &camera = wxGetApp().plater()->get_camera();
    auto color_of = [this](int filament) {
        if (filament >= 1 && size_t(filament) <= m_extruders_colors.size())
            return m_extruders_colors[size_t(filament - 1)];
        return ColorRGBA(0.5f, 0.5f, 0.5f, 1.f);
    };

    shader->start_using();
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    for (size_t idx = 0; idx < ab.parts.size() && idx < trafos.size(); ++idx) {
        auto &p = ab.parts[idx];
        if (p.pending.empty() || !p.field)
            continue;
        ModelVolume *mv = model_part(mo, int(idx));
        if (mv == nullptr || mv->id() != p.volume_id)
            continue;
        const ColorRGBA base = color_of(base_filament_of(*mv));
        shader->set_uniform("view_model_matrix", camera.get_view_matrix() * trafos[idx]);
        for (auto &[key, chunk] : p.chunks) {
            if (chunk.dirty) {
                chunk.models.clear();
                const std::vector<uint64_t> cells(chunk.cells.begin(), chunk.cells.end());
                for (Airbrush::OverlayBucket &b : Airbrush::build_overlay_chunk(*p.field, cells)) {
                    GLModel::Geometry g;
                    g.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
                    g.reserve_vertices(b.corners.size());
                    g.reserve_indices(b.corners.size() / 4 * 6);
                    for (size_t q = 0; q + 3 < b.corners.size(); q += 4) {
                        const unsigned int v = unsigned(g.vertices_count());
                        for (size_t k = 0; k < 4; ++k)
                            g.add_vertex(b.corners[q + k]);
                        g.add_triangle(v, v + 1, v + 2);
                        g.add_triangle(v, v + 2, v + 3);
                    }
                    ColorRGBA c;
                    if (b.slot < 0 || size_t(b.slot) >= p.field->slots().size())
                        c = mix_rgb(base, base, 0.f, 0.75f);
                    else
                        c = mix_rgb(base, color_of(p.field->slots()[size_t(b.slot)]),
                                    float(b.level) / float(Airbrush::AIRBRUSH_OVERLAY_LEVELS), 0.85f);
                    auto model = std::make_unique<GLModel>();
                    model->init_from(std::move(g));
                    chunk.models.emplace_back(c, std::move(model));
                }
                chunk.dirty = false;
            }
            for (auto &[c, model] : chunk.models) {
                model->set_color(c);
                model->render();
            }
        }
    }
    shader->stop_using();
    ab.overlay_ms_max = std::max(ab.overlay_ms_max, ms_since(t0));
}

// ---------------------------------------------------------------------------------------------
// Panel
// ---------------------------------------------------------------------------------------------

void GLGizmoMmuSegmentation::airbrush_render_panel(float left, float sliders_width, float drag_left_width, float max_tooltip_width, float window_width)
{
    AirbrushState           &ab = airbrush();
    Airbrush::BrushSettings &b  = ab.brush;
    bool settings_changed       = false;
    bool bake_changed           = false;
    const float icon_w          = m_imgui->get_slider_icon_size().x;
    const float wrap            = std::max(window_width, m_imgui->scaled(12.f));
    const ImVec4 hint_color     = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    auto hint = [&](const wxString &text) {
        ImGui::PushStyleColor(ImGuiCol_Text, hint_color);
        m_imgui->text_wrapped(text, wrap);
        ImGui::PopStyleColor();
    };
    auto warn = [&](const wxString &text) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGuiWrapper::to_ImVec4(ColorRGB::WARNING()));
        m_imgui->text_wrapped(text, wrap);
        ImGui::PopStyleColor();
    };
    auto slider_row = [&](const wxString &label, const char *id, float *v, float lo, float hi, const std::string &fmt, float speed) {
        ImGui::AlignTextToFramePadding();
        m_imgui->text(label);
        ImGui::SameLine(left);
        ImGui::PushItemWidth(sliders_width);
        bool c = m_imgui->bbl_slider_float_style(id, v, lo, hi, fmt.c_str(), 1.0f, true);
        ImGui::SameLine(drag_left_width + left);
        ImGui::PushItemWidth(1.5f * icon_w);
        c |= ImGui::BBLDragFloat((std::string(id) + "_input").c_str(), v, speed, 0.f, 0.f, fmt.c_str());
        *v = std::clamp(*v, lo, hi);
        return c;
    };
    const std::string mm = into_u8(_L("mm"));

    settings_changed |= slider_row(m_desc.at("airbrush_radius"), "##airbrush_radius", &b.radius_mm, Airbrush::AIRBRUSH_RADIUS_MIN,
                                   Airbrush::AIRBRUSH_RADIUS_MAX, "%.1f " + mm, 0.05f);
    settings_changed |= slider_row(m_desc.at("airbrush_hardness"), "##airbrush_hardness", &b.hardness, 0.f, 1.f, "%.2f", 0.01f);
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("How sharp the edge of the spray is: 0 fades out from the centre, 1 is an even disc."), max_tooltip_width);
    float flow_pct = b.flow * 100.f;
    if (slider_row(m_desc.at("airbrush_flow"), "##airbrush_flow", &flow_pct, Airbrush::AIRBRUSH_FLOW_MIN * 100.f, 100.f, "%.0f%%", 0.5f)) {
        b.flow           = flow_pct / 100.f;
        settings_changed = true;
    }
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("How much colour one pass lays down. Holding still or going over an area again builds it up."), max_tooltip_width);

    ImGui::Separator();
    if (slider_row(m_desc.at("airbrush_grain"), "##airbrush_grain", &b.grain_mm, Airbrush::AIRBRUSH_MIN_GRAIN_WALL_MM, Airbrush::AIRBRUSH_GRAIN_MAX,
                   "%.2f " + mm, 0.01f))
        bake_changed = true;
    hint(GUI::format_wxstr(_L("Speck size. Walls and slopes print specks of %1% mm or more; tops and bottoms %2% mm or more, "
                              "and a finer grain there turns into a flat tone."),
                           Airbrush::AIRBRUSH_MIN_GRAIN_WALL_MM, Airbrush::AIRBRUSH_MIN_GRAIN_TOP_MM));
    if (m_imgui->bbl_checkbox(_L("Mix steps on walls"), b.mix_steps))
        bake_changed = true;
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("Smooth fades on walls and slopes print as layer-alternating mixed filaments (25/50/75%) when the "
                            "project has matching ones. Tops and bottoms always get specks only."),
                         max_tooltip_width);

    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc.at("airbrush_seed"));
    ImGui::SameLine(left);
    ImGui::PushItemWidth(m_imgui->scaled(4.5f));
    int seed = int(std::min<uint32_t>(b.seed, 999999u));
    if (ImGui::InputInt("##airbrush_seed", &seed, 1, 10)) {
        b.seed       = uint32_t(std::clamp(seed, 0, 999999));
        bake_changed = true;
    }
    ImGui::SameLine();
    if (m_imgui->button(_L("Reroll"))) {
        std::random_device rd;
        b.seed       = 1u + rd() % 999999u;
        bake_changed = true;
    }
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("A new random pattern of specks for the same spray."), max_tooltip_width);

    hint(GUI::format_wxstr(_L("Above about %1%%% coverage the specks merge and read as a tone rather than separate specks."),
                           int(std::lround(Airbrush::AIRBRUSH_TONE_COVERAGE * 100.f))));

    // Status of the bake.
    if (ab.baking())
        m_imgui->text(_L("Baking the spray into paint..."));
    else if (ab.stroke_mesh < 0 && ab.any_needs_bake())
        m_imgui->text(_L("Waiting to bake..."));
    else if (ab.has_stats) {
        if (ab.last_stats.grain_cells > 0) {
            const int tone = int(std::lround(100. * double(ab.last_stats.grain_cells_tone) / double(ab.last_stats.grain_cells)));
            if (tone > 0)
                hint(GUI::format_wxstr(_L("%1%%% of the sprayed area reads as tone."), tone));
        }
        if (ab.last_bake_ms >= 0.)
            hint(GUI::format_wxstr(_L("Last bake: %1% s"), wxString::Format("%.2f", (ab.last_bake_ms + std::max(ab.last_selector_ms, 0.)) / 1000.)));
    }
    if (!ab.last_error.empty())
        warn(GUI::format_wxstr(_L("The bake failed: %1%"), ab.last_error));

    // Paint edited elsewhere since the last bake (design 3.7): ask before replacing it.
    for (size_t idx = 0; idx < ab.parts.size(); ++idx) {
        auto &p = ab.parts[idx];
        if (!p.foreign && !p.stale)
            continue;
        ImGui::PushID(int(idx));
        warn(p.stale ? _L("This part's mesh changed since its last airbrush bake.") :
                                        _L("This part's paint was changed by another tool since its last airbrush bake."));
        hint(_L("Re-baking puts the airbrushed areas back to the spray and replaces those edits there. Discarding drops the "
                "spray that has not been baked yet."));
        if (m_imgui->button(_L("Re-bake"))) {
            p.foreign = p.stale = false;
            p.overwrite_next    = true;
            airbrush_schedule(1);
        }
        ImGui::SameLine();
        if (m_imgui->button(_L("Discard spray"))) {
            p = AirbrushState::Part();
            m_parent.set_as_dirty();
        }
        ImGui::PopID();
    }

    // The cost meter (D8).
    ImGui::Separator();
    m_imgui->text(_L("Filament changes for this object"));
    if (ab.cost_result) {
        const Airbrush::AirbrushCost &c = ab.cost_result->cost;
        m_imgui->text(GUI::format_wxstr(_L("%1% changes, about %2% g purge"), c.swaps, wxString::Format("%.1f", c.purge_g)));
        m_imgui->text(GUI::format_wxstr(_L("About %1% of swapping"), from_u8(hours_minutes(c.swap_seconds))));
        if (ab.cost_result->warning.any())
            warn(_L("That is a lot of purge or swap time. Fewer colours, flushing into infill or support, or a "
                                     "toolchanger reduce it."));
        if (ab.cost_swap_assumed)
            hint(_L("The printer profile has no load/unload time; 60 s per change is assumed."));
    }
    if (ab.cost.valid())
        hint(_L("Estimating..."));
    else if (ab.cost_dirty && (!ab.timer || !ab.timer->IsRunning()))
        airbrush_schedule(1);

    if (settings_changed || bake_changed) {
        b.sanitize();
        if (AppConfig *cfg = wxGetApp().app_config)
            cfg->set(AIRBRUSH_CONFIG_KEY, b.to_string());
    }
    if (bake_changed)
        ab.params_dirty = true;
    // Re-bake once the slider is let go, not on every frame of a drag.
    if (ab.params_dirty && !ImGui::IsAnyItemActive()) {
        ab.params_dirty = false;
        airbrush_apply_params_to_object();
    }
}

} // namespace Slic3r::GUI
