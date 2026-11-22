#include "ToolTempList.hpp"

#include "Label.hpp"
#include "StateColor.hpp"
#include "../BambuDevicePalette.hpp"
#include "../GUI_App.hpp"
#include "../I18N.hpp"
#include "libslic3r/AppConfig.hpp"

#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>

#include <boost/log/trivial.hpp>

#include <algorithm>

using namespace Slic3r::GUI;

namespace {

const wxString BLANK_STR("_");

// Colours of the Device tab (StatusPanel.cpp / TempInput.cpp), mapped for the dark theme at paint
// time and never cached, so a theme switch repaints correctly.
wxColour box_colour() { return StateColor::darkModeColorFor(*wxWHITE); }
wxColour line_colour() { return StateColor::darkModeColorFor(wxColour(238, 238, 238)); }
wxColour text_colour() { return StateColor::darkModeColorFor(wxColour("#323A3C")); }
wxColour side_colour() { return StateColor::darkModeColorFor(wxColour("#6B6B6B")); }
wxColour dim_colour() { return StateColor::darkModeColorFor(wxColour("#ACACAC")); }
wxColour accent_colour() { return StateColor::darkModeColorFor(BambuDevicePalette::Green); }

// Screen readers read wxWindow::GetName() through wxWindowAccessible (MSW); elsewhere the name is
// harmless and the tooltip carries the same text.
void set_access_name(wxWindow *w, const wxString &name)
{
    if (!w || w->GetName() == name) return;
    w->SetName(name);
#if wxUSE_ACCESSIBILITY
    if (!w->GetAccessible()) w->SetAccessible(new wxWindowAccessible(w));
#endif
}

wxString tool_word(ToolCellWord w)
{
    switch (w) {
    case ToolCellWord::Empty: return _L("Empty");
    case ToolCellWord::OnTool: return _L("On tool");
    case ToolCellWord::Cutter: return _L("Cutter");
    case ToolCellWord::Pen: return _L("Pen");
    case ToolCellWord::Laser: return _L("Laser");
    case ToolCellWord::Fan: return _L("Fan");
    case ToolCellWord::Unknown: return _L("Unknown");
    default: return wxString();
    }
}

wxString tool_name(const ToolTempEntry &e)
{
    if (e.slot >= 0) return wxString::Format(_L("Rack slot %d"), e.slot + 1);
    if (e.badge.empty()) return _L("Nozzle");
    return wxString::Format(_L("Nozzle %s"), wxString::FromUTF8(e.badge));
}

// Slot / tool, size, temperature and state, for the tooltip and the accessible name.
wxString tool_description(const ToolTempEntry &e, const ToolCellView &v)
{
    wxString s = tool_name(e) + ": ";
    if (v.word == ToolCellWord::Empty) return s + _L("empty");
    if (!tool_kind_has_heater(e.kind)) return s + wxString::Format(_L("%s, no heater"), tool_word(v.word));
    wxString what;
    if (e.kind == ToolKind::Unknown)
        what = _L("unknown tool");
    else if (!e.size_text.empty())
        what = wxString::Format(_L("%s nozzle"), wxString::FromUTF8(e.size_text));
    else
        what = _L("nozzle");
    if (e.mounted) return s + what + ", " + _L("on the tool head");
    s += what;
    if (!e.has_temp) return s + ", " + _L("no reading");
    s += ", " + wxString::Format(_L("%d degrees"), e.temp);
    if (e.target > 0) {
        s += ", " + wxString::Format(_L("target %d"), e.target);
        if (e.heating) s += ", " + _L("heating");
    } else {
        s += ", " + _L("heater off");
    }
    if (e.active) s += ", " + _L("active tool");
    if (!v.settable) s += ", " + _L("read-only");
    return s;
}

// Disc for one character, pill for two or more; same colours as TempInput::draw_badge.
void paint_badge(wxDC &dc, wxWindow *w, const wxString &text, const wxPoint &at, bool highlight, bool dim)
{
    const int d = w->FromDIP(16);
    dc.SetFont(::Label::Body_12);
    const wxSize ts    = dc.GetTextExtent(text);
    const int    width = std::max(d, ts.x + w->FromDIP(6));
    wxColour     fill, letter;
    if (!w->IsEnabled() || dim) {
        fill   = dim_colour();
        letter = *wxWHITE;
    } else if (highlight) {
        fill   = accent_colour();
        letter = *wxWHITE;
    } else {
        fill   = StateColor::darkModeColorFor(wxColour("#262E30"));
        letter = StateColor::darkModeColorFor(*wxWHITE);
    }
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(fill));
    if (width > d)
        dc.DrawRoundedRectangle(at, wxSize(width, d), d / 2.0);
    else
        dc.DrawEllipse(at, wxSize(d, d));
    dc.SetTextForeground(letter);
    dc.DrawText(text, at.x + (width - ts.x) / 2, at.y + (d - ts.y) / 2);
}

