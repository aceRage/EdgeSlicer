#ifndef slic3r_GUI_StartupSoundDialog_hpp_
#define slic3r_GUI_StartupSoundDialog_hpp_

// "Startup sound" dialog (tests/design_startup_sounds.md): pick a MIDI file, see the M1006 lines
// it becomes and how long they play, and hand back the tune block for Machine start G-code
// (Startup sound) or Machine end G-code (Ending sound).
// The conversion is libslic3r/GCode/StartupSound.hpp, which has the unit tests.
//
// No Widgets/*.hpp here: Tab.cpp includes this header and Field.hpp's CheckBox would clash.

#include "GUI_Utils.hpp"

#include "libslic3r/GCode/StartupSound.hpp"

#include <cstdint>
#include <string>
#include <vector>

class wxSpinCtrl;
class wxSpinCtrlDouble;
class wxCheckBox;
class wxStaticText;
class wxTextCtrl;
class Button;

namespace Slic3r {
namespace GUI {

class StartupSoundDialog : public DPIDialog
{
public:
    // Which tune the dialog makes: the one in Machine start G-code or the one in Machine end G-code.
    enum class Target { Start, End };

    explicit StartupSoundDialog(wxWindow *parent, Target target = Target::Start);

    // The tune block (label line, M1006 S1 ... M1006 W) to put into the start or end G-code.
    const std::string &tune_block() const { return m_block; }

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    void choose_file();
    void update_preview();
    StartupSound::Options read_options() const;
    void load_options();
    void save_options() const;

    Target               m_target;
    std::vector<uint8_t> m_midi;
    std::string          m_file_name;
    std::string          m_block;

    wxStaticText     *m_file_label   = nullptr;
    wxSpinCtrlDouble *m_ms_per_unit  = nullptr;
    wxSpinCtrl       *m_quantise     = nullptr;
    wxSpinCtrl       *m_min_note     = nullptr;
    wxSpinCtrl       *m_max_note     = nullptr;
    wxSpinCtrl       *m_max_volume   = nullptr;
    wxSpinCtrlDouble *m_max_seconds  = nullptr;
    wxSpinCtrl       *m_max_steps    = nullptr;
    wxCheckBox       *m_unison       = nullptr;
    wxCheckBox       *m_skip_drums   = nullptr;
    wxStaticText     *m_summary      = nullptr;
    wxTextCtrl       *m_preview      = nullptr;
    Button           *m_ok           = nullptr;
};

} // namespace GUI
} // namespace Slic3r

#endif
