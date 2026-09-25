#pragma once

// "AMS Dryness Control": status, humidity, temperature and remaining time of one heated AMS unit
// (AMS 2 Pro / AMS HT), and Start / Stop of a timed drying run. The rules and payloads live in
// AmsDrying.hpp; this is only the window. Modelled on Bambu Studio's AMSDryCtrWin, without its
// artwork (the drying animation frames are not in our resources) and without its separate guide
// and progress pages: the rotate-spool option sits on the main page and Start sends directly.

#include "GUI_Utils.hpp"
#include "AmsDrying.hpp"
#include "wxExtensions.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

class wxStaticBitmap;
class wxTextCtrl;
class Button;
class ComboBox;
class CheckBox;
class Label;

namespace Slic3r {
class MachineObject;
namespace GUI {

class AMSDryCtrlDialog : public DPIDialog
{
public:
    explicit AMSDryCtrlDialog(wxWindow *parent);

    void               set_ams_id(const std::string &ams_id);
    const std::string &ams_id() const { return m_ams_id; }

    // Refresh from the printer's last report. Closes itself when the unit is gone or no longer
    // supports remote drying.
    void update(MachineObject *obj);

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    std::string    m_ams_id;
    MachineObject *m_obj{nullptr};
    int            m_unit_type{0};
    bool           m_printing{false};
    int            m_recommended{0};
    bool           m_any_inserted{false};
    bool           m_was_idle{true};
    bool           m_list_needs_default{true};
    std::optional<std::chrono::steady_clock::time_point> m_command_hold; // Start/Stop just sent

    // left
    wxStaticBitmap *m_image{nullptr};
    ScalableBitmap  m_image_bmp;
    std::string     m_image_name;
    wxStaticBitmap *m_status_icon{nullptr};
    ScalableBitmap  m_status_icon_bmp;
    Label          *m_status{nullptr};
    Label          *m_humidity{nullptr};
    Label          *m_temperature{nullptr};
    Label          *m_remaining{nullptr};
    wxWindow       *m_remaining_box{nullptr};

    // right
    Label      *m_title{nullptr};
    ComboBox   *m_filament{nullptr};
    wxTextCtrl *m_temp_input{nullptr};
    wxTextCtrl *m_hours_input{nullptr};
    CheckBox   *m_rotate{nullptr};
    Label      *m_rotate_label{nullptr};
    Label      *m_notes{nullptr};
    Label      *m_cannot{nullptr};
    Button     *m_start{nullptr};
    Button     *m_stop{nullptr};

    std::vector<std::string> m_types;

    void create();
    void set_image(const std::string &name, int px);
    void apply_preset_for_selection();
    void revalidate();
    void on_start();
    void on_stop();
    int  selected_index() const;
    bool read_inputs(long &temp, long &hours) const;
};

} // namespace GUI
} // namespace Slic3r
