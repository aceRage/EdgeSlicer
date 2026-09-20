#include "SliceBakeJob.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI {

SliceBakeJob::SliceBakeJob(Plater                  *plater,
                           const PrintObject       *print_object,
                           ObjectID                 model_object_id,
                           const SliceBakeSettings &settings,
                           const std::string       &object_name,
                           const std::string       &export_path)
    : m_plater(plater)
    , m_print_object(print_object)
    , m_object_id(model_object_id)
    , m_settings(settings)
    , m_name(object_name)
    , m_export_path(export_path)
{}

void SliceBakeJob::process(Ctl &ctl)
{
    if (m_print_object == nullptr)
        return;

    const std::string status = _u8L("Baking the slice to a mesh");
    ctl.update_status(0, status);

    // The frame is a property of what the mesh is FOR, not of the bake, so it is set here rather
    // than in the dialog:
    //
    //   Replace    - the mesh goes into the object's existing first volume with an identity volume
    //                transform, and the object's EXISTING instance re-applies the rotation and the
    //                scale. So the vertices must not carry them: Object frame.
    //   Add as new - the mesh becomes a fresh ModelObject whose instance is identity but for an
    //                offset. Nothing will re-apply the rotation or the scale, so the vertices have
    //                to carry them, and only the position comes out: World frame. (Handing the
    //                Object frame here is the bug this route had - a 2x-scaled part baked to a
    //                1x-sized object sitting in the wrong place.)
    //   Export STL - a file has no instance at all, so it wants the part as it stands on the
    //                plate, rotation and scale included: World frame too, and the report's
    //                instance_offset is simply not used.
    SliceBakeOptions opts = m_settings.options;
    opts.frame = m_settings.result == SliceBakeResultMode::Replace ? SliceBakeFrame::Object
                                                                   : SliceBakeFrame::World;

    try {
        m_mesh = slice_bake_to_mesh(*m_print_object, opts, &m_report,
                                    [&ctl, &status](int percent) {
                                        ctl.update_status(percent, status);
                                        return ! ctl.was_canceled();
                                    });
    } catch (const SliceBakeCancelled &) {
        return;                               // finalize() sees canceled == true and changes nothing
    } catch (const std::exception &e) {
        // Clipper and the loft can both throw on degenerate input; the failure is reported rather
        // than unwound out through the worker.
        m_error = e.what();
        return;
    }

    ctl.update_status(100, _u8L("Slice baked."));
}

