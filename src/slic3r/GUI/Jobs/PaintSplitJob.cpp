#include "PaintSplitJob.hpp"

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/format.hpp"

#include "libslic3r/CutUtils.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace Slic3r { namespace GUI {

namespace {
// The mask part is found again in each cut's result by this name, whatever order the parts end up in.
const char *const PAINT_SPLIT_TAG = "__paint_split_mask__";

int find_volume(const ModelObject &o, const std::string &name)
{
    for (size_t i = 0; i < o.volumes.size(); ++i)
        if (o.volumes[i]->is_model_part() && o.volumes[i]->name == name)
            return int(i);
    return -1;
}

std::vector<std::string> &last_log_lines()
{
    static std::vector<std::string> lines;
    return lines;
}

std::string secs(double s)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", s);
    return buf;
}

void set_piece(ModelVolume &v, const std::string &name, int filament)
{
    v.name = name;
    if (filament > 0)
        v.config.set("extruder", filament);
}
} // namespace

PaintSplitJob::PaintSplitJob(Plater *plater, const ModelObject &object, PaintSplitApplyPlan &&plan)
    : m_plater(plater), m_object_id(object.id()), m_name(object.name), m_plan(std::move(plan))
{
    m_source.add_object(object);
}

void PaintSplitJob::process(Ctl &ctl)
{
    if (m_source.objects.empty() || m_plan.pieces.empty())
        return;
    const std::string status = _u8L("Splitting off the selected regions");
    ctl.update_status(0, status);

    ModelObject *source = m_source.objects.front();
    if (m_plan.mask_volume_idx < 0 || size_t(m_plan.mask_volume_idx) >= source->volumes.size()) {
        m_error = PaintSplitError::BadComponent;
        return;
    }
    const std::string mask_name = source->volumes[size_t(m_plan.mask_volume_idx)]->name;
    source->volumes[size_t(m_plan.mask_volume_idx)]->name = PAINT_SPLIT_TAG;

    ModelObjectCutAttributes attrs = ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower |
                                     ModelObjectCutAttribute::KeepPaint;
    if (m_plan.keep_as_parts)
        attrs = attrs | ModelObjectCutAttribute::KeepAsParts;

    // R2 (E7): every region in one cut, from the uncut part. (In turn, each cut read the body the last one left,
    // whose frame moved when a single part remained: ModelObject::delete_volume() folds that part's transformation
    // into the instance, and the region, still in the first cut frame, then missed it: "The cut does not reach the
    // selected region" at region 4 on the owner's figure.)
    PaintSplitKeep keep;
    keep.mask_volume_idx       = m_plan.mask_volume_idx;
    keep.region                = &m_plan.region;
    keep.nearest_skin          = true;
    keep.skin                  = m_plan.skin;
    keep.cut_only_painted_part = m_plan.cut_only_painted_part;
    for (const PaintSplitApplyPiece &p : m_plan.pieces) {
        keep.components.push_back(p.component);
        keep.claims.push_back(p.claim);
    }
    const auto             t_start = std::chrono::steady_clock::now();
    Cut                    cut(source, m_plan.instance, Transform3d::Identity(), attrs);
    PaintSplitReport       rep;
    const ModelObjectPtrs &res = cut.perform_with_paint_split_planes(keep, &rep);
    if (ctl.was_canceled())
        return;
    if (rep.error != PaintSplitError::None || res.empty()) {
        m_error     = rep.error != PaintSplitError::None ? rep.error : PaintSplitError::BooleanFailed;
        m_bad_label = rep.bad_index >= 0 && size_t(rep.bad_index) < m_plan.pieces.size() ? m_plan.pieces[size_t(rep.bad_index)].label : std::string();
        return;
    }
    // Step 0 of the refinement plan: where the time goes, per region.
    for (size_t k = 0; k < m_plan.pieces.size() && k < rep.region_times.size(); ++k) {
        const PaintSplitTimes &t = rep.region_times[k];
        m_log.push_back(GUI::format(_u8L("%1%: field %2% s, cut %3% s"), m_plan.pieces[k].label, secs(t.field + t.mesh + t.smooth), secs(t.cut)));
    }
    m_log.push_back(GUI::format(_u8L("All regions: repair %1% s, field %2% s, cut %3% s, paint %4% s, rest %5% s; %6% of %7% cut(s) from Show cut"),
                                secs(rep.times.repair), secs(rep.times.field + rep.times.mesh + rep.times.smooth), secs(rep.times.cut),
                                secs(rep.times.paint), secs(rep.times.wrapper), rep.claims_reused, m_plan.pieces.size()));
    BOOST_LOG_TRIVIAL(info) << "Paint split: " << m_plan.pieces.size() << " region(s), total " << rep.times.total << " s; repair "
                            << rep.times.repair << " s, field " << rep.times.field << " s, mesh " << rep.times.mesh << " s, smooth "
                            << rep.times.smooth << " s, cut " << rep.times.cut << " s, wrapper " << rep.times.wrapper << " s, paint "
                            << rep.times.paint << " s (" << (rep.exact_paint ? "exact" : "remap") << "); " << rep.claims_reused
                            << " claim(s) from Show cut";
    m_returned += rep.returned_components;
    for (PaintSplitWarning w : rep.warnings)
        if (std::find(m_warnings.begin(), m_warnings.end(), w) == m_warnings.end())
            m_warnings.push_back(w);

    const size_t n = m_plan.pieces.size();
    ModelObjectPtrs pieces;
    ModelObject    *body = nullptr;
    if (m_plan.keep_as_parts) {
        body       = m_source.add_object(*res.front());
        body->name = m_name;
        for (size_t k = 0; k < n; ++k)
            if (int a = find_volume(*body, std::string(PAINT_SPLIT_TAG) + "_A" + std::to_string(k + 1)); a >= 0)
                set_piece(*body->volumes[size_t(a)], m_name + " - " + m_plan.pieces[k].label, m_plan.pieces[k].filament);
        if (int b = find_volume(*body, std::string(PAINT_SPLIT_TAG) + "_B"); b >= 0)
            body->volumes[size_t(b)]->name = mask_name;
        for (int r = 1;; ++r) {
            const int l = find_volume(*body, std::string(PAINT_SPLIT_TAG) + "_loose" + std::to_string(r));
            if (l < 0)
                break;
            body->volumes[size_t(l)]->name = mask_name + " - " + GUI::format(_u8L("Rest %1%"), ++m_rest_count);
        }
    } else {
        if (res.size() != n + 1) {
            m_error = PaintSplitError::EmptyBody;
            return;
        }
        for (size_t k = 0; k < n; ++k) {
            ModelObject *piece = m_source.add_object(*res[k]);
            piece->name        = m_name + " - " + m_plan.pieces[k].label;
            if (int a = find_volume(*piece, PAINT_SPLIT_TAG); a >= 0)
                set_piece(*piece->volumes[size_t(a)], piece->name, m_plan.pieces[k].filament);
            pieces.push_back(piece);
        }
        body       = m_source.add_object(*res[n]);
        body->name = m_name;
        // The rest pieces: objects of their own (the body keeps the other parts and modifiers). Nothing is cut after
        // this, so delete_volume() folding a lone part's transformation into the instance moves nothing.
        for (int r = 1;; ++r) {
            const int l = find_volume(*body, std::string(PAINT_SPLIT_TAG) + "_loose" + std::to_string(r));
            if (l < 0)
                break;
            ModelObject *rest = m_source.add_object(*body);
            for (int v = int(rest->volumes.size()) - 1; v >= 0; --v)
                if (v != l)
                    rest->delete_volume(size_t(v));
            body->delete_volume(size_t(l));
            rest->name = m_name + " - " + GUI::format(_u8L("Rest %1%"), ++m_rest_count);
            if (!rest->volumes.empty())
                rest->volumes.front()->name = rest->name;
            rest->invalidate_bounding_box();
            pieces.push_back(rest);
        }
        body->invalidate_bounding_box();
        for (ModelVolume *v : body->volumes)
            if (v->name == PAINT_SPLIT_TAG)
                v->name = mask_name;
    }
    ctl.update_status(100, status);

    m_results = pieces;
    m_results.push_back(body);
    m_total_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    BOOST_LOG_TRIVIAL(info) << "Paint split: " << n << " region(s) in " << m_total_s << " s";
}

