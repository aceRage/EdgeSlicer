#ifndef slic3r_GUI_ScaleToVolumeDialog_hpp_
#define slic3r_GUI_ScaleToVolumeDialog_hpp_

#include "GUI_Utils.hpp"

#include "libslic3r/ScaleToVolume.hpp"

#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/TextInput.hpp"

#include <wx/stattext.h>

namespace Slic3r { namespace GUI {

// What "Scale to build volume" is run with. The settings struct itself lives in libslic3r so that
// Selection and the unit tests can use it without pulling in wx.
using ScaleToVolumeSettings = scale_to_volume::Settings;
using ScaleToVolumeMode     = scale_to_volume::Mode;

// The dialog shown before a scale-to-fit. Plater::priv::scale_selection_to_fit_print_volume()
// opens it, and hands the result to Selection::scale_to_fit_print_volume().
class ScaleToVolumeDialog : public DPIDialog
{
public:
    // `volume_size` is the build volume's own (W,D,H) in mm, used to warn when the gaps leave a
    // non-positive target on some axis. `is_circular` disables Non-uniform's per-axis promise
    // wording (the fit then uses the bed circle's inscribed square). `selection_rotated` is true
    // when at least one selected instance has a rotation that is not a multiple of 90 degrees, in
    // which case a world-frame non-uniform scale shears it - the warning says so, live.
    ScaleToVolumeDialog(wxWindow                    *parent,
                        const ScaleToVolumeSettings &defaults,
                        const Vec3d                 &volume_size,
                        bool                         is_circular,
                        bool                         selection_rotated);

    // Valid after ShowModal() returns wxID_OK. Also written back to AppConfig at that point.
    const ScaleToVolumeSettings &settings() const { return m_settings; }

    static ScaleToVolumeSettings load_from_config(const ScaleToVolumeSettings &defaults);

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    double read_mm(TextInput *input, double fallback) const;
    void   update_warning();
    void   save_to_config() const;

    // The values the controls currently hold, clamped - what the warning and OK both read.
    ScaleToVolumeSettings current_settings() const;

    ScaleToVolumeSettings m_settings;

    ::ComboBox *m_mode_combo    = nullptr;
    TextInput  *m_edge_input    = nullptr;
    TextInput  *m_top_input     = nullptr;
    ::CheckBox *m_center_cb     = nullptr;
    ::Label    *m_warning_text  = nullptr;

    DialogButtons *m_dlg_btns = nullptr;

    Vec3d m_volume_size      = Vec3d::Zero();
    bool  m_is_circular      = false;
    bool  m_selection_rotated = false;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_ScaleToVolumeDialog_hpp_
