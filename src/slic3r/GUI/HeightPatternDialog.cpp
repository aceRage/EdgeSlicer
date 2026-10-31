#include "HeightPatternDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Model.hpp"

#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/TextInput.hpp"
#include "Widgets/Button.hpp"

#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>
#include <wx/sizer.h>
#include <wx/valtext.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <sstream>

namespace Slic3r { namespace GUI {

namespace {

const char *HCP_SECTION = "height_colour_pattern";

wxString hcp_num(double v) { return wxString::Format("%.2f", v); }

wxColour hcp_parse_colour(const std::string &s)
{
    if (s.size() >= 7 && s[0] == '#') {
        wxColour c(wxString::FromUTF8(s.substr(0, 7)));
        if (c.IsOk())
            return c;
    }
    return wxColour(128, 128, 128);
}

wxColour hcp_text_on(const wxColour &bg)
{
    const double lum = (0.299 * bg.Red() + 0.587 * bg.Green() + 0.114 * bg.Blue()) / 255.;
    return lum > 0.6 ? wxColour(20, 20, 20) : wxColour(250, 250, 250);
}

wxColour hcp_border_colour() { return wxGetApp().dark_mode() ? wxColour(120, 120, 128) : wxColour(170, 170, 170); }
wxColour hcp_panel_colour() { return wxGetApp().dark_mode() ? wxColour(45, 45, 49) : wxColour(255, 255, 255); }
wxColour hcp_muted_text() { return wxGetApp().dark_mode() ? wxColour(170, 170, 175) : wxColour(107, 107, 107); }

// Dialog width and content width in DIP. Nothing may make the dialog wider than this.
constexpr int HCP_W       = 540;
constexpr int HCP_INNER_W = 516;
constexpr int HCP_BAND_ROWS_VISIBLE = 6;

wxString hcp_help_text()
{
    return _L("A band prints with its filament for every part of the object inside it, replacing the part's own filament. "
              "Painted faces keep their paint.");
}

// Set a label that wraps at `width` pixels and is as tall as its wrapped lines (at least
// `min_lines` lines). Label::Wrap alone leaves the one-line best size, which cuts the text off.
void hcp_set_wrapped(::Label *l, const wxString &text, int width, int min_lines)
{
    l->SetLabel(text);
    l->Wrap(width);
    l->InvalidateBestSize();
    const int min_h = min_lines > 0 ? min_lines * l->GetCharHeight() : 0;
    l->SetMinSize(wxSize(width, std::max(l->GetBestSize().y, min_h)));
}

wxBitmap hcp_swatch_bitmap(const wxColour &c, int w, int h)
{
    wxBitmap bmp(w, h);
    wxMemoryDC dc(bmp);
    dc.SetBackground(wxBrush(hcp_panel_colour()));
    dc.Clear();
    dc.SetBrush(wxBrush(c));
    dc.SetPen(wxPen(hcp_border_colour()));
    dc.DrawRoundedRectangle(0, 0, w, h, 3);
    dc.SelectObject(wxNullBitmap);
    return bmp;
}

} // namespace

// A row of filament chips. The palette (every filament of the project) and the sequence (the
// chosen ones, in order) are both this; the owner decides what a click means.
class HeightPatternSwatches : public wxPanel
{
public:
    struct Chip { int filament; wxColour colour; };

    HeightPatternSwatches(wxWindow *parent, int max_width_dip, const wxString &empty_hint)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxFULL_REPAINT_ON_RESIZE)
        , m_max_width(max_width_dip)
        , m_hint(empty_hint)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &HeightPatternSwatches::on_paint, this);
        Bind(wxEVT_LEFT_UP, &HeightPatternSwatches::on_click, this);
        Bind(wxEVT_MOTION, &HeightPatternSwatches::on_motion, this);
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent &) { set_hover(-1); });
        update_size();
    }

    void set_chips(std::vector<Chip> chips) { m_chips = std::move(chips); m_hover = -1; update_size(); Refresh(); }
    void set_on_click(std::function<void(int)> cb) { m_on_click = std::move(cb); }
    void set_tooltip_prefix(const wxString &s) { m_tip = s; }

    void rescale() { update_size(); Refresh(); }