int badge_width(wxDC &dc, wxWindow *w, const wxString &text)
{
    dc.SetFont(::Label::Body_12);
    return std::max(w->FromDIP(16), dc.GetTextExtent(text).x + w->FromDIP(6));
}

bool same_entry(const ToolTempEntry &a, const ToolTempEntry &b)
{
    return a.id == b.id && a.badge == b.badge && a.kind == b.kind && a.slot == b.slot && a.has_temp == b.has_temp && a.temp == b.temp &&
           a.target == b.target && a.settable == b.settable && a.heating == b.heating && a.active == b.active && a.present == b.present &&
           a.mounted == b.mounted && a.size_text == b.size_text;
}

} // namespace

// ---- read-only strip cell --------------------------------------------------------------------

class ToolTempCell : public wxWindow
{
public:
    explicit ToolTempCell(wxWindow *parent) : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        load_icons();
        SetMinSize(wxSize(FromDIP(50), FromDIP(70)));
        Bind(wxEVT_PAINT, &ToolTempCell::on_paint, this);
    }

    // Read-only cells are not tab stops (mockup notes); their text is in the name and tooltip.
    bool AcceptsFocus() const override { return false; }
    bool AcceptsFocusFromKeyboard() const override { return false; }

    void set_entry(const ToolTempEntry &e)
    {
        if (m_has_entry && same_entry(e, m_entry)) return;
        m_entry     = e;
        m_has_entry = true;
        m_view      = tool_cell_view(e, ToolTempList::kRackTargetsSettable);
        const wxString desc = tool_description(e, m_view);
        SetToolTip(desc);
        set_access_name(this, desc);
        Refresh();
    }

    void rescale()
    {
        load_icons();
        SetMinSize(wxSize(FromDIP(50), FromDIP(70)));
        Refresh();
    }

    bool Enable(bool enable = true) override
    {
        const bool changed = wxWindow::Enable(enable);
        if (changed) Refresh();
        return changed;
    }

