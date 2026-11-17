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
#include <set>

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
    std::shared_ptr<SplitArrangeChain> chain = std::make_shared<SplitArrangeChain>(
        m_plater, std::move(groups), m_plan.dowel_options.type == SplitConnectorType::Pin ? _u8L("pins") : _u8L("dowels"));
    wxGetApp().CallAfter([chain]() { chain->run_step(); });
}

// ---- P4: arranging groups on plates of their own ----------------------------------------------------------

SplitArrangeChain::SplitArrangeChain(Plater *plater, std::vector<std::vector<ObjectID>> groups, std::string dowel_word)
    : m_plater(plater), m_groups(std::move(groups)), m_dowel_word(std::move(dowel_word))
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

// Every instance of the model with its group (-1: not the chain's) and the plate it is registered to.
std::vector<SplitArrangeInstance> SplitArrangeChain::instances(std::vector<std::pair<int, int>> *where) const
{
    std::vector<SplitArrangeInstance> out;
    Model                            &model  = m_plater->model();
    PartPlateList                    &plates = m_plater->get_partplate_list();
    for (size_t oi = 0; oi < model.objects.size(); ++oi) {
        const ModelObject *o     = model.objects[oi];
        int                group = -1;
        for (size_t g = 0; g < m_groups.size() && group < 0; ++g)
            if (std::find(m_groups[g].begin(), m_groups[g].end(), o->id()) != m_groups[g].end())
                group = int(g);
        for (size_t i = 0; i < o->instances.size(); ++i) {
            SplitArrangeInstance in;
            in.group     = group;
            in.plate     = plates.find_instance(int(oi), int(i));
            in.printable = o->instances[i]->printable;
            out.push_back(in);
            if (where != nullptr)
                where->emplace_back(int(oi), int(i));
        }
    }
    return out;
}

void SplitArrangeChain::run_step()
{
    release();
    // Skip the groups with nothing (left) in them.
    while (m_step < m_groups.size() &&
           std::none_of(m_groups[m_step].begin(), m_groups[m_step].end(), [this](const ObjectID &id) { return find(id) != nullptr; }))
        ++m_step;
    if (m_step >= m_groups.size()) {
        report();
        return;
    }

    // What to do is worked out by split_arrange_step() (libslic3r, tested). In short: this group and the
    // later ones are taken off the plates they are registered to (a part moved off the plates is still
    // registered to plate 1, and an instance registered to a locked plate is never moved: that kept every
    // dowel in its parking row); the later groups are set not to print, so they wait; and from the second
    // group on, every plate still holding something is locked, so this group gets plates of its own.
    std::vector<std::pair<int, int>>        where;
    const std::vector<SplitArrangeInstance> all    = instances(&where);
    PartPlateList                          &plates = m_plater->get_partplate_list();
    std::vector<bool>                       locked;
    for (int i = 0; i < plates.get_plate_count(); ++i)
        locked.push_back(plates.get_plate(i)->is_locked());
    const SplitArrangeStep step = split_arrange_step(all, int(m_step), locked);
    for (size_t k : step.unregister)
        if (PartPlate *plate = plates.get_plate(all[k].plate); plate != nullptr)
            plate->remove_instance(where[k].first, where[k].second);
    Model &model = m_plater->model();
    for (size_t k : step.hold) {
        ModelObject *o = model.objects[size_t(where[k].first)];
        o->instances[size_t(where[k].second)]->printable = false;
        m_held.emplace_back(o->id(), size_t(where[k].second));
    }
    for (int p : step.lock_plates) {
        plates.get_plate(p)->lock(true);
        m_locked.push_back(p);
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
                self->report();
                return;
            }
            ++self->m_step;
            wxGetApp().CallAfter([self]() { self->run_step(); });
        });
}

void SplitArrangeChain::report()
{
    // Where every group ended up, and anything left off the plates, so nothing is lost silently.
    std::vector<std::pair<int, int>>        where;
    const std::vector<SplitArrangeInstance> all    = instances(&where);
    PartPlateList                          &plates = m_plater->get_partplate_list();
    std::vector<int>                        placed(all.size(), -1);
    for (size_t k = 0; k < all.size(); ++k)
        if (all[k].plate >= 0)
            if (PartPlate *plate = plates.get_plate(all[k].plate);
                plate != nullptr && plate->contain_instance_totally(where[k].first, where[k].second))
                placed[k] = all[k].plate;
    const SplitArrangeReport rep = split_arrange_report(all, placed, int(m_groups.size()));

    auto range = [](const std::vector<int> &ps) {
        std::string out;
        for (size_t i = 0; i < ps.size();) {
            size_t j = i;
            while (j + 1 < ps.size() && ps[j + 1] == ps[j] + 1)
                ++j;
            if (!out.empty())
                out += ", ";
            out += std::to_string(ps[i] + 1);
            if (j > i)
                out += "-" + std::to_string(ps[j] + 1);
            i = j + 1;
        }
        return out;
    };
    const std::string     names[3] = { _u8L("parts"), _u8L("loose parts"), m_dowel_word };
    std::string           text;
    bool                  problem = false;
    std::set<std::string> lost;
    for (size_t g = 0; g < rep.plates.size() && g < 3; ++g) {
        if (!rep.plates[g].empty()) {
            text += (text.empty() ? "" : "; ") + names[g] + ": " +
                    (boost::format(rep.plates[g].size() == 1 ? _u8L("plate %1%") : _u8L("plates %1%")) % range(rep.plates[g])).str();
            if (g > 0 && rep.shares[g]) {
                text += " " + _u8L("(shared with the groups before)");
                problem = true;
            }
        }
        for (size_t k : rep.unplaced[g])
            lost.insert(m_plater->model().objects[size_t(where[k].first)]->name);
    }
    std::string msg = _u8L("Split to Fit arranged the result.");
    if (!text.empty())
        msg += " " + text + ".";
    if (!lost.empty()) {
        problem = true;
        std::string list;
        for (const std::string &n : lost)
            list += (list.empty() ? "" : ", ") + n;
        msg += "\n" + (boost::format(_u8L("Not on any plate (left beside the plates): %1%. Place them by hand or arrange again.")) % list).str();
    }
    NotificationManager *notes = wxGetApp().notification_manager();
    if (problem)
        notes->push_plater_warning_notification(msg);
    else
        notes->push_notification(msg);
}

}} // namespace Slic3r::GUI
