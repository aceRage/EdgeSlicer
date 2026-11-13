#include "StartupSoundDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"

#include "libslic3r/AppConfig.hpp"

#include "Widgets/Button.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"

#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>

#include <wx/checkbox.h>
#include <wx/filedlg.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <iterator>

namespace Slic3r {
namespace GUI {

namespace {

const char *SSD_SECTION = "startup_sound";
// A startup tune is a few seconds; a MIDI file bigger than this is not one.
constexpr std::streamsize SSD_MAX_MIDI_BYTES = 1024 * 1024;

} // namespace

StartupSoundDialog::StartupSoundDialog(wxWindow *parent)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe), wxID_ANY, _L("Startup sound"),
                wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER)
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);
    const int gap = FromDIP(8);

    auto *v_sizer = new wxBoxSizer(wxVERTICAL);

    auto *intro = new wxStaticText(this, wxID_ANY,
        _L("Turn a MIDI file into the tune this printer plays at the start of every print (M1006 lines). "
           "It replaces the tune between \"M1006 S1\" and \"M1006 W\" in Machine start G-code. "
           "The printer waits for the tune to finish, so a longer tune makes every print start later."));
    intro->Wrap(FromDIP(520));
    v_sizer->Add(intro, 0, wxALL, gap);

    auto *file_row = new wxBoxSizer(wxHORIZONTAL);
    auto *choose   = new Button(this, _L("Choose MIDI file") + dots);
    choose->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    choose->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { choose_file(); });
    file_row->Add(choose, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
    m_file_label = new wxStaticText(this, wxID_ANY, _L("No file chosen"));
    file_row->Add(m_file_label, 1, wxALIGN_CENTER_VERTICAL);
    v_sizer->Add(file_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, gap);

    auto *grid = new wxFlexGridSizer(4, gap / 2, gap);
    auto  add_int = [this, grid](const wxString &label, const wxString &tip, int lo, int hi, int value) {
        auto *t = new wxStaticText(this, wxID_ANY, label);
        t->SetToolTip(tip);
        grid->Add(t, 0, wxALIGN_CENTER_VERTICAL);
        auto *s = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(90), -1), wxSP_ARROW_KEYS, lo, hi, value);
        s->SetToolTip(tip);
        s->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent &) { update_preview(); });
        s->Bind(wxEVT_TEXT, [this](wxCommandEvent &) { update_preview(); });
        grid->Add(s, 0);
        return s;
    };
    auto add_double = [this, grid](const wxString &label, const wxString &tip, double lo, double hi, double value, double inc) {
        auto *t = new wxStaticText(this, wxID_ANY, label);
        t->SetToolTip(tip);
        grid->Add(t, 0, wxALIGN_CENTER_VERTICAL);
        auto *s = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(90), -1), wxSP_ARROW_KEYS, lo, hi, value, inc);
        s->SetDigits(1);
        s->SetToolTip(tip);
        s->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent &) { update_preview(); });
        s->Bind(wxEVT_TEXT, [this](wxCommandEvent &) { update_preview(); });
        grid->Add(s, 0);
        return s;
    };
    const StartupSound::Options def;
    m_ms_per_unit = add_double(_L("Milliseconds per unit"),
        _L("How long one unit of the M1006 duration (B/D/F) lasts. Bambu does not document it; "
           "10 ms was measured on an H3C. Change it only if your printer plays the tune too fast or too slow."), 1., 200., def.ms_per_unit, 0.5);
    m_quantise = add_int(_L("Snap notes to (ms)"),
        _L("Note starts and ends snap to this grid. Larger values give fewer, longer steps."), 10, 1000, int(def.quantise_ms));
    m_min_note = add_int(_L("Lowest note"),
        _L("MIDI note number. Lower notes move up by octaves. The stock Bambu tunes use 37 to 61; 60 is middle C."), 1, 127, def.min_note);
    m_max_note = add_int(_L("Highest note"), _L("MIDI note number. Higher notes move down by octaves."), 1, 127, def.max_note);
    m_max_volume = add_int(_L("Volume"), _L("The volume of the loudest note (0 to 100). Softer notes in the file play softer."), 1, 100, def.max_volume);
    m_max_seconds = add_double(_L("Longest tune (s)"), _L("The tune stops here, even if the file goes on."), 0.5, 60., def.max_seconds, 0.5);
    m_max_steps = add_int(_L("Most lines"), _L("The tune stops after this many M1006 steps."), 1, 1000, def.max_steps);
    grid->AddSpacer(0);
    grid->AddSpacer(0);
    v_sizer->Add(grid, 0, wxLEFT | wxRIGHT | wxBOTTOM, gap);

    m_unison = new wxCheckBox(this, wxID_ANY, _L("Play the melody on the free voices"));
    m_unison->SetToolTip(_L("The printer has three voices. For a tune that never has two notes at once, repeat the "
                            "note on the other voices, as the stock Bambu tunes do (louder). A tune with a bass line keeps the third voice silent."));
    m_unison->SetValue(def.unison_fill);
    m_unison->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { update_preview(); });
    v_sizer->Add(m_unison, 0, wxLEFT | wxRIGHT | wxBOTTOM, gap);
    m_skip_drums = new wxCheckBox(this, wxID_ANY, _L("Leave out drums (MIDI channel 10)"));
    m_skip_drums->SetValue(def.skip_percussion);
    m_skip_drums->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { update_preview(); });
    v_sizer->Add(m_skip_drums, 0, wxLEFT | wxRIGHT | wxBOTTOM, gap);

    m_summary = new wxStaticText(this, wxID_ANY, wxEmptyString);
    v_sizer->Add(m_summary, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, gap);

    m_preview = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(520), FromDIP(220)),
                               wxTE_MULTILINE | wxTE_READONLY | wxHSCROLL);
    m_preview->SetFont(wxFont(wxFontInfo(9).Family(wxFONTFAMILY_TELETYPE)));
    v_sizer->Add(m_preview, 1, wxEXPAND | wxLEFT | wxRIGHT, gap);

    auto *dlg_btns = new DialogButtons(this, {"OK", "Cancel"});
    m_ok           = dlg_btns->GetOK();
    m_ok->SetLabel(_L("Insert"));
    m_ok->SetToolTip(_L("Replace the tune in Machine start G-code with this one"));
    m_ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        if (m_block.empty())
            return;
        save_options();
        EndModal(wxID_OK);
    });
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
    v_sizer->Add(dlg_btns, 0, wxEXPAND | wxTOP, gap);

    load_options();
    update_preview();

    SetSizerAndFit(v_sizer);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