private:
    void load_icons()
    {
        m_icon     = ScalableBitmap(this, "monitor_nozzle_temp", 12);
        m_icon_hot = ScalableBitmap(this, "monitor_nozzle_temp_active", 12);
    }

    void on_paint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC pdc(this);
        wxGCDC                dc(pdc);
        const wxSize          sz = GetClientSize();

        dc.SetBackground(wxBrush(box_colour()));
        dc.Clear();

        // border: mounted = accent, empty = dashed grey, otherwise the box's line colour
        wxColour border = m_view.accent_border ? accent_colour() : m_view.dashed_border ? dim_colour() : line_colour();
        wxPen    pen(border, std::max(1, FromDIP(1)), m_view.dashed_border ? wxPENSTYLE_SHORT_DASH : wxPENSTYLE_SOLID);
        dc.SetPen(pen);
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRoundedRectangle(0, 0, sz.x - 1, sz.y - 1, FromDIP(4));
        if (!m_has_entry) return;

        const bool     enabled = IsEnabled();
        const wxString badge   = wxString::FromUTF8(m_entry.badge);
        const wxString word    = tool_word(m_view.word);
        const wxString cur     = wxString::Format("%d", m_entry.temp);
        const wxString tgt     = wxString::Format("/ %d", m_entry.target);
        const wxString size    = wxString::FromUTF8(m_entry.size_text);
        const bool     show_tgt = m_view.show_target && m_entry.target > 0;

        // the lines, top to bottom, centred as a block
        const int gap = FromDIP(2);
        int       h   = FromDIP(16); // badge line
        dc.SetFont(::Label::Head_13);
        const wxSize cur_sz = dc.GetTextExtent(cur);
        dc.SetFont(::Label::Body_11);
        const wxSize word_sz = dc.GetTextExtent(word.IsEmpty() ? wxString("0") : word);
        const wxSize tgt_sz  = dc.GetTextExtent(tgt);
        dc.SetFont(::Label::Body_10);
        const wxSize size_sz = dc.GetTextExtent(size.IsEmpty() ? wxString("0") : size);
        if (m_view.show_temp) h += gap + cur_sz.y;
        if (!word.IsEmpty()) h += gap + word_sz.y;
        if (show_tgt) h += gap + tgt_sz.y;
        if (m_view.show_size) h += gap + size_sz.y;
        int y = std::max(0, (sz.y - h) / 2);

        // badge (+ heater icon while a target is set)
        {
            const int       bw   = badge_width(dc, this, badge);
            const ScalableBitmap &icon = m_view.heating ? m_icon_hot : m_icon;
            const int       iw   = m_view.show_heater_icon && icon.bmp().IsOk() ? icon.GetBmpSize().x + FromDIP(3) : 0;
            const int       x    = (sz.x - bw - iw) / 2;
            paint_badge(dc, this, badge, wxPoint(x, y), m_view.highlight_badge, m_view.dim_badge);
            if (iw > 0) dc.DrawBitmap(icon.bmp(), x + bw + FromDIP(3), y + (FromDIP(16) - icon.GetBmpSize().y) / 2);
            y += FromDIP(16);
        }
        auto line = [&](const wxString &text, const wxFont &font, const wxColour &colour, const wxSize &tsz) {
            y += gap;
            dc.SetFont(font);
            dc.SetTextForeground(enabled ? colour : dim_colour());
            dc.DrawText(text, (sz.x - tsz.x) / 2, y);
            y += tsz.y;
        };
        if (m_view.show_temp) line(cur, ::Label::Head_13, text_colour(), cur_sz);
        if (!word.IsEmpty()) line(word, ::Label::Body_11, m_view.word == ToolCellWord::Empty ? dim_colour() : side_colour(), word_sz);
        if (show_tgt) line(tgt, ::Label::Body_11, side_colour(), tgt_sz);
        if (m_view.show_size) line(size, ::Label::Body_10, side_colour(), size_sz);
    }

    ToolTempEntry  m_entry;
    ToolCellView   m_view;
    bool           m_has_entry{false};
    ScalableBitmap m_icon;
    ScalableBitmap m_icon_hot;
};

// ---- collapsible group header ("Rack  3 warm  v") --------------------------------------------

class ToolGroupHeader : public wxWindow
{
public:
    ToolGroupHeader(wxWindow *parent, std::function<void()> on_toggle)
        : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxWANTS_CHARS), m_on_toggle(std::move(on_toggle))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(-1, FromDIP(24)));
        SetCursor(wxCURSOR_HAND);
        Bind(wxEVT_PAINT, &ToolGroupHeader::on_paint, this);
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &) {
            SetFocus();
            toggle();
        });
        Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent &e) {
            const int k = e.GetKeyCode();
            if (k == WXK_SPACE || k == WXK_RETURN || k == WXK_NUMPAD_ENTER) {
                toggle();
            } else if (k == WXK_TAB) {
                // wxWANTS_CHARS takes Tab from the dialog navigation; pass it on ourselves.
                Navigate(e.ShiftDown() ? wxNavigationKeyEvent::IsBackward : wxNavigationKeyEvent::IsForward);
            } else {
                e.Skip();
            }
        });
        Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent &e) { Refresh(); e.Skip(); });
        Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent &e) { Refresh(); e.Skip(); });
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent &) { m_hover = true; Refresh(); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent &) { m_hover = false; Refresh(); });
    }

    bool AcceptsFocus() const override { return IsShown() && IsEnabled(); }
    bool AcceptsFocusFromKeyboard() const override { return AcceptsFocus(); }

    void set_text(const wxString &title, const wxString &summary, bool open)
    {
        if (title == m_title && summary == m_summary && open == m_open) return;
        m_title   = title;
        m_summary = summary;
        m_open    = open;
        const wxString name = m_title + ", " + m_summary + ", " + (m_open ? _L("expanded") : _L("collapsed"));
        set_access_name(this, name);
        SetToolTip(name);
        Refresh();
    }

    void rescale()
    {
        SetMinSize(wxSize(-1, FromDIP(24)));
        Refresh();
    }

    bool Enable(bool enable = true) override
    {
        const bool changed = wxWindow::Enable(enable);
        if (changed) Refresh();
        return changed;
    }

