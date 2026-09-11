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
static const char *CFG_LAYOUT        = "layout";

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
    if (cfg->has(CFG_SECTION, CFG_LAYOUT))
        s.layout = cfg->get(CFG_SECTION, CFG_LAYOUT) == "grid" ? fill_bed::Layout::Grid
                                                               : fill_bed::Layout::Compact;

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
                             bool                   is_seq_print,
                             std::function<int(const FillBedSettings &)> grid_counter)
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
    m_grid_counter = std::move(grid_counter);

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

    // Every label here is a ::Label, not a bare wxStaticText. A wxStaticText reports the system
    // button face (#F0F0F0) as its background; StateColor maps that to #3F3F46, which is LIGHTER
    // than the dialog's own #2D2D31, so UpdateDlgDarkUI() painted a distinct grey band behind
    // each label in dark mode. ::Label copies the parent's background in its constructor, so it
    // maps to the same colour as the window. Same fix in CloneDialog.
    // ---- minimum gap between copies -----------------------------------------------------------
    auto gap_label = new ::Label(this, Label::Body_14, _L("Minimum gap between copies") + ":");
    m_gap_input    = make_input(m_settings.gap);
    m_gap_input->SetToolTip(_L("The clear distance left between neighbouring copies, measured "
                               "between their outlines. Sequential printing and tree supports may "
                               "raise it to keep the extruder and the brims clear."));
    m_gap_input->GetTextCtrl()->SetFocus();
    f_sizer->Add(gap_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_gap_input, 0, wxALIGN_CENTER_VERTICAL);

    // ---- allow rotation -----------------------------------------------------------------------
    auto rotate_label = new ::Label(this, Label::Body_14, _L("Allow rotation") + ":");
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

    // ---- layout ---------------------------------------------------------------------------
    const wxString layout_tip = _L("Compact nestles copies into whatever pocket the packer finds - "
                                   "denser for awkward outlines, but the result looks irregular and "
                                   "it is capped at a few hundred copies. Grid tiles the bed with the "
                                   "part's bounding box at the exact gap, tries both orientations when "
                                   "rotation is allowed, and skips the cells that hit an exclusion "
                                   "region, the wipe tower or an object already on the plate.");

    auto layout_label = new ::Label(this, Label::Body_14, _L("Layout") + ":");
    layout_label->SetToolTip(layout_tip);
    m_layout_combo = new ::ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, input_size, 0, nullptr, wxCB_READONLY);
    m_layout_combo->Append(_L("Compact"));
    m_layout_combo->Append(_L("Grid"));
    m_layout_combo->SetSelection(m_settings.layout == fill_bed::Layout::Grid ? 1 : 0);
    m_layout_combo->SetToolTip(layout_tip);
    m_layout_combo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &e) {
        e.Skip();
        update_estimate();
    });
    f_sizer->Add(layout_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_layout_combo, 0, wxALIGN_CENTER_VERTICAL);

    // ---- minimum distance from the bed edge ---------------------------------------------------
    auto edge_label = new ::Label(this, Label::Body_14, _L("Minimum distance from bed edge") + ":");
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

    auto front_label = new ::Label(this, Label::Body_14, _L("Keep the front edge clear") + ":");
    front_label->Wrap(FromDIP(300));
    front_label->SetToolTip(front_tip);
    m_front_cb = new ::CheckBox(this);
    m_front_cb->SetValue(m_settings.front_enabled);
    m_front_cb->SetToolTip(front_tip);
    f_sizer->Add(front_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_front_cb, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, FromDIP(5));

    auto front_mm_label = new ::Label(this, Label::Body_14, _L("Distance from the front edge") + ":");
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

    auto around_text = new ::Label(this, Label::Body_14, _L("Fills around existing objects on this plate"));
    around_text->Wrap(FromDIP(320));
    // The light-mode tone only. UpdateDlgDarkUI() at the end of the constructor maps it to
    // #818183 for dark mode; pre-mapping it here would hand that pass a colour it does not
    // know and leave the label at whatever its lightness sanity check produced.
    around_text->SetForegroundColour(wxColour("#6B6B6B"));
    info_sizer->Add(around_text, 0, wxALIGN_LEFT);

    m_estimate_text = new ::Label(this, Label::Head_14, wxEmptyString);
    info_sizer->Add(m_estimate_text, 0, wxALIGN_LEFT | wxTOP, FromDIP(6));

    m_warning_text = new ::Label(this, Label::Body_14, wxEmptyString);
    m_warning_text->Wrap(FromDIP(320));
    // #D9534F is not in StateColor's dark map, so it survives UpdateDlgDarkUI() unchanged -
    // which is what a warning red should do. Left explicit rather than mapped for the same
    // reason as the line above.
    m_warning_text->SetForegroundColour(wxColour("#D9534F"));
    info_sizer->Add(m_warning_text, 0, wxALIGN_LEFT | wxTOP, FromDIP(4));

    v_sizer->Add(info_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    // ---- buttons ------------------------------------------------------------------------------
    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});

    dlg_btns->GetOK()->SetLabel(_L("Fill"));
    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
        m_settings = current_settings();
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

