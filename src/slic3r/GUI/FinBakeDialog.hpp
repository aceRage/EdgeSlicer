#ifndef slic3r_GUI_FinBakeDialog_hpp_
#define slic3r_GUI_FinBakeDialog_hpp_

// The dialog shown before "Bake fins..." (ObjectList::bake_fins): where the baked fins go and how wide
// their tines are, with a top-view preview of the fins the bake will build (planned from the current
// slice, as libslic3r/Support/FinBake.hpp does). The fin settings are shown, not edited: the bake uses
// those of the slice. Built like StabilizerBakeDialog - the app's Label, ComboBox and DialogButtons on a
// DPIDialog, dark mode through UpdateDlgDarkUI - and like it, it only collects the choice; the run is a
// background job (Jobs/FinBakeJob).
//
// Credit: the technique is Slant3D's designed-in supports, as specified by Support Fins
// (gittrahan/support-fins, printfins.com, MIT); EdgeSlicer's own implementation.

#include "GUI_Utils.hpp"

#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/Support/FinBake.hpp"

#include "Widgets/ComboBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"

#include <wx/panel.h>

namespace Slic3r { namespace GUI {

// What the preview draws, in the object's sliced frame (mm, scaled coordinates): the part seen from above,
// the fins and the tines.
struct FinBakePreviewData
{
    ExPolygons part;
    ExPolygons fins;
    Polylines  tines;
    size_t     fin_count  = 0;
    size_t     tine_count = 0;
    double     fin_top    = 0.;
};

class FinBakePreview : public wxPanel
{
public:
    FinBakePreview(wxWindow *parent, const FinBakePreviewData &data, const wxSize &size);

private:
    void on_paint(wxPaintEvent &);

    FinBakePreviewData m_data;
};

class FinBakeDialog : public DPIDialog
{
public:
    // `mode_label` names the object's fin mode (Auto / Painted). `sliced_gap` and `layer_height` are the
    // object's settings from the current slice; `support_filament` its support filament (0 = its own).
    // `by_object` warns that by-object printing prints a separate object apart from the part;
    // `part_allowed` is false when the object's copies differ in rotation or scale, which one shared part
    // cannot follow.
    FinBakeDialog(wxWindow                 *parent,
                  const wxString           &object_name,
                  const wxString           &mode_label,
                  const FinBakePreviewData &preview,
                  double                    sliced_gap,
                  double                    layer_height,
                  int                       support_filament,
                  bool                      by_object,
                  bool                      part_allowed);

    // Valid once ShowModal() returned wxID_OK.
    const FinBakeOptions &options() const { return m_options; }

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    FinBakePlacement placement() const;
    FinBakeTines     tines() const;
    void             update_choice();

    FinBakeOptions m_options;
    double         m_sliced_gap   = 0.;
    bool           m_part_allowed = true;
    bool           m_by_object    = false;
    bool           m_has_support_filament = false;

    ::ComboBox *m_placement_choice = nullptr;
    ::ComboBox *m_tines_choice     = nullptr;
    ::Label    *m_gap_value        = nullptr;
    ::Label    *m_gap_warning      = nullptr;
    ::Label    *m_separate_note    = nullptr;
    ::Label    *m_by_object_note   = nullptr;
    ::Label    *m_filament_note    = nullptr;
    ::Label    *m_one_line_note    = nullptr;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_FinBakeDialog_hpp_