private:
    void toggle()
    {
        if (IsEnabled() && m_on_toggle) m_on_toggle();
    }

    void on_paint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC pdc(this);
        wxGCDC                dc(pdc);
        const wxSize          sz = GetClientSize();
        dc.SetBackground(wxBrush(box_colour()));
        dc.Clear();

        const bool enabled = IsEnabled();
        int        x       = FromDIP(6);
        dc.SetFont(::Label::Head_12);
        dc.SetTextForeground(enabled ? text_colour() : dim_colour());
        const wxSize ts = dc.GetTextExtent(m_title);
        dc.DrawText(m_title, x, (sz.y - ts.y) / 2);
        x += ts.x + FromDIP(6);
        dc.SetFont(::Label::Body_12);
        dc.SetTextForeground(enabled ? side_colour() : dim_colour());
        const wxSize ss = dc.GetTextExtent(m_summary);
        dc.DrawText(m_summary, x, (sz.y - ss.y) / 2);

        // chevron: down when open, right when collapsed
        const int c  = FromDIP(10);
        const int cx = sz.x - FromDIP(6) - c;
        const int cy = (sz.y - c) / 2;
        dc.SetPen(wxPen(m_hover && enabled ? text_colour() : side_colour(), std::max(1, FromDIP(1))));
        if (m_open) {
            dc.DrawLine(cx + c * 2 / 10, cy + c * 35 / 100, cx + c / 2, cy + c * 65 / 100);
            dc.DrawLine(cx + c / 2, cy + c * 65 / 100, cx + c * 8 / 10, cy + c * 35 / 100);
        } else {
            dc.DrawLine(cx + c * 35 / 100, cy + c * 2 / 10, cx + c * 65 / 100, cy + c / 2);
            dc.DrawLine(cx + c * 65 / 100, cy + c / 2, cx + c * 35 / 100, cy + c * 8 / 10);
        }

        // keyboard focus ring
        if (HasFocus()) {
            dc.SetPen(wxPen(accent_colour(), FromDIP(2)));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.DrawRoundedRectangle(FromDIP(1), FromDIP(1), sz.x - FromDIP(2), sz.y - FromDIP(2), FromDIP(4));
        }
    }

    std::function<void()> m_on_toggle;
    wxString              m_title;
    wxString              m_summary;
    bool                  m_open{true};
    bool                  m_hover{false};
};

// ---- the list ---------------------------------------------------------------------------------

ToolTempList::ToolTempList(wxWindow *box, wxWindow *tab_anchor, const StateColor &text_colour, const StateColor &border_colour, int min_temp, int max_temp)
    : m_box(box), m_tab_anchor(tab_anchor), m_text_colour(text_colour), m_border_colour(border_colour), m_min_temp(min_temp), m_max_temp(max_temp)
{
    m_column = new wxBoxSizer(wxVERTICAL);
    m_strip  = new wxBoxSizer(wxVERTICAL);
    m_strip_line = new StaticLine(m_box);
    m_strip_line->SetLineColour(wxColour(238, 238, 238));
    m_strip_line->Hide();
    m_strip->Add(m_strip_line, 0, wxEXPAND | wxBOTTOM, m_box->FromDIP(2));
}

ToolTempList::~ToolTempList()
{
    // StatusBasePanel deletes the list before wx destroys the panel's children, so the box and
    // this list's windows are still alive here: take them down now, while the lambdas bound on
    // them can still reach this object.
    destroy_all();
}

std::string ToolTempList::config_key(ToolStripGroup::What what)
{
    return what == ToolStripGroup::What::Rack ? "device_tool_temps_rack_collapsed" : "device_tool_temps_tools_collapsed";
}

std::string ToolTempList::shape_key(const ToolTempLayoutPlan &plan, const std::vector<ToolTempEntry> &tools, const std::vector<ToolTempEntry> &rack) const
{
    if (plan.uses_fixed_rows() && rack.empty()) return std::string();
    std::string key = "c" + std::to_string(int(plan.column)) + "|t";
    if (!plan.uses_fixed_rows())
        for (const ToolTempEntry &e : tools) key += ":" + e.id;
    key += "|r";
    for (const ToolTempEntry &e : rack) key += ":" + e.id + (tool_cell_view(e, kRackTargetsSettable).settable ? "s" : "");
    return key;
}

