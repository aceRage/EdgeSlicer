#pragma once

// GUI glue for Bambu's two-extruder printers (H2D, H2D Pro, H2C, X2D): turns the live
// MachineObject and the project into the plain data DualNozzleSync works on, and keeps the
// synced printer state in the project config.

#include "libslic3r/BambuDualNozzleSync.hpp"

#include <wx/event.h>
#include <wx/timer.h>

#include <string>
#include <vector>

namespace Slic3r {
class MachineObject;
namespace GUI {
class PartPlate;
class Plater;
namespace DualNozzle {

// The edited printer preset is a Bambu (BBL vendor) printer with two extruders of distinct
// variants - the printers that group filaments per extruder. Derived from the profile, not a
// model list.
bool preset_is_dual_nozzle_bambu();

std::vector<int>    preset_physical_extruder_map();
std::vector<double> preset_nozzle_diameters();

// The machine selected in the Device tab when it is a two-extruder printer, else nullptr.
MachineObject *selected_machine();
// Whether obj is the model the edited preset is for (BambuStudio get_machine_sync_status).
bool machine_matches_preset(MachineObject *obj);

// Snapshot of obj (nullptr or offline: has_report = false). Only loaded, identified trays;
// external spool holders are left out.
DualNozzleSync::PrinterState printer_state(MachineObject *obj);

// Filaments printed on the plate, with the project's colours and filament types.
std::vector<DualNozzleSync::ProjectFilament> plate_filaments(PartPlate *plate);
size_t           project_filament_count();
std::vector<int> project_filament_map(); // project_config filament_map (1-based)

// BambuStudio Sidebar::sync_extruder_list for the parts this fork keeps: writes
// extruder_ams_count and extruder_nozzle_stats into the project config (so slice, 3MF and
// Export Bambu 3MF carry them). Returns true when a value changed.
bool persist_synced_state(const DualNozzleSync::PrinterState &state);

// "Left" / "Right" for logical extruder 0 / 1.
std::string extruder_side_name(int logical_extruder);

// The pre-slice gate (BambuStudio Plater::priv::check_ams_status_impl, Plater.cpp:17049-17140,
// reworked into a confirmation): for each plate about to be sliced whose arrangement was never
// confirmed or whose printer / printer state / filaments changed since, shows
// DualNozzleSliceDialog and stores the result on the plate (manual filament_map, confirmation
// record) and the synced printer state in the project. Returns false when the user cancels.
// No-op (true) for every printer that is not a Bambu two-extruder printer. A remote (phone) slice
// never waits for a dialog: it slices with the stored or automatic arrangement and logs it.
bool confirm_before_slice(Plater *plater, bool slice_all);
// Makes the next confirm_before_slice show the dialog for this plate even when its arrangement
// is still valid (the Slice menu's "Filament arrangement..." entry; BambuStudio opens its
// grouping dialog from a plate icon instead).
void request_arrangement_dialog(int plate_index);

// Watches the selected printer; when it changes, or its AMS / nozzle state changes materially,
// every plate sliced for a different printer or state is marked dirty so Slice re-enables and
// the confirmation runs again.
class Watcher : public wxEvtHandler
{
public:
    explicit Watcher(Plater *plater);
    ~Watcher() override;
    // One check, also used by the timer.
    void check();

private:
    Plater     *m_plater;
    wxTimer     m_timer;
    std::string m_last_key;
};

}}} // namespace Slic3r::GUI::DualNozzle
