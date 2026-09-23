#ifndef slic3r_GUI_DualNozzleSliceDialog_hpp_
#define slic3r_GUI_DualNozzleSliceDialog_hpp_

#include "GUI_Utils.hpp"
#include "libslic3r/BambuDualNozzleSync.hpp"

#include <wx/timer.h>

#include <string>
#include <vector>

class Button;
class StaticBox;
class wxChoice;
class wxStaticText;
class wxBoxSizer;
class wxPanel;

namespace Slic3r { namespace GUI {

class PartPlate;

// Pre-slice confirmation for Bambu's two-extruder printers (H2D, H2D Pro, H2C, X2D).
//
// Shown by Plater::guard_before_slice_* when the plate's arrangement was never confirmed or
// something relevant changed (printer, its AMS / nozzle state, the plate's filaments). It shows
// the target printer (with a picker and Refresh, like the send dialog's printer row), what the
// printer reports per extruder (AMS units incl. AMS HT, nozzles incl. the H2C rack), and the
// filament -> extruder grouping as draggable chips in a Left and a Right column, each with the
// AMS tray it will print from. Auto pre-fills it; the user confirms.
//
// This is BambuStudio's "Filament grouping" dialog (FilamentMapDialog / FilamentMapManualPanel,
// custom mode) merged with its pre-slice "Sync now" prompt (Plater::priv::check_ams_status_impl):
// the extruder grouping only. The nozzle flow type (Standard / High Flow) stays where it is -
// the printer settings, and on Snapmaker printers the separate Custom Filament Grouping dialog -
// as Bambu Studio keeps extruder grouping and nozzle volume type apart.
class DualNozzleSliceDialog : public DPIDialog
{
public:
    DualNozzleSliceDialog(wxWindow *parent, PartPlate *plate, int plate_index, DualNozzleSync::ConfirmReason reason);
    ~DualNozzleSliceDialog() override;

    const DualNozzleSync::Arrangement  &arrangement() const { return m_arr; }
    const DualNozzleSync::PrinterState &printer_state() const { return m_state; }

    // Drop target / buttons: put filament (0-based) on logical extruder 0/1.
    void move_filament(int filament, int logical_extruder);

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    void build_ui();
    void fill_printer_choice();
    void read_printer_state(bool refill_if_untouched);
    void on_timer(wxTimerEvent &);
    void sync_now();
    void auto_fill();
    void swap_sides();
    void rebuild_columns();
    void update_status();
    void update_issues();
    int  nozzle_count(int logical_extruder) const;
    void pick_tray_for(int filament, int logical_extruder);

    PartPlate                                   *m_plate{ nullptr };
    int                                          m_plate_index{ 0 };
    DualNozzleSync::ConfirmReason                m_reason;
    std::vector<DualNozzleSync::ProjectFilament> m_used;
    size_t                                       m_filament_count{ 0 };
    std::vector<double>                          m_diameters;
    DualNozzleSync::PrinterState                 m_state;
    std::string                                  m_state_fp;
    DualNozzleSync::Arrangement                  m_arr;
    bool                                         m_user_touched{ false };
    bool                                         m_model_mismatch{ false };
    std::vector<std::string>                     m_choice_dev_ids;

    wxChoice     *m_printer_choice{ nullptr };
    Button       *m_refresh_btn{ nullptr };
    Button       *m_sync_btn{ nullptr };
    wxStaticText *m_status{ nullptr };
    wxStaticText *m_hint{ nullptr };
    wxStaticText *m_issues{ nullptr };
    StaticBox    *m_side_box[2]{ nullptr, nullptr };
    wxStaticText *m_side_title[2]{ nullptr, nullptr };
    wxBoxSizer   *m_side_rows[2]{ nullptr, nullptr };
    Button       *m_confirm_btn{ nullptr };
    wxTimer       m_timer;
};

}} // namespace Slic3r::GUI

#endif