void ToolTempList::destroy_all()
{
    for (Row &r : m_rows)
        if (r.ctrl) {
            if (r.ctrl->wdialog) r.ctrl->wdialog->Dismiss();
            r.ctrl->Destroy();
        }
    m_rows.clear();
    for (Group &g : m_groups) {
        for (ToolTempCell *c : g.ro_cells)
            if (c) c->Destroy();
        if (g.header) g.header->Destroy();
        if (g.cells) {
            m_strip->Detach(g.cells); // its windows are gone already
            delete g.cells;
        }
    }
    m_groups.clear();
    if (m_grid) {
        m_column->Detach(m_grid);
        delete m_grid;
        m_grid = nullptr;
    }
    m_column->Clear(false);
    m_strip_line->Hide();
}

TempInput *ToolTempList::make_row(const ToolTempEntry &e, TempInput::RowMode mode, const wxSize &min_dip, bool rack)
{
    auto *c = new TempInput(m_box, wxWindow::NewControlId(), BLANK_STR, BLANK_STR, wxString("monitor_nozzle_temp"), wxString("monitor_nozzle_temp_active"),
                            wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER);
    c->SetMinSize(m_box->FromDIP(min_dip));
    c->SetMinTemp(m_min_temp);
    c->SetMaxTemp(m_max_temp);
    c->SetBorderWidth(m_box->FromDIP(2));
    c->SetTextColor(m_text_colour);
    c->SetBorderColor(m_border_colour);
    c->SetRowMode(mode);
    c->SetBadge(wxString::FromUTF8(e.badge), e.active);
    c->Enable(m_enabled);

    Row row;
    row.ctrl        = c;
    row.id          = e.id;
    row.extruder_id = e.extruder_id;
    row.rack        = rack;
    row.mode        = mode;
    row.min_dip     = min_dip;
    m_rows.push_back(row);

    const int type = c->GetType();
    c->Bind(wxEVT_SET_FOCUS, [this, type](wxFocusEvent &ev) {
        ev.Skip();
        if (Row *r = find_row(type)) r->editing = true;
    });
    c->Bind(wxEVT_KILL_FOCUS, [this, type](wxFocusEvent &ev) {
        ev.Skip();
        if (Row *r = find_row(type)) r->editing = false;
    });
    // Esc puts back the printer's target and keeps the field focused (new in this list only).
    c->GetTextCtrl()->Bind(wxEVT_CHAR_HOOK, [this, type](wxKeyEvent &ev) {
        Row *r = find_row(type);
        if (ev.GetKeyCode() != WXK_ESCAPE || !r || !r->ctrl) {
            ev.Skip();
            return;
        }
        r->ctrl->GetTextCtrl()->ChangeValue(wxString::Format("%d", r->shown_target));
        r->ctrl->Warning(false);
        r->ctrl->GetTextCtrl()->SelectAll();
        r->cancelled = true;
    });
    c->GetTextCtrl()->Bind(wxEVT_TEXT, [this, type](wxCommandEvent &ev) {
        ev.Skip();
        if (Row *r = find_row(type)) r->cancelled = false;
    });

    const wxString name = wxString::Format(_L("%s target, degrees Celsius"), tool_name(e));
    set_access_name(c->GetTextCtrl(), name);
    return c;
}

