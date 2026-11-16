#include "SplitToFitJob.hpp"

#include "ArrangeJob.hpp"
#include "Worker.hpp"

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoSplitToFit.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/format.hpp"

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI {

SplitToFitJob::SplitToFitJob(Plater *plater, const ModelObject &object, int instance, const SplitPlan &plan)
    : m_plater(plater), m_object_id(object.id()), m_name(object.name), m_instance(instance), m_plan(plan)
{
    m_source.add_object(object);
}

void SplitToFitJob::process(Ctl &ctl)
{
    if (m_source.objects.empty())
        return;
    const std::string status = _u8L("Splitting the object to fit the printer");
    ctl.update_status(0, status);
    m_result = split_apply(
        *m_source.objects.front(), m_instance, m_plan, [&ctl]() { return ctl.was_canceled(); },
        [&ctl, &status](int done, int all) { ctl.update_status(all > 0 ? std::min(99, 100 * done / all) : 99, status); });
    if (!m_result.canceled)
        ctl.update_status(100, _u8L("Split to Fit finished."));
}

void SplitToFitJob::finalize(bool canceled, std::exception_ptr &eptr)
{
    if (eptr) {
        try {
            std::rethrow_exception(eptr);
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(error) << "Split to Fit failed: " << e.what();
            show_error(m_plater, format_wxstr(_L("Split to Fit could not split \"%1%\": %2%"), m_name, std::string(e.what())));
        }
        eptr = nullptr;
        return;
    }
    NotificationManager *notes = wxGetApp().notification_manager();
    if (canceled || m_result.canceled) {
        notes->push_notification(_u8L("Split to Fit was canceled. The object is unchanged."));
        return;
    }
    if (m_result.parts.empty()) {
        notes->push_plater_warning_notification(_u8L("Split to Fit made no parts. The object is unchanged."));
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
        notes->push_plater_warning_notification(
            (boost::format(_u8L("\"%1%\" was removed while it was being split. Nothing was changed.")) % m_name).str());
        return;
    }

    // D11: keep the model's orientation, parts dropped to the bed. Every part is moved off the plates
    // first, so the arrange below places all of them (it arranges only what is on no plate) instead of
    // leaving a part where it happened to land on plate 1, under the part above it.
    // P2: the loose dowels go with them and are arranged too.
    ModelObjectPtrs all = m_result.parts;
    all.insert(all.end(), m_result.dowels.begin(), m_result.dowels.end());
    double next_x = -10000.;
    for (ModelObject *part : all) {
        part->ensure_on_bed();
        const BoundingBoxf3 bb = part->instance_bounding_box(0);
        Vec3d               off = part->instances.front()->get_offset();
        off.x() += next_x - bb.min.x();
        off.y() += -bb.min.y();
        part->instances.front()->set_offset(off);
        part->invalidate_bounding_box();
        next_x -= bb.size().x() + 20.;
    }

    {
        // One undo step for the whole split: the arrange queued below takes no snapshot of its own.
        Plater::TakeSnapshot snapshot(m_plater, _u8L("Split to Fit"));
        m_plater->apply_cut_object_to_model(size_t(obj_idx), all);

        // A split with dowels is one linked cut group (D10); an object that was already in a linked
        // cut group passes the grown group on to every member.
        const CutObjectBase &cut_id = m_result.parts.front()->cut_id;
        if (!cut_id.id().invalid())
            for (ModelObject *o : model.objects)
                if (o->is_cut() && o->cut_id.has_same_id(cut_id) && !o->cut_id.is_equal(cut_id))
                    o->cut_id.copy(cut_id);
    }

    std::string summary = (boost::format(_u8L("\"%1%\" was split into %2% parts.")) % m_name % m_result.parts.size()).str();
    if (!m_result.dowels.empty())
        summary += " " + (boost::format(m_plan.dowel_options.type == SplitConnectorType::Pin ? _u8L("%1% pins to print were added.") :
                                                                                               _u8L("%1% dowels to print were added.")) %
                          m_result.dowels.size())
                             .str();
    for (const SplitWarning &w : m_result.warnings)
        summary += "\n" + into_u8(split_warning_text(w, m_plan));
    if (m_result.warnings.empty())
        notes->push_notification(summary);
    else
        notes->push_plater_warning_notification(summary);

    // D12: arrange the new parts (and only what is off the plates) over as many plates as needed.
    Plater *plater = m_plater;
    wxGetApp().CallAfter([plater]() {
        Worker &w = plater->get_ui_job_worker();
        queue_job(w, std::make_unique<ArrangeJob>(true));
    });
}

}} // namespace Slic3r::GUI
