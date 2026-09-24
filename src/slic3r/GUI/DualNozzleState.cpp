#include "DualNozzleState.hpp"

#include "GUI_App.hpp"
#include "PartPlate.hpp"
#include "DeviceManager.hpp"
#include "DualNozzleSliceDialog.hpp"
#include "GLToolbar.hpp"
#include "Event.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "NotificationManager.hpp"
#include "Plater.hpp"
#include "RemoteAccess.hpp"
#include "libslic3r/BambuExtruderMap.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <boost/log/trivial.hpp>

#include <cstdio>
#include <cstdlib>

namespace Slic3r { namespace GUI { namespace DualNozzle {

using namespace DualNozzleSync;

bool preset_is_dual_nozzle_bambu()
{
    PresetBundle *pb = wxGetApp().preset_bundle;
    if (!pb || !pb->is_bbl_vendor())
        return false;
    const auto *diameters = pb->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter");
    if (!diameters || diameters->values.size() != 2)
        return false;
    // Both keys support_different_extruders reads are printer options; it does not modify the config.
    int extruder_count = 0;
    return const_cast<DynamicPrintConfig &>(pb->printers.get_edited_preset().config).support_different_extruders(extruder_count) &&
           extruder_count == 2;
}

std::vector<int> preset_physical_extruder_map()
{
    if (PresetBundle *pb = wxGetApp().preset_bundle)
        if (const auto *pem = pb->printers.get_edited_preset().config.option<ConfigOptionInts>("physical_extruder_map"))
            return pem->values;
    return {};
}

std::vector<double> preset_nozzle_diameters()
{
    if (PresetBundle *pb = wxGetApp().preset_bundle)
        if (const auto *d = pb->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter"))
            return d->values;
    return {};
}

MachineObject *selected_machine()
{
    DeviceManager *dev = wxGetApp().getDeviceManager();
    MachineObject *obj = dev ? dev->get_selected_machine() : nullptr;
    return (obj && obj->is_multi_extruders()) ? obj : nullptr;
}

bool machine_matches_preset(MachineObject *obj)
{
    PresetBundle *pb = wxGetApp().preset_bundle;
    if (!obj || !pb)
        return false;
    return pb->printers.get_edited_preset().get_printer_type(pb) == obj->printer_type;
}

static std::string diameter_string(float d)
{
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.2f", d);
    std::string s(buf);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

DualNozzleSync::PrinterState printer_state(MachineObject *obj)
{
    DualNozzleSync::PrinterState s;
    s.physical_extruder_map = preset_physical_extruder_map();
    if (!obj)
        return s;
    s.dev_id       = obj->dev_id;
    s.dev_name     = obj->dev_name;
    s.printer_type = obj->printer_type;
    s.has_report   = obj->is_online() && obj->is_multi_extruders();
    if (!s.has_report)
        return s;

    for (const auto &kv : obj->amsList) {
        const Ams *a = kv.second;
        if (!a)
            continue;
        int ams_id = -1;
        try { ams_id = std::stoi(kv.first); } catch (...) { continue; }
        if (BambuExtruderMap::is_external_spool_ams_id(ams_id))
            continue;
        DualNozzleSync::AmsUnit unit;
        unit.ams_id            = ams_id;
        unit.physical_extruder = a->nozzle;
        // BambuStudio check_ams_status_impl: an N3S (AMS HT) unit counts as a one-slot unit.
        unit.slot_count = (a->type == 4 || BambuExtruderMap::is_ams_ht_id(ams_id)) ? 1 : 4;
        for (const auto &tv : a->trayList) {
            AmsTray *t = tv.second;
            if (!t || !t->is_exists || !t->is_tray_info_ready())
                continue;
            DualNozzleSync::Tray tray;
            tray.ams_id = ams_id;
            try { tray.slot_id = std::stoi(tv.first); } catch (...) { continue; }
            tray.color       = t->color;
            tray.type        = t->get_filament_type();
            tray.filament_id = t->setting_id;
            unit.trays.push_back(tray);
        }
        s.ams.push_back(unit);
    }

    for (const Nozzle &n : obj->m_nozzle_data.nozzles) {
        DualNozzleSync::PrinterNozzle pn;
        pn.pos      = n.on_rack() ? (0x10 + (n.id & 0xF)) : (n.id & 0xF);
        pn.diameter = diameter_string(n.diameter);
        pn.volume   = n.volume_exact;
        pn.normal   = n.is_normal();
        pn.wear     = n.wear;
        pn.fila_id  = n.fila_id;
        pn.color    = n.color_m;
        s.nozzles.push_back(pn);
    }
    return s;
}

size_t project_filament_count()
{
    PresetBundle *pb = wxGetApp().preset_bundle;
    return pb ? pb->filament_presets.size() : 0;
}

std::vector<int> project_filament_map()
{
    if (PresetBundle *pb = wxGetApp().preset_bundle)
        if (const auto *fm = pb->project_config.option<ConfigOptionInts>("filament_map"))
            return fm->values;
    return {};
}

std::vector<DualNozzleSync::ProjectFilament> plate_filaments(PartPlate *plate)
{
    std::vector<DualNozzleSync::ProjectFilament> out;
    PresetBundle *pb = wxGetApp().preset_bundle;
    if (!plate || !pb)
        return out;
    const auto *colours = pb->project_config.option<ConfigOptionStrings>("filament_colour");
    for (int ext : plate->get_extruders(false)) {
        const int idx = ext - 1;
        if (idx < 0 || size_t(idx) >= pb->filament_presets.size())
            continue;
        DualNozzleSync::ProjectFilament f;
        f.index = idx;
        if (colours && size_t(idx) < colours->values.size())
            f.color = colours->values[size_t(idx)];
        if (const Preset *p = pb->filaments.find_preset(pb->filament_presets[size_t(idx)])) {
            f.filament_id = p->filament_id;
            if (const auto *t = p->config.option<ConfigOptionStrings>("filament_type"); t && !t->values.empty())
                f.type = t->values.front();
        }
        out.push_back(f);
    }
    return out;
}

bool persist_synced_state(const DualNozzleSync::PrinterState &state)
{
    PresetBundle *pb = wxGetApp().preset_bundle;
    if (!pb || !state.has_report)
        return false;
    const std::vector<std::string> ams   = DualNozzleSync::extruder_ams_count_strings(state, 2);
    const std::vector<std::string> stats = DualNozzleSync::extruder_nozzle_stats_strings(state, 2, preset_nozzle_diameters());
    bool changed = false;
    auto *ams_opt = pb->project_config.option<ConfigOptionStrings>("extruder_ams_count", true);
    if (ams_opt && ams_opt->values != ams) {
        ams_opt->values = ams;
        changed = true;
    }
    // Nozzle stats only when the printer reports its nozzles; an empty list would tell Bambu
    // Studio "no nozzles" (see BambuExport OMIT_WHEN_EMPTY).
    bool any_nozzle = false;
    for (const auto &s : stats) any_nozzle |= !s.empty();
    if (any_nozzle) {
        auto *stats_opt = pb->project_config.option<ConfigOptionStrings>("extruder_nozzle_stats", true);
        if (stats_opt && stats_opt->values != stats) {
            stats_opt->values = stats;
            changed = true;
        }
    }
    if (changed) {
        std::string a, n;
        for (const auto &s : ams) a += "[" + s + "]";
        for (const auto &s : stats) n += "[" + s + "]";
        BOOST_LOG_TRIVIAL(warning) << "[DualNozzle] synced printer state into the project: extruder_ams_count=" << a
                                   << " extruder_nozzle_stats=" << n << " (" << state.dev_id << ")";
    }
    return changed;
}

std::string extruder_side_name(int logical_extruder) { return logical_extruder == 0 ? "Left" : "Right"; }

static std::string join_map(const std::vector<int> &m)
{
    std::string s;
    for (int v : m) s += std::to_string(v) + " ";
    return s;
}

static int s_requested_plate = -1;

void request_arrangement_dialog(int plate_index) { s_requested_plate = plate_index; }

void open_arrangement_and_reslice(Plater *plater, int plate_index)
{
    if (!plater)
        return;
    PartPlateList &list = plater->get_partplate_list();
    if (plate_index < 0 || plate_index >= list.get_plate_count())
        return;
    if (list.get_curr_plate_index() != plate_index)
        plater->select_plate(plate_index);
    request_arrangement_dialog(plate_index);
    plater->exit_gizmo();
    plater->update(true, true);
    wxPostEvent(plater, SimpleEvent(EVT_GLTOOLBAR_SLICE_PLATE));
}

bool confirm_before_slice(Plater *plater, bool slice_all)
{
    const int requested = s_requested_plate;
    s_requested_plate   = -1;
    if (!plater || !preset_is_dual_nozzle_bambu() || plater->only_gcode_mode())
        return true;
    PartPlateList &list = plater->get_partplate_list();

    std::vector<int> plates;
    if (slice_all) {
        for (int i = 0; i < list.get_plate_count(); ++i)
            if (PartPlate *pl = list.get_plate(i); pl && pl->has_printable_instances())
                plates.push_back(i);
    } else {
        plates.push_back(list.get_curr_plate_index());
    }

    MachineObject *obj      = selected_machine();
    const bool     mismatch = obj && !machine_matches_preset(obj);
    DualNozzleSync::PrinterState   state;
    if (obj && !mismatch)
        state = printer_state(obj);
    else
        state.physical_extruder_map = preset_physical_extruder_map();
    const bool interactive = RemoteAccess::dialog_mode() == RemoteAccess::Mode::Interactive;

    for (int idx : plates) {
        PartPlate *plate = list.get_plate(idx);
        if (!plate || !plate->has_printable_instances())
            continue;
        const std::vector<ProjectFilament> used = plate_filaments(plate);
        if (used.empty())
            continue;
        const Confirmation  stored = Confirmation::deserialize(plate->dual_nozzle_confirm());
        ConfirmReason reason = needs_confirmation(stored, state, used, plate->get_manual_filament_map());
        if (idx == requested)
            reason = ConfirmReason::UserRequested;
        if (reason == ConfirmReason::None) {
            // Sliced for the printer state the arrangement was confirmed against (unsynced: none).
            plate->set_dual_nozzle_sliced_for(stored.synced ? stored.dev_id : std::string(),
                                              stored.synced ? stored.state_fp : std::string());
            if (state.has_report)
                persist_synced_state(state);
            BOOST_LOG_TRIVIAL(warning) << "[DualNozzle] plate " << idx + 1 << ": confirmed arrangement still valid, map "
                                       << join_map(plate->get_manual_filament_map());
            continue;
        }
        if (!interactive) {
            // Nobody can answer a dialog (phone / background request). Slice with what the plate
            // has - its last confirmed map, else the automatic grouping - and say so in the log.
            BOOST_LOG_TRIVIAL(warning) << "[DualNozzle] plate " << idx + 1 << ": remote slice without confirming the arrangement (reason "
                                       << int(reason) << "); slicing with "
                                       << (plate->get_manual_filament_map().empty() ? "automatic grouping" : "the last confirmed map");
            plate->set_dual_nozzle_sliced_for(std::string(), std::string());
            continue;
        }

        DualNozzleSliceDialog dlg(wxGetApp().mainframe, plate, idx, reason);
        if (dlg.ShowModal() != wxID_OK) {
            BOOST_LOG_TRIVIAL(warning) << "[DualNozzle] plate " << idx + 1 << ": arrangement not confirmed, slice cancelled";
            return false;
        }
        const Arrangement  &arr     = dlg.arrangement();
        const DualNozzleSync::PrinterState &st      = dlg.printer_state();
        const std::vector<int> old_map = plate->get_manual_filament_map();
        plate->set_manual_filament_map(arr.filament_map);

        Confirmation c;
        c.synced       = st.has_report;
        c.dev_id       = st.has_report ? st.dev_id : std::string();
        c.state_fp     = state_fingerprint(st);
        c.filaments_fp = filaments_fingerprint(used);
        c.filament_map = arr.filament_map;
        c.trays        = arr.trays;
        plate->set_dual_nozzle_confirm(c.serialize());
        plate->set_dual_nozzle_sliced_for(c.dev_id, c.state_fp);
        if (st.has_report)
            persist_synced_state(st);
        if (old_map != arr.filament_map)
            plate->update_slice_result_valid_state(false);
        plater->set_plater_dirty(true);

        std::string trays;
        for (const auto &kv : arr.trays)
            trays += "F" + std::to_string(kv.first + 1) + "->" + BambuExtruderMap::tray_name(kv.second.ams_id, kv.second.slot_id) + " ";
        BOOST_LOG_TRIVIAL(warning) << "[DualNozzle] plate " << idx + 1 << " confirmed"
                                   << (c.synced ? " (synced with " + c.dev_id + ")" : std::string(" (not synced)"))
                                   << ": filament_map " << join_map(arr.filament_map) << "trays " << trays;
        if (!c.synced)
            if (auto *nm = plater->get_notification_manager())
                nm->push_notification(NotificationType::CustomNotification, NotificationManager::NotificationLevel::WarningNotificationLevel,
                                      _u8L("Slicing without printer data: the filament arrangement is not synced with a printer. "
                                           "It will be checked again when the printer is connected."));
        state = st; // the dialog may have switched printers
    }
    return true;
}

Watcher::Watcher(Plater *plater) : m_plater(plater)
{
    m_timer.SetOwner(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent &) { check(); }, m_timer.GetId());
    m_timer.Start(2000);
}

Watcher::~Watcher() { m_timer.Stop(); }

void Watcher::check()
{
    if (!m_plater || !preset_is_dual_nozzle_bambu())
        return;
    MachineObject *obj   = selected_machine();
    const bool     other = obj && !machine_matches_preset(obj);
    DualNozzleSync::PrinterState   state = (obj && !other) ? printer_state(obj) : DualNozzleSync::PrinterState();
    if (other) {
        // Another model is selected: nothing sliced for this profile is for it.
        state.dev_id     = obj->dev_id;
        state.has_report = obj->is_online();
    }
    const std::string key = state.dev_id + "|" + (state.has_report ? "1" : "0") + "|" + state_fingerprint(state);
    if (key == m_last_key)
        return;
    m_last_key = key;
    if (!state.has_report)
        return; // going offline never invalidates

    PartPlateList   &list = m_plater->get_partplate_list();
    std::vector<int> invalidated;
    for (int i = 0; i < list.get_plate_count(); ++i) {
        PartPlate  *plate = list.get_plate(i);
        std::string dev, fp;
        if (!plate || !plate->is_slice_result_valid() || !plate->dual_nozzle_sliced_for(dev, fp))
            continue;
        if (!other && !slice_invalidated_by(dev, fp, state))
            continue;
        plate->update_slice_result_valid_state(false);
        plate->clear_dual_nozzle_sliced_for();
        invalidated.push_back(i + 1);
    }
    if (invalidated.empty())
        return;
    std::string plates;
    for (int p : invalidated) plates += std::to_string(p) + " ";
    BOOST_LOG_TRIVIAL(warning) << "[DualNozzle] selected printer " << state.dev_id << " (or its AMS/nozzle state) differs from what plates "
                               << plates << "were sliced for; marked for re-slice";
    if (MainFrame *mf = wxGetApp().mainframe)
        mf->update_slice_print_status(MainFrame::eEventSliceUpdate, true, false);
    if (auto *nm = m_plater->get_notification_manager())
        nm->push_notification(NotificationType::CustomNotification, NotificationManager::NotificationLevel::WarningNotificationLevel,
                              _u8L("The selected printer, or its spools or nozzles, changed since the last slice. "
                                   "Slice again to confirm the filament arrangement."));
}

}}} // namespace Slic3r::GUI::DualNozzle
