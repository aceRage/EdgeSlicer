#ifndef slic3r_GUI_SettingsViewSelector_hpp_
#define slic3r_GUI_SettingsViewSelector_hpp_

#include <wx/panel.h>

class ComboBox;

namespace Slic3r {
namespace GUI {

class SegmentedToggle;

// The Basic | Advanced | Bambu selector that replaced the "Advance" switch in the sidebar header and in
// the filament / printer tab header (tests/design_view_modes.md, D1). A three-segment pill when the
// header row has room, a compact dropdown when it does not. It stays enabled in Developer mode, which is
// a flag on top of the view (owner, 2026-11-25). Choosing writes AppConfig through GUI_App::save_view_choice().
class SettingsViewSelector : public wxPanel
{
public:
    explicit SettingsViewSelector(wxWindow* parent);

    // Shows the current choice (GUI_App::get_view_choice()); call after every update_mode().
    void update_from_app();
    void msw_rescale();
    void sys_color_changed();

private:
    void choose(int index);
    void on_parent_size(wxSizeEvent& evt);
    void update_layout();
    void apply_visibility();

    SegmentedToggle* m_toggle    = nullptr;
    ComboBox*        m_combo     = nullptr;
    bool             m_narrow    = false;
    bool             m_layout_pending = false;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_SettingsViewSelector_hpp_
