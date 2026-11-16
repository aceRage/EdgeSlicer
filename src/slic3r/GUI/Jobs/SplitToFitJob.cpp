#include "SplitToFitJob.hpp"

#include "ArrangeJob.hpp"
#include "Worker.hpp"

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoSplitToFit.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/format.hpp"

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>

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

    // D11: keep the model's orientation (P4: or the cut face the part was laid on), parts dropped to
    // the bed. Every part is moved off the plates first, so the arrange below places all of them (it
    // arranges only what is on no plate) instead of leaving a part where it happened to land on plate 1,
    // under the part above it. P2: the loose dowels go with them and are arranged too; P4: every
    // instance of a dowel object (identical dowels are instances of one object).
    ModelObjectPtrs all = m_result.parts;
    all.insert(all.end(), m_result.dowels.begin(), m_result.dowels.end());
    double next_x = -10000.;
    for (ModelObject *part : all) {
        part->ensure_on_bed();
        for (size_t i = 0; i < part->instances.size(); ++i) {
            const BoundingBoxf3 bb  = part->instance_bounding_box(i);
            Vec3d               off = part->instances[i]->get_offset();
            off.x() += next_x - bb.min.x();
            off.y() += -bb.min.y();
            part->instances[i]->set_offset(off);
            next_x -= bb.size().x() + 20.;
        }
        part->invalidate_bounding_box();
    }

    // P4: the arrange groups, as indices into `all`. The main parts first; then the loose pieces on
    // plates of their own when asked; then the dowels on theirs.
    std::vector<int> group_of(all.size(), 2);
    for (size_t i = 0; i < m_result.parts.size(); ++i)
        group_of[i] = m_plan.loose_own_plates && i < m_result.part_loose.size() && m_result.part_loose[i] ? 1 : 0;

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
    // Loading copied `all` (new ids) to the end of the object list, in order.
    std::vector<std::vector<ObjectID>> groups(3);
    if (model.objects.size() >= all.size())
        for (size_t i = 0; i < all.size(); ++i)
            groups[size_t(group_of[i])].push_back(model.objects[model.objects.size() - all.size() + i]->id());

    std::string summary = (boost::format(_u8L("\"%1%\" was split into %2% parts.")) % m_name % m_result.parts.size()).str();
    if (!m_result.dowels.empty())
        summary += " " + (boost::format(m_plan.dowel_options.type == SplitConnectorType::Pin ? _u8L("%1% pins to print were added.") :
                                                                                               _u8L("%1% dowels to print were added.")) %
                          m_result.dowel_count)
                             .str();
    // P4: the parts turned onto a cut face.
    if (const size_t laid = size_t(std::count(m_result.part_laid_flat.begin(), m_result.part_laid_flat.end(), true)); laid > 0)
        summary += " " + (boost::format(_u8L("%1% parts were turned to lie on a cut face.")) % laid).str();
    if (m_result.labels_on_bed > 0)
        summary += " " + (boost::format(_u8L("%1% labels are on a face that now lies on the bed: they print as a dent in the bottom.")) %
                          m_result.labels_on_bed)
                             .str();
    // P5: the matching labels, two per pair of faces.
    if (m_result.labels > 0)
        summary += " " + (boost::format(_u8L("%1% matching labels were recessed into the cut faces.")) % m_result.labels).str();
    for (const SplitWarning &w : m_result.warnings)
        summary += "\n" + into_u8(split_warning_text(w, m_plan));
    if (m_result.warnings.empty())
        notes->push_notification(summary);
    else
        notes->push_plater_warning_notification(summary);

    // D12: arrange the new parts (and only what is off the plates) over as many plates as needed.
    // P4 (items 2, 3): the dowels, and the loose pieces when asked, on plates of their own.
    std::shared_ptr<SplitArrangeChain> chain = std::make_shared<SplitArrangeChain>(m_plater, std::move(groups));
    wxGetApp().CallAfter([chain]() { chain->run_step(); });
}

// ---- P4: arranging groups on plates of their own ----------------------------------------------------------

SplitArrangeChain::SplitArrangeChain(Plater *plater, std::vector<std::vector<ObjectID>> groups)
    : m_plater(plater), m_groups(std::move(groups))
{}

ModelObject *SplitArrangeChain::find(const ObjectID &id) const
{
    for (ModelObject *o : m_plater->model().objects)
        if (o->id() == id)
            return o;
    return nullptr;
}

void SplitArrangeChain::release()
{
    // Back to printable what this chain set not to print, and unlock the plates it locked.
    for (const auto &held : m_held)
        if (ModelObject *o = find(held.first); o != nullptr && held.second < o->instances.size())
            o->instances[held.second]->printable = true;
    m_held.clear();
    PartPlateList &plates = m_plater->get_partplate_list();
    for (int i : m_locked)
        if (i >= 0 && i < plates.get_plate_count())
            plates.get_plate(i)->lock(false);
    m_locked.clear();
}

void SplitArrangeChain::run_step()
{
    release();
    // Skip the groups with nothing (left) in them.
    while (m_step < m_groups.size() &&
           std::none_of(m_groups[m_step].begin(), m_groups[m_step].end(), [this](const ObjectID &id) { return find(id) != nullptr; }))
        ++m_step;
    if (m_step >= m_groups.size())
        return;

    // The groups still to come wait off the plates: a loose object set not to print stays where it is.
    for (size_t g = m_step + 1; g < m_groups.size(); ++g)
        for (const ObjectID &id : m_groups[g])
            if (ModelObject *o = find(id); o != nullptr)
                for (size_t i = 0; i < o->instances.size(); ++i)
                    if (o->instances[i]->printable) {
                        o->instances[i]->printable = false;
                        m_held.emplace_back(id, i);
                    }
    // A later group goes on plates of its own: a locked plate takes nothing new.
    if (m_step > 0) {
        PartPlateList &plates = m_plater->get_partplate_list();
        for (int i = 0; i < plates.get_plate_count(); ++i) {
            PartPlate *plate = plates.get_plate(i);
            if (plate != nullptr && !plate->is_locked() && !plate->empty()) {
                plate->lock(true);
                m_locked.push_back(i);
            }
        }
    }

    Worker &w = m_plater->get_ui_job_worker();
    queue_job(w, std::make_unique<ArrangeJob>(true));
    std::shared_ptr<SplitArrangeChain> self = shared_from_this();
    queue_job(
        w, [](Job::Ctl &) {},
        [self](bool canceled, std::exception_ptr &) {
            if (canceled) {
                self->release();
                self->m_plater->update();
                return;
            }
            ++self->m_step;
            wxGetApp().CallAfter([self]() { self->run_step(); });
        });
}

}} // namespace Slic3r::GUI