private:
    int chip_size() const { return FromDIP(26); }
    int gap() const { return FromDIP(5); }
    int cols() const { return std::max(1, (FromDIP(m_max_width) + gap()) / (chip_size() + gap())); }

    void update_size()
    {
        const int n    = std::max<int>(1, int(m_chips.size()));
        const int c    = cols();
        const int rows = (n + c - 1) / c;
        const int w    = std::min<int>(n, c) * (chip_size() + gap());
        const int h    = rows * (chip_size() + gap());
        const wxSize s(m_chips.empty() ? FromDIP(m_max_width) : w, h);
        SetMinSize(s);
        SetSize(s);
        InvalidateBestSize();
    }

    int index_at(const wxPoint &p) const
    {
        const int step = chip_size() + gap();
        const int c = cols();
        const int col = p.x / step, row = p.y / step;
        if (p.x < 0 || p.y < 0 || col >= c)
            return -1;
        if ((p.x % step) >= chip_size() || (p.y % step) >= chip_size())
            return -1;
        const int idx = row * c + col;
        return idx >= 0 && idx < int(m_chips.size()) ? idx : -1;
    }

    void set_hover(int i)
    {
        if (i == m_hover)
            return;
        m_hover = i;
        SetCursor(i >= 0 ? wxCursor(wxCURSOR_HAND) : wxNullCursor);
        if (i >= 0)
            SetToolTip(m_tip + wxString::Format(" %d", m_chips[size_t(i)].filament));
        else
            UnsetToolTip();
        Refresh();
    }

    void on_motion(wxMouseEvent &e) { set_hover(index_at(e.GetPosition())); e.Skip(); }

    void on_click(wxMouseEvent &e)
    {
        const int i = index_at(e.GetPosition());
        if (i >= 0 && m_on_click)
            m_on_click(i);
    }

    void on_paint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC pdc(this);
        wxGCDC dc(pdc);
        dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        dc.Clear();
        dc.SetFont(Label::Body_12);
        if (m_chips.empty()) {
            dc.SetTextForeground(hcp_muted_text());
            dc.DrawText(m_hint, 0, (GetClientSize().y - dc.GetCharHeight()) / 2);
            return;
        }
        const int step = chip_size() + gap();
        const int c = cols();
        for (size_t i = 0; i < m_chips.size(); ++i) {
            const int x = int(i % size_t(c)) * step, y = int(i / size_t(c)) * step;
            dc.SetBrush(wxBrush(m_chips[i].colour));
            dc.SetPen(wxPen(int(i) == m_hover ? wxColour(0, 150, 136) : hcp_border_colour(), int(i) == m_hover ? 2 : 1));
            dc.DrawRoundedRectangle(x, y, chip_size(), chip_size(), FromDIP(4));
            const wxString txt = wxString::Format("%d", m_chips[i].filament);
            dc.SetTextForeground(hcp_text_on(m_chips[i].colour));
            const wxSize ts = dc.GetTextExtent(txt);
            dc.DrawText(txt, x + (chip_size() - ts.x) / 2, y + (chip_size() - ts.y) / 2);
        }
    }

    std::vector<Chip>          m_chips;
    std::function<void(int)>   m_on_click;
    int                        m_max_width;
    int                        m_hover = -1;
    wxString                   m_hint;
    wxString                   m_tip;
};

// The object drawn to scale, bottom to top, with the bands (and any gap filament) in colour.
class HeightPatternStrip : public wxPanel
{
public:
    explicit HeightPatternStrip(wxWindow *parent) : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxFULL_REPAINT_ON_RESIZE)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(FromDIP(420), FromDIP(40)));
        Bind(wxEVT_PAINT, &HeightPatternStrip::on_paint, this);
    }

    void set_data(std::vector<HeightPatternRange> ranges, double top, std::vector<wxColour> colours)
    {
        m_ranges = std::move(ranges);
        m_top = top;
        m_colours = std::move(colours);
        Refresh();
    }
    void clear() { m_ranges.clear(); Refresh(); }

private:
    void on_paint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC pdc(this);
        wxGCDC dc(pdc);
        dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        dc.Clear();
        const wxSize sz = GetClientSize();
        const int bar_h = FromDIP(16);
        const wxRect bar(0, 0, sz.x - 1, bar_h);
        dc.SetBrush(wxBrush(wxGetApp().dark_mode() ? wxColour(70, 70, 76) : wxColour(232, 232, 232)));
        dc.SetPen(wxPen(hcp_border_colour()));
        dc.DrawRectangle(bar);
        if (m_top > 0.) {
            for (const HeightPatternRange &r : m_ranges) {
                const int x0 = bar.x + int(std::lround(r.lo / m_top * bar.width));
                const int x1 = bar.x + int(std::lround(r.hi / m_top * bar.width));
                const size_t fi = size_t(std::max(1, r.extruder)) - 1;
                dc.SetBrush(wxBrush(fi < m_colours.size() ? m_colours[fi] : wxColour(128, 128, 128)));
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.DrawRectangle(x0, bar.y + 1, std::max(1, x1 - x0), bar.height - 2);
            }
        }
        dc.SetFont(Label::Body_12);
        dc.SetTextForeground(hcp_muted_text());
        dc.DrawText("0", 0, bar_h + FromDIP(3));
        const wxString top_txt = wxString::Format("%s %s", hcp_num(m_top), _L("mm"));
        dc.DrawText(top_txt, sz.x - dc.GetTextExtent(top_txt).x, bar_h + FromDIP(3));
    }

    std::vector<HeightPatternRange> m_ranges;
    double                          m_top = 0.;
    std::vector<wxColour>           m_colours;
};

