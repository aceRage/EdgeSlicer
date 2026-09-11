#include "FillBedDialog.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/FillBedPack.hpp"

#include <wx/sizer.h>
#include <wx/valtext.h>

#include <algorithm>
#include <cmath>

namespace Slic3r { namespace GUI {

static const char *CFG_SECTION       = "fill_bed";
static const char *CFG_MIN_GAP       = "min_gap";
static const char *CFG_ALLOW_ROT     = "allow_rotation";
static const char *CFG_EDGE_MARGIN   = "edge_margin";
static const char *CFG_FRONT_ENABLED = "front_margin_enabled";
static const char *CFG_FRONT_MARGIN  = "front_margin";

static const double MARGIN_MAX = 50.;

static wxString mm_str(double v) { return wxString::Format("%.2f", v); }

FillBedSettings FillBedDialog::load_from_config(const FillBedSettings &defaults)
{
    FillBedSettings s   = defaults;
    AppConfig      *cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return s;

    auto read_double = [cfg](const char *key, double &out) {
        if (!cfg->has(CFG_SECTION, key))
            return;
        try {
            out = std::stod(cfg->get(CFG_SECTION, key));
        } catch (...) {
            // A hand-edited or truncated config must not stop the fill; keep the default.
        }
    };

    read_double(CFG_MIN_GAP, s.gap);
    read_double(CFG_EDGE_MARGIN, s.edge_margin);
    read_double(CFG_FRONT_MARGIN, s.front_margin);
    if (cfg->has(CFG_SECTION, CFG_ALLOW_ROT))
        s.allow_rotation = cfg->get(CFG_SECTION, CFG_ALLOW_ROT) == "true";
    if (cfg->has(CFG_SECTION, CFG_FRONT_ENABLED))
        s.front_enabled = cfg->get(CFG_SECTION, CFG_FRONT_ENABLED) == "true";

    s.gap          = std::clamp(s.gap, 0., MARGIN_MAX);
    s.edge_margin  = std::clamp(s.edge_margin, 0., MARGIN_MAX);
    s.front_margin = std::clamp(s.front_margin, 0., MARGIN_MAX);
    return s;
}

FillBedDialog::FillBedDialog(wxWindow              *parent,
                             const FillBedSettings &defaults,
                             double                 template_w,
                             double                 template_h,
                             double                 bed_area,
                             double                 occupied_area,
                             double                 bed_w,
                             double                 bed_h,
                             double                 brim_width,
                             bool                   is_seq_print)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe),
                wxID_ANY,
                _L("Fill bed with copies"),
                wxDefaultPosition,
                wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX)
    , m_template_w(template_w)
    , m_template_h(template_h)
    , m_bed_area(bed_area)
    , m_occupied(occupied_area)
    , m_bed_w(bed_w)
    , m_bed_h(bed_h)
    , m_brim_width(brim_width)
    , m_is_seq_print(is_seq_print)
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
            update_estimate();
        });
        return in;
    };

    // ---- minimum gap between copies -----------------------------------------------------------
    auto gap_label = new wxStaticText(this, wxID_ANY, _L("Minimum gap between copies") + ":", wxDefaultPosition, wxDefaultSize, 0);
    m_gap_input    = make_input(m_settings.gap);
    m_gap_input->SetToolTip(_L("The clear distance left between neighbouring copies, measured "
                               "between their outlines. Sequential printing and tree supports may "
                               "raise it to keep the extruder and the brims clear."));
    m_gap_input->GetTextCtrl()->SetFocus();
    f_sizer->Add(gap_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_gap_input, 0, wxALIGN_CENTER_VERTICAL);

    // ---- allow rotation -----------------------------------------------------------------------
    auto rotate_label = new wxStaticText(this, wxID_ANY, _L("Allow rotation") + ":", wxDefaultPosition, wxDefaultSize, 0);
    rotate_label->Wrap(FromDIP(300));
    m_rotate_cb = new ::CheckBox(this);
    m_rotate_cb->SetValue(m_settings.allow_rotation);
    m_rotate_cb->SetToolTip(_L("Let copies be turned to pack more of them in. Off keeps every copy "
                               "in the template's own orientation."));
    m_rotate_cb->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &e) {
        e.Skip();
        update_estimate();
    });
    f_sizer->Add(rotate_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_rotate_cb, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, FromDIP(5));

    // ---- minimum distance from the bed edge ---------------------------------------------------
    auto edge_label = new wxStaticText(this, wxID_ANY, _L("Minimum distance from bed edge") + ":", wxDefaultPosition, wxDefaultSize, 0);
    edge_label->Wrap(FromDIP(300));
    m_edge_input = make_input(m_settings.edge_margin);
    m_edge_input->SetToolTip(_L("Keep every copy at least this far from all four bed edges."));
    f_sizer->Add(edge_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_edge_input, 0, wxALIGN_CENTER_VERTICAL);

    // ---- front edge override ------------------------------------------------------------------
    const wxString front_tip = _L("Printers run first-layer calibration along the front of the bed "
                                  "- flow dynamics purge lines, extrusion calibration patches, a "
                                  "purge strip - and a copy placed there collides with those "
                                  "extrusions even though the bed shape says the area is printable. "
                                  "Tick this and set how much of the front strip to keep clear. It "
                                  "never brings a copy closer to the front than the general edge "
                                  "distance above.");

    auto front_label = new wxStaticText(this, wxID_ANY, _L("Keep the front edge clear") + ":", wxDefaultPosition, wxDefaultSize, 0);
    front_label->Wrap(FromDIP(300));
    front_label->SetToolTip(front_tip);
    m_front_cb = new ::CheckBox(this);
    m_front_cb->SetValue(m_settings.front_enabled);
    m_front_cb->SetToolTip(front_tip);
    f_sizer->Add(front_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_front_cb, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, FromDIP(5));

    auto front_mm_label = new wxStaticText(this, wxID_ANY, _L("Distance from the front edge") + ":", wxDefaultPosition, wxDefaultSize, 0);
    front_mm_label->Wrap(FromDIP(300));
    front_mm_label->SetToolTip(front_tip);
    m_front_input = make_input(m_settings.front_margin);
    m_front_input->SetToolTip(front_tip);
    m_front_input->Enable(m_settings.front_enabled);
    f_sizer->Add(front_mm_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_front_input, 0, wxALIGN_CENTER_VERTICAL);

    m_front_cb->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &e) {
        e.Skip();
        m_front_input->Enable(m_front_cb->GetValue());
        update_estimate();
    });

    v_sizer->Add(f_sizer, 0, wxEXPAND | wxALL, FromDIP(10));

    // ---- info and the live estimate -----------------------------------------------------------
    auto info_sizer = new wxBoxSizer(wxVERTICAL);

    auto around_text = new wxStaticText(this, wxID_ANY, _L("Fills around existing objects on this plate"), wxDefaultPosition, wxDefaultSize, 0);
    around_text->Wrap(FromDIP(320));
    around_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#6B6B6B")));
    info_sizer->Add(around_text, 0, wxALIGN_LEFT);

    m_estimate_text = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0);
    m_estimate_text->SetFont(Label::Head_14);
    info_sizer->Add(m_estimate_text, 0, wxALIGN_LEFT | wxTOP, FromDIP(6));

    m_warning_text = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0);
    m_warning_text->Wrap(FromDIP(320));
    m_warning_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#D9534F")));
    info_sizer->Add(m_warning_text, 0, wxALIGN_LEFT | wxTOP, FromDIP(4));

    v_sizer->Add(info_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    // ---- buttons ------------------------------------------------------------------------------
    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});

    dlg_btns->GetOK()->SetLabel(_L("Fill"));
    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
        m_settings.gap            = std::clamp(read_mm(m_gap_input, m_settings.gap), 0., MARGIN_MAX);
        m_settings.allow_rotation = m_rotate_cb->GetValue();
        m_settings.edge_margin    = std::clamp(read_mm(m_edge_input, m_settings.edge_margin), 0., MARGIN_MAX);
        m_settings.front_enabled  = m_front_cb->GetValue();
        m_settings.front_margin   = std::clamp(read_mm(m_front_input, m_settings.front_margin), 0., MARGIN_MAX);
        save_to_config();
        EndModal(wxID_OK);
    });

    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) { EndModal(wxID_CANCEL); });

    v_sizer->Add(dlg_btns, 0, wxEXPAND);

    update_estimate();

    this->SetSizer(v_sizer);
    this->Layout();
    v_sizer->Fit(this);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