void StartupSoundDialog::on_dpi_changed(const wxRect &)
{
    Fit();
    Refresh();
}

void StartupSoundDialog::choose_file()
{
    wxFileDialog dlg(this, _L("Choose a MIDI file"), wxEmptyString, wxEmptyString,
                     _L("MIDI files") + " (*.mid;*.midi)|*.mid;*.midi;*.MID;*.MIDI", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK)
        return;
    const std::string path = into_u8(dlg.GetPath());
    boost::nowide::ifstream f(path.c_str(), std::ios::binary);
    std::vector<uint8_t> bytes;
    if (f) {
        f.seekg(0, std::ios::end);
        const std::streamoff size = f.tellg();
        if (size > 0 && size <= SSD_MAX_MIDI_BYTES) {
            f.seekg(0, std::ios::beg);
            bytes.resize(size_t(size));
            f.read(reinterpret_cast<char *>(bytes.data()), std::streamsize(size));
            if (!f)
                bytes.clear();
        }
    }
    m_midi      = std::move(bytes);
    m_file_name = boost::filesystem::path(path).stem().string();
    m_file_label->SetLabel(from_u8(boost::filesystem::path(path).filename().string()));
    update_preview();
}

StartupSound::Options StartupSoundDialog::read_options() const
{
    StartupSound::Options o;
    o.ms_per_unit     = m_ms_per_unit->GetValue();
    o.quantise_ms     = m_quantise->GetValue();
    o.min_note        = std::min(m_min_note->GetValue(), m_max_note->GetValue());
    o.max_note        = std::max(m_min_note->GetValue(), m_max_note->GetValue());
    o.max_volume      = m_max_volume->GetValue();
    o.max_seconds     = m_max_seconds->GetValue();
    o.max_steps       = m_max_steps->GetValue();
    o.unison_fill     = m_unison->GetValue();
    o.skip_percussion = m_skip_drums->GetValue();
    return o;
}