// One row per band: number, filament swatch and number, from-to Z, and a mark on the last band when
// the top of the object cut it short. A fixed size with its own scroll bar, so 1 band and 50 bands
// take the same room in the dialog.
class HeightPatternBandList : public wxScrolledWindow
{
public:
    HeightPatternBandList(wxWindow *parent, int width_dip)
        : wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxBORDER_SIMPLE)
        , m_width_dip(width_dip)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        rescale();
    }

    void set_bands(std::vector<HeightPatternRange> bands, std::vector<wxColour> colours, bool last_cut_short)
    {
        m_bands = std::move(bands);
        m_colours = std::move(colours);
        m_cut_short = last_cut_short;
        update_virtual_size();
        Scroll(0, 0);
        Refresh();
    }
    void clear() { m_bands.clear(); update_virtual_size(); Refresh(); }

    void rescale()
    {
        const wxSize s(FromDIP(m_width_dip), row_h() * HCP_BAND_ROWS_VISIBLE + 2);
        SetMinSize(s);
        SetMaxSize(s);
        SetSize(s);
        SetScrollRate(0, row_h());
        update_virtual_size();
        Refresh();
    }

    void OnDraw(wxDC &dc) override
    {
        const wxSize client = GetClientSize();
        const int    rh     = row_h();
        const bool   dark   = wxGetApp().dark_mode();
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(hcp_panel_colour()));
        dc.DrawRectangle(0, 0, client.x, std::max(client.y, int(m_bands.size()) * rh) + rh);
        dc.SetFont(Label::Body_14);
        const wxColour text = dark ? wxColour(235, 235, 238) : wxColour(30, 30, 30);
        const int sw_w = FromDIP(18), sw_h = FromDIP(13);
        for (size_t i = 0; i < m_bands.size(); ++i) {
            const int y = int(i) * rh;
            if (i % 2 == 1) {
                dc.SetBrush(wxBrush(dark ? wxColour(55, 55, 60) : wxColour(244, 244, 246)));
                dc.DrawRectangle(0, y, client.x, rh);
            }
            const HeightPatternRange &b = m_bands[i];
            const size_t fi = size_t(std::max(1, b.extruder)) - 1;
            const int ty = y + (rh - dc.GetCharHeight()) / 2;
            dc.SetTextForeground(hcp_muted_text());
            dc.DrawText(wxString::Format("%d", int(i) + 1), FromDIP(8), ty);
            dc.SetBrush(wxBrush(fi < m_colours.size() ? m_colours[fi] : wxColour(128, 128, 128)));
            dc.SetPen(wxPen(hcp_border_colour()));
            dc.DrawRoundedRectangle(FromDIP(48), y + (rh - sw_h) / 2, sw_w, sw_h, FromDIP(3));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetTextForeground(text);
            dc.DrawText(wxString::Format("%d", b.extruder), FromDIP(48) + sw_w + FromDIP(6), ty);
            dc.DrawText(wxString::Format("%s - %s %s", hcp_num(b.lo), hcp_num(b.hi), _L("mm")), FromDIP(120), ty);
            if (m_cut_short && i + 1 == m_bands.size()) {
                dc.SetTextForeground(wxColour("#D9534F"));
                dc.DrawText(_L("cut short by the top"), FromDIP(290), ty);
            }
        }
    }

private:
    int  row_h() const { return FromDIP(24); }
    void update_virtual_size() { SetVirtualSize(GetClientSize().x, std::max<int>(1, int(m_bands.size())) * row_h()); }

    int                             m_width_dip;
    std::vector<HeightPatternRange> m_bands;
    std::vector<wxColour>           m_colours;
    bool                            m_cut_short = false;
};

HeightPatternInput HeightPatternDialog::load_last_input()
{
    HeightPatternInput in;
    in.sequence = { 1, 2 };
    AppConfig *cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return in;
    auto num = [cfg](const char *key, double &out) {
        if (!cfg->has(HCP_SECTION, key))
            return;
        try { out = std::stod(cfg->get(HCP_SECTION, key)); } catch (...) {}
    };
    auto inum = [&num](const char *key, int &out) { double d = out; num(key, d); out = int(std::lround(d)); };
    double unit = 0;
    num("unit", unit);
    in.unit = unit > 0.5 ? HeightPatternUnit::Layers : HeightPatternUnit::Mm;
    num("start", in.start);
    num("band", in.band);
    num("gap", in.gap);
    inum("repeat", in.repeat);
    inum("gap_mode", in.gap_mode);
    inum("gap_filament", in.gap_filament);
    if (cfg->has(HCP_SECTION, "to_top"))
        in.to_top = cfg->get(HCP_SECTION, "to_top") == "true";
    if (cfg->has(HCP_SECTION, "snap"))
        in.snap_to_layers = cfg->get(HCP_SECTION, "snap") != "false";
    if (cfg->has(HCP_SECTION, "sequence")) {
        std::vector<int> seq;
        std::stringstream ss(cfg->get(HCP_SECTION, "sequence"));
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            try { seq.push_back(std::stoi(tok)); } catch (...) { seq.clear(); break; }
        }
        if (!seq.empty())
            in.sequence = seq;
    }
    if (in.repeat < 1) in.repeat = 3;
    if (!(in.band > 0.)) in.band = 10.;
    if (in.gap < 0.) in.gap = 0.;
    if (in.start < 0.) in.start = 0.;
    if (in.gap_mode < 0 || in.gap_mode > 2) in.gap_mode = 0;
    return in;
}