double FillBedDialog::read_mm(TextInput *input, double fallback) const
{
    if (input == nullptr)
        return fallback;
    double   v   = 0.;
    wxString txt = input->GetTextCtrl()->GetValue();
    if (txt.empty() || !txt.ToDouble(&v))
        return fallback;
    return v;
}

void FillBedDialog::update_estimate()
{
    const double gap   = std::clamp(read_mm(m_gap_input, m_settings.gap), 0., MARGIN_MAX);
    const double edge  = std::clamp(read_mm(m_edge_input, m_settings.edge_margin), 0., MARGIN_MAX);
    const bool   fr_on = m_front_cb != nullptr && m_front_cb->GetValue();
    const double front = fr_on ? std::max(edge, std::clamp(read_mm(m_front_input, m_settings.front_margin), 0., MARGIN_MAX)) : edge;

    // The same rectangle the per-side shrink will hand the packer: the whole edge margin off
    // left/right/back, the effective front margin off the front. The bed area passed in has
    // already had arrange's own shrink applied, so this only subtracts what the dialog adds on
    // top of it - a rectangular approximation, which is what a live label wants to be.
    const double w = std::max(0., m_bed_w - 2. * edge);
    const double h = std::max(0., m_bed_h - edge - front);
    // Free area, honouring both the shrink and the objects already on the plate.
    const double free = std::max(0., std::min(m_bed_area, w * h) - m_occupied);

    // The gap the packer will really use, so the label does not promise copies the brim rule
    // will not allow. Rotation is not modelled - the estimate is the un-rotated tiling, and a
    // rotated pack can only do better.
    const double eff_gap = std::max(gap, m_brim_width);
    const int    n       = fill_bed::estimate_count(m_template_w, m_template_h, eff_gap, free);

    if (m_estimate_text != nullptr)
        m_estimate_text->SetLabel(wxString::Format(_L("Estimated copies: %d"), n));

    if (m_warning_text != nullptr) {
        wxString warn;
        if (m_is_seq_print)
            warn = _L("Printing by object: the gap will be raised to the extruder clearance where "
                      "it is smaller.");
        else if (gap < m_brim_width)
            warn = wxString::Format(_L("The template's brim is %.1f mm wide, so the gap used will "
                                       "be at least that much."),
                                    m_brim_width);
        m_warning_text->SetLabel(warn);
        m_warning_text->Wrap(FromDIP(320));
    }

    Layout();
    if (GetSizer() != nullptr)
        GetSizer()->Fit(this);
}

void FillBedDialog::save_to_config() const
{
    AppConfig *cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return;
    cfg->set(CFG_SECTION, CFG_MIN_GAP, mm_str(m_settings.gap).ToStdString());
    cfg->set(CFG_SECTION, CFG_ALLOW_ROT, m_settings.allow_rotation);
    cfg->set(CFG_SECTION, CFG_EDGE_MARGIN, mm_str(m_settings.edge_margin).ToStdString());
    cfg->set(CFG_SECTION, CFG_FRONT_ENABLED, m_settings.front_enabled);
    cfg->set(CFG_SECTION, CFG_FRONT_MARGIN, mm_str(m_settings.front_margin).ToStdString());
}

void FillBedDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    Layout();
    if (GetSizer() != nullptr)
        GetSizer()->Fit(this);
    Refresh();
}

}} // namespace Slic3r::GUI