void SliceBakeJob::finalize(bool canceled, std::exception_ptr &eptr)
{
    if (canceled || eptr)
        return;

    if (! m_error.empty()) {
        show_error(m_plater, from_u8((boost::format(_u8L("Could not bake \"%1%\": %2%")) % m_name % m_error).str()));
        return;
    }
    if (m_mesh.indices.empty()) {
        wxGetApp().notification_manager()->push_plater_warning_notification(
            (boost::format(_u8L("\"%1%\" had no outer wall to bake: %2%")) % m_name
             % (m_report.note.empty() ? _u8L("slice the plate first") : m_report.note)).str());
        return;
    }

    BOOST_LOG_TRIVIAL(info) << "slice_bake: '" << m_name << "' " << m_report.layers_baked << " layers -> "
                            << m_report.triangles << " triangles, "
                            << (m_report.watertight ? "watertight" : "NOT watertight");

    const std::string summary =
        (boost::format(_u8L("Baked %1% layers of \"%2%\" into %3% triangles.")) % m_report.layers_baked % m_name
         % m_report.triangles).str();

    // ---- export ---------------------------------------------------------------------------------
    // Nothing in the scene changes, so there is no snapshot and no plate invalidation.
    if (m_settings.result == SliceBakeResultMode::ExportSTL) {
        if (m_export_path.empty())
            return;                            // the user cancelled the file dialog
        if (! its_write_stl_binary(m_export_path.c_str(), m_name.c_str(), m_mesh)) {
            show_error(m_plater, from_u8((boost::format(_u8L("Could not write %1%.")) % m_export_path).str()));
            return;
        }
        wxGetApp().notification_manager()->push_notification(
            summary + " " + (boost::format(_u8L("Saved to %1%.")) % m_export_path).str());
        return;
    }

    // ---- add as a new object --------------------------------------------------------------------
    // The bake is a fresh object, so nothing about the original is disturbed and the plate's slice
    // stays valid for the original - but the new object is unsliced, so the plate is invalidated
    // all the same, exactly as adding any other object does.
    //
    // ObjectList::load_mesh_object is deliberately NOT used here, and that is the fix for the
    // "wrong size and wrong place" report. It recentres the mesh on its own bounding box and then
    // drops the object into the nearest EMPTY CELL of the plate - sensible for a mesh arriving out
    // of nowhere, wrong for one that has a place it belongs. The object is built by hand instead,
    // with:
    //
    //   * the World-frame mesh, which carries the instance's rotation and scale in its vertices,
    //   * an instance whose rotation and scale are IDENTITY (they are in the vertices already -
    //     re-applying them would square the scale), and
    //   * that instance's offset set to SliceBakeReport::instance_offset, which is where the
    //     sliced object stood.
    //
    // The result stands exactly on top of the original, at exactly its size.
    if (m_settings.result == SliceBakeResultMode::AddNew) {
        Plater::TakeSnapshot snapshot(m_plater, "Bake slice to mesh");

        Model       &model      = m_plater->model();
        ModelObject *new_object = model.add_object();
        new_object->name = m_name + " (baked)";
        ModelVolume *new_volume = new_object->add_volume(TriangleMesh(m_mesh));
        new_volume->name = new_object->name;
        // Same default the load path sets, so the object is not left without one.
        new_object->config.set_key_value("extruder", new ConfigOptionInt(0));

        ModelInstance *inst = new_object->add_instance();
        inst->set_offset(m_report.instance_offset);
        // Left identity on purpose; see above.
        inst->set_rotation(Vec3d::Zero());
        inst->set_scaling_factor(Vec3d::Ones());
        inst->set_mirror(Vec3d::Ones());

        new_object->invalidate_bounding_box();
        // NOT ensure_on_bed(): the mesh's Z is the printed Z, so it already sits on the bed exactly
        // where the slice did, and nudging it would move the bake off the original.
        model.InitializeAssemblyPositions({new_object});
        Slic3r::save_object_mesh(*new_object);

        wxGetApp().obj_list()->paste_objects_into_list({model.objects.size() - 1});
        m_plater->get_partplate_list().notify_instance_update(int(model.objects.size()) - 1, 0);

        wxGetApp().notification_manager()->push_notification(summary);
        return;
    }

    // ---- replace the object ----------------------------------------------------------------------
    Model &model = m_plater->model();
    int    obj_idx = -1;
    for (size_t i = 0; i < model.objects.size(); ++i)
        if (model.objects[i]->id() == m_object_id) {
            obj_idx = int(i);
            break;
        }
    if (obj_idx < 0) {
        wxGetApp().notification_manager()->push_plater_warning_notification(
            (boost::format(_u8L("\"%1%\" was removed while it was being baked; nothing was changed.")) % m_name).str());
        return;
    }
    ModelObject *object = model.objects[size_t(obj_idx)];

    // One snapshot for the whole replacement, so a single Undo puts the original mesh back.
    Plater::TakeSnapshot snapshot(m_plater, "Bake slice to mesh");

    // The bake's vertex indices bear no relation to the source's, so every per-triangle painted
    // layer (supports, seam, fuzzy skin, MMU colour) would be nonsense on the new mesh. This is
    // the same call repair_by_remesh makes before it replaces a volume's mesh, and it is what
    // drops the paint.
    m_plater->clear_before_change_mesh(obj_idx);

    // The bake replaces the object's model PARTS with one volume. Modifiers, negative volumes and
    // support blockers are left alone: they are placement data the user set up around the part,
    // and the bake does not invalidate them.
    ModelVolume *first_part = nullptr;
    for (ModelVolume *v : object->volumes)
        if (v->is_model_part()) { first_part = v; break; }
    if (first_part == nullptr) {
        wxGetApp().notification_manager()->push_plater_warning_notification(
            (boost::format(_u8L("\"%1%\" has no part to replace.")) % m_name).str());
        return;
    }

    // The bake came back in the OBJECT's frame (SliceBakeFrame::Object), i.e. in the same space the
    // source volume's own mesh lives in once its volume transform is applied. The source volume's
    // transform is therefore reset to identity and the mesh takes its place verbatim - and the
    // object's instance, which is not touched at all, re-applies the rotation, the scale and the
    // position. That is what keeps the object at exactly the size and place it had.
    //
    // Every part after the first is dropped: the bake is ONE solid covering all of them (the
    // per-layer union ran over every region of every layer, whatever volume produced it), so
    // keeping the others would leave duplicate geometry inside the bake.
    std::vector<ModelVolume *> keep;
    for (ModelVolume *v : object->volumes)
        if (! v->is_model_part())
            keep.push_back(v);

    first_part->set_mesh(indexed_triangle_set(m_mesh));
    first_part->set_transformation(Geometry::Transformation());
    first_part->set_new_unique_id();
    first_part->calculate_convex_hull();
    first_part->name = m_name + " (baked)";

    for (auto it = object->volumes.begin(); it != object->volumes.end();) {
        if (*it != first_part && (*it)->is_model_part()) {
            delete *it;
            it = object->volumes.erase(it);
        } else {
            ++it;
        }
    }

    object->invalidate_bounding_box();
    object->ensure_on_bed();
    // The object is no longer what its source file holds (ObjectList::split does the same).
    object->input_file.clear();

    m_plater->changed_mesh(obj_idx);
    m_plater->get_partplate_list().notify_instance_update(obj_idx, 0);
    wxGetApp().obj_list()->update_item_error_icon(obj_idx, -1);
    wxGetApp().obj_list()->update_info_items(size_t(obj_idx));
    wxGetApp().obj_list()->update_plate_values_for_items();

    // The plate's G-code was computed from the OLD mesh; re-slicing has to happen against the new
    // one. changed_mesh() already schedules the background process, but the plate's own
    // "the slice is still valid" flag is what the UI reads, so it is cleared explicitly.
    if (PartPlate *plate = m_plater->get_partplate_list().get_curr_plate())
        plate->update_slice_result_valid_state(false);

    wxGetApp().notification_manager()->push_notification(
        summary + " " + _u8L("Painted data was cleared and the plate needs slicing again."));
}

}} // namespace Slic3r::GUI