void HeightPatternDialog::save_last_input() const
{
    AppConfig *cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return;
    const HeightPatternInput &in = m_result.input;
    cfg->set(HCP_SECTION, "unit", in.unit == HeightPatternUnit::Layers ? "1" : "0");
    cfg->set(HCP_SECTION, "start", hcp_num(in.start).ToStdString());
    cfg->set(HCP_SECTION, "band", hcp_num(in.band).ToStdString());
    cfg->set(HCP_SECTION, "gap", hcp_num(in.gap).ToStdString());
    cfg->set(HCP_SECTION, "repeat", std::to_string(in.repeat));
    cfg->set(HCP_SECTION, "to_top", in.to_top);
    cfg->set(HCP_SECTION, "gap_mode", std::to_string(in.gap_mode));
    cfg->set(HCP_SECTION, "gap_filament", std::to_string(in.gap_filament));
    cfg->set(HCP_SECTION, "snap", in.snap_to_layers);
    std::string seq;
    for (int f : in.sequence)
        seq += (seq.empty() ? "" : ",") + std::to_string(f);
    cfg->set(HCP_SECTION, "sequence", seq);
}

HeightPatternDialog::HeightPatternDialog(wxWindow *parent, const HeightPatternDialogContext &ctx, const HeightPatternInput &initial)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe),
                wxID_ANY,
                ctx.own_pattern_id > 0 ? _L("Edit Color Pattern") : _L("Color Pattern"),
                wxDefaultPosition,
                wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX)
    , m_ctx(ctx)
    , m_input(initial)
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);

    // Drop any sequence entry the project no longer has, so the first preview is not an error the
    // user never made. (A pattern that really lost its filaments is dealt with in the object list.)
    if (ctx.own_pattern_id <= 0) {
        m_input.sequence.erase(std::remove_if(m_input.sequence.begin(), m_input.sequence.end(),
                                              [&](int f) { return f < 1 || f > m_ctx.max_filament; }),
                               m_input.sequence.end());
        if (m_input.sequence.empty())
            m_input.sequence = m_ctx.max_filament >= 2 ? std::vector<int>{ 1, 2 } : std::vector<int>{ 1 };
        if (m_input.gap_filament > m_ctx.max_filament)
            m_input.gap_filament = 0;
    }

    auto *v_sizer = new wxBoxSizer(wxVERTICAL);

    // ---- header: which object ----------------------------------------------------------------
    if (!m_ctx.object_name.empty()) {
        auto *obj_label = new ::Label(this, Label::Head_14, wxString::Format(_L("Object: %s"), from_u8(m_ctx.object_name)));
        v_sizer->Add(obj_label, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    }

    auto *grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(16));
    grid->AddGrowableCol(1, 1);
    const wxSize input_size(FromDIP(140), -1);

    auto make_input = [this, input_size](double value, const wxString &unit) {
        auto *in = new ::TextInput(this, hcp_num(value), unit, wxEmptyString, wxDefaultPosition, input_size, wxTE_PROCESS_ENTER);
        in->GetTextCtrl()->SetFont(Label::Body_14);
        in->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
        in->GetTextCtrl()->Bind(wxEVT_TEXT, [this](wxCommandEvent &e) { e.Skip(); update_preview(); });
        return in;
    };
    auto add_row = [this, grid](const wxString &label, wxWindow *ctrl) {
        grid->Add(new ::Label(this, Label::Body_14, label + ":"), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(ctrl, 0, wxALIGN_CENTER_VERTICAL);
    };

    // ---- units + snapping --------------------------------------------------------------------
    {
        auto *row = new wxBoxSizer(wxHORIZONTAL);
        m_unit_combo = new ::ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, input_size, 0, nullptr, wxCB_READONLY);
        m_unit_combo->Append(_L("Millimetres"));
        m_unit_combo->Append(_L("Layers"));
        m_unit_combo->SetSelection(m_input.unit == HeightPatternUnit::Layers ? 1 : 0);
        m_unit_combo->SetToolTip(_L("Enter the start, band height and gap in millimetres or in layers of this object."));
        m_unit_combo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &e) { e.Skip(); update_unit_labels(); update_preview(); });
        row->Add(m_unit_combo, 0, wxALIGN_CENTER_VERTICAL);
        m_snap_cb = new ::CheckBox(this);
        m_snap_cb->SetValue(m_input.snap_to_layers);
        m_snap_cb->SetToolTip(_L("Move every band edge to the nearest layer boundary, so a band starts and ends on a whole layer. "
                                 "The snapped values are what is stored; the live result below shows them."));
        m_snap_cb->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &e) { e.Skip(); update_preview(); });
        row->Add(m_snap_cb, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(16));
        row->Add(new ::Label(this, Label::Body_14, _L("Snap to layers")), 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
        grid->Add(new ::Label(this, Label::Body_14, _L("Units") + ":"), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(row, 0, wxALIGN_CENTER_VERTICAL);
    }

    m_start_input = make_input(m_input.start, _L("mm"));
    m_start_input->SetToolTip(_L("Height of the bottom of the first band, measured from the bottom of the object."));
    add_row(_L("Start at"), m_start_input);

    m_band_input = make_input(m_input.band, _L("mm"));
    m_band_input->SetToolTip(_L("How tall each coloured band is."));
    add_row(_L("Band height"), m_band_input);

    m_gap_input = make_input(m_input.gap, _L("mm"));
    m_gap_input->SetToolTip(_L("The distance between the top of one band and the bottom of the next."));
    add_row(_L("Gap"), m_gap_input);

    // ---- repeat ---------------------------------------------------------------------------------
    {
        auto *row = new wxBoxSizer(wxHORIZONTAL);
        m_repeat_input = new ::TextInput(this, wxString::Format("%d", m_input.repeat), _L("x sequence"), wxEmptyString, wxDefaultPosition, input_size, wxTE_PROCESS_ENTER);
        m_repeat_input->GetTextCtrl()->SetFont(Label::Body_14);
        m_repeat_input->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_DIGITS));
        m_repeat_input->GetTextCtrl()->Bind(wxEVT_TEXT, [this](wxCommandEvent &e) { e.Skip(); update_preview(); });
        row->Add(m_repeat_input, 0, wxALIGN_CENTER_VERTICAL);
        m_repeat_input->SetToolTip(_L("How many times the whole filament sequence is repeated. Every repetition uses every filament in the sequence, in order, so 5 filaments repeated 3 times make 15 bands. Only the top of the object can cut the last repetition short."));
        m_to_top_cb = new ::CheckBox(this);
        m_to_top_cb->SetValue(m_input.to_top);
        m_to_top_cb->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &e) {
            e.Skip();
            m_repeat_input->Enable(!m_to_top_cb->GetValue());
            update_preview();
        });
        row->Add(m_to_top_cb, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(16));
        row->Add(new ::Label(this, Label::Body_14, _L("Repeat to the top")), 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
        m_repeat_input->Enable(!m_input.to_top);
        grid->Add(new ::Label(this, Label::Body_14, _L("Repeat sequence") + ":"), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(row, 0, wxALIGN_CENTER_VERTICAL);
    }

    // ---- gap mode -------------------------------------------------------------------------------
    {
        auto *row = new wxBoxSizer(wxHORIZONTAL);
        m_gap_mode = new ::ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(210), -1), 0, nullptr, wxCB_READONLY);
        m_gap_mode->Append(_L("Object's own filament"));
        m_gap_mode->Append(_L("Next in the sequence"));
        m_gap_mode->Append(_L("A fixed filament"));
        m_gap_mode->SetSelection(std::clamp(m_input.gap_mode, 0, 2));
        m_gap_mode->SetToolTip(_L("What the gaps between the bands print with. The object's own filament writes nothing for the gaps, "
                                  "so it adds the fewest filament changes."));
        m_gap_mode->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &e) { e.Skip(); update_gap_controls(); update_preview(); });
        row->Add(m_gap_mode, 0, wxALIGN_CENTER_VERTICAL);

        m_gap_filament = new ::ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(110), -1), 0, nullptr, wxCB_READONLY);
        for (int f = 1; f <= m_ctx.max_filament; ++f) {
            const wxColour c = size_t(f) <= m_ctx.filament_colours.size() ? hcp_parse_colour(m_ctx.filament_colours[size_t(f) - 1]) : wxColour(128, 128, 128);
            m_gap_filament->Append(wxString::Format("%d", f), hcp_swatch_bitmap(c, FromDIP(18), FromDIP(14)));
        }
        m_gap_filament->SetSelection(m_input.gap_filament >= 1 && m_input.gap_filament <= m_ctx.max_filament ? m_input.gap_filament - 1 : 0);
        m_gap_filament->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &e) { e.Skip(); update_preview(); });
        row->Add(m_gap_filament, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
        grid->Add(new ::Label(this, Label::Body_14, _L("In the gaps use") + ":"), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(row, 0, wxALIGN_CENTER_VERTICAL);
    }

    v_sizer->Add(grid, 0, wxEXPAND | wxALL, FromDIP(12));

    // ---- filament sequence ----------------------------------------------------------------------
    // Two labelled rows so they do not read as duplicates: "Sequence" is what the bands will use,
    // in order (click a chip to remove it); "Add filament" is the project's palette.
    {
        auto *box = new wxBoxSizer(wxVERTICAL);

        auto *seq_head = new wxBoxSizer(wxHORIZONTAL);
        seq_head->Add(new ::Label(this, Label::Head_14, _L("Sequence") + ":"), 0, wxALIGN_CENTER_VERTICAL);
        seq_head->Add(new ::Label(this, Label::Body_12, _L("(used in this order; the whole sequence is repeated)")), 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
        seq_head->AddStretchSpacer(1);
        auto *reverse_btn = new ::Button(this, _L("Reverse"));
        reverse_btn->SetStyle(ButtonStyle::Regular, ButtonType::Window);
        reverse_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
            std::reverse(m_input.sequence.begin(), m_input.sequence.end());
            rebuild_sequence();
            update_preview();
        });
        auto *clear_btn = new ::Button(this, _L("Clear"));
        clear_btn->SetStyle(ButtonStyle::Regular, ButtonType::Window);
        clear_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
            m_input.sequence.clear();
            rebuild_sequence();
            update_preview();
        });
        seq_head->Add(reverse_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
        seq_head->Add(clear_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
        box->Add(seq_head, 0, wxEXPAND | wxBOTTOM, FromDIP(4));

        m_sequence = new HeightPatternSwatches(this, HCP_INNER_W, _L("Click a filament below to add it"));
        m_sequence->set_tooltip_prefix(_L("Click to remove filament"));
        m_sequence->set_on_click([this](int idx) {
            if (idx >= 0 && idx < int(m_input.sequence.size())) {
                m_input.sequence.erase(m_input.sequence.begin() + idx);
                rebuild_sequence();
                update_preview();
            }
        });
        box->Add(m_sequence, 0, wxBOTTOM, FromDIP(8));

        box->Add(new ::Label(this, Label::Head_14, _L("Add filament") + ":"), 0, wxBOTTOM, FromDIP(4));
        m_palette = new HeightPatternSwatches(this, HCP_INNER_W, wxEmptyString);
        m_palette->set_tooltip_prefix(_L("Add filament"));
        std::vector<HeightPatternSwatches::Chip> chips;
        for (int f = 1; f <= m_ctx.max_filament; ++f)
            chips.push_back({ f, size_t(f) <= m_ctx.filament_colours.size() ? hcp_parse_colour(m_ctx.filament_colours[size_t(f) - 1]) : wxColour(128, 128, 128) });
        m_palette->set_chips(chips);
        m_palette->set_on_click([this](int idx) {
            if (idx >= 0 && idx < m_ctx.max_filament) {
                m_input.sequence.push_back(idx + 1);
                rebuild_sequence();
                update_preview();
            }
        });
        box->Add(m_palette, 0);
        v_sizer->Add(box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    }

    // ---- scope ------------------------------------------------------------------------------------
    if (m_ctx.other_plate_objects > 0 && m_ctx.own_pattern_id <= 0) {
        auto *row = new wxBoxSizer(wxHORIZONTAL);
        m_all_cb = new ::CheckBox(this);
        m_all_cb->SetToolTip(_L("Put the same pattern on every object on this plate. Each object gets its own copy, "
                                "worked out against its own height and layers."));
        m_all_cb->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &e) { e.Skip(); update_preview(); });
        row->Add(m_all_cb, 0, wxALIGN_CENTER_VERTICAL);
        row->Add(new ::Label(this, Label::Body_14, wxString::Format(_L("All objects on this plate (%d more)"), m_ctx.other_plate_objects)),
                 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
        v_sizer->Add(row, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    }

    // ---- live result --------------------------------------------------------------------------------
    // Everything below is a fixed width, and the band list a fixed height with its own scroll bar, so
    // the dialog is the same size for 1 band and for 50.
    auto *res = new wxBoxSizer(wxVERTICAL);
    m_strip = new HeightPatternStrip(this);
    res->Add(m_strip, 0, wxEXPAND);

    m_result_text = new ::Label(this, Label::Head_14, wxEmptyString);
    res->Add(m_result_text, 0, wxTOP, FromDIP(8));

    m_band_list = new HeightPatternBandList(this, HCP_INNER_W);
    res->Add(m_band_list, 0, wxTOP, FromDIP(6));

    // Phase 3: what the pattern costs in filament changes (and purge, when the project's purge table covers it).
    m_change_text = new ::Label(this, Label::Body_14, wxEmptyString);
    res->Add(m_change_text, 0, wxTOP, FromDIP(6));

    // Phase 3: bands of this pattern the user edited by hand (edit mode only).
    m_edits_text = new ::Label(this, Label::Body_12, wxEmptyString);
    m_edits_text->SetForegroundColour(wxColour("#C77700"));
    res->Add(m_edits_text, 0, wxTOP, FromDIP(4));

    m_note_text = new ::Label(this, Label::Body_12, wxEmptyString);
    m_note_text->SetForegroundColour(wxColour("#6B6B6B"));
    res->Add(m_note_text, 0, wxTOP, FromDIP(4));

    m_clash_text = new ::Label(this, Label::Body_14, wxEmptyString);
    m_clash_text->SetForegroundColour(wxColour("#D9534F"));
    res->Add(m_clash_text, 0, wxTOP, FromDIP(4));

    m_error_text = new ::Label(this, Label::Body_14, wxEmptyString);
    m_error_text->SetForegroundColour(wxColour("#D9534F"));
    res->Add(m_error_text, 0, wxTOP, FromDIP(4));

    // Help text that is always true (design D7). Wrapped to the dialog width and given the height of
    // its wrapped lines, or the label keeps the one-line height and the second line is cut off.
    m_help_text = new ::Label(this, Label::Body_12, wxEmptyString);
    m_help_text->SetForegroundColour(wxColour("#6B6B6B"));
    hcp_set_wrapped(m_help_text, hcp_help_text(), FromDIP(HCP_INNER_W), 0);
    res->Add(m_help_text, 0, wxTOP, FromDIP(8));
    v_sizer->Add(res, 0, wxLEFT | wxRIGHT, FromDIP(12));

    // ---- buttons ------------------------------------------------------------------------------------
    auto *dlg_btns = new DialogButtons(this, { "OK", "Cancel" });
    m_ok_button = dlg_btns->GetOK();
    m_ok_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        m_input = current_input();
        m_result.input = m_input;
        m_result.all_plate_objects = m_all_cb != nullptr && m_all_cb->GetValue();
        m_result.replace_confirmed = m_has_clash;
        save_last_input();
        EndModal(wxID_OK);
    });
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
    v_sizer->Add(dlg_btns, 0, wxEXPAND | wxTOP, FromDIP(10));

    rebuild_sequence();
    update_unit_labels();
    update_gap_controls();
    update_preview();

    SetSizer(v_sizer);
    refit();
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

