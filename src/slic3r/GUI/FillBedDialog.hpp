#ifndef slic3r_GUI_FillBedDialog_hpp_
#define slic3r_GUI_FillBedDialog_hpp_

#include "GUI_Utils.hpp"

#include "libslic3r/FillBedPack.hpp"

#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/TextInput.hpp"

#include <functional>

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
    // Compact packs with the NFP packer (today's path); Grid tiles the template's bounding box.
    fill_bed::Layout layout = fill_bed::Layout::Compact;
    // Drop the support-clearance floor on the gap entirely.
    //
    // ArrangePolygon::brim_width is NOT a brim width despite the name: ModelArrange.cpp sets it
    // to 1 mm flat, 6 mm when the object has normal support, 24 mm for tree support, and never
    // looks at brim_type or the brim_width setting at all. It is the arrange clearance. The fill
    // path raises the gap to it so neighbouring supports cannot collide - correct by default,
    // but there is no way to say "I know, pack them tighter anyway". This is that way.
    bool ignore_support_clearance = false;
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
                  // The arrange clearance the fill will floor the gap at (ArrangePolygon::
                  // brim_width - see the note on ignore_support_clearance; it is a support
                  // clearance, not a brim).
                  double                 clearance,
                  // The template's real brim, from brim_type/brim_width, so the warning can say
                  // "brim" only when a brim is genuinely what raises the gap. 0 = no brim.
                  double                 brim_width,
                  bool                   is_seq_print,
                  // Returns the EXACT number of copies the Grid layout would place for the
                  // given gap, edge margin, front margin and rotation flag - the job owns the
                  // bed outline and the obstacles, so it hands the dialog a closure rather than
                  // the geometry. Null falls back to the tiling estimate.
                  std::function<int(const FillBedSettings &)> grid_counter = nullptr);

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
    TextInput  *m_front_input     = nullptr;
    ::ComboBox *m_layout_combo    = nullptr;
    ::CheckBox *m_ignore_clear_cb = nullptr;
    ::Label    *m_estimate_text   = nullptr;
    ::Label    *m_warning_text    = nullptr;

    std::function<int(const FillBedSettings &)> m_grid_counter;

    // The values the inputs currently hold, clamped - what the estimate and OK both read.
    FillBedSettings current_settings() const;

    // Geometry the estimate needs; all mm / mm^2.
    double m_template_w   = 0.;
    double m_template_h   = 0.;
    double m_bed_area     = 0.;
    double m_occupied     = 0.;
    double m_bed_w        = 0.;
    double m_bed_h        = 0.;
    double m_clearance    = 0.;
    double m_brim_width   = 0.;
    bool   m_is_seq_print = false;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_FillBedDialog_hpp_