void ToolTempList::rebuild(const ToolTempLayoutPlan &plan, const std::vector<ToolTempEntry> &tools, const std::vector<ToolTempEntry> &rack)
{
    destroy_all();
    m_tools_in_list = !plan.uses_fixed_rows();

    // Column: compact rows or the grid, in tab order right after the fixed nozzle rows.
    wxWindow *prev = m_tab_anchor;
    auto      tab  = [&prev](wxWindow *w) {
        if (prev) w->MoveAfterInTabOrder(prev);
        prev = w;
    };
    if (plan.column == ToolColumn::Compact) {
        for (const ToolTempEntry &e : tools) {
            TempInput *c = make_row(e, TempInput::RowMode::Compact, wxSize(122, 32), false);
            m_column->Add(c, 0, wxEXPAND | wxALL, 1);
            tab(c);
        }
    } else if (plan.column == ToolColumn::Grid) {
        m_grid = new wxFlexGridSizer(2, m_box->FromDIP(2), m_box->FromDIP(2));
        m_grid->AddGrowableCol(0, 1);
        m_grid->AddGrowableCol(1, 1);
        for (const ToolTempEntry &e : tools) {
            TempInput *c = make_row(e, TempInput::RowMode::Cell, wxSize(58, 40), false);
            m_grid->Add(c, 0, wxEXPAND);
            tab(c);
        }
        m_column->Add(m_grid, 0, wxEXPAND | wxALL, 1);
    }

    // Strip under the box: one group per ToolStripGroup, left to right.
    if (plan.has_strip()) m_strip_line->Show();
    for (const ToolStripGroup &sg : plan.strip) {
        const std::vector<ToolTempEntry> &entries = sg.what == ToolStripGroup::What::Rack ? rack : tools;
        Group g;
        g.what = sg.what;
        if (Slic3r::AppConfig *cfg = wxGetApp().app_config) g.open = cfg->get(config_key(sg.what)) != "1";
        const size_t index = m_groups.size();
        g.header           = new ToolGroupHeader(m_box, [this, index]() {
            if (index < m_groups.size()) toggle(m_groups[index]);
        });
        g.cells = new wxGridSizer(std::max(1, sg.rows), std::max(1, sg.cols), m_box->FromDIP(4), m_box->FromDIP(4));
        for (const ToolTempEntry &e : entries) {
            const bool rack_entry = sg.what == ToolStripGroup::What::Rack;
            const bool settable   = rack_entry ? tool_cell_view(e, kRackTargetsSettable).settable : e.settable;
            if (settable) {
                TempInput *c = make_row(e, TempInput::RowMode::Cell, wxSize(50, rack_entry ? 70 : 44), rack_entry);
                g.cells->Add(c, 0, wxEXPAND);
                g.ro_cells.push_back(nullptr);
                g.row_index.push_back(int(m_rows.size()) - 1);
            } else {
                auto *cell = new ToolTempCell(m_box);
                cell->Enable(m_enabled);
                g.cells->Add(cell, 0, wxEXPAND);
                g.ro_cells.push_back(cell);
                g.row_index.push_back(-1);
            }
        }
        g.header->Enable(m_enabled);
        m_strip->Add(g.header, 0, wxEXPAND | wxLEFT | wxRIGHT, m_box->FromDIP(8));
        m_strip->Add(g.cells, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, m_box->FromDIP(8));
        m_strip->Show(g.cells, g.open, true);
        m_groups.push_back(g);
    }
}

ToolTempList::Row *ToolTempList::find_row(int temp_type)
{
    for (Row &r : m_rows)
        if (r.ctrl && r.ctrl->GetType() == temp_type) return &r;
    return nullptr;
}

void ToolTempList::update_row(Row &row, const ToolTempEntry &e, int nozzle_max)
{
    TempInput *c = row.ctrl;
    if (!c) return;
    c->SetBadge(wxString::FromUTF8(e.badge), e.active);
    if (e.has_temp)
        c->SetCurrTemp(e.temp);
    else
        c->SetCurrTemp(BLANK_STR);
    c->SetMaxTemp(nozzle_max);
    if (row.hold > 0) {
        row.hold--;
    } else if (!row.editing) {
        c->SetTagTemp(e.target);
        row.shown_target = e.target;
    }
    if (e.heating)
        c->SetIconActive();
    else
        c->SetIconNormal();
    const ToolCellView v = tool_cell_view(e, kRackTargetsSettable);
    c->SetReadOnly(!v.settable);
    const wxString tip = tool_description(e, v);
    if (c->GetToolTipText() != tip) c->SetToolTip(tip);
}

void ToolTempList::update_header(Group &g, const std::vector<ToolTempEntry> &entries)
{
    if (!g.header) return;
    if (g.what == ToolStripGroup::What::Rack)
        g.header->set_text(_L("Rack"), wxString::Format(_L("%d warm"), rack_warm_count(entries)), g.open);
    else
        g.header->set_text(_L("Tools"), wxString::Format(_L("%d of %d heating or hot"), tools_with_target_count(entries), int(entries.size())), g.open);
}

void ToolTempList::toggle(Group &g)
{
    g.open = !g.open;
    if (Slic3r::AppConfig *cfg = wxGetApp().app_config) cfg->set(config_key(g.what), g.open ? "0" : "1");
    m_strip->Show(g.cells, g.open, true);
    const std::vector<ToolTempEntry> &entries = g.what == ToolStripGroup::What::Rack ? m_last_rack : m_last_tools;
    update_header(g, entries);
    relayout();
}