// What the controls hold right now, clamped. The estimate and OK read the same thing, so the
// number on the label is the number the job is given.
FillBedSettings FillBedDialog::current_settings() const
{
    FillBedSettings s = m_settings;
    s.gap            = std::clamp(read_mm(m_gap_input, m_settings.gap), 0., MARGIN_MAX);
    s.allow_rotation = m_rotate_cb != nullptr ? m_rotate_cb->GetValue() : m_settings.allow_rotation;
    s.edge_margin    = std::clamp(read_mm(m_edge_input, m_settings.edge_margin), 0., MARGIN_MAX);
    s.front_enabled  = m_front_cb != nullptr ? m_front_cb->GetValue() : m_settings.front_enabled;
    s.front_margin   = std::clamp(read_mm(m_front_input, m_settings.front_margin), 0., MARGIN_MAX);
    s.layout         = (m_layout_combo != nullptr && m_layout_combo->GetSelection() == 1)
                           ? fill_bed::Layout::Grid
                           : fill_bed::Layout::Compact;
    return s;
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
    const FillBedSettings cur   = current_settings();
    const double          gap   = cur.gap;
    const double          edge  = cur.edge_margin;
    const double          front = cur.front_enabled ? std::max(edge, cur.front_margin) : edge;
    const bool            grid  = cur.layout == fill_bed::Layout::Grid;

    // The same rectangle the per-side shrink will hand the packer: the whole edge margin off
    // left/right/back, the effective front margin off the front. The bed area passed in has
    // already had arrange's own shrink applied, so this only subtracts what the dialog adds on
    // top of it - a rectangular approximation, which is what a live label wants to be.
    const double w = std::max(0., m_bed_w - 2. * edge);
    const double h = std::max(0., m_bed_h - edge - front);
    // Free area, honouring both the shrink and the objects already on the plate.
    const double free = std::max(0., std::min(m_bed_area, w * h) - m_occupied);

    // The gap the packer will really use, so the label does not promise copies the brim rule
    // will not allow.
    const double eff_gap = std::max(gap, m_brim_width);

    // Grid is deterministic, so the count is exact: the job builds the real grid against the
    // real bed outline and the real obstacles and just reports how many cells came back.
    // Compact has no such closed form, so it keeps the tiling estimate - rotation is not
    // modelled there either, and a rotated pack can only do better.
    const int tiled = fill_bed::estimate_count(m_template_w, m_template_h, eff_gap, free);
    int       n     = tiled;
    bool      exact = false;
    if (grid && m_grid_counter) {
        n     = m_grid_counter(cur);
        exact = true;
    }

    if (m_estimate_text != nullptr)
        m_estimate_text->SetLabel(exact ? wxString::Format(_L("Copies: %d"), n)
                                        : wxString::Format(_L("Estimated copies: %d"), n));

    if (m_warning_text != nullptr) {
        wxString warn;
        if (m_is_seq_print)
            warn = _L("Printing by object: the gap will be raised to the extruder clearance where "
                      "it is smaller.");
        else if (gap < m_brim_width)
            warn = wxString::Format(_L("The template's brim is %.1f mm wide, so the gap used will "
                                       "be at least that much."),
                                    m_brim_width);
        // Compact is O(n^2), so above the cap the fill switches to Grid by itself rather than
        // leaving most of the bed empty. Say so before the user presses Fill, not after.
        if (!grid && tiled > fill_bed::COUNT_CAP) {
            if (!warn.empty())
                warn += "\n";
            warn += wxString::Format(_L("Grid layout will be used above %d copies."), fill_bed::COUNT_CAP);
        }
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
    cfg->set(CFG_SECTION, CFG_LAYOUT, m_settings.layout == fill_bed::Layout::Grid ? "grid" : "compact");
}

void FillBedDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    Layout();
    if (GetSizer() != nullptr)
        GetSizer()->Fit(this);
    Refresh();
}

}} // namespace Slic3r::GUI