double HeightPatternDialog::read_number(::TextInput *input, double fallback) const
{
    if (input == nullptr)
        return fallback;
    double   v   = 0.;
    wxString txt = input->GetTextCtrl()->GetValue();
    txt.Replace(",", ".");
    if (txt.empty() || !txt.ToDouble(&v))
        return fallback;
    return v;
}

HeightPatternInput HeightPatternDialog::current_input() const
{
    HeightPatternInput in = m_input;   // sequence lives in m_input
    in.unit           = m_unit_combo->GetSelection() == 1 ? HeightPatternUnit::Layers : HeightPatternUnit::Mm;
    in.snap_to_layers = m_snap_cb->GetValue();
    in.start          = read_number(m_start_input, -1.);
    in.band           = read_number(m_band_input, 0.);
    in.gap            = read_number(m_gap_input, -1.);
    in.to_top         = m_to_top_cb->GetValue();
    in.repeat         = int(std::lround(read_number(m_repeat_input, 0.)));
    in.gap_mode       = std::clamp(m_gap_mode->GetSelection(), 0, 2);
    in.gap_filament   = in.gap_mode == int(HeightPatternGapMode::Fixed) ? m_gap_filament->GetSelection() + 1 : 0;
    return in;
}

void HeightPatternDialog::update_unit_labels()
{
    const bool layers = m_unit_combo->GetSelection() == 1;
    const wxString u = layers ? _L("layers") : _L("mm");
    m_start_input->SetLabel(u);
    m_band_input->SetLabel(u);
    m_gap_input->SetLabel(u);
    if (m_start_input->GetParent() != nullptr)
        m_start_input->GetParent()->Layout();
}

