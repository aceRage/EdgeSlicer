#include "ScaleToVolumeDialog.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"

#include "libslic3r/AppConfig.hpp"

#include <wx/sizer.h>
#include <wx/valtext.h>

#include <algorithm>
#include <cmath>

namespace Slic3r { namespace GUI {

static const char *CFG_SECTION     = "scale_to_volume";
static const char *CFG_MODE        = "mode";
static const char *CFG_EDGE_GAP    = "edge_gap";
static const char *CFG_TOP_GAP     = "top_gap";
static const char *CFG_AUTO_CENTER = "auto_center";

// Same ceiling as FillBedDialog's margins: a gap bigger than this is almost certainly a typo, and
// the build volumes this ships on are far too small for it to be a real request.
static const double GAP_MAX = 50.;

static wxString mm_str(double v) { return wxString::Format("%.2f", v); }

ScaleToVolumeSettings ScaleToVolumeDialog::load_from_config(const ScaleToVolumeSettings &defaults)
{
    ScaleToVolumeSettings s   = defaults;
    AppConfig            *cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return s;

    auto read_double = [cfg](const char *key, double &out) {
        if (!cfg->has(CFG_SECTION, key))
            return;
        try {
            out = std::stod(cfg->get(CFG_SECTION, key));
        } catch (...) {
            // A hand-edited or truncated config must not stop the scale; keep the default.
        }
    };

    read_double(CFG_EDGE_GAP, s.edge_gap_mm);
    read_double(CFG_TOP_GAP, s.top_gap_mm);
    if (cfg->has(CFG_SECTION, CFG_MODE))
        s.mode = cfg->get(CFG_SECTION, CFG_MODE) == "nonuniform" ? ScaleToVolumeMode::NonUniform
                                                                 : ScaleToVolumeMode::Uniform;
    if (cfg->has(CFG_SECTION, CFG_AUTO_CENTER))
        s.auto_center = cfg->get(CFG_SECTION, CFG_AUTO_CENTER) == "true";

    s.edge_gap_mm = std::clamp(s.edge_gap_mm, 0., GAP_MAX);
    s.top_gap_mm  = std::clamp(s.top_gap_mm, 0., GAP_MAX);
    return s;
}

ScaleToVolumeDialog::ScaleToVolumeDialog(wxWindow                    *parent,
                                         const ScaleToVolumeSettings &defaults,
                                         const Vec3d                 &volume_size,
                                         bool                         is_circular,
                                         bool                         selection_rotated)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe),
                wxID_ANY,
                _L("Scale to build volume"),
                wxDefaultPosition,
                wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX)
    , m_volume_size(volume_size)
    , m_is_circular(is_circular)
    , m_selection_rotated(selection_rotated)
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);

    m_settings = load_from_config(defaults);

    auto v_sizer = new wxBoxSizer(wxVERTICAL);
    auto f_sizer = new wxFlexGridSizer(2, 2, FromDIP(4), FromDIP(20));

    const wxSize input_size(FromDIP(120), -1);

    auto make_input = [this, input_size](double value) {
        auto *in = new ::TextInput(this, mm_str(value), _L("mm"), wxEmptyString, wxDefaultPosition, input_size, wxTE_PROCESS_ENTER);
        in->GetTextCtrl()->SetFont(Label::Body_14);
        in->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
        in->GetTextCtrl()->Bind(wxEVT_TEXT, [this](wxCommandEvent &e) {
            e.Skip();
            update_warning();
        });
        return in;
    };

    // Every label here is a ::Label, not a bare wxStaticText - a wxStaticText reports the system
    // button face as its background and UpdateDlgDarkUI() then paints a visible grey band behind
    // it in dark mode. Same reasoning as FillBedDialog.
    // ---- scale mode ---------------------------------------------------------------------------
    const wxString mode_tip = _L("Uniform keeps the object's proportions: it is scaled by the "
                                 "single largest factor that still fits every axis inside the "
                                 "volume. Non-uniform scales each axis on its own so the object's "
                                 "bounding box fills the volume exactly - the proportions change, "
                                 "and a part that is rotated off the bed axes is sheared rather "
                                 "than rotated to fit, because the scale is applied in the bed's "
                                 "own frame.");

    auto mode_label = new ::Label(this, Label::Body_14, _L("Scale mode") + ":");
    mode_label->SetToolTip(mode_tip);
    m_mode_combo = new ::ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, input_size, 0, nullptr, wxCB_READONLY);
    m_mode_combo->Append(_L("Uniform"));
    m_mode_combo->Append(_L("Non-uniform"));
    m_mode_combo->SetSelection(m_settings.mode == ScaleToVolumeMode::NonUniform ? 1 : 0);
    m_mode_combo->SetToolTip(mode_tip);
    m_mode_combo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &e) {
        e.Skip();
        update_warning();
    });
    f_sizer->Add(mode_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_mode_combo, 0, wxALIGN_CENTER_VERTICAL);

    // ---- edge gap -----------------------------------------------------------------------------
    auto edge_label = new ::Label(this, Label::Body_14, _L("Gap from bed edges") + ":");
    edge_label->Wrap(FromDIP(300));
    m_edge_input = make_input(m_settings.edge_gap_mm);
    m_edge_input->SetToolTip(_L("Keep the scaled object at least this far from every bed edge. "
                                "Taken off both sides of X and Y, so the usable width shrinks by "
                                "twice this value."));
    m_edge_input->GetTextCtrl()->SetFocus();
    f_sizer->Add(edge_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_edge_input, 0, wxALIGN_CENTER_VERTICAL);

    // ---- top gap ------------------------------------------------------------------------------
    auto top_label = new ::Label(this, Label::Body_14, _L("Gap below maximum height") + ":");
    top_label->Wrap(FromDIP(300));
    m_top_input = make_input(m_settings.top_gap_mm);
    m_top_input->SetToolTip(_L("Keep the top of the scaled object at least this far below the "
                               "printer's maximum height. The object always sits on the bed, so "
                               "this is taken off the top only."));
    f_sizer->Add(top_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_top_input, 0, wxALIGN_CENTER_VERTICAL);

    // ---- auto-center --------------------------------------------------------------------------
    const wxString center_tip = _L("On, the scaled object is moved to the middle of the bed. Off, "
                                   "it keeps where it is and is only nudged the minimum needed to "
                                   "bring it back inside the bed edges. Either way it is dropped "
                                   "onto the bed.");

    auto center_label = new ::Label(this, Label::Body_14, _L("Centre on the bed") + ":");
    center_label->Wrap(FromDIP(300));
    center_label->SetToolTip(center_tip);
    m_center_cb = new ::CheckBox(this);
    m_center_cb->SetValue(m_settings.auto_center);
    m_center_cb->SetToolTip(center_tip);
    m_center_cb->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &e) {
        e.Skip();
        update_warning();
    });
    f_sizer->Add(center_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_center_cb, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, FromDIP(5));

    v_sizer->Add(f_sizer, 0, wxEXPAND | wxALL, FromDIP(10));

    // ---- warning ------------------------------------------------------------------------------
    m_warning_text = new ::Label(this, Label::Body_14, wxEmptyString);
    m_warning_text->Wrap(FromDIP(320));
    // #D9534F is not in StateColor's dark map, so it survives UpdateDlgDarkUI() unchanged - which
    // is what a warning red should do.
    m_warning_text->SetForegroundColour(wxColour("#D9534F"));
    v_sizer->Add(m_warning_text, 0, wxALIGN_LEFT | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    // ---- buttons ------------------------------------------------------------------------------
    m_dlg_btns = new DialogButtons(this, {"OK", "Cancel"});

    m_dlg_btns->GetOK()->SetLabel(_L("Scale"));
    m_dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
        m_settings = current_settings();
        save_to_config();
        EndModal(wxID_OK);
    });

    m_dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) { EndModal(wxID_CANCEL); });

    v_sizer->Add(m_dlg_btns, 0, wxEXPAND);

    update_warning();

    this->SetSizer(v_sizer);
    this->Layout();
    v_sizer->Fit(this);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