const std::vector<std::string> &PaintSplitJob::last_log() { return last_log_lines(); }

void PaintSplitJob::finalize(bool canceled, std::exception_ptr &eptr)
{
    if (eptr) {
        try {
            std::rethrow_exception(eptr);
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(error) << "Paint split failed: " << e.what();
            show_error(m_plater, format_wxstr(_L("Paint Split could not split \"%1%\": %2%"), m_name, std::string(e.what())));
        }
        eptr = nullptr;
        return;
    }
    NotificationManager *notes = wxGetApp().notification_manager();
    if (canceled) {
        notes->push_notification(_u8L("Paint Split was canceled. The object is unchanged."));
        return;
    }
    if (m_error != PaintSplitError::None || m_results.empty()) {
        // The regions cut before the refusal still say where their time went (step 0).
        std::vector<std::string> &log = last_log_lines();
        log                           = m_log;
        if (m_error != PaintSplitError::None)
            log.insert(log.begin(), GUI::format(_u8L("Paint Split stopped at %1%: %2%"), m_bad_label.empty() ? std::string("?") : m_bad_label,
                                                _u8L(paint_split_error_message(m_error))));
        notes->push_plater_warning_notification(
            format(_u8L("Paint Split did not split \"%1%\": %2%"), m_name,
                   m_error == PaintSplitError::None ? _u8L("nothing to split.") :
            m_bad_loop >= 0 ? GUI::format(_u8L("%1%, edge %2%: %3%"), m_bad_label, m_bad_loop + 1, _u8L(paint_split_error_message(m_error))) :
                              _u8L(paint_split_error_message(m_error))));
        return;
    }

    Model &model   = m_plater->model();
    int    obj_idx = -1;
    for (size_t i = 0; i < model.objects.size(); ++i)
        if (model.objects[i]->id() == m_object_id) {
            obj_idx = int(i);
            break;
        }
    if (obj_idx < 0) {
        notes->push_plater_warning_notification(format(_u8L("\"%1%\" was removed while it was being split. Nothing was changed."), m_name));
        return;
    }

    // New objects: the pieces stand on the bed beside the body instead of inside it.
    if (!m_plan.keep_as_parts && m_results.size() > 1) {
        ModelObject  *body = m_results.back();
        BoundingBoxf3 bb   = body->instance_bounding_box(size_t(m_plan.instance));
        double        x    = bb.max.x() + 10.;
        for (size_t i = 0; i + 1 < m_results.size(); ++i) {
            ModelObject *piece = m_results[i];
            piece->ensure_on_bed();
            const BoundingBoxf3 pb = piece->instance_bounding_box(size_t(m_plan.instance));
            for (ModelInstance *inst : piece->instances) {
                Vec3d off = inst->get_offset();
                off.x() += x - pb.min.x();
                inst->set_offset(off);
            }
            piece->invalidate_bounding_box();
            x += pb.size().x() + 10.;
        }
        body->ensure_on_bed();
    }

    {
        // One undo step: Ctrl+Z brings the uncut object back.
        Plater::TakeSnapshot snapshot(m_plater, _u8L("Paint split"));
        m_plater->apply_cut_object_to_model(size_t(obj_idx), m_results);
    }
    for (PaintSplitWarning w : m_warnings)
        notes->push_notification(_u8L(paint_split_warning_message(w)));
    // The time line (design 8.1 step 0): a notification now, the per-region lines in the gizmo's Output Log.
    const std::string         total = GUI::format(_u8L("Paint Split: %1% region(s) in %2% s."), m_plan.pieces.size(), secs(m_total_s));
    std::vector<std::string> &log   = last_log_lines();
    log                             = m_log;
    log.insert(log.begin(), total);
    notes->push_notification(total);
}

}} // namespace Slic3r::GUI