void HeightPatternDialog::update_gap_controls()
{
    const bool fixed = m_gap_mode->GetSelection() == int(HeightPatternGapMode::Fixed);
    m_gap_filament->Show(fixed);
    refit();
}

void HeightPatternDialog::rebuild_sequence()
{
    std::vector<HeightPatternSwatches::Chip> chips;
    for (int f : m_input.sequence)
        chips.push_back({ f, f >= 1 && size_t(f) <= m_ctx.filament_colours.size() ? hcp_parse_colour(m_ctx.filament_colours[size_t(f) - 1]) : wxColour(128, 128, 128) });
    m_sequence->set_chips(chips);
    refit();
}

void HeightPatternDialog::refit()
{
    if (GetSizer() == nullptr)
        return;
    // A fixed width: nothing in the dialog is allowed to widen it (long text wraps, the band list
    // scrolls). The height follows the content, which does not depend on the number of bands.
    GetSizer()->SetMinSize(FromDIP(HCP_W), 0);
    Layout();
    GetSizer()->Fit(this);
}

void HeightPatternDialog::update_preview()
{
    if (m_result_text == nullptr || m_ok_button == nullptr || m_strip == nullptr || m_band_list == nullptr || m_change_text == nullptr || m_edits_text == nullptr)
        return;   // still constructing

    const HeightPatternInput in = current_input();
    const HeightPatternPreview pv = preview_height_pattern(m_ctx.object, in, m_ctx.own_pattern_id, m_ctx.layer_tops,
                                                           m_ctx.layer_height, m_ctx.object_height, m_ctx.max_filament);
    const double top = m_ctx.layer_tops.empty() ? m_ctx.object_height : m_ctx.layer_tops.back();
    const int    w   = FromDIP(HCP_INNER_W);

    std::vector<wxColour> colours;
    for (const std::string &s : m_ctx.filament_colours)
        colours.push_back(hcp_parse_colour(s));

    m_has_clash = pv.ok() && !pv.clashes.empty();
    m_ok_button->Enable(pv.ok());
    m_ok_button->SetLabel(m_has_clash ? _L("Replace and apply") : _L("Apply"));

    wxString note, error;
    if (pv.ok()) {
        // One short wrapped line (two lines are always reserved, so the dialog does not jump) and the
        // per-band detail in the scrolling list below it.
        hcp_set_wrapped(m_result_text, from_u8(height_pattern_short_summary(pv.expansion)), w, 2);
        // Say so when snapping moved the start, in the unit the user typed it in.
        const HeightColourPattern typed = height_pattern_from_input(in, m_ctx.layer_height, m_ctx.layer_tops);
        if (in.snap_to_layers && !pv.expansion.ranges.empty() && std::abs(pv.expansion.ranges.front().lo - typed.start_z) > 0.005)
            note = wxString::Format(_L("The start snaps from %s to %s mm to fall on a layer boundary."), hcp_num(typed.start_z), hcp_num(pv.expansion.ranges.front().lo));
        m_strip->set_data(pv.expansion.ranges, top, colours);
        m_band_list->set_bands(height_pattern_band_ranges(pv.expansion), colours, pv.expansion.clipped_at_top);
    } else {
        hcp_set_wrapped(m_result_text, wxEmptyString, w, 2);
        m_strip->clear();
        m_band_list->clear();
        error = from_u8(pv.error);
    }
    if (m_all_cb != nullptr && m_all_cb->GetValue() && m_ctx.other_heights_differ) {
        if (!note.empty())
            note += " ";
        note += _L("The objects are not all the same height: each gets the pattern worked out from its own bottom, so the bands will not line up.");
    }
    hcp_set_wrapped(m_note_text, note, w, 0);
    hcp_set_wrapped(m_clash_text, m_has_clash ? from_u8(height_pattern_clash_text(pv.clashes)) + ". " + _L("Applying replaces them.") : wxString(), w, 0);
    hcp_set_wrapped(m_error_text, error, w, 0);

    // Phase 3: filament changes (and purge) this pattern adds to one object.
    wxString changes_line;
    if (pv.ok()) {
        const HeightPatternChangeEstimate est = estimate_height_pattern_changes(pv.expansion.ranges, top, m_ctx.change_input);
        if (est.changes == 0)
            changes_line = _L("Adds no filament changes.");
        else {
            changes_line = wxString::Format(est.changes == 1 ? _L("Adds about %d filament change per object") : _L("Adds about %d filament changes per object"), est.changes);
            if (est.purge_known && est.purge_g > 0.)
                changes_line += wxString::Format(_L(", about %.1f g of purge"), est.purge_g);
            changes_line += ".";
        }
    }
    hcp_set_wrapped(m_change_text, changes_line, w, 1);

    // Phase 3: bands of this pattern edited by hand (edit mode). Apply rewrites all of them.
    wxString edits_line;
    if (!m_ctx.hand_edits.empty()) {
        edits_line = _L("Edited by hand") + ": ";
        for (size_t i = 0; i < m_ctx.hand_edits.size(); ++i) {
            const HeightPatternBandDiff &d = m_ctx.hand_edits[i];
            if (i > 0)
                edits_line += "; ";
            edits_line += d.band_number > 0 ? wxString::Format(_L("band %d"), d.band_number) : _L("gap");
            edits_line += wxString::Format(" %s-%s", hcp_num(d.lo), hcp_num(d.hi));
            switch (d.status) {
            case HeightPatternBandStatus::Removed:            edits_line += " (" + _L("moved or removed") + ")"; break;
            case HeightPatternBandStatus::FilamentChanged:    edits_line += " (" + wxString::Format(_L("filament %d instead of %d"), d.actual_extruder, d.expected_extruder) + ")"; break;
            case HeightPatternBandStatus::LayerHeightChanged: edits_line += " (" + _L("layer height") + ")"; break;
            }
        }
        edits_line += ". " + _L("Apply writes every band again; use Reset in the object list to put back only these.");
    }
    hcp_set_wrapped(m_edits_text, edits_line, w, 0);

    // Phase 3: the 3D view draws the bands live.
    if (m_ctx.on_preview)
        m_ctx.on_preview(pv.ok() ? pv.expansion.ranges : std::vector<HeightPatternRange>());

    refit();
}

void HeightPatternDialog::on_dpi_changed(const wxRect &)
{
    if (m_palette != nullptr) m_palette->rescale();
    if (m_sequence != nullptr) m_sequence->rescale();
    if (m_band_list != nullptr) m_band_list->rescale();
    if (m_help_text != nullptr)
        hcp_set_wrapped(m_help_text, hcp_help_text(), FromDIP(HCP_INNER_W), 0);
    update_preview();
    Refresh();
}

}} // namespace Slic3r::GUI
