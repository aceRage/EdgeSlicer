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
    const std::string status = _u8L("Splitting off the selected region");
    ctl.update_status(0, status);

    ModelObject *current = m_source.objects.front();
    if (m_plan.mask_volume_idx < 0 || size_t(m_plan.mask_volume_idx) >= current->volumes.size()) {
        m_error = PaintSplitError::BadComponent;
        return;
    }
    const std::string mask_name = current->volumes[size_t(m_plan.mask_volume_idx)]->name;
    current->volumes[size_t(m_plan.mask_volume_idx)]->name = PAINT_SPLIT_TAG;
    int mask_idx = m_plan.mask_volume_idx;

    ModelObjectCutAttributes attrs = ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower |
                                     ModelObjectCutAttribute::KeepPaint;
    if (m_plan.keep_as_parts)
        attrs = attrs | ModelObjectCutAttribute::KeepAsParts;

    ModelObjectPtrs pieces;
    const auto      t_start = std::chrono::steady_clock::now();
    for (size_t k = 0; k < m_plan.pieces.size(); ++k) {
        if (ctl.was_canceled())
            return;
        const PaintSplitApplyPiece &p = m_plan.pieces[k];
        PaintSplitKeep keep;
        keep.mask_volume_idx = mask_idx;
        keep.region          = &m_plan.region;
        keep.component       = p.component;
        keep.planes          = p.planes;
        keep.cutters         = p.cutters;
        keep.nearest_skin    = p.nearest_skin;
        keep.skin            = p.skin;
        keep.assign.body_hints = p.body_hints;
        keep.cut_only_painted_part = m_plan.cut_only_painted_part;

        Cut                    cut(current, m_plan.instance, Transform3d::Identity(), attrs);
        PaintSplitReport       rep;
        const ModelObjectPtrs &res = cut.perform_with_paint_split_planes(keep, &rep);
        if (rep.error != PaintSplitError::None || res.empty()) {
            m_error     = rep.error != PaintSplitError::None ? rep.error : PaintSplitError::BooleanFailed;
            m_bad_loop  = rep.bad_loop;
            m_bad_label = p.label;
            return;
        }
        // Step 0 of the refinement plan: where the time goes, per region.
        {
            const PaintSplitTimes &t = rep.times;
            m_log.push_back(GUI::format(_u8L("%1%: field %2% s, cut %3% s, paint %4% s"), p.label, secs(t.field + t.mesh + t.smooth),
                                        secs(t.repair + t.cut + t.wrapper), secs(t.paint)));
            BOOST_LOG_TRIVIAL(info) << "Paint split " << p.label << ": total " << t.total << " s; repair " << t.repair << " s, field "
                                    << t.field << " s, mesh " << t.mesh << " s, smooth " << t.smooth << " s, cut " << t.cut
                                    << " s, wrapper " << t.wrapper << " s, paint " << t.paint << " s ("
                                    << (rep.exact_paint ? "exact" : "remap") << "); claim " << rep.claim.triangles << " triangles, voxel "
                                    << rep.claim.voxel << " mm, near the skin " << rep.claim.near_skin_voxel << " mm; piece "
                                    << rep.piece_volume << " mm3";
        }
        m_returned += rep.returned_components;
        m_attached += rep.attached_components;
        for (PaintSplitWarning w : rep.warnings)
            if (std::find(m_warnings.begin(), m_warnings.end(), w) == m_warnings.end())
                m_warnings.push_back(w);

        if (m_plan.keep_as_parts) {
            ModelObject *obj = m_source.add_object(*res.front());
            obj->name        = m_name;
            // The piece is the tag's "_A", the body its "_B"; the body keeps the tag for the next cut.
            if (int a = find_volume(*obj, std::string(PAINT_SPLIT_TAG) + "_A"); a >= 0)
                set_piece(*obj->volumes[size_t(a)], m_name + " - " + p.label, p.filament);
            mask_idx = find_volume(*obj, std::string(PAINT_SPLIT_TAG) + "_B");
            if (mask_idx >= 0)
                obj->volumes[size_t(mask_idx)]->name = PAINT_SPLIT_TAG;
            // A band's rest pieces: parts of their own beside the body.
            for (int r = 1;; ++r) {
                const int l = find_volume(*obj, std::string(PAINT_SPLIT_TAG) + "_loose" + std::to_string(r));
                if (l < 0)
                    break;
                obj->volumes[size_t(l)]->name = mask_name + " - " + GUI::format(_u8L("Rest %1%"), ++m_rest_count);
            }
            current = obj;
        } else {
            if (res.size() < 2) {
                m_error = PaintSplitError::EmptyBody;
                return;
            }
            ModelObject *piece = m_source.add_object(*res[0]);
            ModelObject *body  = m_source.add_object(*res[1]);
            piece->name        = m_name + " - " + p.label;
            body->name         = m_name;
            if (int a = find_volume(*piece, PAINT_SPLIT_TAG); a >= 0)
                set_piece(*piece->volumes[size_t(a)], piece->name, p.filament);
            pieces.push_back(piece);
            // A band's rest pieces: objects of their own (the body keeps the other parts and modifiers).
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
            mask_idx = find_volume(*body, PAINT_SPLIT_TAG);
            current  = body;
        }
        if (mask_idx < 0 && k + 1 < m_plan.pieces.size()) {
            m_error = PaintSplitError::EmptyBody;
            return;
        }
        ctl.update_status(int(100 * (k + 1) / m_plan.pieces.size()), status);
    }
    // The body keeps the part's own name.
    for (ModelObject *o : pieces)
        for (ModelVolume *v : o->volumes)
            if (v->name == PAINT_SPLIT_TAG)
                v->name = mask_name;
    for (ModelVolume *v : current->volumes)
        if (v->name == PAINT_SPLIT_TAG)
            v->name = mask_name;

    m_results = pieces;
    m_results.push_back(current);
    m_total_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    BOOST_LOG_TRIVIAL(info) << "Paint split: " << m_plan.pieces.size() << " region(s) in " << m_total_s << " s";
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
