#ifndef slic3r_GUI_FillBedDialog_hpp_
#define slic3r_GUI_FillBedDialog_hpp_

#include "GUI_Utils.hpp"

#include "Widgets/CheckBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/TextInput.hpp"

#include <wx/stattext.h>

namespace Slic3r { namespace GUI {

class Plater;

// What "Fill bed with copies" is run with. Plater::fill_bed_with_instances() fills the defaults
// in from the current plate, shows the dialog, and hands the result to FillBedJob.
struct FillBedSettings
{
    // Minimum gap between copies, mm. Fed to ArrangeParams::min_obj_distance, so sequential
    // print and the brim rule can still raise it.
    double gap            = 0.;
    bool   allow_rotation = false;
    // Minimum distance from every bed edge, mm.
    double edge_margin    = 0.;
    bool   front_enabled  = false;
    // Minimum distance from the front (min-Y) bed edge, mm. Effective value is
    // max(front_margin, edge_margin).
    double front_margin   = 0.;
};

// The dialog shown before a bed fill. Both entry points - the object right-click menu item and
// the Clone dialog's Fill button - go through Plater::fill_bed_with_instances(), so both get it.
class FillBedDialog : public DPIDialog
{
public:
    // `defaults` carries the values a first run should show (today's behaviour); anything the
    // user has already chosen is read back from AppConfig and wins over it.
    // `bed_area` (mm^2) and the template's bbox drive the live "Estimated copies" label.
    FillBedDialog(wxWindow             *parent,
                  const FillBedSettings &defaults,
                  double                 template_w,
                  double                 template_h,
                  double                 bed_area,
                  double                 occupied_area,
                  double                 bed_w,
                  double                 bed_h,
                  double                 brim_width,
                  bool                   is_seq_print);

    // Valid after ShowModal() returns wxID_OK. Also written back to AppConfig at that point.
    const FillBedSettings &settings() const { return m_settings; }

    static FillBedSettings load_from_config(const FillBedSettings &defaults);

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    double read_mm(TextInput *input, double fallback) const;
    void   update_estimate();
    void   save_to_config() const;

    FillBedSettings m_settings;

    TextInput *m_gap_input        = nullptr;
    ::CheckBox *m_rotate_cb       = nullptr;
    TextInput *m_edge_input       = nullptr;
    ::CheckBox *m_front_cb        = nullptr;
    TextInput *m_front_input      = nullptr;
    wxStaticText *m_estimate_text = nullptr;
    wxStaticText *m_warning_text  = nullptr;

    // Geometry the estimate needs; all mm / mm^2.
    double m_template_w   = 0.;
    double m_template_h   = 0.;
    double m_bed_area     = 0.;
    double m_occupied     = 0.;
    double m_bed_w        = 0.;
    double m_bed_h        = 0.;
    double m_brim_width   = 0.;
    bool   m_is_seq_print = false;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_FillBedDialog_hpp_