void StartupSoundDialog::load_options()
{
    AppConfig *cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return;
    auto num = [cfg](const char *key, double fallback) {
        const std::string v = cfg->get(SSD_SECTION, key);
        try { return v.empty() ? fallback : std::stod(v); } catch (...) { return fallback; }
    };
    m_ms_per_unit->SetValue(num("ms_per_unit", m_ms_per_unit->GetValue()));
    m_quantise->SetValue(int(num("quantise_ms", m_quantise->GetValue())));
    m_min_note->SetValue(int(num("min_note", m_min_note->GetValue())));
    m_max_note->SetValue(int(num("max_note", m_max_note->GetValue())));
    m_max_volume->SetValue(int(num("max_volume", m_max_volume->GetValue())));
    m_max_seconds->SetValue(num("max_seconds", m_max_seconds->GetValue()));
    m_max_steps->SetValue(int(num("max_steps", m_max_steps->GetValue())));
    const std::string unison = cfg->get(SSD_SECTION, "unison_fill");
    if (!unison.empty())
        m_unison->SetValue(unison == "1");
    const std::string drums = cfg->get(SSD_SECTION, "skip_percussion");
    if (!drums.empty())
        m_skip_drums->SetValue(drums == "1");
}

void StartupSoundDialog::save_options() const
{
    AppConfig *cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return;
    const StartupSound::Options o = read_options();
    cfg->set(SSD_SECTION, "ms_per_unit", std::to_string(o.ms_per_unit));
    cfg->set(SSD_SECTION, "quantise_ms", std::to_string(int(o.quantise_ms)));
    cfg->set(SSD_SECTION, "min_note", std::to_string(o.min_note));
    cfg->set(SSD_SECTION, "max_note", std::to_string(o.max_note));
    cfg->set(SSD_SECTION, "max_volume", std::to_string(o.max_volume));
    cfg->set(SSD_SECTION, "max_seconds", std::to_string(o.max_seconds));
    cfg->set(SSD_SECTION, "max_steps", std::to_string(o.max_steps));
    cfg->set(SSD_SECTION, "unison_fill", o.unison_fill ? "1" : "0");
    cfg->set(SSD_SECTION, "skip_percussion", o.skip_percussion ? "1" : "0");
}

void StartupSoundDialog::update_preview()
{
    m_block.clear();
    if (m_summary == nullptr || m_preview == nullptr)
        return; // still building the dialog
    wxString summary;
    if (m_midi.empty()) {
        summary = m_file_name.empty() ? _L("Choose a MIDI file (format 0 or 1, up to 1 MB).")
                                      : _L("The file could not be read, or it is larger than 1 MB.");
    } else {
        const StartupSound::Options opt = read_options();
        const StartupSound::Result  r   = StartupSound::midi_to_steps(m_midi, opt);
        if (!r.error.empty()) {
            summary = from_u8(r.error);
        } else {
            m_block = StartupSound::steps_to_gcode(r.steps, m_file_name);
            summary = wxString::Format(_L("%d lines, %.1f s (at %.1f ms per unit)."), int(r.steps.size()), r.duration_s, opt.ms_per_unit);
            if (r.notes_dropped > 0)
                summary += " " + wxString::Format(_L("%d of %d notes left out (more than three at once, or past the limit)."),
                                                  r.notes_dropped, r.notes_in);
            if (r.notes_folded > 0)
                summary += " " + wxString::Format(_L("%d notes moved by octaves into range."), r.notes_folded);
            if (r.percussion_skipped > 0)
                summary += " " + wxString::Format(_L("%d drum notes left out."), r.percussion_skipped);
            if (r.truncated)
                summary += " " + _L("The tune was cut at the length limit.");
        }
    }
    m_summary->SetLabel(summary);
    m_summary->Wrap(FromDIP(520));
    m_preview->ChangeValue(from_u8(m_block));
    if (m_ok != nullptr)
        m_ok->Enable(!m_block.empty());
    Layout();
}

} // namespace GUI
} // namespace Slic3r
