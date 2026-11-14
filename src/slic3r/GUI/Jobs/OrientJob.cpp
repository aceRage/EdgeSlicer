#include "OrientJob.hpp"

#include "libslic3r/Model.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "libslic3r/PresetBundle.hpp"


namespace Slic3r { namespace GUI {


void OrientJob::clear_input()
{
    const Model &model = m_plater->model();

    size_t count = 0, cunprint = 0; // To know how much space to reserve
    for (auto obj : model.objects)
        for (auto mi : obj->instances)
            mi->printable ? count++ : cunprint++;

    m_selected.clear();
    m_unselected.clear();
    m_unprintable.clear();
    m_selected.reserve(count);
    m_unselected.reserve(count);
    m_unprintable.reserve(cunprint);
}

//BBS: add only one plate mode and lock logic
// asked[o][i]: instance i of object o was asked for. Which of them are turned is decided by
// orientation::instances_to_orient (libslic3r/Orient.cpp).
void OrientJob::prepare_selection(const std::vector<std::vector<bool>> &asked, bool only_one_plate)
{
    Model& model = m_plater->model();
    PartPlateList& plate_list = m_plater->get_partplate_list();

    std::vector<std::vector<bool>> on_locked_plate(asked.size());
    std::vector<bool>              printable(asked.size(), false);
    for (size_t oidx = 0; oidx < asked.size(); ++oidx) {
        ModelObject* mo = model.objects[oidx];
        printable[oidx] = mo->printable;
        on_locked_plate[oidx].assign(mo->instances.size(), false);
        if (only_one_plate)
            continue;
        for (size_t inst_idx = 0; inst_idx < mo->instances.size(); ++inst_idx) {
            int plate_index = plate_list.find_instance(oidx, inst_idx);
            on_locked_plate[oidx][inst_idx] = plate_index >= 0 && plate_index < plate_list.get_plate_count() && plate_list.is_locked(plate_index);
        }
    }

    bool asked_locked = false;
    const std::vector<std::vector<bool>> turn = orientation::instances_to_orient(asked, on_locked_plate, printable, only_one_plate, &asked_locked);

    for (size_t oidx = 0; oidx < asked.size(); ++oidx) {
        ModelObject* mo = model.objects[oidx];
        for (size_t inst_idx = 0; inst_idx < mo->instances.size(); ++inst_idx) {
            if (on_locked_plate[oidx][inst_idx])
                continue;
            auto& cont = turn[oidx][inst_idx] ? m_selected : (mo->printable ? m_unselected : m_unprintable);
            cont.emplace_back(get_orient_mesh(mo->instances[inst_idx]));
        }
    }

    if (m_selected.empty() && asked_locked)
        m_plater->get_notification_manager()->push_notification(NotificationType::BBLPlateInfo,
            NotificationManager::NotificationLevel::WarningNotificationLevel, into_u8(_L("All the selected objects are on a locked plate.\nCannot auto-orient these objects.")));
}

void OrientJob::prepare_selected() {
    clear_input();

    Model &model = m_plater->model();

    std::vector<std::vector<bool>> asked(model.objects.size());
    for (size_t oidx = 0; oidx < model.objects.size(); ++oidx)
        asked[oidx].assign(model.objects[oidx]->instances.size(), false);

    // A selected object is asked for with all of its instances.
    for (auto &s : m_plater->get_selection().get_content())
        if (s.first < int(asked.size()) && !s.second.empty())
            asked[size_t(s.first)].assign(asked[size_t(s.first)].size(), true);

   //BBS: add only one plate mode
    prepare_selection(asked, false);
}

//BBS: prepare current part plate for orienting
void OrientJob::prepare_partplate() {
    clear_input();

    PartPlateList& plate_list = m_plater->get_partplate_list();
    PartPlate* plate = plate_list.get_curr_plate();
    assert(plate != nullptr);

    if (plate->empty())
    {
        //no instances on this plate
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": no instances in current plate!");

        return;
    }

    if (plate->is_locked()) {
        m_plater->get_notification_manager()->push_notification(NotificationType::BBLPlateInfo,
            NotificationManager::NotificationLevel::WarningNotificationLevel, into_u8(_L("This plate is locked.\nCannot auto-orient on this plate.")));
        return;
    }

    Model& model = m_plater->model();