ScaleToVolumeSettings ScaleToVolumeDialog::current_settings() const
{
    ScaleToVolumeSettings s = m_settings;
    s.mode        = (m_mode_combo != nullptr && m_mode_combo->GetSelection() == 1)
                        ? ScaleToVolumeMode::NonUniform
                        : ScaleToVolumeMode::Uniform;
    s.edge_gap_mm = std::clamp(read_mm(m_edge_input, m_settings.edge_gap_mm), 0., GAP_MAX);
    s.top_gap_mm  = std::clamp(read_mm(m_top_input, m_settings.top_gap_mm), 0., GAP_MAX);
    s.auto_center = m_center_cb != nullptr ? m_center_cb->GetValue() : m_settings.auto_center;
    return s;
}

double ScaleToVolumeDialog::read_mm(TextInput *input, double fallback) const
{
    if (input == nullptr)
        return fallback;
    double   v   = 0.;
    wxString txt = input->GetTextCtrl()->GetValue();
    if (txt.empty() || !txt.ToDouble(&v))
        return fallback;
    return v;
}

void ScaleToVolumeDialog::update_warning()
{
    const ScaleToVolumeSettings cur = current_settings();

    wxString warn;
    bool     ok_enabled = true;

    // The gaps can eat a whole axis. fit()'s own `factor <= 0` guard would then silently do
    // nothing, which reads as a bug rather than as a rejected input - so say it and block OK.
    if (scale_to_volume::target_degenerate(m_volume_size, cur.edge_gap_mm, cur.top_gap_mm)) {
        warn       = _L("The gaps leave no room to scale into. Reduce them.");
        ok_enabled = false;
    } else if (cur.mode == ScaleToVolumeMode::NonUniform && m_selection_rotated) {
        // A non-uniform scale is applied in the bed's frame, composed OUTSIDE any existing
        // instance rotation - and a diagonal scale does not commute with a rotation, so a part
        // that is not square to the bed comes out sheared. That is the right trade-off for
        // "fill the volume", but the user should hear it before pressing Scale.
        warn = _L("The selection is rotated; non-uniform scaling will shear it. Uniform keeps "
                  "its shape.");
    } else if (cur.mode == ScaleToVolumeMode::NonUniform && m_is_circular) {
        // A circle has no per-axis extent to fill, so the non-uniform target is the largest
        // axis-aligned square that fits inside it.
        warn = _L("This bed is round, so non-uniform scaling fills the largest square that fits "
                  "inside it.");
    }

    if (m_warning_text != nullptr) {
        m_warning_text->SetLabel(warn);
        m_warning_text->Wrap(FromDIP(320));
    }
    if (m_dlg_btns != nullptr && m_dlg_btns->GetOK() != nullptr)
        m_dlg_btns->GetOK()->Enable(ok_enabled);

    Layout();
    if (GetSizer() != nullptr)
        GetSizer()->Fit(this);
}

void ScaleToVolumeDialog::save_to_config() const
{
    AppConfig *cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return;
    cfg->set(CFG_SECTION, CFG_MODE,
             m_settings.mode == ScaleToVolumeMode::NonUniform ? "nonuniform" : "uniform");
    cfg->set(CFG_SECTION, CFG_EDGE_GAP, mm_str(m_settings.edge_gap_mm).ToStdString());
    cfg->set(CFG_SECTION, CFG_TOP_GAP, mm_str(m_settings.top_gap_mm).ToStdString());
    cfg->set(CFG_SECTION, CFG_AUTO_CENTER, m_settings.auto_center);
}

void ScaleToVolumeDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    Layout();
    if (GetSizer() != nullptr)
        GetSizer()->Fit(this);
    Refresh();
}

}} // namespace Slic3r::GUI
