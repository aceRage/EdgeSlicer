#include "QuadRemeshJob.hpp"

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "libslic3r/Model.hpp"

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <thread>

namespace Slic3r { namespace GUI {

QuadRemeshJob::QuadRemeshJob(Plater *plater, std::vector<Target> targets)
    : m_plater(plater), m_targets(std::move(targets))
{}

void QuadRemeshJob::process(Ctl &ctl)
{
    const std::string status = _u8L("Quad remeshing");
    const int         n      = int(m_targets.size());

    for (int i = 0; i < n; ++i) {
        if (ctl.was_canceled())
            return;                      // a cancel between parts must not start the next one
        Target &t = m_targets[i];
        ctl.update_status(100 * i / std::max(1, n), status + " " + t.name);

        // The remesh runs on its own thread so this one can keep watching the clock and the
        // cancel flag. QuadriFlow exposes no progress or cancellation hook, so those are the only
        // two things that can end the wait early.
        //
        // shared_ptr, and the helper owns a COPY of the input: once the wait times out this job
        // stops caring about the helper, but the helper is still running and must not be left
        // reading storage that dies with the target. See the note on QUAD_REMESH_TIMEOUT_SECONDS.
        struct Work
        {
            indexed_triangle_set mesh;
            QuadRemeshOptions    opts;
            indexed_triangle_set out;
            QuadRemeshReport     report;
        };
        auto work  = std::make_shared<Work>();
        work->mesh = t.mesh;
        work->opts = t.opts;

        auto done   = std::make_shared<std::promise<void>>();
        auto future = done->get_future();
        std::thread([work, done]() {
            try {
                work->out = quad_remesh_triangulated(work->mesh, work->opts, &work->report);
            } catch (...) {
                work->out           = indexed_triangle_set();
                work->report        = QuadRemeshReport{};
                work->report.status = QuadRemeshStatus::Failed;
                work->report.note   = "the quad remesher threw";
            }
            done->set_value();
        }).detach();

        // Poll rather than one wait_for, so Cancel is noticed within a tick instead of possibly
        // taking the whole cap.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(QUAD_REMESH_TIMEOUT_SECONDS);
        bool       finished = false;
        for (;;) {
            if (future.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready) {
                finished = true;
                break;
            }
            if (ctl.was_canceled())
                return;                  // finalize() sees canceled == true and changes nothing
            if (std::chrono::steady_clock::now() >= deadline)
                break;
        }

        if (!finished) {
            t.timed_out = true;
            BOOST_LOG_TRIVIAL(warning) << "quad_remesh: given up on part after "
                                       << QUAD_REMESH_TIMEOUT_SECONDS << " s";
        } else {
            t.result = std::move(work->out);
            t.report = work->report;
            t.ok     = !t.result.indices.empty();
            BOOST_LOG_TRIVIAL(info) << "quad_remesh: " << t.report.triangles_before
                                    << " triangles -> " << t.report.quads_after << " quads / "
                                    << t.report.triangles_after << " triangles"
                                    << (t.report.status == QuadRemeshStatus::Ok ? "" : ", refused: " + t.report.note);
        }

        // The input copy is only needed by the remesh; release it before the next (possibly large)
        // part. The helper thread has its own copy.
        t.mesh = indexed_triangle_set();
    }

    ctl.update_status(100, _u8L("Quad remesh done."));
}

void QuadRemeshJob::finalize(bool canceled, std::exception_ptr &eptr)
{
    if (canceled || eptr)
        return;

    Model &model = m_plater->model();

    // Taken lazily: a run in which every part failed must not leave an empty step on the undo
    // stack.
    std::optional<Plater::TakeSnapshot> snapshot;

    size_t total_before = 0, total_after = 0, total_quads = 0;
    int    remeshed = 0, failed = 0, timed_out = 0;
    std::string         first_refusal;
    std::vector<size_t> touched_objects;

    for (Target &t : m_targets) {
        if (t.timed_out) {
            ++timed_out;
            ++failed;
            continue;
        }
        if (!t.ok) {
            ++failed;
            if (first_refusal.empty() && !t.report.note.empty())
                first_refusal = t.report.note;
            continue;
        }

        // Re-find the volume by id: the model may have been edited while the remesh ran, and a
        // stale pointer would be written straight through.
        ModelObject *mo   = nullptr;
        ModelVolume *mv   = nullptr;
        size_t       oidx = 0;
        for (size_t i = 0; i < model.objects.size() && mv == nullptr; ++i) {
            if (model.objects[i]->id() != t.object_id)
                continue;
            for (ModelVolume *v : model.objects[i]->volumes)
                if (v->id() == t.volume_id) {
                    mo   = model.objects[i];
                    mv   = v;
                    oidx = i;
                    break;
                }
        }
        if (mo == nullptr || mv == nullptr) {
            ++failed;
            continue;                    // the part is gone; skip rather than guess
        }
        // ... and that it is still the mesh we remeshed.
        if (mv->mesh().its.vertices.size() != t.mesh_vertices || mv->mesh().its.indices.size() != t.mesh_indices) {
            ++failed;
            continue;
        }

        if (!snapshot)
            snapshot.emplace(m_plater, "Quad remesh");
        if (std::find(touched_objects.begin(), touched_objects.end(), oidx) == touched_objects.end()) {
            // Facet indices are meaningless after any remesh, so the painted data goes - this also
            // fires the existing "custom supports removed" notification.
            m_plater->clear_before_change_mesh(int(oidx));
            touched_objects.push_back(oidx);
        }

        total_before += t.report.triangles_before;
        total_after += t.report.triangles_after;
        total_quads += t.report.quads_after;

        mv->set_mesh(std::move(t.result));
        mv->set_new_unique_id();
        mv->calculate_convex_hull();
        ++remeshed;
    }

    for (size_t oidx : touched_objects) {
        ModelObject *mo = model.objects[oidx];
        mo->invalidate_bounding_box();
        mo->ensure_on_bed();
        m_plater->changed_mesh(int(oidx));
        m_plater->get_partplate_list().notify_instance_update(int(oidx), 0);
    }
    if (ObjectList *list = m_plater->sidebar().obj_list(); list != nullptr) {
        for (size_t oidx : touched_objects) {
            list->update_item_error_icon(int(oidx), -1);
            list->update_info_items(int(oidx));
        }
        list->update_plate_values_for_items();
    }

    NotificationManager *notify = m_plater->get_notification_manager();
    if (notify == nullptr)
        return;

    wxString msg;
    if (remeshed > 0) {
        msg = GUI::format(_L("Quad remeshed %1% part(s)."), remeshed) + " " +
              GUI::format(_L("Triangles: %1% -> %2% (%3% quads)."), total_before, total_after, total_quads);
    } else {
        msg = _L("Nothing was quad remeshed.");
    }
    // A timeout and a refusal need different answers from the user, so they are reported
    // separately rather than both as "failed".
    if (timed_out > 0)
        msg += " " + GUI::format(_L("%1% part(s) were given up on after %2% seconds. Try a lower "
                                    "target face count, or split the part up."),
                                 timed_out, QUAD_REMESH_TIMEOUT_SECONDS);
    const int refused = failed - timed_out;
    if (refused > 0 && !first_refusal.empty())
        msg += " " + GUI::format(_L("%1% part(s) were skipped: %2%"), refused, from_u8(first_refusal));
    else if (refused > 0)
        msg += " " + GUI::format(_L("%1% part(s) failed."), refused);
    notify->push_notification(into_u8(msg));
}

}} // namespace Slic3r::GUI