    // Each instance on this plate, and no other: an object with copies on other plates
    // has only its copies here turned.
    std::vector<std::vector<bool>> asked(model.objects.size());
    for (size_t oidx = 0; oidx < model.objects.size(); ++oidx)
    {
        ModelObject* mo = model.objects[oidx];
        asked[oidx].assign(mo->instances.size(), false);
        for (size_t inst_idx = 0; inst_idx < mo->instances.size(); ++inst_idx)
            asked[oidx][inst_idx] = plate->contain_instance(oidx, inst_idx);
    }

    prepare_selection(asked, true);
}

//BBS: add partplate logic
void OrientJob::prepare()
{
    int state = m_plater->get_prepare_state();
    m_plater->get_notification_manager()->bbl_close_plateinfo_notification();
    if (state == Job::JobPrepareState::PREPARE_STATE_DEFAULT) {
        // only_on_partplate = false;
        prepare_selected();
    }
    else if (state == Job::JobPrepareState::PREPARE_STATE_MENU) {
        // only_on_partplate = true;   // only arrange items on current plate
        prepare_partplate();
    }
}

void OrientJob::process(Ctl &ctl)
{
    static const auto arrangestr = _u8L("Orienting...");

    ctl.update_status(0, arrangestr);
    ctl.call_on_main_thread([this]{ prepare(); }).wait();;

    auto start = std::chrono::steady_clock::now();

    const GLCanvas3D::OrientSettings& settings = m_plater->canvas3D()->get_orient_settings();

    orientation::OrientParams params;
    orientation::OrientParamsArea params_area;
    if (settings.min_area) {
        memcpy(&params, &params_area, sizeof(params));
        params.min_volume = false;
    }
    else {
        params.min_volume = true;
    }

    auto count = unsigned(m_selected.size() + m_unprintable.size());
    params.stopcondition = [&ctl]() { return ctl.was_canceled(); };

    params.progressind = [this, count, &ctl](unsigned st, std::string orientstr) {
        st += m_unprintable.size();
        if (st > 0) ctl.update_status(int(st / float(count) * 100), _u8L("Orienting") + " " + orientstr);
    };

    orientation::orient(m_selected, m_unselected, params);

    auto time_elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start);

    std::stringstream ss;
    if (!m_selected.empty())
        ss << std::fixed << std::setprecision(3) << "Orient " << m_selected.back().name << " in " << time_elapsed.count() << " seconds. "
        << "Orientation: " << m_selected.back().orientation.transpose() << "; v,phi: " << m_selected.back().axis.transpose() << ", " << m_selected.back().angle << "; euler: " << m_selected.back().euler_angles.transpose();

    // finalize just here.
    ctl.update_status(100,
        ctl.was_canceled() ? _u8L("Orienting canceled.")
        : _u8L(ss.str().c_str()));
    wxGetApp().plater()->show_status_message(ctl.was_canceled() ? "Orienting canceled." : ss.str());
}

OrientJob::OrientJob() : m_plater{wxGetApp().plater()} {}

void OrientJob::finalize(bool canceled, std::exception_ptr &eptr)
{
    try {
        if (eptr)
            std::rethrow_exception(eptr);
        eptr = nullptr;
    } catch (...) {
        eptr = std::current_exception();
    }

    // Ignore the arrange result if aborted.
    if (canceled || eptr)
        return;

    for (OrientMesh& mesh : m_selected)
    {
        mesh.apply();
    }


    m_plater->update();

    // BBS
    //wxGetApp().obj_manipul()->set_dirty();
}

orientation::OrientMesh OrientJob::get_orient_mesh(ModelInstance* instance)
{
    using OrientMesh = orientation::OrientMesh;
    OrientMesh om;
    auto obj = instance->get_object();
    om.name = obj->name;
    // This instance alone: obj->mesh() would place and merge every instance of the object.
    om.mesh = orientation::instance_mesh(*instance);
    if (obj->config.has("support_threshold_angle"))
        om.overhang_angle = obj->config.opt_int("support_threshold_angle");
    else {
        const Slic3r::DynamicPrintConfig& config = wxGetApp().preset_bundle->full_config();
        om.overhang_angle = config.opt_int("support_threshold_angle");
    }

    om.setter = [instance](const OrientMesh& p) {
        instance->rotate(p.rotation_matrix);
        instance->get_object()->invalidate_bounding_box();
        instance->get_object()->ensure_on_bed();
    };
    return om;
}

}} // namespace Slic3r::GUI