void ToolTempList::relayout()
{
    m_box->Layout();
    if (m_on_layout) m_on_layout();
}

void ToolTempList::update(const ToolTempLayoutPlan &plan, const std::vector<ToolTempEntry> &tools, const std::vector<ToolTempEntry> &rack, int nozzle_max)
{
    m_last_plan       = plan;
    m_last_tools      = tools;
    m_last_rack       = rack;
    m_last_nozzle_max = nozzle_max;
    m_has_last        = true;

    const std::string key = shape_key(plan, tools, rack);
    if (key.empty()) {
        clear();
        return;
    }
    if (key != m_shape) {
        rebuild(plan, tools, rack);
        m_shape = key;
        relayout();
    }

    auto entry_for = [&](const std::string &id) -> const ToolTempEntry * {
        for (const ToolTempEntry &e : tools)
            if (e.id == id) return &e;
        for (const ToolTempEntry &e : rack)
            if (e.id == id) return &e;
        return nullptr;
    };
    for (Row &r : m_rows)
        if (const ToolTempEntry *e = entry_for(r.id)) update_row(r, *e, nozzle_max);

    for (Group &g : m_groups) {
        const std::vector<ToolTempEntry> &entries = g.what == ToolStripGroup::What::Rack ? rack : tools;
        for (size_t i = 0; i < g.ro_cells.size() && i < entries.size(); ++i)
            if (g.ro_cells[i]) g.ro_cells[i]->set_entry(entries[i]);
        update_header(g, entries);
    }
}

void ToolTempList::clear()
{
    m_has_last = false;
    if (m_shape.empty() && m_rows.empty() && m_groups.empty()) return;
    destroy_all();
    m_shape.clear();
    m_tools_in_list = false;
    relayout();
}

bool ToolTempList::on_finish(int temp_type, const SendFn &send)
{
    Row *r = find_row(temp_type);
    if (!r) return false;
    if (r->cancelled) {
        // Esc put the printer's own target back: nothing to send.
        r->cancelled = false;
        if (r->ctrl->GetTextCtrl()->GetValue() == wxString::Format("%d", r->shown_target)) return true;
    }
    if (r->rack) {
        // Phase 5 is not built: a rack nozzle's target is never sent (owner decision D4).
        BOOST_LOG_TRIVIAL(info) << "tool temps: rack target for " << r->id << " not sent (rack targets are read-only)";
        return true;
    }
    if (send) send(r->ctrl, r->extruder_id, r->hold);
    return true;
}

void ToolTempList::enable(bool on)
{
    m_enabled = on;
    for (Row &r : m_rows)
        if (r.ctrl) r.ctrl->Enable(on);
    for (Group &g : m_groups) {
        if (g.header) g.header->Enable(on);
        for (ToolTempCell *c : g.ro_cells)
            if (c) c->Enable(on);
    }
}

void ToolTempList::reset_values()
{
    for (Row &r : m_rows)
        if (r.ctrl) {
            r.ctrl->SetLabel(BLANK_STR);
            r.ctrl->GetTextCtrl()->SetValue(BLANK_STR);
        }
}

void ToolTempList::rescale()
{
    // Sizes, gaps and borders were set in pixels for the old scale: build again at the new one.
    for (Row &r : m_rows)
        if (r.ctrl) {
            r.ctrl->SetMinSize(m_box->FromDIP(r.min_dip));
            r.ctrl->Rescale();
        }
    for (Group &g : m_groups) {
        if (g.header) g.header->rescale();
        for (ToolTempCell *c : g.ro_cells)
            if (c) c->rescale();
    }
    if (m_has_last) {
        m_shape.clear();
        update(m_last_plan, m_last_tools, m_last_rack, m_last_nozzle_max);
    }
}

void ToolTempList::sys_color_changed()
{
    // Custom-painted parts read their colours at paint time; reload the icons and repaint.
    for (Group &g : m_groups) {
        if (g.header) g.header->Refresh();
        for (ToolTempCell *c : g.ro_cells)
            if (c) c->rescale();
    }
    for (Row &r : m_rows)
        if (r.ctrl) {
            r.ctrl->GetTextCtrl()->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
            r.ctrl->GetTextCtrl()->SetForegroundColour(StateColor::darkModeColorFor(*wxBLACK));
            r.ctrl->Rescale();
            r.ctrl->Refresh();
        }
    m_strip_line->Refresh();
}
